param(
    [string]$IdfPath = (Join-Path $PSScriptRoot '..\..\..\work\esp-idf'),
    [string]$ClangPath = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\bin\clang.exe'
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $ClangPath)) { throw 'Set -ClangPath to a Windows clang installation.' }
# The MSVC-targeting clang driver discovers the installed Visual Studio and
# Windows SDK itself; no developer shell or environment changes are needed.
$cjson = Join-Path $IdfPath 'components\json\cJSON'
if (!(Test-Path -LiteralPath (Join-Path $cjson 'cJSON.c'))) { throw 'Set -IdfPath to the ESP-IDF 5.2.3 checkout (including cJSON submodule).' }
$testOut = Join-Path $env:TEMP ('esp32-dot-audio-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testOut | Out-Null
$testExe = Join-Path $testOut 'test_audio.exe'
& $ClangPath -std=c17 -Werror -D_CRT_SECURE_NO_WARNINGS -Wno-unused-variable -Wno-unused-function `
    "-I$(Join-Path $PSScriptRoot 'audio_host_stubs')" "-I$cjson" `
    (Join-Path $PSScriptRoot 'test_audio.c') (Join-Path $cjson 'cJSON.c') -o $testExe
if ($LASTEXITCODE -ne 0) { throw 'Audio test compilation failed.' }
& $testExe
if ($LASTEXITCODE -ne 0) { throw 'Audio tests failed.' }
