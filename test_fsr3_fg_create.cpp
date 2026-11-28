// test_fsr3_fg_create.cpp — standalone FG context creation probe for SDK 2.3.0.
// The bridge's FG context creation fails with INVALID_ARGUMENT (code 2)
// in v5.0, v5.1 AND v5.2, so the override and the DEBUG_CHECKING flag
// were both ruled out. This test tries the FG context creation directly
// (no bridge, no game) with several pNext combinations, to find what
// SDK 2.3.0's 3.1.6 provider actually wants:
//   A. backend desc only (what v5.2 does — expect code 2, confirms the bug)
//   B. backend + FG Version struct, version = FFX_FRAMEGENERATION_VERSION (4.0.1)
//   C. backend + FG Version struct, version = 3.1.6 (0xC01006)
//   D. backend + FG Version struct (4.0.1) + ffxOverrideVersion pinned to 3.1.6
// Compile (Developer PowerShell for VS 2022, in E:\Claude9):
//   cl.exe /EHsc /std:c++17 test_fsr3_fg_create.cpp /I E:\Claude9\fsr3-sdk-2.3.0\Kits\FidelityFX\api\include /I E:\Claude9\fsr3-sdk-2.3.0\Kits\FidelityFX\framegeneration\include /link d3d12.lib dxgi.lib user32.lib
// Run: .\test_fsr3_fg_create.exe
// (Or the vcvarsall one-liner with /k so the window stays open.)

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
using FfxQueryFn = ffxReturnCode_t (*)(ffxContext *, ffxQueryDescHeader *);

static const wchar_t *kLoader = L"E:\\Claude9\\fsr3-sdk-2.3.0\\Kits\\FidelityFX\\signedbin\\amd_fidelityfx_loader_dx12.dll";

static uint64_t queryFgProviderId(FfxQueryFn query, ID3D12Device *device) {
    ffxQueryDescGetVersions qv{};
    qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    qv.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    qv.device = device;
    uint64_t count = 0;
    qv.outputCount = &count;
    qv.versionIds = nullptr;
    qv.versionNames = nullptr;
    if (query(nullptr, &qv.header) != FFX_API_RETURN_OK || count == 0) return 0;
    uint64_t ids[1] = {};
    uint64_t outCount = 1;
    ffxQueryDescGetVersions qv2{};
    qv2.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    qv2.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    qv2.device = device;
    qv2.outputCount = &outCount;
    qv2.versionIds = ids;
    qv2.versionNames = nullptr;
    if (query(nullptr, &qv2.header) != FFX_API_RETURN_OK || outCount == 0) return 0;
    return ids[0];
}

int main() {
    AddDllDirectory(L"E:\\Claude9\\fsr3-sdk-2.3.0\\Kits\\FidelityFX\\signedbin");
    // Also set the DLL directory: test_fsr3_wrap_v2.cpp (which
    // enumerated the providers fine and wrapped successfully) calls
    // BOTH AddDllDirectory and SetDllDirectoryW. A plain
    // LoadLibrary from inside the loader resolves the effect DLL
    // via the SetDllDirectory path; AddDllDirectory alone was not
    // enough (this test's first run returned 0 FG providers).
    SetDllDirectoryW(L"E:\\Claude9\\fsr3-sdk-2.3.0\\Kits\\FidelityFX\\signedbin");
    HMODULE loader = LoadLibraryExW(kLoader, nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!loader) { printf("loader: LoadLibrary failed (err=%lu)\n", GetLastError()); return 1; }
    printf("loader: loaded\n");
    auto createContext = reinterpret_cast<FfxCreateContextFn>(GetProcAddress(loader, "ffxCreateContext"));
    auto destroyContext = reinterpret_cast<FfxDestroyContextFn>(GetProcAddress(loader, "ffxDestroyContext"));
    auto query = reinterpret_cast<FfxQueryFn>(GetProcAddress(loader, "ffxQuery"));
    if (!createContext || !destroyContext || !query) { printf("loader: missing ffx* exports\n"); return 1; }

    ID3D12Device *device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) || !device) {
        printf("device: FAILED\n"); return 1;
    }
    printf("device: ok\n");

    uint64_t fgId = queryFgProviderId(query, device);
    printf("FG provider id: %llu (expect 17726168133342859270 = 3.1.6)\n", (unsigned long long)fgId);

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;

    auto tryCreate = [&](const char *label, ffxCreateContextDescHeader *pNext) {
        ffxCreateContextDescFrameGeneration desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        desc.flags = 0;
        desc.displaySize = {2560, 1440};
        desc.maxRenderSize = {2560, 1440};
        desc.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        desc.header.pNext = pNext;
        ffxContext ctx = nullptr;
        ffxReturnCode_t code = createContext(&ctx, &desc.header, nullptr);
        printf("%s: code=%d ctx=%p %s\n", label, (int)code, (void *)ctx,
               code == FFX_API_RETURN_OK ? "SUCCEEDED" : "failed");
        if (ctx) destroyContext(&ctx, nullptr);
        return code;
    };

    // A. backend only (v5.2's approach)
    tryCreate("A backend-only", &backend.header);

    // B. backend + FG Version struct, 4.0.1
    ffxCreateContextDescFrameGenerationVersion ver401{};
    ver401.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver401.version = FFX_FRAMEGENERATION_VERSION; // 4.0.1
    ver401.header.pNext = &backend.header;
    tryCreate("B version 4.0.1 + backend", &ver401.header);

    // C. backend + FG Version struct, 3.1.6
    ffxCreateContextDescFrameGenerationVersion ver316{};
    ver316.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver316.version = 0xC01006u; // version 3.1.6 = (3<<22)|(1<<12)|6 (FFX_MAKE_VERSION is not in these headers)
    ver316.header.pNext = &backend.header;
    tryCreate("C version 3.1.6 + backend", &ver316.header);

    // D. backend + FG Version struct (4.0.1) + override pinned to 3.1.6
    if (fgId != 0) {
        ffxOverrideVersion ovr{};
        ovr.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        ovr.versionId = fgId;
        ovr.header.pNext = &backend.header;
        ffxCreateContextDescFrameGenerationVersion verD{};
        verD.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        verD.version = FFX_FRAMEGENERATION_VERSION; // 4.0.1
        verD.header.pNext = &ovr.header;
        tryCreate("D version 4.0.1 + override 3.1.6 + backend", &verD.header);
    } else {
        printf("D skipped (no FG provider id)\n");
    }

    printf("done\n");
    return 0;
}
