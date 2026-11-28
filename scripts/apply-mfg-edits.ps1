# Applies the small MFG Unlocker edits that are not in the drop-in files:
# main.rs (GetMfg command), victory-gpu lib.rs (pub mod mfg),
# victory-hle lib.rs (re-export mfg), .gitignore (/mfg/).
# Run from the repository folder (E:\Claude9). Idempotent: running it
# twice does nothing the second time.
# Uses .NET only — no PowerShell cmdlets at all, so it works even when
# every module is missing from the session.
$ErrorActionPreference = 'Stop'

$repo = [System.IO.Path]::GetDirectoryName($PSScriptRoot)
if (-not $repo) { $repo = [System.Environment]::CurrentDirectory }

function Add-After([string]$File, [string]$Anchor, [string]$Insert) {
    $text = [System.IO.File]::ReadAllText($File)
    if ($text.Contains($Insert.Trim())) { [Console]::WriteLine("already in ${File}: skipped"); return }
    if (-not $text.Contains($Anchor)) { throw "anchor not found in ${File}: ${Anchor}" }
    $idx = $text.IndexOf($Anchor, [System.StringComparison]::Ordinal)
    $text = $text.Substring(0, $idx) + $Anchor + "`r`n" + $Insert + $text.Substring($idx + $Anchor.Length)
    [System.IO.File]::WriteAllText($File, $text)
    [Console]::WriteLine("edited ${File}")
}

# crates/victory-gpu/src/lib.rs
Add-After ([System.IO.Path]::Combine($repo, 'crates', 'victory-gpu', 'src', 'lib.rs')) `
    'pub mod fsr3;' `
    '/// MFG Unlocker (`VICTORY_MFG`), unlocking DLSS frame generation on
/// RTX 30-series through the released MFGUnlocker.dll.
#[cfg(windows)]
pub mod mfg;'

# crates/victory-hle/src/lib.rs
Add-After ([System.IO.Path]::Combine($repo, 'crates', 'victory-hle', 'src', 'lib.rs')) `
    'pub use victory_gpu::fsr3;' `
    '/// MFG Unlocker (`VICTORY_MFG`), unlocking DLSS frame generation on
/// RTX 30-series through the released MFGUnlocker.dll.
#[cfg(windows)]
pub use victory_gpu::mfg;'

# apps/victory/src/main.rs — the GetMfg command
Add-After ([System.IO.Path]::Combine($repo, 'apps', 'victory', 'src', 'main.rs')) `
    '    #[cfg(windows)]
    GetFsr3,' `
    '    /// Fetch the MFG Unlocker release (v1.7.2, about 5 MB) and put its
    /// `MFGUnlocker.dll` where `play` looks for it, unlocking DLSS
    /// frame generation on RTX 30-series and other GPUs DLSS frame
    /// generation cannot run on.
    #[cfg(windows)]
    GetMfg,'

# apps/victory/src/main.rs — the match arm
Add-After ([System.IO.Path]::Combine($repo, 'apps', 'victory', 'src', 'main.rs')) `
    '        Command::GetFsr3 => play::get_fsr3(),' `
    '        Command::GetMfg => play::get_mfg(),'

# .gitignore — keep the fetched DLL out of the repo
$gitignore = [System.IO.Path]::Combine($repo, '.gitignore')
$gitText = [System.IO.File]::ReadAllText($gitignore)
if ($gitText -notmatch '(?m)^/mfg/$') {
    [System.IO.File]::AppendAllText($gitignore, "/mfg/`r`n")
    [Console]::WriteLine("edited ${gitignore}")
} else {
    [Console]::WriteLine("already in ${gitignore}: skipped")
}

[Console]::WriteLine('')
[Console]::WriteLine('Done. Now rebuild: cargo build --release --locked -p victory')
