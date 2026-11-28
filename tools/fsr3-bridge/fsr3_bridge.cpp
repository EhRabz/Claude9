// fsr3_bridge.cpp - FSR 3 frame generation for VIctory, via the
// FidelityFX API (FSR 3.1, FidelityFX SDK 1.1.4).
//
// This is the ONLY file in the repository that knows AMD's FidelityFX
// structs. It is compiled on Windows against the real SDK headers
// (`ffx-api/include`), so a struct that changed in a newer SDK fails
// the build here, loudly, instead of corrupting memory at run time
// from a hand-mirrored layout in Rust.
//
// What it does, per GPUOpen ("AMD FSR Frame Generation API",
// "FSR 3.1 Integration Overview", FSR 3.1.4):
//   1. Loads AMD's signed DLL at run time (amd_fidelityfx_dx12.dll,
//      SDK 2.0 fallback amd_fidelityfx_loader_dx12.dll) and resolves
//      the five ffx* functions with GetProcAddress.
//   2. fsr3_fg_create: creates a frame-generation context
//      (ffxCreateContextDescFrameGeneration + backend desc). SDK 1.1.4
//      has no version struct to chain (that arrived in SDK 2.1), so
//      the backend desc is chained directly.
//   3. fsr3_fg_wrap_swapchain: wraps the game's swap chain in FSR 3's
//      frame-interpolation swap chain
//      (ffxCreateContextDescFrameGenerationSwapChainWrapDX12, type
//      0x30001). Presents from then on go through the wrapped chain.
//      The wrap holds an extra reference on the chain across the call:
//      the first FSR run's wrap failed with RUNTIME_ERROR and the host
//      then crashed in dxgi.dll presenting through the chain, which
//      points at the DLL consuming a reference to it in its error
//      cleanup (the hardening worked: the second run survived the
//      failed wrap and ran 3,763 frames). The wrap also writes
//      fsr3_wrap_diag.txt beside this DLL (chain desc, whether its
//      device is the FG context's device, the DLL's return code, and
//      probes of the D3D12 calls the wrap needs: the game queue's
//      timestamp frequency, a test present queue, a fence, a command
//      allocator/list, a test interpolation texture, GetBuffer and
//      GetParent, the swap chain's IDXGISwapChain3/4 support, a
//      descriptor heap, copy/compute queues and a shared fence) so a
//      failed wrap can be traced to the call that actually fails,
//      without another build.
//   4. fsr3_fg_prepare: dispatches the frame-generation prepare pass
//      (ffxDispatchDescFrameGenerationPrepare) into the caller's
//      command list, from depth + motion vectors, with the camera
//      info linked after it
//      (ffxDispatchDescFrameGenerationPrepareCameraInfo - a required
//      input in FSR 3.1.4). There is no PrepareV2 in SDK 1.1.4 (it
//      replaced these two structs in SDK 2.1 / FSR 3.1.6).
//   5. fsr3_fg_configure: configures frame generation once per frame
//      (ffxConfigureDescFrameGeneration), registering the frame-
//      generation callback (which dispatches the interpolation) and
//      the present callback (which copies the back buffer, with the
//      UI already in it, into the output buffer).
//   6. fsr3_fg_recreate: on a resize, remakes only the frame-
//      generation context at the new size, in place - the wrapped
//      swap chain (its swapchain context) stays alive, because the
//      host ResizeBuffers it and keeps presenting through it.
//
// Known approximations, stated so they can be fixed with real data:
//   - Camera vectors: the title gives no camera matrices (same as
//     DLSS in this layer, which builds its constants from FOV/near/
//     far alone). Prepare is given position (0,0,0), up (0,1,0),
//     right (1,0,0), forward (0,0,-1). If FSR 3's optical flow needs
//     the real camera, this is where it goes in.
//   - UI: the host draws its F1 menu / stats panel onto the back
//     buffer before Present, so the present callback copies that
//     buffer as-is. HUDLessColor (the scene texture, no host UI) is
//     passed so FSR 3 can tell UI from scene. The F1 menu should be
//     closed while frame generation is on.
//   - SDR only: VIctory presents SDR (2560x1440 SDR). The
//     back-buffer transfer function is SRGB and the luminance range
//     is [0, 1].
//   - Async workloads are off (flag bit 4 must be 0): the optical-
//     flow and interpolation work runs on the graphics queue, in
//     the order the layer submits it.
//   - Reset: FSR 3.1.4's prepare has only `unused_reset`, which the
//     implementation ignores - there is no working reset flag in
//     this SDK. A scene cut resets FSR 3 only through a disjoint
//     frameID (a step other than +1), which the Rust side owns:
//     it must skip an id when the title cuts. Until then, a hard
//     cut may ghost for a frame or two, as in every 3.1.4
//     integration.

#include "fsr3_bridge.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <ffx_api/ffx_api.h>
#include <ffx_api/dx12/ffx_api_dx12.h>

// The frame-generation structs live in their own header, which
// ffx_api.h does not include. Its name differs between SDK
// generations (ffx_framegeneration.h in 1.1.x, ffx_fg.h in the
// migration guide's naming); take whichever this SDK has.
#if __has_include(<ffx_api/ffx_framegeneration.h>)
#  include <ffx_api/ffx_framegeneration.h>
#elif __has_include(<ffx_api/ffx_fg.h>)
#  include <ffx_api/ffx_fg.h>
#else
#  error "no frame-generation header in this SDK (looked for ffx_api/ffx_framegeneration.h and ffx_api/ffx_fg.h)"
#endif

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>
#include <type_traits>

// The bridge DLL's own base, for finding AMD's DLL beside it.
EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace {

// The five FidelityFX API functions, resolved from AMD's DLL.
// Signatures are the documented ones (ffx_api.h):
//   ffxCreateContext(ffxContext*, ffxCreateContextDescHeader*,
//                    const ffxAllocationCallbacks*)
//   ffxDestroyContext(ffxContext*, const ffxAllocationCallbacks*)
//   ffxDispatch(ffxContext*, const ffxDispatchDescHeader*)
//   ffxQuery(ffxContext*, ffxQueryDescHeader*)
//   ffxConfigure(ffxContext*, ffxConfigureDescHeader*)
using FfxCreateContextFn = ffxReturnCode_t (*)(ffxContext *, ffxCreateContextDescHeader *,
                                                const ffxAllocationCallbacks *);
using FfxDestroyContextFn = ffxReturnCode_t (*)(ffxContext *, const ffxAllocationCallbacks *);
using FfxDispatchFn = ffxReturnCode_t (*)(ffxContext *, const ffxDispatchDescHeader *);
using FfxQueryFn = ffxReturnCode_t (*)(ffxContext *, ffxQueryDescHeader *);
using FfxConfigureFn = ffxReturnCode_t (*)(ffxContext *, ffxConfigureDescHeader *);

struct FfxApi {
    HMODULE module = nullptr;
    FfxCreateContextFn createContext = nullptr;
    FfxDestroyContextFn destroyContext = nullptr;
    FfxDispatchFn dispatch = nullptr;
    FfxQueryFn query = nullptr;
    FfxConfigureFn configure = nullptr;
};

// Named Fsr3FgState (not Fsr3Fg) so it cannot be confused with the
// opaque `typedef struct Fsr3Fg Fsr3Fg;` in fsr3_bridge.h.
struct Fsr3FgState {
    FfxApi api;
    ffxContext fgContext = nullptr;
    ffxContext swapContext = nullptr;
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *gameQueue = nullptr;
    uint32_t displayWidth = 0, displayHeight = 0;
    uint32_t renderWidth = 0, renderHeight = 0;
    uint32_t backBufferFormat = 0; // DXGI_FORMAT
    uint32_t flags = 0;
    uint64_t lastFrameID = 0;
    bool enabled = false;
};

// Load AMD's DLL from the same folder as this bridge DLL.
bool loadAmd(FfxApi &api) {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH) == 0) {
        return false;
    }
    // __ImageBase is the bridge DLL's own base; its folder is AMD's.
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) *slash = L'\0';
    const wchar_t *names[] = {L"amd_fidelityfx_dx12.dll", L"amd_fidelityfx_loader_dx12.dll"};
    for (const wchar_t *name : names) {
        wchar_t full[MAX_PATH];
        _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s", path, name);
        HMODULE module = LoadLibraryW(full);
        if (!module) continue;
        auto get = [&](const char *n) {
            return reinterpret_cast<void *>(GetProcAddress(module, n));
        };
        api.createContext = reinterpret_cast<FfxCreateContextFn>(get("ffxCreateContext"));
        api.destroyContext = reinterpret_cast<FfxDestroyContextFn>(get("ffxDestroyContext"));
        api.dispatch = reinterpret_cast<FfxDispatchFn>(get("ffxDispatch"));
        api.query = reinterpret_cast<FfxQueryFn>(get("ffxQuery"));
        api.configure = reinterpret_cast<FfxConfigureFn>(get("ffxConfigure"));
        if (api.createContext && api.destroyContext && api.dispatch && api.query && api.configure) {
            api.module = module;
            return true;
        }
        FreeLibrary(module);
        api = FfxApi{};
    }
    return false;
}

