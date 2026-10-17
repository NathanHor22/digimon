#requires -Version 5.1
<#
.SYNOPSIS
Build ESP32-dot and package its verified firmware; never access the device.
.DESCRIPTION
Uses ESP-IDF 5.2.3 and its existing tools/Python environment. The prepared local
SDK contains a Windows compiler-path fix; this wrapper never modifies an SDK.
For a new setup, install ESP-IDF/tools at paths without spaces.
#>
[CmdletBinding()]
param(
    [string]$IdfPath,
    [Alias('IdfToolsPath')]
    [string]$ToolsPath,
    [string]$PythonEnvironment,
    [string]$BuildDirectory,
    [string]$FirmwarePath,
    [string]$PythonPath,
    [switch]$Check,
    [switch]$Quiet
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $PythonPath) {
    $taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
    $candidates = @()
    if ($PythonEnvironment) { $candidates += Join-Path $PythonEnvironment 'Scripts/python.exe' }
    if ($env:IDF_PYTHON_ENV_PATH) { $candidates += Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe' }
    $candidates += Join-Path $taskRoot 'work/idf-tools/python_env/idf5.2_py3.12_env/Scripts/python.exe'
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $PythonPath = $candidate; break }
    }
    if (-not $PythonPath) {
        $found = Get-Command python.exe -ErrorAction SilentlyContinue
        if ($found) { $PythonPath = $found.Source }
        else { throw 'Pass -PythonPath pointing to Python 3.9 or later. No tools are installed automatically.' }
    }
}
$buildArgs = @((Join-Path $PSScriptRoot 'build.py'))
foreach ($item in @(
    @('--idf-path', $IdfPath), @('--tools-path', $ToolsPath),
    @('--python-env', $PythonEnvironment), @('--build-dir', $BuildDirectory),
    @('--firmware-dir', $FirmwarePath)
)) {
    if ($item[1]) { $buildArgs += @($item[0], $item[1]) }
}
if ($Check) { $buildArgs += '--check' }
if ($Quiet) { $buildArgs += '--quiet' }
& $PythonPath @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build stopped (exit $LASTEXITCODE)." }
