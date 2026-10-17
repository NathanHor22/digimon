#requires -Version 5.1
<#
.SYNOPSIS
Configure or test the ESP32-dot firmware over USB; no companion service.
.DESCRIPTION
Secrets are prompted with Read-Host -AsSecureString, transmitted only to the
local serial port, and never written to files or printed. The firmware stores
credentials on the ESP32. This is not a flasher; install ESP32-dot first.
#>
[CmdletBinding()]
param(
    [ValidateSet('Configure', 'Status', 'Say', 'Cancel')]
    [string]$Action = 'Configure',
    [string]$Port = 'COM6',
    [string]$DeviceId = 'quipus-original-01',
    [switch]$TunnelScopeAuth,
    [switch]$TextOnly,
    [string]$Text,
    [ValidateRange(0, 300)]
    [int]$WaitForReplySeconds = 0,
    [string]$TranscriptionModel = 'gpt-transcribe',
    [string]$SpeechModel = 'gpt-4o-mini-tts',
    [string]$Voice = 'coral'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$serial = $null
$config = $null
$payload = $null
$secretText = $null
$utf8 = New-Object System.Text.UTF8Encoding($false)
$script:ReplyQueue = New-Object 'System.Collections.Generic.Queue[object]'

function Read-SecretText([string]$Prompt) {
    $secure = Read-Host $Prompt -AsSecureString
    $pointer = [IntPtr]::Zero
    try {
        $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
        return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
    }
    finally {
        if ($pointer -ne [IntPtr]::Zero) {
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
        }
        $secure.Dispose()
    }
}

function Assert-Text([string]$Value, [int]$MaximumBytes, [bool]$Required,
    [bool]$HeaderValue, [string]$Label) {
    if (($Required -and [string]::IsNullOrEmpty($Value)) -or
        $utf8.GetByteCount($Value) -gt $MaximumBytes -or
        $Value -match '[\x00-\x1f\x7f]' -or
        ($HeaderValue -and $Value -match '[^\x21-\x7e]')) {
        throw "Invalid $Label. Check its length and characters; its value has not been printed."
    }
}

function Send-DeviceCommand([object]$Command) {
    $wire = $null
    try {
        $wire = ConvertTo-Json -InputObject $Command -Compress -Depth 8
        if ($utf8.GetByteCount($wire) -gt 4095) {
            throw 'The command exceeds the firmware serial limit.'
        }
        $serial.WriteLine($wire)
    }
    finally {
        # Managed strings cannot be reliably zeroed; discard references promptly.
        $wire = $null
    }
}

function Read-DeviceResponse([string]$Expected, [int]$TimeoutSeconds,
    [string]$RequestId = '') {
    if ($Expected -eq 'reply') {
        while ($script:ReplyQueue.Count -gt 0) {
            $queued = $script:ReplyQueue.Dequeue()
            if (-not $RequestId -or [string]$queued.request_id -eq $RequestId) { return $queued }
        }
    }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $line = $null
        $message = $null
        try { $line = $serial.ReadLine() }
        catch [TimeoutException] { continue }
        if ([string]::IsNullOrWhiteSpace($line) -or $line.Length -gt 8192) { continue }
        if (-not $line.TrimStart().StartsWith('{')) { continue }
        try { $message = ConvertFrom-Json -InputObject $line -ErrorAction Stop }
        catch { continue } # Never echo a rejected serial line or boot log.
        $line = $null
        if (-not $message.PSObject.Properties['dot']) { continue }
        $kind = [string]$message.dot
        if ($kind -eq 'reply' -and $message.PSObject.Properties['text'] -and
            $message.PSObject.Properties['request_id']) {
            if ($Expected -eq 'reply') {
                if (-not $RequestId -or [string]$message.request_id -eq $RequestId) { return $message }
                continue
            }
            $script:ReplyQueue.Enqueue($message)
        }
        if ($kind -eq $Expected) { return $message }
    }
    return $null
}

function Wait-DeviceStatus {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt 35) {
        Send-DeviceCommand @{ op = 'status' }
        $status = Read-DeviceResponse 'status' 1
        if ($null -ne $status -and $status.PSObject.Properties['board_id'] -and
            $status.PSObject.Properties['firmware_version'] -and
            $status.board_id -eq 'lantern-original' -and
            $status.firmware_version -eq '0.2.0') {
            return $status
        }
    }
    throw 'No ESP32-dot firmware response. Flash this project first, select the correct port, and close other serial monitors.'
}

function Show-DeviceStatus([object]$Status) {
    # Deliberate allowlist: configuration/credential fields are never emitted.
    $safe = [ordered]@{}
    foreach ($name in @('device_id', 'board_id', 'firmware_version', 'state',
        'wifi_connected', 'clock_fresh', 'tunnel_connected', 'tunnel_http_status',
        'subscription_ready', 'audio_ready', 'pending', 'reply_queued',
        'last_request_id', 'last_error', 'free_heap', 'volume',
        'microphone_muted', 'hands_free', 'name')) {
        if ($Status.PSObject.Properties[$name]) { $safe[$name] = $Status.$name }
    }
    Write-Output (ConvertTo-Json -InputObject $safe -Depth 4)
}

function Assert-DeviceAccepted([object]$Response, [string]$Operation) {
    if ($null -eq $Response -or -not $Response.PSObject.Properties['ok']) {
        throw "No acknowledgement for $Operation. Success has not been assumed."
    }
    if ($Response.ok -ne $true) {
        $code = 'device rejected the operation'
        if ($Response.PSObject.Properties['error'] -and
            [string]$Response.error -match '^ESP_[A-Z0-9_]+$') {
            $code = [string]$Response.error
        }
        throw "$Operation failed: $code"
    }
}

