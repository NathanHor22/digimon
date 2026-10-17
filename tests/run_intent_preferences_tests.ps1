param(
    [string]$CJsonDirectory,
    [string]$Clang,
    [string]$BuildDirectory = (Join-Path (Get-Location) 'work/intent-preferences-native-tests')
)
$ErrorActionPreference = 'Stop'
if (-not $CJsonDirectory) {
    $workspaceCopy = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../work/upstream/cJSON'))
    if (Test-Path -LiteralPath (Join-Path $workspaceCopy 'cJSON.c')) { $CJsonDirectory = $workspaceCopy }
    elseif ($env:IDF_PATH) { $CJsonDirectory = Join-Path $env:IDF_PATH 'components/json/cJSON' }
    else { throw 'Pass -CJsonDirectory pointing to the ESP-IDF cJSON source directory.' }
}
if (-not $Clang) {
    $candidate = Get-Command clang.exe -ErrorAction SilentlyContinue
    if ($candidate) { $Clang = $candidate.Source }
    else {
        $vsClang = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/bin/clang.exe'
        if (Test-Path -LiteralPath $vsClang) { $Clang = $vsClang }
        else { throw 'Pass -Clang pointing to a Windows clang compiler with the Windows SDK.' }
    }
}
$CJsonDirectory = (Resolve-Path -LiteralPath $CJsonDirectory).Path
New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
$binary = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'test_intent_preferences.exe'
& $Clang -std=c11 -Wall -Wextra -Wno-deprecated-declarations "-I$CJsonDirectory" "-I$(Join-Path $PSScriptRoot 'mcp_host_stubs')" "-I$(Join-Path $PSScriptRoot '../main')" (Join-Path $PSScriptRoot 'test_intent_preferences.c') (Join-Path $CJsonDirectory 'cJSON.c') -o $binary
if ($LASTEXITCODE -ne 0) { throw "Intent/preferences test compilation failed: $LASTEXITCODE" }
& $binary
if ($LASTEXITCODE -ne 0) { throw "Intent/preferences tests failed: $LASTEXITCODE" }
