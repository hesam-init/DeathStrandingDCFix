#include "stdafx.h"
#include "overlay.hpp"
#include "helper.hpp"
#include "fonts/roboto_medium.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>
#include <safetyhook.hpp>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>

#if defined(_MSC_VER)
#include <intrin.h>
#define RETURN_ADDRESS() _ReturnAddress()
#else
#define RETURN_ADDRESS() __builtin_return_address(0)
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    // First DXGI factory the game created (kept alive), see Overlay_CreateDXGIFactory1
    std::atomic<IDXGIFactory*> GameFactory = nullptr;
}

// The ImGui DX12 backend's CreateDXGIFactory1 call is renamed to this (see cmake/Dependencies.cmake) so the dll
// has no static dxgi.dll import (that would load dxgi.dll before the game does and break dxgi.dll proxies such as
// OptiScaler). It hands out the game's own factory: creating another one would create another DXVK Vulkan instance
// under Proton, which OptiScaler's Vulkan hooks would pick up.
extern "C" HRESULT WINAPI Overlay_CreateDXGIFactory1(REFIID riid, void** factory)
{
    if (IDXGIFactory* gameFactory = GameFactory.load())
        return gameFactory->QueryInterface(riid, factory);

    using CreateDXGIFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
    static auto pCreateDXGIFactory1 = reinterpret_cast<CreateDXGIFactory1Fn>(reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1")));
    return pCreateDXGIFactory1 ? pCreateDXGIFactory1(riid, factory) : E_NOTIMPL;
}

// How the overlay hooks the game without disturbing OptiScaler and other graphics mods:
//  - No dummy D3D12 device or swapchain is ever created. Under Proton every DXGI swapchain also creates a Vulkan
//    surface, and OptiScaler's vkCreateWin32SurfaceKHR hook moves its own menu onto the newest surface, so a dummy
//    swapchain left OptiScaler's menu attached to a window that no longer exists.
//  - Only ds.exe's import table is patched (GetProcAddress, GetRawInputData, SetCursorPos, ClipCursor), so other
//    modules' calls are untouched and nothing collides with OptiScaler's inline hooks on the same functions.
//  - The game resolves CreateDXGIFactory1/2 through GetProcAddress (dxgi.dll is delay-loaded). The factories it gets
//    have their CreateSwapChain/CreateSwapChainForHwnd vtable slots swapped, which hands us the game's swapchain and
//    command queue as they are created. Only that swapchain instance is vtable-hooked (Present/Present1), as
//    REFramework does (https://github.com/praydog/REFramework).
namespace
{
    constexpr int kFramesInFlight = 3;
    constexpr UINT kSrvHeapSize = 64;
    constexpr float kFontSize = 16.00f;

    std::function<void()> DrawMenuFn;
    std::atomic<bool> bMenuOpen = false;
    std::atomic<bool> bInputPassthrough = false;
    bool bHomeHeld = false;

    // Function types
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    using CreateDXGIFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    using CreateDXGIFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
    using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
    using SetCursorPosFn = BOOL(WINAPI*)(int, int);
    using ClipCursorFn = BOOL(WINAPI*)(const RECT*);

    // vtable slots
    constexpr size_t kPresentIndex = 8;
    constexpr size_t kPresent1Index = 22;
    constexpr size_t kCreateSwapChainIndex = 10;
    constexpr size_t kCreateSwapChainForHwndIndex = 15;

    // Real functions behind the ds.exe imports we replaced
    GetProcAddressFn RealGetProcAddress = nullptr;
    GetRawInputDataFn RealGetRawInputData = nullptr;
    SetCursorPosFn RealSetCursorPos = nullptr;
    ClipCursorFn RealClipCursor = nullptr;
    std::atomic<CreateDXGIFactoryFn> RealCreateDXGIFactory = nullptr;
    std::atomic<CreateDXGIFactoryFn> RealCreateDXGIFactory1 = nullptr;
    std::atomic<CreateDXGIFactory2Fn> RealCreateDXGIFactory2 = nullptr;

    // Address range of ds.exe, to tell the game's own swapchain apart from ones other mods create
    uintptr_t GameBegin = 0;
    uintptr_t GameEnd = 0;

    // Factory vtables we patched, with their original swapchain creation functions
    struct FactoryOriginals
    {
        CreateSwapChainFn CreateSwapChain = nullptr;
        CreateSwapChainForHwndFn CreateSwapChainForHwnd = nullptr;
    };
    std::unordered_map<void**, FactoryOriginals> PatchedFactories;
    std::mutex FactoryMutex;

    // Per-instance vtable hook of a game swapchain. Never destroyed: restoring the vtable could write into a
    // swapchain the game already released, so a replaced hook is intentionally leaked.
    struct SwapChainHook
    {
        SafetyHookVmt Vmt;
        SafetyHookVm Present;
        SafetyHookVm Present1;
        ID3D12CommandQueue* Queue = nullptr;
    };
    std::unordered_map<void*, SwapChainHook*> SwapChainHooks;
    std::mutex SwapChainMutex;
    std::atomic<IDXGISwapChain*> GameSwapChain = nullptr;

    thread_local bool bInOverlay = false;
    thread_local bool bInPresent = false; // Present may call Present1 internally

    // Renderer
    struct FrameContext
    {
        ID3D12CommandAllocator* Allocator = nullptr;
        ID3D12GraphicsCommandList* CommandList = nullptr;
        UINT64 FenceValue = 0;
    };

    bool bInitialised = false;
    bool bInitFailed = false;
    HWND GameWindow = nullptr;
    WNDPROC OriginalWndProc = nullptr;
    ID3D12Device* Device = nullptr;
    ID3D12CommandQueue* Queue = nullptr;
    ID3D12DescriptorHeap* RtvHeap = nullptr;
    ID3D12DescriptorHeap* SrvHeap = nullptr;
    UINT SrvIncrement = 0;
    std::vector<UINT> SrvFreeList;
    ID3D12Fence* Fence = nullptr;
    HANDLE FenceEvent = nullptr;
    UINT64 FenceLastValue = 0;
    FrameContext Frames[kFramesInFlight];
    UINT FrameIndex = 0;
    DXGI_FORMAT RtvFormat = DXGI_FORMAT_UNKNOWN;

    // ImGui is not thread-safe: the window thread (WndProc) and render thread (Present) both touch it.
    std::recursive_mutex ImGuiMutex;

    bool IsBlockingInput()
    {
        return bMenuOpen && !bInputPassthrough;
    }

    void PatchVTable(void** vtable, size_t index, void* function)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            vtable[index] = function;
            VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
        }
    }

    void WaitForFence(UINT64 value)
    {
        if (value == 0 || Fence->GetCompletedValue() >= value)
            return;
        Fence->SetEventOnCompletion(value, FenceEvent);
        WaitForSingleObject(FenceEvent, INFINITE);
    }

    void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle)
    {
        IM_ASSERT(!SrvFreeList.empty());
        UINT idx = SrvFreeList.back();
        SrvFreeList.pop_back();
        cpuHandle->ptr = SrvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)idx * SrvIncrement;
        gpuHandle->ptr = SrvHeap->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)idx * SrvIncrement;
    }

    void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE)
    {
        SrvFreeList.push_back((UINT)((cpuHandle.ptr - SrvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / SrvIncrement));
    }

    bool InitDX12Backend()
    {
        ImGui_ImplDX12_InitInfo initInfo;
        initInfo.Device = Device;
        initInfo.CommandQueue = Queue;
        initInfo.NumFramesInFlight = kFramesInFlight;
        initInfo.RTVFormat = RtvFormat;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.SrvDescriptorHeap = SrvHeap;
        initInfo.SrvDescriptorAllocFn = SrvAlloc;
        initInfo.SrvDescriptorFreeFn = SrvFree;
        return ImGui_ImplDX12_Init(&initInfo);
    }

    // REFramework's ImGui style (REFramework::set_imgui_style)
    void ApplyStyle(float fScale)
    {
        ImGui::StyleColorsDark();

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.00f;
        style.ChildRounding = 0.00f;
        style.PopupRounding = 0.00f;
        style.FrameRounding = 0.00f;
        style.ScrollbarRounding = 2.00f;
        style.GrabRounding = 0.00f;
        style.TabRounding = 0.00f;
        style.WindowBorderSize = 2.00f;
        style.WindowPadding = ImVec2(2.00f, 0.00f);

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.105f, 0.11f, 1.00f);
        colors[ImGuiCol_NavCursor] = ImVec4(0.30f, 0.305f, 0.31f, 1.00f);
        colors[ImGuiCol_PlotHistogram] = ImVec4(0.30f, 0.305f, 0.31f, 1.00f);
        colors[ImGuiCol_Header] = ImVec4(0.20f, 0.205f, 0.21f, 1.00f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.305f, 0.31f, 1.00f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.55f, 0.5505f, 0.551f, 1.00f);
        colors[ImGuiCol_Button] = ImVec4(0.20f, 0.205f, 0.21f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.305f, 0.31f, 1.00f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.55f, 0.5505f, 0.551f, 1.00f);
        colors[ImGuiCol_CheckMark] = ImVec4(0.55f, 0.5505f, 0.551f, 1.00f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.211f, 0.210f, 0.25f, 1.00f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.305f, 0.31f, 1.00f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.55f, 0.5505f, 0.551f, 1.00f);
        colors[ImGuiCol_Tab] = ImVec4(0.25f, 0.2505f, 0.251f, 1.00f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.38f, 0.3805f, 0.381f, 1.00f);
        colors[ImGuiCol_TabSelected] = ImVec4(0.28f, 0.2805f, 0.281f, 1.00f);
        colors[ImGuiCol_TabDimmed] = ImVec4(0.25f, 0.2505f, 0.251f, 1.00f);
        colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.80f, 0.805f, 0.81f, 1.00f);
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.20f, 0.205f, 0.21f, 0.00f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.30f, 0.305f, 0.31f, 1.00f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(0.55f, 0.5505f, 0.551f, 1.00f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.25f, 0.2505f, 0.251f, 1.00f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.55f, 0.5505f, 0.551f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.25f, 0.2505f, 0.251f, 1.00f);

        // Scale with resolution so it stays readable at 1440p/4K (REFramework scales its font by display height)
        style.ScaleAllSizes(fScale);
        style.FontSizeBase = kFontSize;
        style.FontScaleDpi = fScale;

        ImGuiIO& io = ImGui::GetIO();
        ImFontConfig fontConfig;
        std::snprintf(fontConfig.Name, sizeof(fontConfig.Name), "Roboto Medium");
        if (!io.Fonts->AddFontFromMemoryCompressedBase85TTF(RobotoMedium_compressed_data_base85, kFontSize, &fontConfig))
            io.Fonts->AddFontDefault();
    }

    // Keys that are never blocked so other overlays' hotkeys keep working while the menu is open
    // (OptiScaler: Insert for its menu, PageUp/PageDown/End for its FG/FPS shortcuts).
    bool IsPassthroughKey(UINT vk)
    {
        return vk == VK_INSERT || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_END || (vk >= VK_F1 && vk <= VK_F24);
    }

    // Block mouse movement/presses and key presses, but let releases through so the game doesn't see stuck keys.
    void BlockRawInput(RAWINPUT* raw)
    {
        if (raw->header.dwType == RIM_TYPEMOUSE) {
            RAWMOUSE& mouse = raw->data.mouse;
            if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                mouse.lLastX = 0;
                mouse.lLastY = 0;
            }
            mouse.usButtonFlags &= (RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP);
            mouse.usButtonData = 0;
        }
        else if (raw->header.dwType == RIM_TYPEKEYBOARD) {
            RAWKEYBOARD& keyboard = raw->data.keyboard;
            if (!(keyboard.Flags & RI_KEY_BREAK) && !IsPassthroughKey(keyboard.VKey)) {
                keyboard.MakeCode = 0;
                keyboard.VKey = 0xFF; // Reserved/no key
                keyboard.Message = WM_NULL;
            }
        }
    }

    UINT WINAPI GetRawInputData_Hook(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader)
    {
        UINT result = RealGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
        if (IsBlockingInput() && pData && uiCommand == RID_INPUT && result != (UINT)-1 && result >= sizeof(RAWINPUTHEADER))
            BlockRawInput(static_cast<RAWINPUT*>(pData));
        return result;
    }

    // The game recenters/clips the cursor for camera control, which would make the menu unusable.
    BOOL WINAPI SetCursorPos_Hook(int x, int y)
    {
        if (IsBlockingInput())
            return TRUE;
        return RealSetCursorPos(x, y);
    }

    BOOL WINAPI ClipCursor_Hook(const RECT* rect)
    {
        if (IsBlockingInput())
            return TRUE;
        return RealClipCursor(rect);
    }

    // While the menu is open the cursor is kept inside the game window, otherwise it escapes to other monitors
    // (under Wayland, releasing the game's clip frees the pointer entirely).
    RECT SavedClipRect = {};
    RECT AppliedClipRect = {};
    bool bClipApplied = false;

    void ConfineCursor()
    {
        if (GetForegroundWindow() != GameWindow) {
            bClipApplied = false; // The clip is dropped when focus is lost, reapply once we're back
            return;
        }

        RECT rect = {};
        GetClientRect(GameWindow, &rect);
        MapWindowPoints(GameWindow, nullptr, reinterpret_cast<POINT*>(&rect), 2);
        if (bClipApplied && EqualRect(&rect, &AppliedClipRect))
            return;
        if (RealClipCursor(&rect)) {
            AppliedClipRect = rect;
            bClipApplied = true;
        }
    }

    // Messages that always reach the game, even while the menu wants input (from REFramework::on_message)
    bool IsAlwaysAllowedMessage(UINT msg)
    {
        switch (msg) {
        case WM_DEVICECHANGE: case WM_SHOWWINDOW: case WM_ACTIVATE: case WM_ACTIVATEAPP:
        case WM_CLOSE: case WM_DPICHANGED: case WM_SIZING: case WM_MOUSEACTIVATE:
            return true;
        }
        return false;
    }

    LRESULT CALLBACK WndProc_Hook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (bMenuOpen) {
            bool bBlock = false;
            {
                std::lock_guard lock(ImGuiMutex);
                if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
                    return TRUE;

                // Like REFramework, only swallow input ImGui is actually using (cursor over the menu, typing in a
                // field), so the game and other overlays such as OptiScaler's menu still get the rest. Releases always
                // pass so nothing gets stuck down. Camera movement is blocked separately through raw input.
                const ImGuiIO& io = ImGui::GetIO();
                if (!bInputPassthrough && !IsAlwaysAllowedMessage(msg)) {
                    switch (msg) {
                    case WM_MOUSEMOVE:
                    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
                    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
                    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
                    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
                    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                        bBlock = io.WantCaptureMouse;
                        break;
                    case WM_KEYDOWN: case WM_CHAR:
                        bBlock = io.WantCaptureKeyboard || io.WantTextInput;
                        break;
                    }
                }
            }
            if (bBlock)
                return DefWindowProcW(hWnd, msg, wParam, lParam);
        }
        return CallWindowProcW(OriginalWndProc, hWnd, msg, wParam, lParam);
    }

    bool InitRenderer(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
    {
        DXGI_SWAP_CHAIN_DESC scDesc = {};
        if (FAILED(swapChain->GetDesc(&scDesc)) || FAILED(queue->GetDevice(IID_PPV_ARGS(&Device)))) {
            spdlog::error("Overlay: Failed to get swapchain description or device.");
            bInitFailed = true;
            return false;
        }

        Queue = queue;
        Queue->AddRef();
        GameWindow = scDesc.OutputWindow;
        RtvFormat = scDesc.BufferDesc.Format;

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = 1;
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.NumDescriptors = kSrvHeapSize;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        bool bOk = SUCCEEDED(Device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&RtvHeap)))
            && SUCCEEDED(Device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&SrvHeap)))
            && SUCCEEDED(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
        for (FrameContext& frame : Frames) {
            bOk = bOk
                && SUCCEEDED(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.Allocator)))
                && SUCCEEDED(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.Allocator, nullptr, IID_PPV_ARGS(&frame.CommandList)))
                && SUCCEEDED(frame.CommandList->Close());
        }
        FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!bOk || !FenceEvent) {
            spdlog::error("Overlay: Failed to create D3D12 resources.");
            bInitFailed = true;
            return false;
        }

        SrvIncrement = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (UINT i = kSrvHeapSize; i > 0; i--)
            SrvFreeList.push_back(i - 1);

        std::lock_guard lock(ImGuiMutex);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.MouseDrawCursor = true;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

        RECT clientRect = {};
        GetClientRect(GameWindow, &clientRect);
        float fScale = std::clamp((float)(clientRect.bottom - clientRect.top) / 1080.00f, 1.00f, 3.00f);
        ApplyStyle(fScale);

        if (!ImGui_ImplWin32_Init(GameWindow) || !InitDX12Backend()) {
            spdlog::error("Overlay: Failed to initialise ImGui backends.");
            bInitFailed = true;
            return false;
        }

        OriginalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(GameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc_Hook)));
        if (!OriginalWndProc)
            spdlog::warn("Overlay: Failed to hook window procedure, input will not be blocked.");

        spdlog::info("Overlay: Initialised. Format: {:d}, Scale: {}", (int)RtvFormat, fScale);
        bInitialised = true;
        return true;
    }

    void SetMenuOpen(bool bOpen)
    {
        std::lock_guard lock(ImGuiMutex);
        ImGuiIO& io = ImGui::GetIO();
        io.ClearInputKeys();
        io.ClearInputMouse();
        if (bOpen) {
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
            GetClipCursor(&SavedClipRect);
            bClipApplied = false;
        }
        else {
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            RealClipCursor(&SavedClipRect); // Give the game back its own clip
        }
        bMenuOpen = bOpen;
    }

    void Render(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
    {
        IDXGISwapChain3* swapChain3 = nullptr;
        if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))))
            return;
        ID3D12Resource* backBuffer = nullptr;
        HRESULT hr = swapChain->GetBuffer(swapChain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
        swapChain3->Release();
        if (FAILED(hr))
            return;

        D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();

        // Wait until the GPU is done with this frame slot (ImGui's vertex/index buffers rotate in step with it)
        FrameContext& frame = Frames[FrameIndex++ % kFramesInFlight];
        WaitForFence(frame.FenceValue);

        std::lock_guard lock(ImGuiMutex);

        // Swapchain format changed (e.g. HDR toggled) or the game presents from a different queue (swapchain
        // recreated, frame generation toggled): rebuild the DX12 backend for it.
        if (bbDesc.Format != RtvFormat || queue != Queue) {
            WaitForFence(FenceLastValue);
            if (queue != Queue) {
                // Our resources belong to the original device and can't be used with a new one
                ID3D12Device* queueDevice = nullptr;
                bool bSameDevice = SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) && queueDevice == Device;
                if (queueDevice)
                    queueDevice->Release();
                if (!bSameDevice) {
                    spdlog::error("Overlay: The game switched to a new D3D12 device, overlay disabled.");
                    bInitFailed = true;
                    SetMenuOpen(false);
                    backBuffer->Release();
                    return;
                }
            }
            ImGui_ImplDX12_Shutdown();
            RtvFormat = bbDesc.Format;
            if (queue != Queue) {
                Queue->Release();
                Queue = queue;
                Queue->AddRef();
            }
            if (!InitDX12Backend()) {
                spdlog::error("Overlay: Failed to reinitialise DX12 backend for format {:d}.", (int)RtvFormat);
                bInitFailed = true;
                SetMenuOpen(false);
                backBuffer->Release();
                return;
            }
        }

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();

        ImGuiIO& io = ImGui::GetIO();
        // Back buffer can differ from the client area (custom resolutions)
        if (io.DisplaySize.x > 0.00f && io.DisplaySize.y > 0.00f)
            io.DisplayFramebufferScale = ImVec2((float)bbDesc.Width / io.DisplaySize.x, (float)bbDesc.Height / io.DisplaySize.y);
        // Poll buttons as well in case the game registered raw input without legacy mouse messages
        if (GetForegroundWindow() == GameWindow) {
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        }

        ImGui::NewFrame();
        DrawMenuFn();
        ImGui::Render();

        frame.Allocator->Reset();
        frame.CommandList->Reset(frame.Allocator, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        frame.CommandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvHeap->GetCPUDescriptorHandleForHeapStart();
        Device->CreateRenderTargetView(backBuffer, nullptr, rtv);
        frame.CommandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        frame.CommandList->SetDescriptorHeaps(1, &SrvHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), frame.CommandList);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        frame.CommandList->ResourceBarrier(1, &barrier);
        frame.CommandList->Close();

        ID3D12CommandList* lists[] = { frame.CommandList };
        Queue->ExecuteCommandLists(1, lists);
        Queue->Signal(Fence, ++FenceLastValue);
        frame.FenceValue = FenceLastValue;

        backBuffer->Release();
    }

    void OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, UINT flags)
    {
        if (bInOverlay || bInitFailed || (flags & DXGI_PRESENT_TEST))
            return;
        bInOverlay = true;

        // Toggle with Home. Polled here so it works however the game reads its input.
        bool bHomeDown = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        bool bToggle = bHomeDown && !bHomeHeld && GetForegroundWindow() == GameWindow;
        bHomeHeld = bHomeDown;

        // The renderer is only created on the first Home press, so the overlay costs nothing until it is used
        if (bToggle && (bInitialised || InitRenderer(swapChain, queue)))
            SetMenuOpen(!bMenuOpen);

        if (bInitialised && bMenuOpen) {
            if (IsBlockingInput())
                ConfineCursor();
            Render(swapChain, queue);
        }

        bInOverlay = false;
    }

    SwapChainHook* FindSwapChainHook(void* swapChain)
    {
        std::lock_guard lock(SwapChainMutex);
        auto it = SwapChainHooks.find(swapChain);
        return it != SwapChainHooks.end() ? it->second : nullptr;
    }

    // Only swapchains we vtable-hooked call these, and they are registered before their hook goes live.
    HRESULT STDMETHODCALLTYPE Present_Hook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
    {
        SwapChainHook* hook = FindSwapChainHook(swapChain);
        auto original = hook->Present.original<PresentFn>();
        if (bInPresent)
            return original(swapChain, syncInterval, flags);
        bInPresent = true;
        if (swapChain == GameSwapChain)
            OnPresent(swapChain, hook->Queue, flags);
        HRESULT hr = original(swapChain, syncInterval, flags);
        bInPresent = false;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Present1_Hook(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* presentParameters)
    {
        SwapChainHook* hook = FindSwapChainHook(swapChain);
        auto original = hook->Present1.original<Present1Fn>();
        if (bInPresent)
            return original(swapChain, syncInterval, flags, presentParameters);
        bInPresent = true;
        if (swapChain == GameSwapChain)
            OnPresent(swapChain, hook->Queue, flags);
        HRESULT hr = original(swapChain, syncInterval, flags, presentParameters);
        bInPresent = false;
        return hr;
    }

    // Called for every swapchain created through a factory the game got. Only the game's own (created from ds.exe
    // code) is hooked; e.g. OptiScaler's frame generation swapchains are left alone.
    void CaptureSwapChain(void* caller, IUnknown* device, HWND hWnd, IDXGISwapChain* swapChain)
    {
        const uintptr_t callerAddress = reinterpret_cast<uintptr_t>(caller);
        if (callerAddress < GameBegin || callerAddress >= GameEnd) {
            HMODULE callerModule = nullptr;
            wchar_t callerPath[MAX_PATH] = {};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(caller), &callerModule))
                GetModuleFileNameW(callerModule, callerPath, MAX_PATH);
            spdlog::info("Overlay: Ignoring swapchain 0x{:x} created by {} (not the game).", (uintptr_t)swapChain, std::filesystem::path(callerPath).filename().string());
            return;
        }

        ID3D12CommandQueue* queue = nullptr;
        if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
            spdlog::warn("Overlay: Game swapchain 0x{:x} isn't D3D12, the overlay is unavailable.", (uintptr_t)swapChain);
            return;
        }

        // Held until the hook is registered so Present_Hook always finds it
        std::lock_guard lock(SwapChainMutex);

        auto vmt = safetyhook::VmtHook::create(swapChain);
        if (!vmt) {
            spdlog::error("Overlay: Failed to vtable-hook game swapchain 0x{:x}.", (uintptr_t)swapChain);
            queue->Release();
            return;
        }

        auto* hook = new SwapChainHook{ std::move(*vmt) };
        bool bHooked = true;
        if (auto present = hook->Vmt.hook_method(kPresentIndex, &Present_Hook))
            hook->Present = std::move(*present);
        else
            bHooked = false;
        if (auto present1 = hook->Vmt.hook_method(kPresent1Index, &Present1_Hook))
            hook->Present1 = std::move(*present1);
        else
            bHooked = false;
        if (!bHooked) {
            spdlog::error("Overlay: Failed to hook Present on game swapchain 0x{:x}.", (uintptr_t)swapChain);
            delete hook; // Gives the swapchain its own vtable back
            queue->Release();
            return;
        }

        hook->Queue = queue;
        SwapChainHooks[swapChain] = hook; // A previous hook at this address is leaked on purpose, see SwapChainHook
        GameWindow = hWnd;
        GameSwapChain = swapChain;
        spdlog::info("Overlay: Captured game swapchain 0x{:x} (window 0x{:x}, queue 0x{:x}).", (uintptr_t)swapChain, (uintptr_t)hWnd, (uintptr_t)queue);
    }

    FactoryOriginals GetFactoryOriginals(void* factory)
    {
        std::lock_guard lock(FactoryMutex);
        auto it = PatchedFactories.find(*reinterpret_cast<void***>(factory));
        return it != PatchedFactories.end() ? it->second : FactoryOriginals{};
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChain_Hook(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain)
    {
        void* caller = RETURN_ADDRESS();
        CreateSwapChainFn original = GetFactoryOriginals(factory).CreateSwapChain;
        if (!original)
            return DXGI_ERROR_INVALID_CALL;

        HRESULT hr = original(factory, device, desc, swapChain);
        if (SUCCEEDED(hr) && desc && swapChain && *swapChain)
            CaptureSwapChain(caller, device, desc->OutputWindow, *swapChain);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd_Hook(IDXGIFactory2* factory, IUnknown* device, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain)
    {
        void* caller = RETURN_ADDRESS();
        CreateSwapChainForHwndFn original = GetFactoryOriginals(factory).CreateSwapChainForHwnd;
        if (!original)
            return DXGI_ERROR_INVALID_CALL;

        HRESULT hr = original(factory, device, hWnd, desc, fullscreenDesc, restrictToOutput, swapChain);
        if (SUCCEEDED(hr) && swapChain && *swapChain)
            CaptureSwapChain(caller, device, hWnd, *swapChain);
        return hr;
    }

    // Swaps the swapchain creation slots of a factory the game created (once per vtable, shared by its class).
    void HookFactory(void* factoryObject)
    {
        auto* factory = static_cast<IUnknown*>(factoryObject);

        IDXGIFactory* dxgiFactory = nullptr;
        if (!GameFactory.load() && SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&dxgiFactory)))) {
            IDXGIFactory* expected = nullptr;
            if (!GameFactory.compare_exchange_strong(expected, dxgiFactory))
                dxgiFactory->Release();
        }

        void** vtable = *reinterpret_cast<void***>(factory);
        std::lock_guard lock(FactoryMutex);
        if (PatchedFactories.contains(vtable))
            return;

        FactoryOriginals originals;
        originals.CreateSwapChain = reinterpret_cast<CreateSwapChainFn>(vtable[kCreateSwapChainIndex]);
        // CreateSwapChainForHwnd only exists if the factory implements IDXGIFactory2 through this same vtable
        IDXGIFactory2* factory2 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
            if (*reinterpret_cast<void***>(factory2) == vtable)
                originals.CreateSwapChainForHwnd = reinterpret_cast<CreateSwapChainForHwndFn>(vtable[kCreateSwapChainForHwndIndex]);
            factory2->Release();
        }

        // Registered before the slots are swapped so the hooks always find their originals
        PatchedFactories[vtable] = originals;
        PatchVTable(vtable, kCreateSwapChainIndex, reinterpret_cast<void*>(&CreateSwapChain_Hook));
        if (originals.CreateSwapChainForHwnd)
            PatchVTable(vtable, kCreateSwapChainForHwndIndex, reinterpret_cast<void*>(&CreateSwapChainForHwnd_Hook));
        spdlog::info("Overlay: Hooked DXGI factory vtable 0x{:x}.", (uintptr_t)vtable);
    }

    HRESULT WINAPI CreateDXGIFactory_Hook(REFIID riid, void** factory)
    {
        HRESULT hr = RealCreateDXGIFactory.load()(riid, factory);
        if (SUCCEEDED(hr) && factory && *factory)
            HookFactory(*factory);
        return hr;
    }

    HRESULT WINAPI CreateDXGIFactory1_Hook(REFIID riid, void** factory)
    {
        HRESULT hr = RealCreateDXGIFactory1.load()(riid, factory);
        if (SUCCEEDED(hr) && factory && *factory)
            HookFactory(*factory);
        return hr;
    }

    HRESULT WINAPI CreateDXGIFactory2_Hook(UINT flags, REFIID riid, void** factory)
    {
        HRESULT hr = RealCreateDXGIFactory2.load()(flags, riid, factory);
        if (SUCCEEDED(hr) && factory && *factory)
            HookFactory(*factory);
        return hr;
    }

    // dxgi.dll is delay-loaded by the game, so its factory functions are looked up through this.
    FARPROC WINAPI GetProcAddress_Hook(HMODULE module, LPCSTR name)
    {
        FARPROC proc = RealGetProcAddress(module, name);
        if (!proc || IS_INTRESOURCE(name))
            return proc;

        if (std::strcmp(name, "CreateDXGIFactory2") == 0) {
            RealCreateDXGIFactory2 = reinterpret_cast<CreateDXGIFactory2Fn>(reinterpret_cast<void*>(proc));
            proc = reinterpret_cast<FARPROC>(reinterpret_cast<void*>(&CreateDXGIFactory2_Hook));
        }
        else if (std::strcmp(name, "CreateDXGIFactory1") == 0) {
            RealCreateDXGIFactory1 = reinterpret_cast<CreateDXGIFactoryFn>(reinterpret_cast<void*>(proc));
            proc = reinterpret_cast<FARPROC>(reinterpret_cast<void*>(&CreateDXGIFactory1_Hook));
        }
        else if (std::strcmp(name, "CreateDXGIFactory") == 0) {
            RealCreateDXGIFactory = reinterpret_cast<CreateDXGIFactoryFn>(reinterpret_cast<void*>(proc));
            proc = reinterpret_cast<FARPROC>(reinterpret_cast<void*>(&CreateDXGIFactory_Hook));
        }
        else {
            return proc;
        }

        spdlog::info("Overlay: Intercepted the game's {} lookup.", name);
        return proc;
    }

    void HookGameImport(HMODULE gameModule, const char* dllName, const char* functionName, void* real, void* detour)
    {
        if (real && Memory::HookIAT(gameModule, dllName, real, detour))
            spdlog::info("Overlay: Hooked game import {}!{}", dllName, functionName);
        else
            spdlog::error("Overlay: Failed to hook game import {}!{}", dllName, functionName);
    }
}