FfxApiSurfaceFormat surfaceFormat(uint32_t dxgiFormat) {
    switch (dxgiFormat) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

uint32_t fgFlags(uint32_t flags) {
    uint32_t out = 0;
    if (flags & FSR3_FG_FLAG_HDR) out |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    if (flags & FSR3_FG_FLAG_DEPTH_INVERTED) out |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    if (flags & FSR3_FG_FLAG_DISPLAY_MV) out |= FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    if (flags & FSR3_FG_FLAG_MV_JITTER) out |= FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (flags & FSR3_FG_FLAG_ASYNC_WORKLOADS) out |= FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
    return out;
}

// The D3D12 state a title texture is in, as AMD's state: the backend
// barriers from the declared state to what it needs, so the declared
// state must be the real one - a raw D3D12 value cast to the AMD
// enum would be meaningless bits. States with no FFX equivalent
// (render target, depth write) fall back to generic read.
FfxApiResourceState apiState(uint32_t d3d12State) {
    if (d3d12State & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    if (d3d12State & D3D12_RESOURCE_STATE_COPY_SOURCE) return FFX_API_RESOURCE_STATE_COPY_SRC;
    if (d3d12State & D3D12_RESOURCE_STATE_COPY_DEST) return FFX_API_RESOURCE_STATE_COPY_DEST;
    if (d3d12State & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    if (d3d12State & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) return FFX_API_RESOURCE_STATE_COMPUTE_READ;
    return FFX_API_RESOURCE_STATE_GENERIC_READ;
}

// An FfxApiResource for a title texture, in the D3D12 state it is in.
// The DX12 backend helper fills the description from the D3D12
// resource itself, so no layout is hand-mirrored here either.
FfxApiResource apiResource(ID3D12Resource *resource, uint32_t d3d12State) {
    return ffxApiGetResourceDX12(resource, apiState(d3d12State));
}

// Whether a linked struct carries a header. SDK 1.1.4's
// ffxDispatchDescFrameGenerationPrepareCameraInfo does (its type id
// goes in that header); later SDKs stripped it when the struct was
// deprecated in favour of PrepareV2.
template <typename T, typename = void>
struct HasHeader : std::false_type {};
template <typename T>
struct HasHeader<T, std::void_t<decltype(std::declval<T>().header)>> : std::true_type {};

// Chain `linked` after a dispatch desc, setting its header type when
// it has one. The header is the struct's first member, so the
// struct's address is the header's address either way.
template <typename T, typename U>
ffxDispatchDescHeader *linkHeader(T &linked, U type) {
    if constexpr (HasHeader<T>::value) {
        linked.header.type = static_cast<decltype(linked.header.type)>(type);
    }
    return reinterpret_cast<ffxDispatchDescHeader*>(&linked);
}

// Called by the FSR swap chain during Present, to generate a frame.
// Dispatches the interpolation with the parameters FSR 3 supplies.
ffxReturnCode_t frameGenerationCallback(ffxDispatchDescFrameGeneration *params, void *userCtx) {
    auto *fg = static_cast<Fsr3FgState *>(userCtx);
    if (!fg || !params) return FFX_API_RETURN_ERROR;
    return fg->api.dispatch(&fg->fgContext, &params->header);
}

// Called by the FSR swap chain for every frame presented (real or
// generated). Copies the back buffer - UI already composited into it
// by the host - into the output buffer.
ffxReturnCode_t presentCallback(ffxCallbackDescFrameGenerationPresent *params, void * /*userCtx*/) {
    if (!params || !params->commandList) return FFX_API_RETURN_ERROR;
    auto *list = static_cast<ID3D12GraphicsCommandList *>(params->commandList);
    auto *back = static_cast<ID3D12Resource *>(params->currentBackBuffer.resource);
    auto *out = static_cast<ID3D12Resource *>(params->outputSwapChainBuffer.resource);
    if (!back || !out) return FFX_API_RETURN_ERROR;
    list->CopyResource(out, back);
    return FFX_API_RETURN_OK;
}

// Make the frame-generation context from fg's current fields.
// AMD's debug checker (SDK 1.1.4): FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING
// in the frame-generation create flags makes the runtime check its inputs
// and report issues; ffxConfigureDescGlobalDebug1 routes those messages to
// fpMessage (if fpMessage is null they go to the debugger TTY instead).
// Values verified against GPUOpen's ffx-api headers; the fallbacks only
// apply if this SDK's headers predate them.
#ifndef FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING
#define FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING (1u << 6)
#endif
#ifndef FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1
#define FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1 0x0000001u
#define FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE 0xfffffffu
typedef void (*ffxApiMessage)(uint32_t type, const wchar_t *message);
struct ffxConfigureDescGlobalDebug1 {
    ffxConfigureDescHeader header;
    ffxApiMessage fpMessage;
    uint32_t debugLevel;
};
#endif

// Forward: registers the debug-message callback (defined after
// bridgeFolder, which it needs to find fsr3_debug.txt).
void registerDebug(Fsr3FgState *fg);

// The swapchain context is untouched: it keeps pointing at this
// same Fsr3FgState for its callbacks.
int createFgContext(Fsr3FgState *fg) {
    ffxCreateBackendDX12Desc backendDesc{};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.device = fg->device;

    ffxCreateContextDescFrameGeneration createFg{};
    createFg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    createFg.displaySize = {fg->displayWidth, fg->displayHeight};
    createFg.maxRenderSize = {fg->renderWidth, fg->renderHeight};
    // Debug checking on while the wrap is being diagnosed: the DLL
    // then reports bad inputs instead of failing silently.
    createFg.flags = fgFlags(fg->flags) | FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    createFg.backBufferFormat = surfaceFormat(fg->backBufferFormat);
    if (createFg.backBufferFormat == FFX_API_SURFACE_FORMAT_UNKNOWN) {
        return -1;
    }

    // SDK 1.1.4 has no version struct to chain (that arrived in
    // SDK 2.1); the backend desc is chained directly.
    createFg.header.pNext = &backendDesc.header;

    ffxReturnCode_t code = fg->api.createContext(&fg->fgContext, &createFg.header, nullptr);
    if (code != FFX_API_RETURN_OK || !fg->fgContext) {
        return static_cast<int>(code);
    }
    // Route the DLL's debug messages to fsr3_debug.txt, so it can
    // say why a later call (e.g. the swap-chain wrap) fails.
    registerDebug(fg);
    return 0;
}

// The folder this bridge DLL was loaded from (AMD's DLL lives there too).
bool bridgeFolder(wchar_t *out, size_t count) {
    if (GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), out, static_cast<DWORD>(count)) == 0) return false;
    wchar_t *slash = wcsrchr(out, L'\\');
    if (!slash) return false;
    *slash = L'\0';
    return true;
}

// The DLL's debug messages go to fsr3_debug.txt beside this DLL.
// The callback can be called from the DLL's own threads, so it
// opens, writes, and closes the file on each call. A failure here
// must never break the caller, so every error is ignored.
void debugMessage(uint32_t type, const wchar_t *message) {
    wchar_t folder[MAX_PATH];
    if (!bridgeFolder(folder, MAX_PATH)) return;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\fsr3_debug.txt", folder);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || !f) return;
    fwprintf(f, L"[type %u] %s\n", (unsigned)type, message ? message : L"(null)");
    fclose(f);
}

void registerDebug(Fsr3FgState *fg) {
    if (!fg || !fg->fgContext || !fg->api.configure) return;
    ffxConfigureDescGlobalDebug1 debug{};
    debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
    debug.fpMessage = &debugMessage;
    debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
    // A failed configure must not break context creation, but record
    // the result. fsr3_debug.txt is otherwise only created when the DLL
    // sends a message, so a missing file told us nothing — write a
    // marker now, so the file's existence proves registration ran and
    // the code proves whether it worked.
    ffxReturnCode_t code = fg->api.configure(&fg->fgContext, &debug.header);
    wchar_t folder[MAX_PATH];
    if (bridgeFolder(folder, MAX_PATH)) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\fsr3_debug.txt", folder);
        FILE *f = nullptr;
        if (_wfopen_s(&f, path, L"a") == 0 && f) {
            fwprintf(f, L"[bridge] debug callback registered, ffxConfigure returned %d (type 0x%08x)\n",
                     (int)code, (unsigned)FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1);
            fclose(f);
        }
    }
}

