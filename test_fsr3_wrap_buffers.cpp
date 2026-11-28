// test_fsr3_wrap_buffers.cpp — does the 3.1.7 Wrap need 3 back buffers?
// The v5.3 bridge's FG context now succeeds (SetDllDirectoryW fixed
// provider discovery), but the Wrap still fails code 3 in-game, where
// the chain has 2 buffers. The standalone test that SUCCEEDED used
// 3 buffers. This test tries the Wrap with BufferCount=2 and then
// BufferCount=3 on the same device, to find whether the buffer count
// is what decides it.
// Compile (Developer PowerShell for VS 2022, in E:\Claude9):
//   cl.exe /EHsc /std:c++17 test_fsr3_wrap_buffers.cpp /I E:\Claude9\fsr3-sdk-2.3.0\Kits\FidelityFX\api\include /I E:\Claude9\fsr3-sdk-2.3.0\Kits\FidelityFX\framegeneration\include /link d3d12.lib dxgi.lib user32.lib
// Run: .\test_fsr3_wrap_buffers.exe

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>

#include <ffx_api.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

using FfxCreateContextFn = ffxReturnCode_t (*)(ffxContext *, ffxCreateContextDescHeader *, const ffxAllocationCallbacks *);
using FfxDestroyContextFn = ffxReturnCode_t (*)(ffxContext *, const ffxAllocationCallbacks *);

static const wchar_t *kLoader = L"E:\\Claude9\\fsr3-sdk-2.3.0\\Kits\\FidelityFX\\signedbin\\amd_fidelityfx_loader_dx12.dll";
static const wchar_t *kFolder = L"E:\\Claude9\\fsr3-sdk-2.3.0\\Kits\\FidelityFX\\signedbin";

