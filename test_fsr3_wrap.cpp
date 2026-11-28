// Standalone test for the FSR 3 swap-chain wrap, outside victory.
// Builds a plain D3D12 device + swap chain (the same shape victory
// makes: 2560x1440, R8G8B8A8_UNORM, 2 buffers, FLIP_DISCARD, with
// FRAME_LATENCY_WAITABLE_OBJECT), creates the bridge's FG context,
// and wraps the chain. If the wrap succeeds here, the bridge + DLL +
// wrap desc are correct and the failure is victory-specific. If it
// fails here too, the problem is in the bridge / DLL / setup, not
// victory.
//
// This version enables the D3D12 DEBUG LAYER before creating the
// device, so that when the DLL's wrap fails, the debug layer records
// the exact failing D3D12 call. The info-queue messages are dumped
// to the console after the wrap. That names the blocker.
//
// Build (VS 2022 BuildTools, from a Developer PowerShell or any
// PowerShell with cl.exe on PATH - the Get-FSR3.cmd build already
// proved cl.exe works):
//   cl.exe /EHsc /std:c++17 test_fsr3_wrap.cpp /link d3d12.lib dxgi.lib user32.lib
// Run (the exe can live anywhere; it loads the bridge by full path):
//   .\test_fsr3_wrap.exe
// The bridge (fsr3_bridge.dll) and AMD's DLL (amd_fidelityfx_dx12.dll)
// must already be in E:\Claude9\fsr3-sdk\ - the bridge loads AMD's
// DLL from its own folder.

#include <windows.h>
#include <cstdint>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_5.h>
#include <cstdio>

typedef int (*PFN_fsr3_fg_create)(void *, void *, uint32_t, uint32_t,
                                  uint32_t, uint32_t, uint32_t, uint32_t,
                                  void **);
