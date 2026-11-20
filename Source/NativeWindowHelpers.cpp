#include "NativeWindowHelpers.h"

#if defined (_WIN32)

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace altra::native
{

namespace
{
    struct SearchContext
    {
        DWORD processId = 0;
        HWND ignore = nullptr;
        HWND exclude = nullptr;
        HWND best = nullptr;
        long long bestArea = 0;
    };

    BOOL CALLBACK considerWindow (HWND hwnd, LPARAM param)
    {
        auto& ctx = *reinterpret_cast<SearchContext*> (param);

        if (hwnd == ctx.ignore || hwnd == ctx.exclude || ! IsWindowVisible (hwnd))
            return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId (hwnd, &pid);

        if (pid != ctx.processId)
            return TRUE;

        // The main window has no owner and isn't a tool window.
        if (GetWindow (hwnd, GW_OWNER) != nullptr
            || (GetWindowLongPtr (hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0)
            return TRUE;

        RECT r {};
        GetWindowRect (hwnd, &r);
        const auto area = (long long) (r.right - r.left) * (long long) (r.bottom - r.top);

        if (area > ctx.bestArea)
        {
            ctx.best = hwnd;
            ctx.bestArea = area;
        }

        return TRUE;
    }
}

void* findHostMainWindow (void* hintWindow, void* windowToIgnore)
{
    auto hint = static_cast<HWND> (hintWindow);
    HWND pluginWindow = nullptr;

    if (hint != nullptr && IsWindow (hint))
    {
        pluginWindow = GetAncestor (hint, GA_ROOT);              // the host's plug-in window
        const auto rootOwner = GetAncestor (hint, GA_ROOTOWNER); // what that window belongs to

        // If the plug-in window has an owner, that owner is the main window.
        if (rootOwner != nullptr && rootOwner != pluginWindow)
            return rootOwner;
    }

    // Otherwise take the largest visible, unowned top-level window in this process,
    // which is the host's main window in practice.
    SearchContext ctx;
    ctx.processId = GetCurrentProcessId();
    ctx.ignore = static_cast<HWND> (windowToIgnore);
    ctx.exclude = pluginWindow;
    EnumWindows (considerWindow, reinterpret_cast<LPARAM> (&ctx));
    return ctx.best;
}

void setTopLevelOwner (void* window, void* ownerWindow)
{
    auto hwnd = static_cast<HWND> (window);

    if (hwnd == nullptr || ! IsWindow (hwnd))
        return;

    // GWLP_HWNDPARENT sets the owner of a top-level window (it doesn't make it a child window).
    SetWindowLongPtr (hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR> (ownerWindow));

    // JUCE gives windows that appear on the taskbar WS_EX_APPWINDOW, which would keep an owned
    // window on the taskbar. Owned windows are reached through their owner, so remove it.
    auto exStyle = GetWindowLongPtr (hwnd, GWL_EXSTYLE);

    if (ownerWindow != nullptr)
        exStyle &= ~(LONG_PTR) WS_EX_APPWINDOW;

    SetWindowLongPtr (hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos (hwnd, nullptr, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void* getTopLevelOwner (void* window)
{
    auto hwnd = static_cast<HWND> (window);
    return hwnd != nullptr && IsWindow (hwnd) ? GetWindow (hwnd, GW_OWNER) : nullptr;
}

} // namespace altra::native

#else

namespace altra::native
{
    void* findHostMainWindow (void*, void*)  { return nullptr; }
    void setTopLevelOwner (void*, void*)     {}
    void* getTopLevelOwner (void*)           { return nullptr; }
}

#endif
