$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$path = Join-Path $PSScriptRoot '..\tools\provision.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
if ($errors) { throw 'Provisioner syntax invalid' }
$functions = $ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $false)
foreach ($definition in $functions) { . ([scriptblock]::Create($definition.Extent.Text)) }
function Check([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message } }
$utf8 = New-Object System.Text.UTF8Encoding($false)
$script:ReplyQueue = New-Object 'System.Collections.Generic.Queue[object]'
$serial = [pscustomobject]@{
    Written = New-Object 'System.Collections.Generic.List[string]'
    Lines = New-Object 'System.Collections.Generic.Queue[string]'
}
$serial | Add-Member -MemberType ScriptMethod -Name WriteLine -Value {
    param([string]$Value) $this.Written.Add($Value)
}
$serial | Add-Member -MemberType ScriptMethod -Name ReadLine -Value {
    if ($this.Lines.Count -eq 0) { throw [TimeoutException]::new() }
    return $this.Lines.Dequeue()
}
Assert-Text 'network' 32 $true $false 'SSID'
Assert-Text '' 64 $false $false 'password'
$rejected = $false
try { Assert-Text ([string][char]10) 64 $false $false 'password' } catch { $rejected = $true }
Check $rejected 'Control-character validation failed'
$rejected = $false
try { Assert-Text ('a' * 256) 255 $true $true 'key' } catch { $rejected = $true }
Check $rejected 'Key length bound failed'
Send-DeviceCommand @{ op='configure'; config=@{
    tunnel_scope_auth=$true; wifi_password='test-"quote-\slash'; epoch=1792220000
} }
Check ($serial.Written.Count -eq 1) 'Unexpected serial write count'
$decoded = ConvertFrom-Json -InputObject $serial.Written[0]
Check ($decoded.config.wifi_password -eq 'test-"quote-\slash') 'Secret wire escaping changed data'
Check ($decoded.config.tunnel_scope_auth -eq $true) 'Scope flag lost JSON Boolean type'
Check ($decoded.config.epoch -eq 1792220000) 'Epoch was not transmitted'
$serial.Lines.Enqueue('ignored boot line containing FAKE_SECRET_DO_NOT_PRINT')
$serial.Lines.Enqueue('{"dot":"reply","request_id":"old","text":"old reply"}')
$serial.Lines.Enqueue('{"dot":"status","board_id":"lantern-original","firmware_version":"0.2.0","state":"ready","audio_api_key":"FAKE_SECRET_DO_NOT_PRINT"}')
$status = Read-DeviceResponse 'status' 2
Check ($status.dot -eq 'status') 'Status was not selected'
$shown = Show-DeviceStatus $status
Check (-not $shown.Contains('FAKE_SECRET_DO_NOT_PRINT')) 'Status printing leaked unknown credential property'
$serial.Lines.Enqueue('{"dot":"reply","request_id":"new","text":"correct reply"}')
$reply = Read-DeviceResponse 'reply' 2 'new'
Check ($reply.request_id -eq 'new') 'A stale reply passed request correlation'
Assert-DeviceAccepted ([pscustomobject]@{ok=$true}) 'test'
$rejected = $false
try { Assert-DeviceAccepted ([pscustomobject]@{ok=$false;error='ESP_ERR_INVALID_STATE'}) 'test' }
catch { $rejected = $true }
Check $rejected 'Negative firmware acknowledgement accepted'
$constructed = New-Object System.IO.Ports.SerialPort('COM99', 115200,
    [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
Check (-not $constructed.IsOpen) 'Constructor unexpectedly opened a port'
$constructed.Dispose()
'PASS: provisioner parser, validation, JSON escaping, Boolean/epoch data, secret output filtering, correlated replies, acknowledgements, unopened SerialPort construction'
