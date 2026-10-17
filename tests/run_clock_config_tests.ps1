param(
    [string]$Clang,
    [string]$BuildDirectory = (Join-Path (Get-Location) 'work/clock-config-native-tests')
)
$ErrorActionPreference = 'Stop'
if (-not $Clang) {
    $candidate = Get-Command clang.exe -ErrorAction SilentlyContinue
    if ($candidate) { $Clang = $candidate.Source }
    else {
        $vsClang = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/bin/clang.exe'
        if (Test-Path -LiteralPath $vsClang) { $Clang = $vsClang }
        else { throw 'Pass -Clang pointing to a Windows clang compiler with the Windows SDK.' }
    }
}
New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
$binary = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'test_clock_config.exe'
& $Clang -std=c11 -Wall -Wextra -Wno-deprecated-declarations "-I$(Join-Path $PSScriptRoot 'clock_config_stubs')" "-I$(Join-Path $PSScriptRoot 'mcp_host_stubs')" "-I$(Join-Path $PSScriptRoot '../main')" (Join-Path $PSScriptRoot 'test_clock_config.c') -o $binary
if ($LASTEXITCODE -ne 0) { throw "Clock/config test compilation failed: $LASTEXITCODE" }
& $binary
if ($LASTEXITCODE -ne 0) { throw "Clock/config tests failed: $LASTEXITCODE" }
$wifiBinary = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'test_wifi.exe'
& $Clang -std=c11 -Wall -Wextra -Wno-deprecated-declarations "-I$(Join-Path $PSScriptRoot 'wifi_host_stubs')" "-I$(Join-Path $PSScriptRoot 'mcp_host_stubs')" "-I$(Join-Path $PSScriptRoot '../main')" (Join-Path $PSScriptRoot 'test_wifi.c') -o $wifiBinary
if ($LASTEXITCODE -ne 0) { throw "Wi-Fi test compilation failed: $LASTEXITCODE" }
& $wifiBinary
if ($LASTEXITCODE -ne 0) { throw "Wi-Fi tests failed: $LASTEXITCODE" }