typedef int (*PFN_fsr3_fg_wrap_swapchain)(void *, void **, void **);
typedef int (*PFN_fsr3_fg_create_swapchain_for_hwnd)(void *, void **, void **);
typedef int (*PFN_fsr3_query_versions)(void *);

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int main() {
    // Enable the D3D12 debug layer BEFORE creating the device, so
    // the failing D3D12 call inside AMD's DLL gets recorded in the
    // info queue. Without this the DLL fails silently (code 3, no
    // message). In victory the device is already made, so the debug
    // layer cannot be turned on there - but here we own the device.
    ID3D12Debug *d3dDebug = nullptr;
    HRESULT hrDbg = D3D12GetDebugInterface(IID_PPV_ARGS(&d3dDebug));
    if (SUCCEEDED(hrDbg) && d3dDebug) {
        d3dDebug->EnableDebugLayer();
        printf("D3D12 debug layer: enabled\n");
    } else {
        printf("D3D12 debug layer: NOT available (hr=0x%08x) - messages will be missing\n", (unsigned)hrDbg);
    }

    // A window for the swap chain. It never shows a frame; it only
    // has to exist so CreateSwapChainForHwnd has an HWND.
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Fsr3WrapTest";
    if (!RegisterClassW(&wc)) { printf("RegisterClass failed %lu\n", GetLastError()); return 1; }
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"FSR3 wrap test",
                                WS_OVERLAPPEDWINDOW, 100, 100, 2560, 1440,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { printf("CreateWindow failed %lu\n", GetLastError()); return 1; }

    // A plain D3D12 device on the default adapter, and a direct queue.
    ID3D12Device *device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                                   IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) { printf("D3D12CreateDevice hr=0x%08x\n", (unsigned)hr); return 1; }
    printf("device: ok\n");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (FAILED(hr) || !queue) { printf("CreateCommandQueue hr=0x%08x\n", (unsigned)hr); return 1; }
    printf("queue: ok\n");

    // The swap chain, the same shape victory makes.
    IDXGIFactory2 *factory = nullptr;
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) { printf("CreateDXGIFactory2 hr=0x%08x\n", (unsigned)hr); return 1; }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 2560;
    desc.Height = 1440;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Round 9: 3 back buffers instead of 2. FSR 3 frame generation
    // interpolates between rendered frames, so the swap-chain proxy
    // may need a third buffer to hold a frame while it generates
    // the interpolated one. Victory and the earlier test both used 2
    // (double buffering); if the DLL's wrap requires triple
    // buffering it would reject a 2-buffer chain at dispatch with
    // no D3D12 call — which matches the silent code-3 failure.
    desc.BufferCount = 3;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    IDXGISwapChain1 *chain1 = nullptr;
    hr = factory->CreateSwapChainForHwnd(queue, hwnd, &desc, nullptr, nullptr, &chain1);
    if (FAILED(hr) || !chain1) { printf("CreateSwapChainForHwnd hr=0x%08x\n", (unsigned)hr); return 1; }
    printf("swap chain: ok (flags=0x%x)\n", (unsigned)desc.Flags);
    IDXGISwapChain3 *chain = nullptr;
    hr = chain1->QueryInterface(IID_PPV_ARGS(&chain));
    if (FAILED(hr) || !chain) { printf("chain QI to IDXGISwapChain3 hr=0x%08x\n", (unsigned)hr); return 1; }
    IDXGISwapChain4 *chain4 = nullptr;
    hr = chain->QueryInterface(IID_PPV_ARGS(&chain4));
    if (FAILED(hr) || !chain4) { printf("chain QI to IDXGISwapChain4 hr=0x%08x\n", (unsigned)hr); return 1; }

    // Load the bridge and resolve its functions.
    HMODULE bridge = LoadLibraryW(L"E:\\Claude9\\fsr3-sdk\\fsr3_bridge.dll");
    if (!bridge) { printf("LoadLibrary fsr3_bridge.dll failed %lu\n", GetLastError()); return 1; }
    auto fg_create = (PFN_fsr3_fg_create)GetProcAddress(bridge, "fsr3_fg_create");
    auto fg_wrap = (PFN_fsr3_fg_wrap_swapchain)GetProcAddress(bridge, "fsr3_fg_wrap_swapchain");
    auto fg_forhwnd = (PFN_fsr3_fg_create_swapchain_for_hwnd)GetProcAddress(bridge, "fsr3_fg_create_swapchain_for_hwnd");
    auto fg_query_versions = (PFN_fsr3_query_versions)GetProcAddress(bridge, "fsr3_query_versions");
    if (!fg_create || !fg_wrap) { printf("GetProcAddress failed (create=%p wrap=%p)\n", (void*)fg_create, (void*)fg_wrap); return 1; }
    if (!fg_forhwnd) { printf("note: fsr3_fg_create_swapchain_for_hwnd not found (old bridge?) - ForHwnd test skipped\n"); }
    if (!fg_query_versions) { printf("note: fsr3_query_versions not found (old bridge?) - provider enumeration skipped\n"); }
    printf("bridge: loaded\n");

    // The FG context, same sizes/format as the chain.
    void *fg = nullptr;
    int code = fg_create(device, queue, 2560, 1440, 2560, 1440,
                         (uint32_t)DXGI_FORMAT_R8G8B8A8_UNORM, 0, &fg);
    if (code != 0 || !fg) { printf("fsr3_fg_create failed code=%d\n", code); return 1; }
    printf("FG context: made\n");

    // Round 11: ask the DLL which providers it actually has, before
    // trying to create a swap-chain context. If the DLL reports no
    // provider for the swap-chain desc types, the DLL build is
    // missing the effect and no desc tweak will fix the wrap.
    // Results go to fsr3_versions.txt beside the bridge DLL.
    if (fg_query_versions) {
        int qrc = fg_query_versions(fg);
        printf("fsr3_query_versions returned %d (see fsr3_versions.txt beside the bridge DLL)\n", qrc);
    }

    // The wrap. This is the call that fails inside victory.
    void *raw = chain4;
    void *wrapped = nullptr;
    code = fg_wrap(fg, &raw, &wrapped);
    printf("fsr3_fg_wrap_swapchain returned code=%d wrapped=%p\n", code, wrapped);
    if (code == 0 && wrapped) {
        printf("WRAP SUCCEEDED standalone: the bridge + DLL + wrap desc are correct;\n");
        printf("the failure inside victory is victory-specific (its device/queue/chain).\n");
    } else {
        printf("WRAP FAILED standalone too (code=%d): the problem is in the bridge,\n", code);
        printf("the DLL, or the setup - not victory. Check fsr3_wrap_diag.txt beside\n");
        printf("the bridge DLL for the probes.\n");
        // Round 10: Wrap failed again, so try the ForHwnd variant -
        // the path the official FSR 3 sample uses. It creates a NEW
        // proxy chain from the window instead of wrapping this one.
        // If ForHwnd succeeds, Wrap is the broken path and the fix
        // is to switch the bridge (and victory) to ForHwnd.
        if (fg_forhwnd) {
            void *raw2 = chain4;
            void *newChain = nullptr;
            int code2 = fg_forhwnd(fg, &raw2, &newChain);
            printf("fsr3_fg_create_swapchain_for_hwnd returned code=%d newChain=%p\n", code2, newChain);
            if (code2 == 0 && newChain) {
                printf("FORHWND SUCCEEDED standalone: the swap-chain context CAN be created;\n");
                printf("only the Wrap path is broken. Switch the bridge to ForHwnd.\n");
            } else {
                printf("FORHWND FAILED too (code=%d): swap-chain context creation itself is broken\n", code2);
                printf("in this DLL/setup, not just the Wrap path.\n");
            }
        }
    }

    // Dump the D3D12 debug layer's info queue. If the wrap failed,
    // the messages here name the exact D3D12 call that failed and
    // why - the DLL itself stays silent, but the debug layer does
    // not. Print the last few messages (the failure is at the end).
    ID3D12InfoQueue *iq = nullptr;
    HRESULT hrIq = device->QueryInterface(IID_PPV_ARGS(&iq));
    if (SUCCEEDED(hrIq) && iq) {
        UINT64 count = iq->GetNumStoredMessages();
        printf("D3D12 info-queue messages: %llu\n", (unsigned long long)count);
        UINT64 start = count > 20 ? count - 20 : 0;
        for (UINT64 i = start; i < count; ++i) {
            SIZE_T len = 0;
            iq->GetMessage(i, nullptr, &len);
            if (len == 0) continue;
            D3D12_MESSAGE *msg = (D3D12_MESSAGE *)malloc(len);
            if (!msg) continue;
            if (SUCCEEDED(iq->GetMessage(i, msg, &len)) && msg->pDescription) {
                printf("  [%llu] %s\n", (unsigned long long)i, msg->pDescription);
            }
            free(msg);
        }
    } else {
        printf("D3D12 info-queue: not available (hr=0x%08x)\n", (unsigned)hrIq);
    }
    return 0;
}
