#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Port = 'COM6',
    [string]$FirmwarePath = (Join-Path $PSScriptRoot '../firmware'),
    [string]$PythonPath,
    [string]$BackupDir,
    [ValidateSet('no_reset','default_reset')]
    [string]$Before = 'no_reset',
    [ValidateSet(115200,460800)]
    [int]$ConnectionBaud = 115200,
    [string]$ExistingBackup,
    [switch]$Flash
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (-not $PythonPath) {
    $PythonPath = Join-Path $taskRoot 'work/idf-tools/python_env/idf5.2_py3.12_env/Scripts/python.exe'
    if (-not (Test-Path -LiteralPath $PythonPath)) {
        throw 'Pass -PythonPath with a Python installation containing esptool 4.x.'
    }
}
if (-not $BackupDir) { $BackupDir = Join-Path $taskRoot 'work/private/backups' }
$flashArgs = @((Join-Path $PSScriptRoot 'flash.py'), '--port', $Port,
    '--firmware', [IO.Path]::GetFullPath($FirmwarePath), '--backup-dir', [IO.Path]::GetFullPath($BackupDir),
    '--before', $Before, '--connection-baud', [string]$ConnectionBaud)
if ($ExistingBackup) { $flashArgs += @('--existing-backup', [IO.Path]::GetFullPath($ExistingBackup)) }
if ($Flash) { $flashArgs += '--flash' }
& $PythonPath @flashArgs
if ($LASTEXITCODE -ne 0) { throw "Flasher stopped (exit $LASTEXITCODE)." }
