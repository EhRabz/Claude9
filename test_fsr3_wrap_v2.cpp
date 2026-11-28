// Standalone FSR 3 swap-chain test against FidelityFX SDK 2.3.0
// (FSR 3.1.7) — DIRECT, no bridge.
//
// The SDK 1.1.4 DLL (provider 1.1.3) cannot create a frame-generation
// swap-chain context on this machine by any method (Wrap and ForHwnd
// both fail code 3, silently). SDK 2.3.0 ships FSR 3.1.7, and a public
// probe confirmed its ForHwnd swap-chain creation works on NVIDIA.
// This test answers one question: does the 2.3.0 loader +
// framegeneration DLL create a swap-chain context on THIS GPU?
//
// It loads the LOADER DLL (amd_fidelityfx_loader_dx12.dll) directly
// — the loader is interface-compatible with the old single DLL and
// exposes the same 5 functions. The framegeneration DLL
// (amd_fidelityfx_framegeneration_dx12.dll) must sit beside the
// loader (same folder). The loader resolves effect DLLs from its
// own folder, but a public probe found provider discovery also
// needs the folder on the DLL search path, so this test calls
// AddDllDirectory on the loader's folder before loading.
//
// SDK 2.1+ differences from 1.1.4 (read from the actual 2.3.0
// headers):
// - The swap-chain context creation wants a
//   ffxCreateContextDescFrameGenerationSwapChainVersionDX12 chained
//   in pNext, with version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION
//   (3.1.7). The swap-chain desc structs themselves (Wrap 0x30001,
//   New 0x30005, ForHwnd 0x30006) and their fields are unchanged.
// - The FG context creation wants a
//   ffxCreateContextDescFrameGenerationVersion (version =
//   FFX_FRAMEGENERATION_VERSION, 4.0.1) — but this test does NOT
//   create an FG context: the swap-chain creation only needs the
//   device and queue, and skipping the FG context isolates the
//   exact question (does the swap-chain provider initialise?).
// - ffxQueryDescGetVersions (type 4) and ffxOverrideVersion (type 5)
//   are unchanged, so the test enumerates the providers first and
//   can pin to the 3.1.7 provider id if the default selection fails.
//
// Usage:
//   1. Download SDK 2.3.0 source zip from
//      https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.3.0
//      ("Source code (zip)") and extract it, e.g. to
//      E:\Claude9\fsr3-sdk-2.3.0\  (the folder will be named
//      FidelityFX-SDK-2.3.0; rename it or adjust the paths below).
//      The signed DLLs are in
//      E:\Claude9\fsr3-sdk-2.3.0\Kits\FidelityFX\signedbin\
//   2. From Developer PowerShell for VS 2022 (or the vcvarsall
//      one-liner), compile:
//        cl.exe /EHsc /std:c++17 test_fsr3_wrap_v2.cpp ^
//          /I E:\Claude9\fsr3-sdk-2.3.0\Kits\FidelityFX\api\include ^
//          /I E:\Claude9\fsr3-sdk-2.3.0\Kits\FidelityFX\framegeneration\include ^
//          /link d3d12.lib dxgi.lib user32.lib
//      (adjust the two /I paths if you extracted the SDK elsewhere)
//   3. Run it from the signedbin folder so the loader finds the
//      framegeneration DLL, or pass the loader path:
//        .\test_fsr3_wrap_v2.exe E:\Claude9\fsr3-sdk-2.3.0\Kits\FidelityFX\signedbin\amd_fidelityfx_loader_dx12.dll
//
// If Wrap or ForHwnd succeeds here, SDK 2.3.0 fixes the problem and
// the bridge + fetcher get migrated to it. If both still fail code 3,
// SDK 2.x does not fix it either.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <cstdio>
#include <string>

// SDK 2.3.0 headers. The include paths on the cl.exe command line
// make these resolve: ffx_api.h lives in api/include, the DX12
// backend header in api/include/dx12, and the framegeneration
// swap-chain structs in framegeneration/include/dx12.
#include "ffx_api.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_framegeneration.h"
#include "dx12/ffx_api_framegeneration_dx12.h"

typedef ffxReturnCode_t (*PFN_ffxCreateContext)(ffxContext *, ffxCreateContextDescHeader *, const ffxAllocationCallbacks *);
typedef ffxReturnCode_t (*PFN_ffxDestroyContext)(ffxContext *, const ffxAllocationCallbacks *);
typedef ffxReturnCode_t (*PFN_ffxQuery)(ffxContext *, ffxQueryDescHeader *);