int main() {
    // Load Streamline's interposer FIRST, exactly like the game
    // does (victory loads sl.interposer.dll at startup, before
    // the device and swap chain are made). The interposer hooks
    // DXGI globally, so even a chain created through the "real"
    // factory may come back as a Streamline proxy — and the FSR
    // Wrap may reject a proxied chain. If loading the interposer
    // here makes the Wraps fail, Streamline is the cause.
    HMODULE slInterposer = LoadLibraryW(L"E:\\sdk\\bin\\x64\\sl.interposer.dll");
    printf("Streamline interposer: %s\n", slInterposer ? "loaded" : "NOT loaded (not the cause, or path wrong)");

    AddDllDirectory(kFolder);
    SetDllDirectoryW(kFolder);
    HMODULE loader = LoadLibraryExW(kLoader, nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!loader) { printf("loader: LoadLibrary failed (err=%lu)\n", GetLastError()); return 1; }
    printf("loader: loaded\n");
    auto createContext = reinterpret_cast<FfxCreateContextFn>(GetProcAddress(loader, "ffxCreateContext"));
    auto destroyContext = reinterpret_cast<FfxDestroyContextFn>(GetProcAddress(loader, "ffxDestroyContext"));
    if (!createContext || !destroyContext) { printf("loader: missing ffx* exports\n"); return 1; }

    ID3D12Device *device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) || !device) {
        printf("device: FAILED\n"); return 1;
    }
    printf("device: ok\n");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || !queue) {
        printf("queue: FAILED\n"); return 1;
    }
    printf("queue: ok\n");

    // A SECOND, different native queue. In the game, the chain is
    // created with victory's queue (a Streamline proxy queue) but
    // the FSR Wrap is told that same proxy queue and fails. The
    // question is whether the Wrap cares which queue it is given:
    // if it accepts a queue DIFFERENT from the chain's creation
    // queue, then passing a separate native queue to the bridge
    // (while the chain keeps the proxy queue) is a viable fix.
    ID3D12CommandQueue *queue2 = nullptr;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue2))) || !queue2) {
        printf("queue2: FAILED\n"); return 1;
    }
    printf("queue2: ok (a different native queue, for the Wrap desc)\n");

    // Use a REAL window, exactly like test_fsr3_wrap_v2 (which
    // SUCCEEDED). The previous version used GetConsoleWindow();
    // the console window is not a proper swap-chain target and
    // may be why both buffer counts failed there, even though
    // the provider was enumerated.
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"fsr3-buffers-test", WS_OVERLAPPEDWINDOW,
                                0, 0, 2560, 1440, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) { printf("CreateWindow failed\n"); return 1; }
    printf("window: ok\n");

    // Query the Wrap providers (loads the framegeneration DLL),
    // matching test v2 which queried FG + Wrap + ForHwnd first.
    {
        auto query = reinterpret_cast<ffxReturnCode_t (*)(ffxContext *, ffxQueryDescHeader *)>(
            GetProcAddress(loader, "ffxQuery"));
        if (!query) { printf("loader: missing ffxQuery\n"); return 1; }
        ffxQueryDescGetVersions qv{};
        qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        qv.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
        qv.device = device;
        uint64_t count = 0;
        qv.outputCount = &count;
        qv.versionIds = nullptr;
        qv.versionNames = nullptr;
        ffxReturnCode_t qrc = query(nullptr, &qv.header);
        printf("Wrap providers: query rc=%d count=%llu\n", (int)qrc, (unsigned long long)count);
    }

    // Now create an FG context FIRST, exactly like the bridge does
    // in-game (fsr3_fg_create runs before fsr3_fg_wrap_swapchain).
    // The standalone tests that SUCCEEDED never created an FG
    // context; the game always does. If creating the FG context is
    // what breaks the subsequent Wrap, both Wraps below will fail
    // and we will know it is the FG context, not the chain.
    // (createContext / destroyContext are already defined above.)
    ffxContext fgCtx = nullptr;
    {
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device;
        ffxCreateContextDescFrameGeneration fgDesc{};
        fgDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        // NOTE: try the game's likely camera flags. The game (GTA, SDR)
        // almost certainly uses reversed-Z depth, so DEPTH_INVERTED is
        // on; HDR is off (SDR). The standalone tests that SUCCEEDED
        // used flags=0. If the Wrap fails with DEPTH_INVERTED here,
        // the FG context flags are the cause.
        fgDesc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        fgDesc.displaySize = {2560, 1440};
        fgDesc.maxRenderSize = {2560, 1440};
        fgDesc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        fgDesc.header.pNext = &backend.header;
        ffxReturnCode_t fgCode = createContext(&fgCtx, &fgDesc.header, nullptr);
        printf("FG context: code=%d ctx=%p %s\n", (int)fgCode, (void *)fgCtx,
               fgCode == FFX_API_RETURN_OK ? "SUCCEEDED" : "FAILED");
    }

    uint32_t counts[] = {2u, 3u};
    for (int ci = 0; ci < 2; ++ci) {
        uint32_t buffers = counts[ci];
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = 2560;
        scd.Height = 1440;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = buffers;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        // NOTE: pass nullptr for the fullscreen desc, EXACTLY like
        // victory's presenter() does
        // (CreateSwapChainForHwnd(&queue, hwnd, &desc, None, None)).
        // The standalone tests that SUCCEEDED passed &fsd
        // (Windowed=TRUE). If the Wrap fails with nullptr here,
        // the fullscreen desc is the difference and the fix is to
        // pass a fullscreen desc in presenter().
        IDXGIFactory2 *factory = nullptr;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || !factory) {
            printf("buffers=%u: factory FAILED\n", (unsigned)buffers); continue;
        }
        IDXGISwapChain1 *chain1 = nullptr;
        if (FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &scd, nullptr, nullptr, &chain1)) || !chain1) {
            printf("buffers=%u: CreateSwapChainForHwnd FAILED\n", (unsigned)buffers);
            factory->Release();
            continue;
        }
        IDXGISwapChain4 *chain = nullptr;
        if (FAILED(chain1->QueryInterface(IID_PPV_ARGS(&chain))) || !chain) {
            printf("buffers=%u: QueryInterface IDXGISwapChain4 FAILED\n", (unsigned)buffers);
            chain1->Release();
            factory->Release();
            continue;
        }
        chain1->Release();
        printf("buffers=%u: swap chain ok\n", (unsigned)buffers);

        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device;
        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
        version.header.pNext = &backend.header;

        ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrapDesc{};
        wrapDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
        wrapDesc.swapchain = &chain;
        // NOTE: the chain was created with `queue`, but the Wrap is
        // told `queue2` (a different native queue). This tests
        // whether the Wrap cares which queue it is given.
        wrapDesc.gameQueue = queue2;
        wrapDesc.header.pNext = &version.header;

        ffxContext swapCtx = nullptr;
        ffxReturnCode_t code = createContext(&swapCtx, &wrapDesc.header, nullptr);
        printf("buffers=%u: Wrap code=%d wrapped=%p %s\n", (unsigned)buffers, (int)code, (void *)chain,
               code == FFX_API_RETURN_OK ? "SUCCEEDED" : "FAILED");
        if (swapCtx) destroyContext(&swapCtx, nullptr);
        // Release the chain only when the Wrap succeeded (then
        // `chain` is the proxy). On failure the DLL may have
        // clobbered `chain`, so do not touch it — the original
        // leaks one ref, harmless for a test that exits right
        // after. (chain1 was already released after QueryInterface.)
        if (code == FFX_API_RETURN_OK) chain->Release();
        factory->Release();
    }
    if (fgCtx) destroyContext(&fgCtx, nullptr);
    printf("done\n");
    return 0;
}