try {
    if ($Port -notmatch '^COM[1-9][0-9]*$') { throw 'Use a Windows serial port such as COM6.' }
    if ($Action -eq 'Configure' -and -not $TunnelScopeAuth) {
        throw 'ChatGPT NoAuth setup requires explicit -TunnelScopeAuth. Use only a dedicated personal tunnel: everyone granted tunnel Use is trusted to control this device.'
    }
    if ($Action -eq 'Configure' -and $DeviceId -notmatch '^[A-Za-z0-9_.-]{1,63}$') {
        throw 'DeviceId must contain 1 to 63 ASCII letters, digits, periods, underscores, or hyphens.'
    }
    $serial = New-Object System.IO.Ports.SerialPort($Port, 115200,
        [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $serial.Encoding = $utf8
    $serial.NewLine = [string][char]10
    $serial.ReadTimeout = 250
    $serial.WriteTimeout = 3000
    $serial.DtrEnable = $false
    $serial.RtsEnable = $false
    try { $serial.Open() }
    catch { throw "Cannot open $Port. Check the USB cable/port and close other serial programs." }
    $status = Wait-DeviceStatus

    switch ($Action) {
        'Status' { Show-DeviceStatus $status }
        'Cancel' {
            Send-DeviceCommand @{ op = 'cancel' }
            Assert-DeviceAccepted (Read-DeviceResponse 'cancel' 5) 'cancel'
            Write-Output 'Cancelled the pending device request.'
        }
        'Say' {
            if ([string]::IsNullOrEmpty($Text)) { $Text = Read-Host 'Text to send to your subscribed dot' }
            Assert-Text $Text 2000 $true $false 'message'
            Send-DeviceCommand @{ op = 'say'; text = $Text }
            Assert-DeviceAccepted (Read-DeviceResponse 'say' 5) 'say'
            Write-Output 'Device accepted the message for delivery.'
            if ($WaitForReplySeconds -gt 0) {
                Send-DeviceCommand @{ op = 'status' }
                $sentStatus = Read-DeviceResponse 'status' 5
                if ($null -eq $sentStatus -or -not $sentStatus.PSObject.Properties['last_request_id']) {
                    throw 'Message was accepted, but its request ID could not be read. Check status.'
                }
                $reply = Read-DeviceResponse 'reply' $WaitForReplySeconds ([string]$sentStatus.last_request_id)
                if ($null -eq $reply) {
                    throw 'No USB text reply arrived within the requested wait. Check status and the dot subscription; configured audio replies play on the speaker.'
                }
                Write-Output (ConvertTo-Json -InputObject ([ordered]@{
                    request_id = [string]$reply.request_id
                    text = [string]$reply.text
                }) -Depth 4)
            }
        }
        'Configure' {
            Write-Host 'USB setup: enter secrets locally. No values are saved to this computer.'
            Write-Host 'Use a personal tunnel associated only with your intended personal OpenAI organization/workspace.'
            $ssid = Read-Host 'Wi-Fi SSID (2.4 GHz)'
            Assert-Text $ssid 32 $true $false 'Wi-Fi SSID'
            $wifiPassword = Read-SecretText 'Wi-Fi password (leave empty for an open network)'
            Assert-Text $wifiPassword 64 $false $false 'Wi-Fi password'
            $tunnelId = Read-Host 'Tunnel ID from OpenAI Platform'
            if ($tunnelId -notmatch '^tunnel_(?:[a-z0-9]{4}_)?[a-z0-9]{32}$') {
                throw 'Use the complete tunnel ID returned by OpenAI Platform.'
            }
            $tunnelKey = Read-SecretText 'Restricted tunnel runtime API key (not an Admin key)'
            Assert-Text $tunnelKey 255 $true $true 'tunnel API key'
            $audioKey = ''
            if (-not $TextOnly) {
                $audioKey = Read-SecretText 'Audio API key with transcription and speech access'
                Assert-Text $audioKey 255 $true $true 'audio API key'
            }
            Assert-Text $TranscriptionModel 63 $true $true 'transcription model'
            Assert-Text $SpeechModel 63 $true $true 'speech model'
            Assert-Text $Voice 31 $true $true 'voice'
            $config = [ordered]@{
                device_id = $DeviceId
                wifi_ssid = $ssid
                wifi_password = $wifiPassword
                tunnel_id = $tunnelId
                tunnel_api_key = $tunnelKey
                audio_api_key = $audioKey
                mcp_bearer = ''
                tunnel_scope_auth = $true
                stt_model = $TranscriptionModel
                tts_model = $SpeechModel
                voice = $Voice
                epoch = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
            }
            $payload = @{ op = 'configure'; config = $config }
            Send-DeviceCommand $payload
            Assert-DeviceAccepted (Read-DeviceResponse 'configure' 10) 'configure'
            Write-Output 'Configuration saved on the ESP32. Waiting for its restart.'
            foreach ($name in @('wifi_password', 'tunnel_api_key', 'audio_api_key')) {
                $config[$name] = $null
            }
            $wifiPassword = $null; $tunnelKey = $null; $audioKey = $null
            $payload = $null; $config = $null
            Start-Sleep -Milliseconds 700
            Show-DeviceStatus (Wait-DeviceStatus)
        }
    }
}
finally {
    $payload = $null; $config = $null; $secretText = $null
    $wifiPassword = $null; $tunnelKey = $null; $audioKey = $null
    $Text = $null
    if ($null -ne $serial) {
        if ($serial.IsOpen) { $serial.Close() }
        $serial.Dispose()
    }
}