// Query GetVersions for one create-desc type and print the result.
// Returns the first version id, or 0 if none.
static uint64_t queryVersions(PFN_ffxQuery query, uint64_t createDescType, void *device, const char *label) {
    ffxQueryDescGetVersions qv{};
    qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    qv.createDescType = createDescType;
    qv.device = device;
    uint64_t count = 0;
    qv.outputCount = &count;
    qv.versionIds = nullptr;
    qv.versionNames = nullptr;
    ffxReturnCode_t rc = query(nullptr, &qv.header);
    printf("%s (type 0x%llx): query rc=%d count=%llu\n", label,
           (unsigned long long)createDescType, (int)rc, (unsigned long long)count);
    if (rc != FFX_API_RETURN_OK || count == 0) return 0;
    uint64_t cap = count > 8 ? 8 : count;
    uint64_t ids[8] = {};
    const char *names[8] = {};
    uint64_t outCount = cap;
    ffxQueryDescGetVersions qv2{};
    qv2.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    qv2.createDescType = createDescType;
    qv2.device = device;
    qv2.outputCount = &outCount;
    qv2.versionIds = ids;
    qv2.versionNames = names;
    if (query(nullptr, &qv2.header) == FFX_API_RETURN_OK) {
        for (uint64_t i = 0; i < outCount && i < 8; ++i) {
            printf("  [%llu] id=%llu name=%s\n", (unsigned long long)i,
                   (unsigned long long)ids[i], names[i] ? names[i] : "(null)");
        }
        return ids[0];
    }
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    std::wstring loaderPath = L"E:\\Claude9\\fsr3-sdk-2.3.0\\Kits\\FidelityFX\\signedbin\\amd_fidelityfx_loader_dx12.dll";
    if (argc > 1) loaderPath = argv[1];

    // The loader finds the framegeneration DLL in its own folder,
    // but a public probe found provider discovery also needs the
    // folder on the DLL search path. Add it first.
    std::wstring folder = loaderPath;
    size_t slash = folder.find_last_of(L"\\/");
    if (slash != std::wstring::npos) folder = folder.substr(0, slash);
    AddDllDirectory(folder.c_str());
    // Also set the DLL directory so LoadLibrary calls from the
    // loader resolve the effect DLL from this folder.
    SetDllDirectoryW(folder.c_str());

    HMODULE loader = LoadLibraryExW(loaderPath.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!loader) {
        printf("LoadLibrary failed for %ls (err=%lu)\n", loaderPath.c_str(), GetLastError());
        return 1;
    }
    auto createContext = (PFN_ffxCreateContext)GetProcAddress(loader, "ffxCreateContext");
    auto destroyContext = (PFN_ffxDestroyContext)GetProcAddress(loader, "ffxDestroyContext");
    auto query = (PFN_ffxQuery)GetProcAddress(loader, "ffxQuery");
    if (!createContext || !query) {
        printf("GetProcAddress failed (create=%p query=%p)\n", (void *)createContext, (void *)query);
        return 1;
    }
    printf("loader: loaded (%ls)\n", loaderPath.c_str());

    // Plain D3D12 device + queue + swap chain, same shape as
    // victory's: 2560x1440, R8G8B8A8_UNORM, 3 buffers,
    // FLIP_DISCARD, FRAME_LATENCY_WAITABLE_OBJECT.
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"fsr3-test", WS_OVERLAPPEDWINDOW,
                                0, 0, 2560, 1440, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) { printf("CreateWindow failed\n"); return 1; }

    ID3D12Debug *debug = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug) {
        debug->EnableDebugLayer();
        printf("D3D12 debug layer: enabled\n");
        debug->Release();
    } else {
        printf("D3D12 debug layer: NOT available (Graphics Tools not installed?)\n");
    }

    ID3D12Device *device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) { printf("D3D12CreateDevice failed hr=0x%08lx\n", (unsigned long)hr); return 1; }
    printf("device: ok\n");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (FAILED(hr) || !queue) { printf("CreateCommandQueue failed hr=0x%08lx\n", (unsigned long)hr); return 1; }
    printf("queue: ok\n");

    IDXGIFactory2 *factory = nullptr;
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) { printf("CreateDXGIFactory2 failed hr=0x%08lx\n", (unsigned long)hr); return 1; }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = 2560;
    scd.Height = 1440;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 3;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsd{};
    fsd.Windowed = TRUE;
    IDXGISwapChain1 *chain1 = nullptr;
    hr = factory->CreateSwapChainForHwnd(queue, hwnd, &scd, &fsd, nullptr, &chain1);
    if (FAILED(hr) || !chain1) { printf("CreateSwapChainForHwnd failed hr=0x%08lx\n", (unsigned long)hr); return 1; }
    IDXGISwapChain4 *chain = nullptr;
    hr = chain1->QueryInterface(IID_PPV_ARGS(&chain));
    chain1->Release();
    if (FAILED(hr) || !chain) { printf("QI IDXGISwapChain4 failed hr=0x%08lx\n", (unsigned long)hr); return 1; }
    printf("swap chain: ok (flags=0x40, buffers=3)\n");

    // Enumerate the providers first. Expect the swap-chain types to
    // report version 3.1.7.
    uint64_t fgId = queryVersions(query, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, device, "framegeneration");
    uint64_t wrapId = queryVersions(query, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12, device, "swapchain-wrap");
    uint64_t forHwndId = queryVersions(query, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12, device, "swapchain-for-hwnd");
    (void)fgId;

    // Common pNext pieces for the swap-chain creation: a backend
    // desc (device) and, new in SDK 2.1+, the swap-chain version
    // struct (3.1.7). Chain: desc -> version -> backend.
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;

    // --- Wrap ---
    {
        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 ver{};
        ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
        ver.header.pNext = &backend.header;

        ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrapDesc{};
        wrapDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
        IDXGISwapChain4 *sc = chain;
        wrapDesc.swapchain = &sc;
        wrapDesc.gameQueue = queue;
        wrapDesc.header.pNext = &ver.header;

        ffxContext swapCtx = nullptr;
        ffxReturnCode_t code = createContext(&swapCtx, &wrapDesc.header, nullptr);
        printf("Wrap: ffxCreateContext returned code=%d wrapped=%p\n", (int)code, (void *)sc);
        if (code == FFX_API_RETURN_OK && swapCtx && sc) {
            printf("WRAP SUCCEEDED (SDK 2.3.0 / FSR 3.1.7)\n");
            destroyContext(&swapCtx, nullptr);
        } else {
            printf("WRAP FAILED (code=%d)\n", (int)code);
            // Retry with an explicit provider-version override pinned
            // to the enumerated 3.1.7 provider id.
            if (wrapId != 0) {
                ffxOverrideVersion ov{};
                ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
                ov.versionId = wrapId;
                ov.header.pNext = &ver.header;
                wrapDesc.header.pNext = &ov.header;
                IDXGISwapChain4 *sc2 = chain;
                wrapDesc.swapchain = &sc2;
                ffxContext swapCtx2 = nullptr;
                ffxReturnCode_t code2 = createContext(&swapCtx2, &wrapDesc.header, nullptr);
                printf("Wrap (with version override): code=%d wrapped=%p\n", (int)code2, (void *)sc2);
                if (code2 == FFX_API_RETURN_OK && swapCtx2 && sc2) {
                    printf("WRAP SUCCEEDED with version override\n");
                    destroyContext(&swapCtx2, nullptr);
                } else {
                    printf("WRAP FAILED even with version override (code=%d)\n", (int)code2);
                }
            }
            if (swapCtx) destroyContext(&swapCtx, nullptr);
        }
    }

    // --- ForHwnd ---
    {
        ffxCreateContextDescFrameGenerationSwapChainVersionDX12 ver{};
        ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
        ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
        ver.header.pNext = &backend.header;

        ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 fhDesc{};
        fhDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
        IDXGISwapChain4 *newChain = nullptr;
        fhDesc.swapchain = &newChain;
        fhDesc.hwnd = hwnd;
        fhDesc.desc = &scd;
        fhDesc.fullscreenDesc = &fsd;
        fhDesc.dxgiFactory = factory;
        fhDesc.gameQueue = queue;
        fhDesc.header.pNext = &ver.header;

        ffxContext swapCtx = nullptr;
        ffxReturnCode_t code = createContext(&swapCtx, &fhDesc.header, nullptr);
        printf("ForHwnd: ffxCreateContext returned code=%d newChain=%p\n", (int)code, (void *)newChain);
        if (code == FFX_API_RETURN_OK && swapCtx && newChain) {
            printf("FORHWND SUCCEEDED (SDK 2.3.0 / FSR 3.1.7)\n");
            destroyContext(&swapCtx, nullptr);
        } else {
            printf("FORHWND FAILED (code=%d)\n", (int)code);
            if (forHwndId != 0) {
                ffxOverrideVersion ov{};
                ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
                ov.versionId = forHwndId;
                ov.header.pNext = &ver.header;
                fhDesc.header.pNext = &ov.header;
                IDXGISwapChain4 *newChain2 = nullptr;
                fhDesc.swapchain = &newChain2;
                ffxContext swapCtx2 = nullptr;
                ffxReturnCode_t code2 = createContext(&swapCtx2, &fhDesc.header, nullptr);
                printf("ForHwnd (with version override): code=%d newChain=%p\n", (int)code2, (void *)newChain2);
                if (code2 == FFX_API_RETURN_OK && swapCtx2 && newChain2) {
                    printf("FORHWND SUCCEEDED with version override\n");
                    destroyContext(&swapCtx2, nullptr);
                } else {
                    printf("FORHWND FAILED even with version override (code=%d)\n", (int)code2);
                }
            }
            if (swapCtx) destroyContext(&swapCtx, nullptr);
        }
    }

    printf("done\n");
    return 0;
}
