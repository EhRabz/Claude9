//! DLSS Multi Frame Generation unlocked on RTX 30-series (and 20-series)
//! through the released MFG Unlocker DLL (dashdogy/MFG-Unlocker, v1.7.2,
//! MIT), for GPUs Streamline's DLSS Frame Generation refuses.
//!
//! - `VICTORY_MFG=1`: load `MFGUnlocker.dll` before Streamline is loaded.
//!   Its DllMain arms the unlock; when Streamline then loads its DLSS-G
//!   provider (`sl.dlss_g.dll` / `nvngx_dlssg.dll`), the Unlocker patches
//!   the provider in memory only — no file, driver or Streamline code is
//!   changed:
//!   - the provider's minimum GPU architecture, declared as Ada (0x190)
//!     in `NVSDK_NGX_GetGPUArchitecture` and in
//!     `NVSDK_NGX_D3D12_GetFeatureRequirements` (frame-generation feature
//!     0x0B), is rewritten to Ampere (0x170) / Turing (0x160), so NGX and
//!     Streamline admit the card;
//!   - the provider's count/index gate is patched so more than one frame
//!     can be generated per rendered frame (up to 6x shown);
//!   - the provider's frame-generation programs are supplied for SM86
//!     (Ampere) from the Unlocker's own prebuilt kernel cache.
//! - `VICTORY_MFG_DIR`: the folder of `MFGUnlocker.dll` (default: the
//!   `mfg` folder beside this program, then `%LOCALAPPDATA%\victory\mfg`,
//!   then `mfg` under the current folder). `Get-MFG.cmd` / `victory
//!   get-mfg` fetches the author's release there.
//!
//! Why the released DLL, and not a port of the Unlocker's source: the
//! source alone cannot reproduce it. Its build needs separately prepared
//! inputs that are not in its tree — the SM86 kernel cache and the XeSS
//! program set (see its BUILD.md) — so a port would be a reimplementation
//! of NVIDIA's frame-generation programs, not an integration. The DLL is
//! the author's own release, fetched by the user from the author's GitHub
//! and SHA-256-checked by `Get-MFG.cmd`; it is never part of this
//! repository, the same as NVIDIA's and AMD's files.
//!
//! Anything missing (the DLL, the switch) leaves DLSS frame generation
//! exactly as Streamline reports it — on an RTX 3080 Ti, not available —
//! with a warning in the log. The Unlocker and FSR 3 frame generation
//! are mutually exclusive: never both proxy swap chains (see
//! `fsr3::init`, which refuses FSR 3 when this is loaded, and the
//! launchers, which never ask for both).
//!
//! What it does not do: it shows more frames per rendered frame; it does
//! not render faster. The submitting thread still waits on the GPU work
//! (the 2026-11-28 logs: waiting for flights / for the card dominates at
//! ~127ms a frame), so the rendered rate stays where it was and only
//! the shown rate rises. 2x is the likely win; 3x-6x depends on the
//! count-gate patch landing for the exact DLSS-G provider version
//! Streamline 2.14.16 ships — the Unlocker's log beside this program
//! (`RTXMFG-*.log`) says which provider it saw and what it patched.
//! Its Backspace menu is a bonus, not the unlock: it hooks the present
//! for its overlay, and this title presents through Streamline's proxy,
//! so the menu may not appear here. The unlock does not need it.

#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use windows_sys::Win32::System::LibraryLoader::{
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR, LOAD_LIBRARY_SEARCH_SYSTEM32, LoadLibraryExW,
};

/// The released DLL, fetched by `Get-MFG.cmd` / `victory get-mfg`.
/// Never committed to the repository.
pub const MFG_DLL: &str = "MFGUnlocker.dll";
/// The pinned release. `Get-MFG.cmd` checks both hashes.
pub const MFG_VERSION: &str = "1.7.2";
/// SHA-256 of `MFGUnlocker-v1.7.2.zip`, from the release's SHA256SUMS.txt.
pub const MFG_ZIP_SHA256: &str = "E002D624E3E7BA5CEDAB7211043C096FE8F555C30FF43500235A0686C7893F22";
/// SHA-256 of the `MFGUnlocker.dll` inside it.
pub const MFG_DLL_SHA256: &str = "A9B1C01727FA72763BEF8F604E44EC165CE1C98272DE00719572B766A11CF28F";

