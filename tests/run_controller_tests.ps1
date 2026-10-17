param(
    [string]$IdfPath = (Join-Path $PSScriptRoot '..\..\..\work\esp-idf'),
    [string]$ClangPath = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\bin\clang.exe'
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $ClangPath)) { throw 'Set -ClangPath to a Windows clang installation.' }
$cjson = Join-Path $IdfPath 'components\json\cJSON'
if (!(Test-Path -LiteralPath (Join-Path $cjson 'cJSON.c'))) { throw 'Set -IdfPath to the ESP-IDF checkout including the cJSON submodule.' }
$testOut = Join-Path $env:TEMP ('esp32-dot-controller-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testOut | Out-Null
$testExe = Join-Path $testOut 'test_controller.exe'
& $ClangPath -std=c17 -Werror -D_CRT_SECURE_NO_WARNINGS -Wno-unused-variable -Wno-unused-function `
    "-I$(Join-Path $PSScriptRoot 'controller_host_stubs')" "-I$cjson" `
    (Join-Path $PSScriptRoot 'test_controller.c') (Join-Path $cjson 'cJSON.c') -o $testExe
if ($LASTEXITCODE -ne 0) { throw 'Controller test compilation failed.' }
& $testExe
if ($LASTEXITCODE -ne 0) { throw 'Controller tests failed.' }
