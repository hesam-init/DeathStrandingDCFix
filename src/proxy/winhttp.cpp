#include "winhttp.hpp"

#include <cstdint>
#include <cwchar>

namespace
{
    HMODULE RealModule = nullptr;

    // Exports that the system winhttp.dll doesn't have (Wine implements fewer than Windows) end up here.
    extern "C" DWORD WINAPI ProxyMissing()
    {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return 0;
    }
}

// Each export is a "jmp qword ptr [rip+0]" followed by the target address, so no assembly is needed.
// The .def file exports these stubs under the real winhttp names.
#pragma pack(push, 1)
struct ProxyStub
{
    std::uint8_t Code[6];
    void* Target;
};
#pragma pack(pop)

#if defined(_MSC_VER)
#pragma section(".proxy", read, write, execute)
#define PROXY_STUB_SECTION __declspec(allocate(".proxy"))
#else
#define PROXY_STUB_SECTION __attribute__((section(".proxy")))
#endif

extern "C"
{
#define X(name) PROXY_STUB_SECTION ProxyStub name##_stub = { { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 }, reinterpret_cast<void*>(&ProxyMissing) };
#include "winhttp_exports.inc"
#undef X
}

namespace Proxy
{
    bool Init(HMODULE thisModule)
    {
        wchar_t path[MAX_PATH] = {};
        UINT len = GetSystemDirectoryW(path, MAX_PATH);
        if (len > 0 && len < MAX_PATH - 16) {
            wcscat_s(path, L"\\winhttp.dll");
            RealModule = LoadLibraryW(path);
        }
        // Never forward to ourselves (would loop forever)
        if (RealModule == thisModule)
            RealModule = nullptr;

        auto Resolve = [](ProxyStub& stub, const char* name) {
            DWORD oldProtect = 0;
            VirtualProtect(&stub, sizeof(stub), PAGE_EXECUTE_READWRITE, &oldProtect);
            if (RealModule) {
                if (FARPROC fn = GetProcAddress(RealModule, name))
                    stub.Target = reinterpret_cast<void*>(fn);
            }
        };
#define X(name) Resolve(name##_stub, #name);
#include "winhttp_exports.inc"
#undef X

        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        return RealModule != nullptr;
    }

    HMODULE GetRealModule()
    {
        return RealModule;
    }
}