// Probe the D3D12 calls the wrap needs, so a RUNTIME_ERROR from
// AMD's DLL can be traced to the call that actually fails. Every
// probe creates and immediately releases its object; a failure here
// is the wrap's failure, not a new one. The first round of probes
// (queue timestamp, present queue, fence, allocator/list, texture,
// GetBuffer, GetParent) all passed, so the failure is in something
// they did not cover. The prime suspect now is the swap chain
// itself: the DLL needs the current back-buffer index, which lives
// on IDXGISwapChain3, and victory's swap chain may be a proxy that
// only implements the base interface.
struct WrapProbes {
    HRESULT queueTimestampHr = E_FAIL;
    UINT64 queueTimestampFreq = 0;
    HRESULT testQueueHr = E_FAIL;
    HRESULT testFenceHr = E_FAIL;
    HRESULT testAllocatorHr = E_FAIL;
    HRESULT testListHr = E_FAIL;
    HRESULT testTextureHr = E_FAIL;
    HRESULT heapHr = E_FAIL;
    HRESULT copyQueueHr = E_FAIL;
    HRESULT computeQueueHr = E_FAIL;
    HRESULT sharedFenceHr = E_FAIL;
    HRESULT getBufferHr = E_FAIL;
    HRESULT getParentHr = E_FAIL;
    HRESULT sc3Hr = E_FAIL;
    UINT backBufferIndex = 0xFFFFFFFF;
    HRESULT sc4Hr = E_FAIL;
    HRESULT fullscreenHr = E_FAIL;
    BOOL fullscreen = FALSE;
    HRESULT outputHr = E_FAIL;
    // Round 3: the wrap passed every probe so far, so the failure is
    // in something none of them covered - the game queue's device,
    // the chain's window / fullscreen / color state, the output's
    // display modes, or the device's shader-model support (the DLL's
    // shaders need compute shader 6.2).
    HRESULT queueDeviceHr = E_FAIL;
    bool queueDeviceMatches = false;
    UINT queueType = 0xFFFFFFFF;
    INT queuePriority = 0;
    UINT queueNodeMask = 0;
    HRESULT shaderModelHr = E_FAIL;
    UINT shaderModel = 0;
    HRESULT rootSigHr = E_FAIL;
    UINT rootSigVersion = 0;
    HRESULT queryHeapHr = E_FAIL;
    HRESULT hwndHr = E_FAIL;
    HWND hwnd = nullptr;
    HRESULT fullscreenDescHr = E_FAIL;
    UINT refreshNum = 0;
    UINT refreshDen = 0;
    BOOL fullscreenWindowed = FALSE;
    HRESULT colorSpaceHr = E_FAIL;
    UINT colorSpace = 0xFFFFFFFF;
    HRESULT maxLatencyHr = E_FAIL;
    UINT maxLatency = 0xFFFFFFFF;
    HRESULT modeListHr = E_FAIL;
    UINT modeCount = 0;
    // Round 4: the wrap still fails with the device confirmed fine
    // (SM 6.6, RS 1.1), so the failure is in what the DLL itself does
    // during the wrap: frame pacing (max frame latency / the latency
    // waitable object) and creating its interpolation pipelines
    // (a CS 6.2 compute PSO) and its internal resources.
    HRESULT setLatencyHr = E_FAIL;
    HRESULT latencyWaitableHr = E_FAIL;
    HRESULT compileHr = E_FAIL;
    HRESULT rootSigCreateHr = E_FAIL;
    HRESULT psoHr = E_FAIL;
    HRESULT simulAccessHr = E_FAIL;
    HRESULT rtvUavTexHr = E_FAIL;
    // Round 5: CS 6.2 via DXC - the DLL's shaders are CS 6.2, and the
    // CS 5.1 test only proves pipeline creation works up to 5.1.
    // dxcLoadHr/dxcCreateHr record whether DXC itself loaded and the
    // compiler was created, so a failed compile can be told apart
    // from DXC never running (all three default to E_FAIL).
    // dxcFromBridge is 1 when dxcompiler.dll came from this bridge
    // DLL's own folder (the default search does not find it: the
    // Windows SDK keeps it in its bin folder, not System32).
    HRESULT dxcLoadHr = E_FAIL;
    HRESULT dxcCreateHr = E_FAIL;
    HRESULT dxcCompileHr = E_FAIL;
    HRESULT pso62Hr = E_FAIL;
    bool dxcFromBridge = false;
    char dxcError[512] = {};
    // Round 6: frame latency is now fixed (flags=0x40, set-max-latency
    // and the waitable object both work) and the wrap STILL fails
    // code 3, with every probe passing - so the failure is in
    // something none of the probes covered. The DLL must do real work
    // during the wrap: submit a command list to the game queue and
    // wait on a fence, read the back buffer (SRV), and use wave
    // intrinsics in its shaders (WaveOps). Test those three.
    HRESULT execListHr = E_FAIL;
    HRESULT signalHr = E_FAIL;
    HRESULT fenceWaitHr = E_FAIL;
    HRESULT waveOpsHr = E_FAIL;
    BOOL waveOps = FALSE;
    UINT resourceBindingTier = 0xFFFFFFFF;
    HRESULT backSrvHr = E_FAIL;
};