namespace Overlay
{
    void Init(std::function<void()> drawMenu)
    {
        DrawMenuFn = std::move(drawMenu);

        HMODULE gameModule = GetModuleHandleW(nullptr);
        auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(gameModule);
        auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<std::uint8_t*>(gameModule) + dosHeader->e_lfanew);
        GameBegin = reinterpret_cast<uintptr_t>(gameModule);
        GameEnd = GameBegin + ntHeaders->OptionalHeader.SizeOfImage;

        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        RealGetProcAddress = reinterpret_cast<GetProcAddressFn>(reinterpret_cast<void*>(GetProcAddress(kernel32, "GetProcAddress")));
        RealGetRawInputData = reinterpret_cast<GetRawInputDataFn>(reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputData")));
        RealSetCursorPos = reinterpret_cast<SetCursorPosFn>(reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos")));
        RealClipCursor = reinterpret_cast<ClipCursorFn>(reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor")));

        // Called before the game's main thread runs, so the game can't have created its DXGI factory or swapchain yet
        HookGameImport(gameModule, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(RealGetProcAddress), reinterpret_cast<void*>(&GetProcAddress_Hook));
        HookGameImport(gameModule, "USER32.dll", "GetRawInputData", reinterpret_cast<void*>(RealGetRawInputData), reinterpret_cast<void*>(&GetRawInputData_Hook));
        HookGameImport(gameModule, "USER32.dll", "SetCursorPos", reinterpret_cast<void*>(RealSetCursorPos), reinterpret_cast<void*>(&SetCursorPos_Hook));
        HookGameImport(gameModule, "USER32.dll", "ClipCursor", reinterpret_cast<void*>(RealClipCursor), reinterpret_cast<void*>(&ClipCursor_Hook));

        spdlog::info("Overlay: Ready. Press Home in-game to open the menu.");
    }

    bool IsOpen()
    {
        return bMenuOpen;
    }

    void Close()
    {
        if (bMenuOpen)
            SetMenuOpen(false);
    }

    void SetInputPassthrough(bool bEnabled)
    {
        if (bInputPassthrough == bEnabled)
            return;
        bInputPassthrough = bEnabled;
        bClipApplied = false; // Re-confine on the next frame when blocking again
    }
}
