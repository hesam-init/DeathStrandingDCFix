#pragma once

#include <windows.h>

// The fix is loaded by the game as winhttp.dll (ds.exe imports WINHTTP.dll) and forwards every winhttp
// export to the real system winhttp.dll.
namespace Proxy
{
    // Loads the system winhttp.dll and points every export stub at it. Called first in DllMain.
    bool Init(HMODULE thisModule);

    // Handle of the system winhttp.dll, or nullptr if it couldn't be loaded.
    HMODULE GetRealModule();
}