// Minimal DXC (DirectX Shader Compiler) interfaces for compiling a CS 6.2
// shader at runtime. GUIDs from Microsoft's dxcapi.h (DirectXShaderCompiler
// repo). Loaded dynamically from dxcompiler.dll so the bridge gains no new
// link dependency. Only the methods the probe calls are declared, in vtable
// order.
struct DxcBufferProbe { LPCVOID Ptr; SIZE_T Size; UINT Encoding; };
struct IDxcBlobProbe : public IUnknown {
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};
struct IDxcResultProbe : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetStatus(HRESULT *pStatus) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResult(IDxcBlobProbe **ppResult) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetErrorBuffer(IDxcBlobProbe **ppErrors) = 0;
};
struct IDxcCompiler3Probe : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Compile(const DxcBufferProbe *pSource, LPCWSTR *pArguments, UINT32 argCount, IUnknown *pIncludeHandler, REFIID riid, LPVOID *ppResult) = 0;
    virtual HRESULT STDMETHODCALLTYPE Disassemble(const DxcBufferProbe *pObject, REFIID riid, LPVOID *ppResult) = 0;
};
static const GUID CLSID_DxcCompilerProbe = { 0x73e22d93, 0xe6ce, 0x47f3, { 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0 } };
static const GUID IID_IDxcCompiler3Probe = { 0x228B4687, 0x5A6A, 0x4730, { 0x90, 0x0C, 0x97, 0x02, 0xB2, 0x20, 0x3F, 0x54 } };
static const GUID IID_IDxcResultProbe = { 0x58346CDA, 0xDDE7, 0x4497, { 0x94, 0x61, 0x6F, 0x87, 0xAF, 0x5E, 0x06, 0x59 } };
typedef HRESULT (WINAPI *PFN_DxcCreateInstanceProbe)(REFCLSID rclsid, REFIID riid, LPVOID *ppv);