/// What the user asked for, read from the environment once.
#[derive(Clone, Debug)]
pub struct Asked {
    pub enabled: bool,
    pub dir: PathBuf,
}

impl Asked {
    pub fn from_env() -> Result<Self, String> {
        let var = |name: &str| std::env::var(name).ok();
        let on = |name: &str| var(name).is_some_and(|v| v.trim() == "1");
        Ok(Asked {
            enabled: on("VICTORY_MFG"),
            dir: var("VICTORY_MFG_DIR").map(PathBuf::from).unwrap_or_else(default_dir),
        })
    }

    pub fn any(&self) -> bool {
        self.enabled
    }
}

/// Where the DLL is looked for without `VICTORY_MFG_DIR`: the `mfg`
/// folder beside this program (what `Get-MFG.cmd` and `victory get-mfg`
/// populate), then `%LOCALAPPDATA%\victory\mfg`, then `mfg` under the
/// current folder. The first that exists wins; otherwise the first
/// candidate, so the error names a real place.
fn default_dir() -> PathBuf {
    let mut candidates = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            candidates.push(dir.join("mfg"));
        }
    }
    if let Ok(local) = std::env::var("LOCALAPPDATA") {
        candidates.push(PathBuf::from(local).join("victory").join("mfg"));
    }
    candidates.push(PathBuf::from("mfg"));
    for c in &candidates {
        if c.join(MFG_DLL).is_file() {
            return c.clone();
        }
    }
    candidates.into_iter().next().unwrap_or_else(|| PathBuf::from("mfg"))
}

struct Library(windows_sys::Win32::Foundation::HMODULE);

// The handle is never freed: the DLL pins itself in DllMain and its
// loader hooks must live for the process. Freeing it mid-game would
// drop the patch while Streamline still serves frame generation.
unsafe impl Send for Library {}
unsafe impl Sync for Library {}

static LOADED: OnceLock<Library> = OnceLock::new();

fn wide(path: &Path) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    path.as_os_str().encode_wide().chain([0]).collect()
}

/// Load the Unlocker before Streamline is loaded (call from `run.rs`,
/// before `streamline::init`). Its DllMain arms the backend; the patch
/// itself lands when Streamline loads the DLSS-G provider. Fails soft.
pub fn init(asked: &Asked) -> bool {
    if !asked.enabled {
        return false;
    }
    let path = match std::path::absolute(asked.dir.join(MFG_DLL)) {
        Ok(p) => p,
        Err(e) => {
            tracing::warn!("MFG Unlocker: {e}; DLSS frame generation stays as Streamline reports it");
            return false;
        }
    };
    if !path.is_file() {
        tracing::warn!(
            folder = %asked.dir.display(),
            "MFG Unlocker: no {MFG_DLL} in that folder (VICTORY_MFG_DIR); run Get-MFG.cmd to fetch v{MFG_VERSION}. DLSS frame generation stays as Streamline reports it"
        );
        return false;
    }
    let file = wide(&path);
    // Its own folder and System32 only, never the current folder.
    // SAFETY: a NUL-terminated full path.
    let module = unsafe {
        LoadLibraryExW(
            file.as_ptr(),
            std::ptr::null_mut(),
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32,
        )
    };
    if module.is_null() {
        tracing::warn!(
            "MFG Unlocker: {MFG_DLL} did not load ({}); DLSS frame generation stays as Streamline reports it",
            std::io::Error::last_os_error()
        );
        return false;
    }
    let _ = LOADED.set(Library(module));
    tracing::info!(
        folder = %asked.dir.display(),
        version = MFG_VERSION,
        "MFG Unlocker loaded: DLSS Frame Generation is unlocked for this GPU when Streamline loads its provider (see RTXMFG-*.log beside this program for what it patched)"
    );
    true
}

/// Whether the Unlocker is loaded in this process.
pub fn loaded() -> bool {
    LOADED.get().is_some()
}
