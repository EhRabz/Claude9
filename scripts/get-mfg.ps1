# Fetches the released MFG Unlocker (dashdogy/MFG-Unlocker, MIT) and puts
# MFGUnlocker.dll into mfg\ here (git-ignored), where
# scripts\play-foxhound.ps1 and `victory play` look for it. The DLL is
# the author's own release, under the author's licence; this repository
# does not and may not carry it, so each person gets it from the author
# directly. Get-MFG.cmd in the repository's folder runs this.
# Nothing of the Unlocker's is needed to build the layer: the DLL is
# loaded when the game runs, before Streamline, and patches the DLSS-G
# provider in memory only.
#
# v1.7.2 is the pinned version. Both hashes below come from the
# release's own SHA256SUMS.txt; the download fails loudly here rather
# than silently installing a different file. To bump the version, change
# $Version and both hashes from the new release's SHA256SUMS.txt.
param(
    # Another folder for the DLL than mfg here (`victory get-mfg`
    # names one under %LOCALAPPDATA%\victory).
    [string]$InstallDir = ''
)
$Version = '1.7.2'
$ZipSha256 = 'E002D624E3E7BA5CEDAB7211043C096FE8F555C30FF43500235A0686C7893F22'
$DllSha256 = 'A9B1C01727FA72763BEF8F604E44EC165CE1C98272DE00719572B766A11CF28F'
# The cmdlets this script uses (Join-Path, Test-Path, ...) come from
# Microsoft.PowerShell.Management. A session with module auto-loading
# off does not have them, and the script then fails on its first line
# ("Join-Path is not recognized") - as happened when it was run from
# inside another PowerShell window. Load the module explicitly, from
# its usual places if the module path itself is broken.
# SHA-256 of a file via .NET (Get-FileHash needs PS 4+ and its module;
# this works everywhere). Returns the uppercase hex hash.
function Get-Sha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        try { $bytes = $sha.ComputeHash($stream) } finally { $stream.Close() }
    } finally { $sha.Dispose() }
    -join ($bytes | ForEach-Object { $_.ToString('X2') })
}
if (-not (Get-Command Get-FileHash -ErrorAction SilentlyContinue)) { Import-Module Microsoft.PowerShell.Utility -ErrorAction SilentlyContinue }
if (-not (Get-Command Join-Path -ErrorAction SilentlyContinue)) {
    Import-Module Microsoft.PowerShell.Management -ErrorAction SilentlyContinue
}
if (-not (Get-Command Join-Path -ErrorAction SilentlyContinue)) {
    foreach ($base in @("$env:SystemRoot\System32\WindowsPowerShell\v1.0\Modules", "$PSHOME\Modules")) {
        $psd1 = [System.IO.Path]::Combine($base, 'Microsoft.PowerShell.Management', 'Microsoft.PowerShell.Management.psd1')
        if ([System.IO.File]::Exists($psd1)) { Import-Module $psd1 -ErrorAction Stop; break }
    }
}
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest's progress bar makes it many times slower
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$install = if ($InstallDir) { $InstallDir } else { Join-Path $repo 'mfg' }
New-Item -ItemType Directory -Force $install | Out-Null

# One file from a URL, with curl's progress line where Windows has curl (it has since Windows 10 1803).
function Get-File([string]$Url, [string]$To) {
    New-Item -ItemType Directory -Force (Split-Path $To) | Out-Null
    $part = "$To.part"
    if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
        & curl.exe --location --fail --silent --show-error --progress-bar --output $part $Url
        if ($LASTEXITCODE -ne 0) { Remove-Item $part -ErrorAction SilentlyContinue; throw "could not get $Url" }
    } else {
        Invoke-WebRequest -Uri $Url -OutFile $part -UseBasicParsing -ErrorAction Stop
    }
    Move-Item $part $To -Force
}

Write-Host "MFG Unlocker v$Version, from github.com/dashdogy/MFG-Unlocker/releases:"
$url = "https://github.com/dashdogy/MFG-Unlocker/releases/download/v$Version/MFGUnlocker-v$Version.zip"
$zip = Join-Path $install "MFGUnlocker-v$Version.zip"
try { Get-File $url $zip } catch {
    throw "Could not download MFG Unlocker v$Version from $url. Check the version exists (v1.7.2 is the pinned one): $($_.Exception.Message)"
}
$hash = Get-Sha256 $zip
if ($hash -ne $ZipSha256) { Remove-Item $zip -Force; throw "MFGUnlocker-v$Version.zip has SHA-256 $hash, not the pinned $ZipSha256. Not installing it." }
# The zip holds one file, MFGUnlocker.dll. Not Expand-Archive -Force: that calls the 3-argument
# ExtractToDirectory, which an older .NET Framework does not have
# ("Cannot find an overload ... argument count: 3"). The
# 2-argument form has existed since .NET 4.5, and the folder was
# just removed below, so nothing needs overwriting.
$scratch = Join-Path $install 'zip-src'
Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
[System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $scratch)
Remove-Item $zip -Force
$dll = Get-ChildItem $scratch -Filter 'MFGUnlocker.dll' | Select-Object -First 1
if (-not $dll) { throw "No MFGUnlocker.dll in MFGUnlocker-v$Version.zip; that is not the MFG Unlocker release." }
$dllHash = Get-Sha256 $dll.FullName
if ($dllHash -ne $DllSha256) { throw "MFGUnlocker.dll inside the zip has SHA-256 $dllHash, not the pinned $DllSha256. Not installing it." }
Copy-Item $dll.FullName (Join-Path $install 'MFGUnlocker.dll') -Force
Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue

$missing = @('MFGUnlocker.dll') | Where-Object { -not (Test-Path (Join-Path $install $_)) }
if ($missing) { throw "Not there after all: $($missing -join ', ')" }
Write-Host ''
Write-Host "Done. In $install : MFGUnlocker.dll (v$Version)."
Write-Host 'Next: .\scripts\play-foxhound.ps1 -Exe <path of your own copy''s executable> -Mfg'
Write-Host '  (or: victory play --mfg). DLSS frame generation unlocked on RTX 30-series, 2x to 6x shown per rendered frame.'
