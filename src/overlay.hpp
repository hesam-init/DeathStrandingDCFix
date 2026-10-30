#pragma once

#include <functional>

namespace Overlay
{
    // Hooks the game's imports so its DXGI factory and swapchain are captured as the game creates them, then draws
    // drawMenu() each frame while the menu is open. Must be called before the game starts its renderer.
    // The menu is toggled with the Home key. Game input is blocked while it is open.
    void Init(std::function<void()> drawMenu);

    bool IsOpen();

    // Closes the menu (e.g. from its window close button).
    void Close();

    // When enabled, the game keeps receiving mouse/keyboard input while the menu is open.
    void SetInputPassthrough(bool bEnabled);
}