WrapProbes probeWrap(ID3D12Device *device, ID3D12CommandQueue *gameQueue, IDXGISwapChain4 *swapchain) {
    WrapProbes p{};
    if (gameQueue) {
        p.queueTimestampHr = gameQueue->GetTimestampFrequency(&p.queueTimestampFreq);
        // The DLL uses the game queue with the FG context's device.
        // If victory's queue is a proxy whose device differs, or
        // whose type is not a direct (graphics) queue, the wrap
        // cannot use them together.
        ID3D12Device *queueDevice = nullptr;
        p.queueDeviceHr = gameQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
        if (queueDevice) {
            p.queueDeviceMatches = (device && queueDevice == device);
            queueDevice->Release();
        }
        D3D12_COMMAND_QUEUE_DESC qdesc = gameQueue->GetDesc();
        p.queueType = static_cast<UINT>(qdesc.Type);
        p.queuePriority = qdesc.Priority;
        p.queueNodeMask = qdesc.NodeMask;
    }
    if (device) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue *testQueue = nullptr;
        p.testQueueHr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&testQueue));
        if (testQueue) testQueue->Release();
        ID3D12Fence *fence = nullptr;
        p.testFenceHr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (fence) fence->Release();
        ID3D12CommandAllocator *alloc = nullptr;
        p.testAllocatorHr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (alloc) {
            ID3D12GraphicsCommandList *list = nullptr;
            p.testListHr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
            if (list) {
                list->Close();
                list->Release();
            }
            alloc->Release();
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC tex{};
        tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex.Width = 64;
        tex.Height = 64;
        tex.DepthOrArraySize = 1;
        tex.MipLevels = 1;
        tex.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        tex.SampleDesc.Count = 1;
        tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource *resource = nullptr;
        p.testTextureHr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
                                                          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
        if (resource) resource->Release();
        // A descriptor heap (the DLL needs one for its interpolation
        // resources) and queues of the other types, in case it makes
        // one of those instead of a direct queue.
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 16;
        ID3D12DescriptorHeap *descHeap = nullptr;
        p.heapHr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descHeap));
        if (descHeap) descHeap->Release();
        D3D12_COMMAND_QUEUE_DESC cqd{};
        cqd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ID3D12CommandQueue *copyQueue = nullptr;
        p.copyQueueHr = device->CreateCommandQueue(&cqd, IID_PPV_ARGS(&copyQueue));
        if (copyQueue) copyQueue->Release();
        D3D12_COMMAND_QUEUE_DESC kqd{};
        kqd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        ID3D12CommandQueue *computeQueue = nullptr;
        p.computeQueueHr = device->CreateCommandQueue(&kqd, IID_PPV_ARGS(&computeQueue));
        if (computeQueue) computeQueue->Release();
        // A shared fence, in case the DLL shares one between its own
        // present queue and the game's.
        ID3D12Fence *sharedFence = nullptr;
        p.sharedFenceHr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sharedFence));
        if (sharedFence) sharedFence->Release();
        // The device's shader-model and root-signature support: the
        // DLL's shaders need compute shader 6.2 (6.6 on some
        // hardware). Ask for the highest we care about (6.6 / 1.1):
        // CheckFeatureSupport writes back the highest supported
        // NO HIGHER than the value passed in, so asking for 6.0 can
        // only ever return 6.0 - the earlier probe's "highest=0x60"
        // proved nothing but "at least 6.0". (Victory's own mesh
        // check asks for 6.6 on this same device and passes.)
        D3D12_FEATURE_DATA_SHADER_MODEL sm{};
        sm.HighestShaderModel = D3D_SHADER_MODEL_6_6;
        p.shaderModelHr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
        if (p.shaderModelHr == S_OK) p.shaderModel = static_cast<UINT>(sm.HighestShaderModel);
        D3D12_FEATURE_DATA_ROOT_SIGNATURE rs{};
        rs.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        p.rootSigHr = device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rs, sizeof(rs));
        if (p.rootSigHr == S_OK) p.rootSigVersion = static_cast<UINT>(rs.HighestVersion);
        // A timestamp query heap, which the DLL uses for pacing.
        D3D12_QUERY_HEAP_DESC qhd{};
        qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhd.Count = 2;
        ID3D12QueryHeap *queryHeap = nullptr;
        p.queryHeapHr = device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&queryHeap));
        if (queryHeap) queryHeap->Release();
        // The DLL's interpolation resources may need simultaneous
        // access (shared between its present queue and the game queue)
        // or a render-target + UAV texture.
        D3D12_RESOURCE_DESC simTex = tex;
        simTex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        ID3D12Resource *simRes = nullptr;
        p.simulAccessHr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &simTex,
                                                          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&simRes));
        if (simRes) simRes->Release();
        D3D12_RESOURCE_DESC rtTex = tex;
        rtTex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtTex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ID3D12Resource *rtRes = nullptr;
        p.rtvUavTexHr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rtTex,
                                                        D3D12_RESOURCE_STATE_COMMON, &clear, IID_PPV_ARGS(&rtRes));
        if (rtRes) rtRes->Release();
        // The DLL creates compute pipelines from CS 6.2 shaders during
        // the wrap. Test pipeline creation on this device twice: once
        // with a CS 5.1 shader (the legacy D3DCompiler can do that)
        // and once with a CS 6.2 shader (via DXC, the only compiler
        // that does SM 6.x). If CS 5.1 works but CS 6.2 fails, the
        // device's pipeline creation caps at 5.1 - which would be the
        // wrap blocker, since the DLL's interpolation shaders are
        // CS 6.2.
        static const char csSrc[] =
            "RWStructuredBuffer<uint> g_out : register(u0);\n"
            "[numthreads(1,1,1)]\n"
            "void main(uint3 id : SV_DispatchThreadID) { g_out[id.x] = id.x; }\n";
        // One root signature for both PSO tests.
        ID3D12RootSignature *probeRoot = nullptr;
        {
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            range.NumDescriptors = 1;
            range.BaseShaderRegister = 0;
            range.RegisterSpace = 0;
            range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.DescriptorTable.NumDescriptorRanges = 1;
            param.DescriptorTable.pDescriptorRanges = &range;
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_ROOT_SIGNATURE_DESC rsDesc{};
            rsDesc.NumParameters = 1;
            rsDesc.pParameters = &param;
            rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
            ID3DBlob *rsBlob = nullptr, *rsErr = nullptr;
            HRESULT rsSerHr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr);
            if (rsErr) rsErr->Release();
            if (SUCCEEDED(rsSerHr) && rsBlob) {
                p.rootSigCreateHr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&probeRoot));
                rsBlob->Release();
            } else {
                p.rootSigCreateHr = rsSerHr;
            }
        }
        // CS 5.1 via the legacy compiler.
        typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT, void **, void **);
        HMODULE d3dc = LoadLibraryW(L"d3dcompiler_47.dll");
        if (d3dc) {
            auto d3dCompile = (PFN_D3DCompile)GetProcAddress(d3dc, "D3DCompile");
            if (d3dCompile) {
                ID3DBlob *bytecode = nullptr, *errors = nullptr;
                p.compileHr = d3dCompile(csSrc, sizeof(csSrc) - 1, "probe", nullptr, nullptr, "main", "cs_5_1", 0, 0, (void **)&bytecode, (void **)&errors);
                if (errors) errors->Release();
                if (bytecode && probeRoot) {
                    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
                    psoDesc.pRootSignature = probeRoot;
                    psoDesc.CS.pShaderBytecode = bytecode->GetBufferPointer();
                    psoDesc.CS.BytecodeLength = bytecode->GetBufferSize();
                    ID3D12PipelineState *pso = nullptr;
                    p.psoHr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso));
                    if (pso) pso->Release();
                }
                if (bytecode) bytecode->Release();
            }
            FreeLibrary(d3dc);
        }
        // CS 6.2 via DXC (dxcompiler.dll, loaded dynamically).
        // Try the default search first, then this bridge DLL's own
        // folder: dxcompiler.dll is not in System32 on this machine
        // (the Windows SDK keeps it in its bin folder), so the
        // default search fails and the DLLs have to sit beside the
        // bridge. dxil.dll must sit beside it too (DXC depends on it).
        HMODULE dxc = LoadLibraryW(L"dxcompiler.dll");
        if (!dxc) {
            wchar_t folder[MAX_PATH];
            if (bridgeFolder(folder, MAX_PATH)) {
                wchar_t full[MAX_PATH];
                _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\dxcompiler.dll", folder);
                dxc = LoadLibraryW(full);
                if (dxc) p.dxcFromBridge = true;
            }
        }
        if (dxc) {
            auto dxcCreate = (PFN_DxcCreateInstanceProbe)GetProcAddress(dxc, "DxcCreateInstance");
            p.dxcLoadHr = dxcCreate ? S_OK : E_FAIL;
            if (dxcCreate) {
                IDxcCompiler3Probe *compiler = nullptr;
                p.dxcCreateHr = dxcCreate(CLSID_DxcCompilerProbe, IID_IDxcCompiler3Probe, (LPVOID *)&compiler);
                if (SUCCEEDED(p.dxcCreateHr) && compiler) {
                    DxcBufferProbe src{};
                    src.Ptr = csSrc;
                    src.Size = sizeof(csSrc) - 1;
                    src.Encoding = 65001; // DXC_CP_UTF8
                    const wchar_t *args[] = { L"-T", L"cs_6_2", L"-E", L"main" };
                    IDxcResultProbe *result = nullptr;
                    HRESULT compileCallHr = compiler->Compile(&src, args, 4, nullptr, IID_IDxcResultProbe, (LPVOID *)&result);
                    if (SUCCEEDED(compileCallHr) && result) {
                        HRESULT status = E_FAIL;
                        result->GetStatus(&status);
                        p.dxcCompileHr = status;
                        if (SUCCEEDED(status)) {
                            IDxcBlobProbe *blob = nullptr;
                            if (SUCCEEDED(result->GetResult(&blob)) && blob && probeRoot) {
                                D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
                                psoDesc.pRootSignature = probeRoot;
                                psoDesc.CS.pShaderBytecode = blob->GetBufferPointer();
                                psoDesc.CS.BytecodeLength = blob->GetBufferSize();
                                ID3D12PipelineState *pso = nullptr;
                                p.pso62Hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso));
                                if (pso) pso->Release();
                            }
                            if (blob) blob->Release();
                        }
                        // Capture DXC's own error text, so a failed
                        // compile says why instead of just an HRESULT.
                        IDxcBlobProbe *errBlob = nullptr;
                        if (SUCCEEDED(result->GetErrorBuffer(&errBlob)) && errBlob) {
                            SIZE_T len = errBlob->GetBufferSize();
                            if (len > 0) {
                                SIZE_T copy = (len < sizeof(p.dxcError) - 1) ? len : sizeof(p.dxcError) - 1;
                                memcpy(p.dxcError, errBlob->GetBufferPointer(), copy);
                                p.dxcError[copy] = '\0';
                            }
                            errBlob->Release();
                        }
                        result->Release();
                    } else {
                        p.dxcCompileHr = compileCallHr;
                    }
                    compiler->Release();
                }
            }
            FreeLibrary(dxc);
        }
        // Round 6: frame latency is fixed and the wrap still fails
        // with every probe passing, so the failure is in something
        // none of them covered. The DLL must do real work during the
        // wrap: submit a command list to the GAME queue and wait on a
        // fence. An empty list is harmless to the game. If the queue
        // cannot execute a list (victory's queue is a proxy), or a
        // fence wait on it never completes, that is the blocker.
        if (gameQueue) {
            ID3D12CommandAllocator *execAlloc = nullptr;
            ID3D12GraphicsCommandList *execList = nullptr;
            ID3D12Fence *execFence = nullptr;
            HRESULT allocHr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&execAlloc));
            if (SUCCEEDED(allocHr) && execAlloc) {
                HRESULT listHr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, execAlloc, nullptr, IID_PPV_ARGS(&execList));
                if (SUCCEEDED(listHr) && execList) {
                    execList->Close();
                    ID3D12CommandList *lists[] = { execList };
                    gameQueue->ExecuteCommandLists(1, lists);
                    // ExecuteCommandLists returns void, so the signal
                    // and wait are what prove the queue ran the list.
                    HRESULT fenceHr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&execFence));
                    if (SUCCEEDED(fenceHr) && execFence) {
                        p.signalHr = gameQueue->Signal(execFence, 1);
                        if (SUCCEEDED(p.signalHr)) {
                            HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                            if (evt) {
                                HRESULT evtHr = execFence->SetEventOnCompletion(1, evt);
                                if (SUCCEEDED(evtHr)) {
                                    DWORD w = WaitForSingleObject(evt, 5000);
                                    if (w == WAIT_OBJECT_0) p.fenceWaitHr = S_OK;
                                    else if (w == WAIT_TIMEOUT) p.fenceWaitHr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
                                    else p.fenceWaitHr = HRESULT_FROM_WIN32(GetLastError());
                                } else {
                                    p.fenceWaitHr = evtHr;
                                }
                                CloseHandle(evt);
                            }
                        }
                        execFence->Release();
                    }
                    // Record whether the list itself was created, so
                    // a signal failure is not mistaken for a list one.
                    p.execListHr = listHr;
                    execList->Release();
                } else {
                    p.execListHr = listHr;
                }
                execAlloc->Release();
            } else {
                p.execListHr = allocHr;
            }
        }
        // The DLL's interpolation shaders use wave intrinsics, so
        // WaveOps must be on; and the resource binding tier decides
        // how its descriptor tables can be laid out. Both come from
        // CheckFeatureSupport on the device.
        {
            D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
            HRESULT optsHr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
            if (SUCCEEDED(optsHr)) p.resourceBindingTier = static_cast<UINT>(opts.ResourceBindingTier);
            D3D12_FEATURE_DATA_D3D12_OPTIONS1 opts1{};
            p.waveOpsHr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opts1, sizeof(opts1));
            if (SUCCEEDED(p.waveOpsHr)) p.waveOps = opts1.WaveOps;
        }
        if (probeRoot) probeRoot->Release();
    }
    if (swapchain) {
        ID3D12Resource *back = nullptr;
        p.getBufferHr = swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
        // Round 6: the DLL reads the back buffer as the interpolation
        // input, so a shader-resource view for it must be creatable.
        // (A UAV is not tested: swap-chain buffers do not carry
        // ALLOW_UNORDERED_ACCESS, and the DLL writes its own
        // intermediate textures, not the back buffer, through a UAV.)
        if (back && device) {
            D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
            srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvHeapDesc.NumDescriptors = 1;
            srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            ID3D12DescriptorHeap *srvHeap = nullptr;
            HRESULT srvHeapHr = device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap));
            if (SUCCEEDED(srvHeapHr) && srvHeap) {
                D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
                device->CreateShaderResourceView(back, nullptr, h);
                // CreateShaderResourceView returns void; the only
                // failure it can report is a bad heap or resource,
                // which the heap creation above already rules out.
                // So a successful heap + this call means the SRV
                // path is usable. Record S_OK when we get here.
                p.backSrvHr = S_OK;
                srvHeap->Release();
            } else {
                p.backSrvHr = srvHeapHr;
            }
        }
        if (back) back->Release();
        IDXGIFactory *factory = nullptr;
        p.getParentHr = swapchain->GetParent(IID_PPV_ARGS(&factory));
        if (factory) factory->Release();
        // The current back-buffer index lives on IDXGISwapChain3.
        // If victory's swap chain is a proxy that only implements
        // the base interface, this is the call the wrap dies on.
        IDXGISwapChain3 *sc3 = nullptr;
        p.sc3Hr = swapchain->QueryInterface(IID_PPV_ARGS(&sc3));
        if (sc3) {
            p.backBufferIndex = sc3->GetCurrentBackBufferIndex();
            sc3->Release();
        }
        IDXGISwapChain4 *sc4 = nullptr;
        p.sc4Hr = swapchain->QueryInterface(IID_PPV_ARGS(&sc4));
        if (sc4) sc4->Release();
        BOOL fs = FALSE;
        p.fullscreenHr = swapchain->GetFullscreenState(&fs, nullptr);
        p.fullscreen = fs;
        IDXGIOutput *output = nullptr;
        p.outputHr = swapchain->GetContainingOutput(&output);
        // The window and fullscreen state, which the DLL needs for
        // frame pacing, and the color space / frame latency.
        HWND w = nullptr;
        p.hwndHr = swapchain->GetHwnd(&w);
        p.hwnd = w;
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc{};
        p.fullscreenDescHr = swapchain->GetFullscreenDesc(&fsDesc);
        if (p.fullscreenDescHr == S_OK) {
            p.refreshNum = fsDesc.RefreshRate.Numerator;
            p.refreshDen = fsDesc.RefreshRate.Denominator;
            p.fullscreenWindowed = fsDesc.Windowed;
        }
        // Color-space support and frame latency live on IDXGISwapChain3.
        // (There is no getter for the color space itself, so the probe
        // asks whether the SDR color space is supported.)
        IDXGISwapChain3 *sc3b = nullptr;
        if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3b)))) {
            UINT support = 0;
            p.colorSpaceHr = sc3b->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &support);
            if (p.colorSpaceHr == S_OK) p.colorSpace = support;
            UINT latency = 0;
            p.maxLatencyHr = sc3b->GetMaximumFrameLatency(&latency);
            if (p.maxLatencyHr == S_OK) p.maxLatency = latency;
            sc3b->Release();
        }
        // Frame pacing: the DLL may set the max frame latency and wait
        // on the latency waitable object. GetMaximumFrameLatency
        // already failed above (the chain has no
        // FRAME_LATENCY_WAITABLE_OBJECT flag), so test whether setting
        // it and fetching the waitable object work at all. Both live
        // on IDXGISwapChain2 (not the base chain): SetMaximumFrameLatency
        // returns an HRESULT, but GetFrameLatencyWaitableObject takes
        // no arguments and returns the HANDLE directly - NULL when the
        // chain was not created with FRAME_LATENCY_WAITABLE_OBJECT.
        IDXGISwapChain2 *sc2 = nullptr;
        if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc2)))) {
            p.setLatencyHr = sc2->SetMaximumFrameLatency(1);
            HANDLE waitable = sc2->GetFrameLatencyWaitableObject();
            p.latencyWaitableHr = (waitable != nullptr) ? S_OK : E_FAIL;
            sc2->Release();
        }
        // The display modes of the output, for the refresh rate.
        if (output) {
            UINT count = 0;
            p.modeListHr = output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &count, nullptr);
            p.modeCount = count;
            output->Release();
        }
    }
    return p;
}

