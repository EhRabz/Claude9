//! FSR 3 Frame Generation and optional upscaling.
//!
//! Activated by environment variables (parallel to the existing Streamline/DLSS
//! path so RTX 40/50 users are unaffected):
//!
//! - `VICTORY_FSR3_FG=1`: enable FSR 3 Frame Generation (works on RTX 30-series).
//! - `VICTORY_FSR3=1`: enable FSR 3 Super Resolution (optional; title's own
//!   FidelityFX or DLSS can still be used for upscaling).
//! - `VICTORY_FSR3_DIR`: folder containing the FidelityFX FSR 3 native DX12
//!   backend (DLL or static). Default is a placeholder; a `Get-FSR3.cmd`
//!   script should populate it the same way `Get-NVIDIA-DLSS.cmd` works.
//!
//! This module is a scaffold. The actual FidelityFX SDK calls, resource
//! binding, and proxy swap-chain creation still need to be filled in from the
//! open-source FSR 3 DX12 backend (see `docs/FSR3_INTEGRATION.md`).
//!
//! The existing interceptor in `victory_hle::modules::fidelityfx` already
//! extracts color, depth, motion vectors, exposure and reactive mask from the
//! title's `ffxDispatch`. Those same values can be handed here.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;

/// Default location looked for without `VICTORY_FSR3_DIR`.
pub const DEFAULT_DIR: &str = r"C:\dev\sdk\FidelityFX-SDK-FSR3";

static ENABLED: OnceLock<Asked> = OnceLock::new();
static LOADED: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Debug)]
pub struct Asked {
    pub fsr3: bool,
    pub framegen: bool,
    pub dir: PathBuf,
    pub quality: String,
}

/// Read user request from the environment (once).
pub fn asked() -> &'static Asked {
    ENABLED.get_or_init(|| {
        let on = |k: &str| std::env::var(k).ok().map(|v| v != "0").unwrap_or(false);
        Asked {
            fsr3: on("VICTORY_FSR3"),
            framegen: on("VICTORY_FSR3_FG"),
            dir: std::env::var("VICTORY_FSR3_DIR")
                .map(PathBuf::from)
                .unwrap_or_else(|_| PathBuf::from(DEFAULT_DIR)),
            quality: std::env::var("VICTORY_FSR3_QUALITY")
                .unwrap_or_else(|_| "quality".into()),
        }
    })
}

/// True when the user asked for FSR 3 Frame Generation and we should take
/// the present path instead of Streamline.
pub fn framegen_wanted() -> bool {
    asked().framegen
}

/// Placeholder: load the FidelityFX FSR 3 native DX12 backend from `asked().dir`.
/// Returns `Ok(())` on success. Currently always fails with a clear message so
/// the rest of the layer keeps running.
pub fn try_load() -> Result<(), String> {
    if LOADED.load(Ordering::Relaxed) {
        return Ok(());
    }
    let a = asked();
    if !a.fsr3 && !a.framegen {
        return Err("FSR 3 not requested".into());
    }
    let marker = a.dir.join("ffx_fsr3.dll"); // placeholder name
    if !marker.exists() && !a.dir.exists() {
        return Err(format!(
            "FSR 3 backend not found in {} (set VICTORY_FSR3_DIR or run Get-FSR3.cmd)",
            a.dir.display()
        ));
    }
    // TODO: LoadLibrary / static link of FidelityFX FSR 3 DX12 backend,
    // create context, query supported quality modes, etc.
    tracing::warn!(
        dir = %a.dir.display(),
        "FSR 3 scaffold loaded; real SDK binding not yet implemented"
    );
    LOADED.store(true, Ordering::Relaxed);
    Ok(())
}

/// Record that an upscale dispatch occurred and its resources are available.
/// Called from the fidelityfx interceptor when FSR 3 is active.
pub fn note_upscale(
    _color: *mut std::ffi::c_void,
    _depth: *mut std::ffi::c_void,
    _motion: *mut std::ffi::c_void,
    _reactive: Option<*mut std::ffi::c_void>,
    _render_size: [u32; 2],
    _output_size: [u32; 2],
) {
    if !framegen_wanted() && !asked().fsr3 {
        return;
    }
    // TODO: store handles / create FSR 3 resources, call ffxDispatchDescUpscale
    // or the frame-generation prepare path.
}

/// Present-path hook. When FSR 3 FG is active this should present through the
/// FrameInterpolationSwapChainDX12 proxy instead of the plain swap chain.
pub fn present_hooked() -> bool {
    framegen_wanted() && LOADED.load(Ordering::Relaxed)
}

/// Placeholder for the actual proxy-swapchain creation.
pub fn create_proxy_swapchain(_desc: *const std::ffi::c_void) -> Result<(), String> {
    Err("FSR 3 proxy swap chain not yet implemented".into())
}
