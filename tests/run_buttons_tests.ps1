param([string]$ClangPath = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\bin\clang.exe')
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $ClangPath)) { throw 'Set -ClangPath to a Windows clang installation.' }
$testOut = Join-Path $env:TEMP ('esp32-dot-buttons-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testOut | Out-Null
$testExe = Join-Path $testOut 'test_buttons.exe'
& $ClangPath -std=c17 -Wall -Wextra -Werror (Join-Path $PSScriptRoot 'test_buttons.c') `
    (Join-Path $PSScriptRoot '../main/dot_buttons.c') -o $testExe
if ($LASTEXITCODE -ne 0) { throw 'Button test compilation failed.' }
& $testExe
if ($LASTEXITCODE -ne 0) { throw 'Button tests failed.' }
