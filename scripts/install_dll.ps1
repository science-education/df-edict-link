<#
.SYNOPSIS
  Install the WinUSB replacement USB_PC.dll into a PASORAMA installation.

.DESCRIPTION
  Backs up the original vendor USB_PC.dll (once, to USB_PC.dll.orig_backup) and
  copies the freshly built replacement over it. PASORAMA must be closed first.
  Must be run elevated (Program Files is write-protected).

.PARAMETER PasoramaDir
  The PASORAMA install directory that contains USB_PC.dll and PASORAMA.exe.
  Default: "C:\Program Files\PASORAMA\df-x10000".

.PARAMETER Dll
  Path to the replacement USB_PC.dll to install. Default: ..\build\USB_PC.dll.

.EXAMPLE
  # from an elevated PowerShell:
  .\scripts\install_dll.ps1
  .\scripts\install_dll.ps1 -PasoramaDir "D:\Apps\PASORAMA\df-x10000"
#>
param(
    [string]$PasoramaDir = "C:\Program Files\PASORAMA\df-x10000",
    [string]$Dll = (Join-Path $PSScriptRoot "..\build\USB_PC.dll")
)
$ErrorActionPreference = 'Stop'

$dst    = Join-Path $PasoramaDir "USB_PC.dll"
$backup = Join-Path $PasoramaDir "USB_PC.dll.orig_backup"

if (-not (Test-Path $Dll))       { throw "Replacement DLL not found: $Dll  (run build.bat first)" }
if (-not (Test-Path $dst))       { throw "USB_PC.dll not found in $PasoramaDir - is PASORAMA installed there?" }

# refuse if not elevated
$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw "Run this in an elevated PowerShell (Program Files is write-protected)." }

Get-Process PASORAMA -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# back up the genuine vendor DLL exactly once, never overwrite the backup
if (-not (Test-Path $backup)) {
    Copy-Item $dst $backup -Force
    Write-Host "Backed up original -> $backup"
} else {
    Write-Host "Original backup already present -> $backup (left untouched)"
}

Copy-Item $Dll $dst -Force
Write-Host "Installed replacement -> $dst"
Get-FileHash $dst -Algorithm SHA256 | Format-List
Write-Host "Done. Launch PASORAMA.exe normally."
