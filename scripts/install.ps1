# VOMeter installer for Windows.
#
# Run from an extracted release zip (right-click > Run with PowerShell), or install
# the latest release in one go from an *Administrator* PowerShell:
#   irm https://raw.githubusercontent.com/kyle95wm/vo-meter/main/scripts/install.ps1 | iex
#
# Installs to the standard VST3 folder: C:\Program Files\Common Files\VST3
# (writing there needs Administrator rights; the script re-launches itself elevated if needed).

$ErrorActionPreference = 'Stop'
$Repo = 'kyle95wm/vo-meter'
$Name = 'VOMeter'
$Dest = Join-Path $env:CommonProgramFiles 'VST3'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    if ($PSCommandPath) {
        Write-Host 'Administrator rights are needed to write to the VST3 folder; relaunching elevated...'
        Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
        exit
    }
    throw 'Please run this from an Administrator PowerShell.'
}

$src = $null
if ($PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot "$Name.vst3"))) {
    $src = Join-Path $PSScriptRoot "$Name.vst3"
} else {
    Write-Host "Fetching the latest $Name release for Windows..."
    $release = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ 'User-Agent' = 'VOMeter-installer' }
    $asset = $release.assets | Where-Object { $_.name -like '*-Windows.zip' } | Select-Object -First 1
    if (-not $asset) { throw "Couldn't find a Windows download in the latest release of $Repo." }
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid())
    New-Item -ItemType Directory $tmp | Out-Null
    $zip = Join-Path $tmp 'VOMeter.zip'
    Invoke-WebRequest $asset.browser_download_url -OutFile $zip
    Expand-Archive $zip -DestinationPath $tmp
    $src = Join-Path $tmp "VOMeter\$Name.vst3"
}

New-Item -ItemType Directory -Force $Dest | Out-Null
$target = Join-Path $Dest "$Name.vst3"
if (Test-Path $target) { Remove-Item $target -Recurse -Force }
Copy-Item $src $Dest -Recurse
Write-Host "Installed $Name.vst3 -> $Dest"
Write-Host "Done. Rescan plugins in your DAW if $Name doesn't show up."
if ($PSCommandPath) { Read-Host 'Press Enter to close' }