// Write the wrap diagnostics to fsr3_wrap_diag.txt beside this DLL:
// the chain's desc, whether its device is the FG context's device,
// the probes above, and the DLL's return code. Diagnostics must
// never break the wrap, so every failure here is ignored.
void writeWrapDiag(const wchar_t *folder, const DXGI_SWAP_CHAIN_DESC1 *desc1, HRESULT hrDesc,
                   bool deviceMatches, HRESULT hrDev, void *gameQueue, int resultCode, bool wrapped,
                   const WrapProbes *probes) {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\fsr3_wrap_diag.txt", folder);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fwprintf(f, L"FSR 3 swap-chain wrap diagnostics\n");
    // Version marker: proves which bridge wrote this file. v3.8's probe
    // asks for shader model 6.6 and root signature 1.1, records each
    // DXC step separately, falls back to loading dxcompiler.dll from
    // this bridge DLL's own folder, and adds round 6: execute a
    // command list on the game queue + fence wait, WaveOps / resource
    // binding tier, and a back-buffer SRV.
    fwprintf(f, L"bridge v3.8: probe asks shader model 6.6, root signature 1.1; DXC steps recorded separately + DXC error text; DXC falls back to the bridge folder; round 6 adds queue execution, WaveOps, back-buffer SRV\n");
    if (desc1 && hrDesc == S_OK) {
        fwprintf(f, L"chain desc: %ux%u format=%u buffers=%u usage=0x%x swapEffect=%d sample=%u/%u flags=0x%x stereo=%d scaling=%d alpha=%d\n",
                 (unsigned)desc1->Width, (unsigned)desc1->Height, (unsigned)desc1->Format,
                 (unsigned)desc1->BufferCount, (unsigned)desc1->BufferUsage,
                 (int)desc1->SwapEffect, (unsigned)desc1->SampleDesc.Count,
                 (unsigned)desc1->SampleDesc.Quality, (unsigned)desc1->Flags,
                 (int)desc1->Stereo, (int)desc1->Scaling, (int)desc1->AlphaMode);
    } else {
        fwprintf(f, L"chain desc: GetDesc1 failed hr=0x%08x\n", (unsigned)hrDesc);
    }
    fwprintf(f, L"chain device: GetDevice hr=0x%08x, matches the FG context device: %s\n",
             (unsigned)hrDev, deviceMatches ? L"yes" : L"NO");
    fwprintf(f, L"game queue: %p\n", gameQueue);
    if (probes) {
        fwprintf(f, L"probes: queue timestamp hr=0x%08x freq=%llu; test queue hr=0x%08x; test fence hr=0x%08x; test allocator hr=0x%08x; test list hr=0x%08x; test texture hr=0x%08x; descriptor heap hr=0x%08x; copy queue hr=0x%08x; compute queue hr=0x%08x; shared fence hr=0x%08x; GetBuffer(0) hr=0x%08x; GetParent hr=0x%08x\n",
                 (unsigned)probes->queueTimestampHr, (unsigned long long)probes->queueTimestampFreq,
                 (unsigned)probes->testQueueHr, (unsigned)probes->testFenceHr,
                 (unsigned)probes->testAllocatorHr, (unsigned)probes->testListHr,
                 (unsigned)probes->testTextureHr, (unsigned)probes->heapHr,
                 (unsigned)probes->copyQueueHr, (unsigned)probes->computeQueueHr,
                 (unsigned)probes->sharedFenceHr, (unsigned)probes->getBufferHr,
                 (unsigned)probes->getParentHr);
        fwprintf(f, L"probes: swapchain3 hr=0x%08x backBufferIndex=%u; swapchain4 hr=0x%08x; fullscreen hr=0x%08x fullscreen=%d; containing output hr=0x%08x\n",
                 (unsigned)probes->sc3Hr, (unsigned)probes->backBufferIndex,
                 (unsigned)probes->sc4Hr, (unsigned)probes->fullscreenHr,
                 (int)probes->fullscreen, (unsigned)probes->outputHr);
        fwprintf(f, L"probes: queue device hr=0x%08x matches=%s; queue type=%u priority=%d nodeMask=0x%x; shader model hr=0x%08x highest=0x%x; root signature hr=0x%08x version=%u; timestamp query heap hr=0x%08x\n",
                 (unsigned)probes->queueDeviceHr, probes->queueDeviceMatches ? L"yes" : L"NO",
                 (unsigned)probes->queueType, (int)probes->queuePriority,
                 (unsigned)probes->queueNodeMask,
                 (unsigned)probes->shaderModelHr, (unsigned)probes->shaderModel,
                 (unsigned)probes->rootSigHr, (unsigned)probes->rootSigVersion,
                 (unsigned)probes->queryHeapHr);
        fwprintf(f, L"probes: hwnd hr=0x%08x hwnd=%p; fullscreen desc hr=0x%08x refresh=%u/%u windowed=%d; color space hr=0x%08x support=%u; max latency hr=0x%08x latency=%u; display modes hr=0x%08x count=%u\n",
                 (unsigned)probes->hwndHr, probes->hwnd,
                 (unsigned)probes->fullscreenDescHr, (unsigned)probes->refreshNum,
                 (unsigned)probes->refreshDen, (int)probes->fullscreenWindowed,
                 (unsigned)probes->colorSpaceHr, (unsigned)probes->colorSpace,
                 (unsigned)probes->maxLatencyHr, (unsigned)probes->maxLatency,
                 (unsigned)probes->modeListHr, (unsigned)probes->modeCount);
        fwprintf(f, L"probes: set max frame latency hr=0x%08x; latency waitable hr=0x%08x; compile CS 5.1 hr=0x%08x; root signature create hr=0x%08x; compute PSO hr=0x%08x; simultaneous-access texture hr=0x%08x; RTV+UAV texture hr=0x%08x; DXC load hr=0x%08x (from bridge dir=%d); DXC create hr=0x%08x; compile CS 6.2 (DXC) hr=0x%08x; compute PSO CS 6.2 hr=0x%08x\n",
                 (unsigned)probes->setLatencyHr, (unsigned)probes->latencyWaitableHr,
                 (unsigned)probes->compileHr, (unsigned)probes->rootSigCreateHr,
                 (unsigned)probes->psoHr, (unsigned)probes->simulAccessHr,
                 (unsigned)probes->rtvUavTexHr, (unsigned)probes->dxcLoadHr,
                 probes->dxcFromBridge ? 1 : 0,
                 (unsigned)probes->dxcCreateHr, (unsigned)probes->dxcCompileHr,
                 (unsigned)probes->pso62Hr);
        fwprintf(f, L"probes: exec list hr=0x%08x; queue signal hr=0x%08x; fence wait hr=0x%08x; WaveOps hr=0x%08x waveOps=%d; resource binding tier=%u; back-buffer SRV hr=0x%08x\n",
                 (unsigned)probes->execListHr, (unsigned)probes->signalHr,
                 (unsigned)probes->fenceWaitHr,
                 (unsigned)probes->waveOpsHr, (int)probes->waveOps,
                 (unsigned)probes->resourceBindingTier,
                 (unsigned)probes->backSrvHr);
        if (probes->dxcError[0] != '\0') {
            // DXC's error text is ASCII; print it as-is (truncated to
            // the 511 bytes the probe kept).
            fwprintf(f, L"dxc error: %S\n", probes->dxcError);
        }
    }
    fwprintf(f, L"wrap result: code=%d (%s)\n", resultCode, wrapped ? L"wrapped OK" : L"FAILED");
    fclose(f);
}

} // namespace

