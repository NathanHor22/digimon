#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$CJsonDirectory,
    [string]$Clang,
    [string]$BuildDirectory = (Join-Path (Get-Location) 'work/tunnel-native-tests')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $CJsonDirectory) {
    $workspaceCopy = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../work/upstream/cJSON'))
    if (Test-Path -LiteralPath (Join-Path $workspaceCopy 'cJSON.c')) {
        $CJsonDirectory = $workspaceCopy
    } elseif ($env:IDF_PATH) {
        $CJsonDirectory = Join-Path $env:IDF_PATH 'components/json/cJSON'
    } else {
        throw 'Pass -CJsonDirectory pointing to cJSON.c and cJSON.h from ESP-IDF 5.2.3.'
    }
}
if (-not $Clang) {
    $candidate = Get-Command clang.exe -ErrorAction SilentlyContinue
    if ($candidate) { $Clang = $candidate.Source }
    else {
        $vsClang = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/bin/clang.exe'
        if (Test-Path -LiteralPath $vsClang) { $Clang = $vsClang }
        else { throw 'Pass -Clang pointing to Windows clang with the Visual Studio C runtime and Windows SDK.' }
    }
}
$CJsonDirectory = (Resolve-Path -LiteralPath $CJsonDirectory).Path
foreach ($name in @('cJSON.c', 'cJSON.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $CJsonDirectory $name))) {
        throw "Missing $name in CJsonDirectory."
    }
}
$Clang = (Resolve-Path -LiteralPath $Clang).Path
New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
$binary = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'test_tunnel.exe'
$main = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../main'))
$source = Join-Path $PSScriptRoot 'test_tunnel.c'
$stubs = Join-Path $PSScriptRoot 'tunnel_host_stubs'
$compilerArgs = @('-std=c11', '-Wall', '-Wextra', '-Werror',
    '-Wno-deprecated-declarations', '-DDOT_TUNNEL_PROTOCOL_ONLY',
    "-I$CJsonDirectory", "-I$stubs", "-I$main", $source,
    (Join-Path $main 'dot_tunnel.c'), (Join-Path $CJsonDirectory 'cJSON.c'),
    '-o', $binary)
& $Clang @compilerArgs
if ($LASTEXITCODE -ne 0) { throw "Tunnel test compilation failed: $LASTEXITCODE" }
& $binary
if ($LASTEXITCODE -ne 0) { throw "Tunnel tests failed: $LASTEXITCODE" }
