param(
    [string]$CJsonDirectory,
    [string]$Clang,
    [string]$BuildDirectory = (Join-Path (Get-Location) 'work/mcp-native-tests')
)
$ErrorActionPreference = 'Stop'
if (-not $CJsonDirectory) {
    $workspaceCopy = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../work/upstream/cJSON'))
    if (Test-Path -LiteralPath (Join-Path $workspaceCopy 'cJSON.c')) {
        $CJsonDirectory = $workspaceCopy
    } elseif ($env:IDF_PATH) {
        $CJsonDirectory = Join-Path $env:IDF_PATH 'components/json/cJSON'
    } else {
        throw 'Pass -CJsonDirectory pointing to the ESP-IDF cJSON source directory (cJSON.c and cJSON.h).'
    }
}
if (-not $Clang) {
    $candidate = Get-Command clang.exe -ErrorAction SilentlyContinue
    if ($candidate) { $Clang = $candidate.Source }
    else {
        $vsClang = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/bin/clang.exe'
        if (Test-Path -LiteralPath $vsClang) { $Clang = $vsClang }
        else { throw 'Pass -Clang pointing to a Windows clang compiler with the Windows SDK and BCrypt library.' }
    }
}
$CJsonDirectory = (Resolve-Path -LiteralPath $CJsonDirectory).Path
foreach ($source in @('cJSON.c', 'cJSON.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $CJsonDirectory $source))) { throw "Missing $source in CJsonDirectory" }
}
New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
$binary = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'test_mcp.exe'
$source = Join-Path $PSScriptRoot 'test_mcp.c'
$stubs = Join-Path $PSScriptRoot 'mcp_host_stubs'
$main = Join-Path $PSScriptRoot '../main'
& $Clang -std=c11 -Wall -Wextra -Wno-unused-variable -Wno-deprecated-declarations "-I$CJsonDirectory" "-I$stubs" "-I$main" $source (Join-Path $CJsonDirectory 'cJSON.c') -o $binary -lbcrypt
if ($LASTEXITCODE -ne 0) { throw "MCP test compilation failed: $LASTEXITCODE" }
& $binary (Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'mcp-schema-fixtures.json')
if ($LASTEXITCODE -ne 0) { throw "MCP tests failed: $LASTEXITCODE" }
