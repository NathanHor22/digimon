#requires -Version 5.1
param(
    [string]$Clang = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/bin/clang.exe',
    [string]$BuildDirectory = (Join-Path (Get-Location) 'work/ui-native-tests')
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Clang)) { throw 'Pass -Clang pointing to Windows clang with the Windows SDK.' }
New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
$binary = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'test_ui.exe'
$main = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../main'))
$compilerArgs = @('-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
    "-I$main", "-I$(Join-Path $PSScriptRoot 'tunnel_host_stubs')",
    (Join-Path $PSScriptRoot 'test_ui.c'), (Join-Path $main 'dot_ui.c'), '-o', $binary)
& $Clang @compilerArgs
if ($LASTEXITCODE -ne 0) { throw 'UI test compilation failed.' }
& $binary
if ($LASTEXITCODE -ne 0) { throw 'UI tests failed.' }
