# FSR 3 Frame Generation for VIctory-NVIDIA (RTX 30-series focus)

## Why this exists

NVIDIA DLSS Frame Generation (Streamline / `sl.dlss_g`) is limited to RTX 40-series and newer.
An RTX 3080 Ti (Ampere) cannot use it. AMD FSR 3 Frame Generation works on Ampere (and most other GPUs)
because it is a software optical-flow + interpolation solution with a proxy swap chain for pacing.

This document describes how to add FSR 3 FG (and optionally FSR 3 upscaling) by reusing the existing
FidelityFX interception path that already feeds DLSS.

## Existing integration points (do not reinvent)

| What | Where | Notes |
|------|-------|-------|
| Title's FidelityFX upscale dispatch | `crates/victory-hle/src/modules/fidelityfx.rs` | Intercepts `ffxDispatch` of upscale. Already hands color/depth/MV/exposure/reactive to Streamline when `VICTORY_DLSS` or `VICTORY_FRAMEGEN` is set. |
| Streamline / DLSS | `crates/victory-gpu/src/streamline.rs` + `streamline/` | Runtime load of `sl.interposer.dll`, resource tagging, present-path FG. |
| Depth + MV capture for FG | Same modules | Already noted for the present path. Reusable for FSR 3. |
| Settings / env | `VICTORY_DLSS`, `VICTORY_FRAMEGEN`, launcher scripts, F1 menu | Extend similarly. |

## Recommended approach (minimal risk)

1. **Keep the existing Streamline path untouched** for RTX 40/50 users.
2. Add a parallel path activated by new env vars:
   - `VICTORY_FSR3=1` — enable FSR 3 upscaling (optional)
   - `VICTORY_FSR3_FG=1` — enable FSR 3 Frame Generation (the important one for 3080 Ti)
3. When FSR 3 FG is requested, **do not** load Streamline for frame gen. Use the FidelityFX SDK's
   `FrameInterpolationSwapChainDX12` instead.
4. Reuse the same resource extraction that `fidelityfx.rs` already does for DLSS.

## High-level architecture

```
Title ffxDispatch (upscale)
        │
        ▼
fidelityfx.rs interceptor
        │
        ├── VICTORY_DLSS / VICTORY_FRAMEGEN → existing Streamline path
        │
        └── VICTORY_FSR3_FG (or VICTORY_FSR3)
                │
                ▼
        fsr3.rs (new)
                │
                ├── Record color / depth / MV / exposure / reactive
                ├── (optional) run FSR 3 upscaler
                └── On present → FSR 3 Frame Interpolation SwapChain
```

## What must be supplied to FSR 3 Frame Generation

From the existing `DispatchUpscale` layout in `fidelityfx.rs` (already parsed):

- Color buffer (prefer hudless / without interface plane when available)
- Depth buffer
- Motion vectors
- Exposure / pre-exposure
- Reactive mask (optional but improves quality)
- Jitter, motion-vector scale, render size, output size
- Camera near/far/FOV
- Frame time delta

These are already extracted for DLSS. The same extraction can be handed to FSR 3.

## Implementation steps (ordered)

### 1. SDK acquisition
- Download FidelityFX SDK FSR3 branch (e.g. `fsr3-v3.0.3` or later 3.1.x) from GPUOpen.
- Use the **native DirectX 12** backend (`BuildFSRSolutionNative.bat` / DLL route).
- Do **not** commit the SDK binaries (same policy as NVIDIA Streamline). Provide a `Get-FSR3.cmd` analogous to `Get-NVIDIA-DLSS.cmd`.

### 2. New module: `crates/victory-gpu/src/fsr3.rs`
Mirror the structure of `streamline.rs`:
- Runtime load of the FSR 3 DLL / static link option.
- Context creation (`ffxFsr3ContextCreate` or the newer FSR API).
- Resource binding and dispatch.
- Proxy swap chain creation for frame interpolation (`ffxCreateFrameinterpolationSwapchainDX12` or equivalent).
- Env-var reading (`VICTORY_FSR3`, `VICTORY_FSR3_FG`, `VICTORY_FSR3_QUALITY`, etc.).

### 3. Interceptor change (`fidelityfx.rs`)
In the `ffxDispatch` handler for upscale:
- If `VICTORY_FSR3_FG` (or `VICTORY_FSR3`) is set and FSR 3 loaded successfully,
  call the new FSR 3 path instead of (or in addition to) Streamline.
- Still fall back to the title's own upscaler if FSR 3 is unavailable.

### 4. Present path
- When FSR 3 FG is active, the swap chain must be the FSR 3 proxy swap chain so that
  interpolation and frame pacing happen correctly.
- This is the most invasive part. Study the existing Streamline present hooking
  (`VICTORY_STREAMLINE_PRESENT`, device/factory upgrade) and replicate the pattern
  for FSR 3's `FrameInterpolationSwapChainDX12`.

### 5. Settings / launcher
- Add env vars and F1 menu entries:
  - FSR 3 on/off
  - Quality mode (Quality / Balanced / Performance / Native AA)
  - Frame Generation on/off
  - Optional reactive mask toggle
- Update `scripts/play-nvidia.ps1` (or add `play-fsr3.ps1`) so a 3080 Ti user can
  start with `-Fsr3FrameGen` without touching NVIDIA Streamline.

### 6. Testing checklist (must be done on real hardware + title)
- [ ] FSR 3 context creates on 3080 Ti
- [ ] Color / depth / MV correctly bound (no black or frozen frames)
- [ ] Frame generation produces ~2× visual rate while input latency stays at the real render rate
- [ ] No crashes on scene cuts, menu transitions, or resolution changes
- [ ] Coexistence: DLSS path still works when FSR 3 env vars are off
- [ ] VRAM budget (3080 Ti has 12 GB) — FSR 3 FG has its own memory cost

## Quality / performance notes for 3080 Ti

- FSR 3 FG quality is generally good but can show artifacts on fast motion, thin geometry, or UI.
- Prefer a hudless color buffer when possible (the codebase already has some interface-plane awareness).
- Start with Quality or Balanced mode; Performance mode is softer.
- Optical flow cost is non-trivial; on a 3080 Ti you should still see a clear net gain in visual smoothness.

## Licensing

FidelityFX SDK FSR 3 source is MIT (see GPUOpen). Keep the same "download at runtime / do not commit binaries"
policy that the repo already uses for NVIDIA Streamline.

## Suggested first milestone

Ship **FSR 3 Frame Generation only** (upscaling can stay on the title's own FSR or be added later).
This gives the 3080 Ti user the feature they actually need while minimizing risk to the existing DLSS path.

---

*Generated as a starting point for VIctory-NVIDIA. The existing Streamline code in `streamline.rs` and the interceptor in `fidelityfx.rs` are the best templates.*