extern "C" {

int fsr3_bridge_version(void) { return FSR3_BRIDGE_ABI_VERSION; }

int fsr3_fg_create(
    void *device, void *gameQueue,
    uint32_t displayWidth, uint32_t displayHeight,
    uint32_t renderWidth, uint32_t renderHeight,
    uint32_t backBufferFormat, uint32_t flags,
    void **outFg) {
    if (!device || !gameQueue || !outFg || displayWidth == 0 || displayHeight == 0) return -1;
    auto *fg = new (std::nothrow) Fsr3FgState{};
    if (!fg) return -1;
    if (!loadAmd(fg->api)) {
        delete fg;
        return -1;
    }
    fg->device = static_cast<ID3D12Device *>(device);
    fg->gameQueue = static_cast<ID3D12CommandQueue *>(gameQueue);
    fg->displayWidth = displayWidth;
    fg->displayHeight = displayHeight;
    fg->renderWidth = renderWidth;
    fg->renderHeight = renderHeight;
    fg->backBufferFormat = backBufferFormat;
    fg->flags = flags;

    int code = createFgContext(fg);
    if (code != 0) {
        delete fg;
        return code;
    }
    *outFg = fg;
    return 0;
}

int fsr3_fg_recreate(
    void *fgIn,
    uint32_t displayWidth, uint32_t displayHeight,
    uint32_t renderWidth, uint32_t renderHeight,
    uint32_t backBufferFormat, uint32_t flags) {
    auto *fg = static_cast<Fsr3FgState *>(fgIn);
    if (!fg || displayWidth == 0 || displayHeight == 0) return -1;
    // The old context goes first; the wrapped swap chain (its
    // swapchain context, whose callbacks point at this Fsr3FgState)
    // stays alive and is ResizeBuffers'd by the host right after.
    if (fg->fgContext) {
        fg->api.destroyContext(&fg->fgContext, nullptr);
        fg->fgContext = nullptr;
    }
    fg->displayWidth = displayWidth;
    fg->displayHeight = displayHeight;
    fg->renderWidth = renderWidth;
    fg->renderHeight = renderHeight;
    fg->backBufferFormat = backBufferFormat;
    fg->flags = flags;
    fg->lastFrameID = 0;
    fg->enabled = false;
    return createFgContext(fg);
}

int fsr3_fg_wrap_swapchain(void *fgIn, void **swapchainInOut, void **outWrapped) {
    auto *fg = static_cast<Fsr3FgState *>(fgIn);
    if (!fg || !swapchainInOut || !*swapchainInOut || !outWrapped) return -1;
    auto *swapchain = static_cast<IDXGISwapChain4 *>(*swapchainInOut);

    // Hold one extra reference on the chain across the call. The DLL
    // takes the chain by pointer-to-pointer, and a failed creation can
    // consume a reference to it in its error cleanup - that is the
    // prime suspect for the first FSR run's crash: the wrap failed
    // with RUNTIME_ERROR, and half a second later the host crashed in
    // dxgi.dll presenting through a chain whose reference was gone.
    // The extra reference is never released: one leaked reference on
    // a chain that lives for the process anyway.
    swapchain->AddRef();

    // Diagnostics for the wrap failure (code 3 = RUNTIME_ERROR: a
    // D3D12 call inside AMD's DLL failed). The chain's desc and
    // whether its device is the FG context's device decide the next
    // fix; they are written to fsr3_wrap_diag.txt beside this DLL.
    DXGI_SWAP_CHAIN_DESC1 desc1{};
    HRESULT hrDesc = swapchain->GetDesc1(&desc1);
    ID3D12Device *chainDevice = nullptr;
    HRESULT hrDev = swapchain->GetDevice(IID_PPV_ARGS(&chainDevice));
    bool deviceMatches = (hrDev == S_OK && chainDevice && chainDevice == fg->device);
    if (chainDevice) chainDevice->Release();

    // Probe before the call, so the diag file says which of the
    // D3D12 calls the wrap needs is the one that fails.
    WrapProbes probes = probeWrap(fg->device, fg->gameQueue, swapchain);

    ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrapDesc{};
    wrapDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
    wrapDesc.swapchain = &swapchain;
    wrapDesc.gameQueue = fg->gameQueue;

    ffxReturnCode_t code = fg->api.createContext(&fg->swapContext, &wrapDesc.header, nullptr);
    bool ok = (code == FFX_API_RETURN_OK && fg->swapContext && swapchain);
    wchar_t folder[MAX_PATH];
    if (bridgeFolder(folder, MAX_PATH)) {
        writeWrapDiag(folder, &desc1, hrDesc, deviceMatches, hrDev, fg->gameQueue,
                      static_cast<int>(code), ok, &probes);
    }
    if (!ok) {
        // A failed creation can leave a half-made swapchain context
        // behind; destroy it so nothing half-initialised survives.
        if (fg->swapContext) {
            fg->api.destroyContext(&fg->swapContext, nullptr);
            fg->swapContext = nullptr;
        }
        return static_cast<int>(code);
    }
    // The wrap replaces the chain in place; `swapchain` now points at
    // the frame-interpolation proxy. Hand it back with a new reference.
    swapchain->AddRef();
    *outWrapped = swapchain;
    return 0;
}

int fsr3_fg_prepare(
    void *fgIn, void *commandList,
    void *depth, uint32_t depthState,
    void *motionVectors, uint32_t motionVectorState,
    float jitterX, float jitterY,
    float motionVectorScaleX, float motionVectorScaleY,
    float frameTimeMs, int reset,
    float cameraNear, float cameraFar, float cameraFovAngleVertical,
    uint64_t frameID) {
    auto *fg = static_cast<Fsr3FgState *>(fgIn);
    if (!fg || !commandList || !depth || !motionVectors) return -1;
    if (fg->lastFrameID != 0 && frameID != fg->lastFrameID + 1) {
        // A step other than +1 resets FSR 3's logic; that is expected
        // after a scene cut, so it is not an error here - the reset
        // flag carries it.
    }
    (void)reset; // FSR 3.1.4's prepare has only `unused_reset`, which
                 // the implementation ignores (see the file header):
                 // there is no working reset flag in this SDK.

    ffxDispatchDescFrameGenerationPrepare prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepare.frameID = static_cast<uint32_t>(frameID);
    prepare.flags = 0;
    prepare.commandList = commandList;
    prepare.renderSize = {fg->renderWidth, fg->renderHeight};
    prepare.jitterOffset = {jitterX, jitterY};
    prepare.motionVectorScale = {motionVectorScaleX, motionVectorScaleY};
    prepare.frameTimeDelta = frameTimeMs;
    prepare.unused_reset = false;
    prepare.cameraNear = cameraNear;
    prepare.cameraFar = cameraFar;
    prepare.cameraFovAngleVertical = cameraFovAngleVertical;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = apiResource(static_cast<ID3D12Resource *>(depth), depthState);
    prepare.motionVectors = apiResource(static_cast<ID3D12Resource *>(motionVectors), motionVectorState);

    // The camera info is a linked struct after the prepare desc -
    // a required input in FSR 3.1.4. The title gives no camera
    // matrices (see the file header).
    ffxDispatchDescFrameGenerationPrepareCameraInfo cameraInfo{};
    cameraInfo.cameraPosition[0] = 0.0f;
    cameraInfo.cameraPosition[1] = 0.0f;
    cameraInfo.cameraPosition[2] = 0.0f;
    cameraInfo.cameraUp[0] = 0.0f;
    cameraInfo.cameraUp[1] = 1.0f;
    cameraInfo.cameraUp[2] = 0.0f;
    cameraInfo.cameraRight[0] = 1.0f;
    cameraInfo.cameraRight[1] = 0.0f;
    cameraInfo.cameraRight[2] = 0.0f;
    cameraInfo.cameraForward[0] = 0.0f;
    cameraInfo.cameraForward[1] = 0.0f;
    cameraInfo.cameraForward[2] = -1.0f;
    prepare.header.pNext = reinterpret_cast<decltype(prepare.header.pNext)>(
        linkHeader(cameraInfo, FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO));

    ffxReturnCode_t code = fg->api.dispatch(&fg->fgContext, &prepare.header);
    if (code == FFX_API_RETURN_OK) fg->lastFrameID = frameID;
    return static_cast<int>(code);
}

int fsr3_fg_configure(
    void *fgIn, void *swapChain,
    void *hudlessColor, uint32_t hudlessState,
    uint64_t frameID, int enabled) {
    auto *fg = static_cast<Fsr3FgState *>(fgIn);
    if (!fg || !swapChain) return -1;

    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = swapChain;
    config.frameGenerationCallback = &frameGenerationCallback;
    config.frameGenerationCallbackUserContext = fg;
    config.presentCallback = &presentCallback;
    config.presentCallbackUserContext = fg;
    config.frameGenerationEnabled = enabled != 0;
    config.allowAsyncWorkloads = (fg->flags & FSR3_FG_FLAG_ASYNC_WORKLOADS) != 0;
    config.HUDLessColor = hudlessColor
        ? apiResource(static_cast<ID3D12Resource *>(hudlessColor), hudlessState)
        : FfxApiResource{};
    config.flags = 0;
    config.onlyPresentGenerated = false;
    config.generationRect = {0, 0, static_cast<int32_t>(fg->displayWidth), static_cast<int32_t>(fg->displayHeight)};
    config.frameID = static_cast<uint32_t>(frameID);

    ffxReturnCode_t code = fg->api.configure(&fg->fgContext, &config.header);
    if (code == FFX_API_RETURN_OK) fg->enabled = enabled != 0;
    return static_cast<int>(code);
}

void fsr3_fg_destroy(void *fgIn) {
    auto *fg = static_cast<Fsr3FgState *>(fgIn);
    if (!fg) return;
    if (fg->swapContext) fg->api.destroyContext(&fg->swapContext, nullptr);
    if (fg->fgContext) fg->api.destroyContext(&fg->fgContext, nullptr);
    if (fg->api.module) FreeLibrary(fg->api.module);
    delete fg;
}

} // extern "C"
