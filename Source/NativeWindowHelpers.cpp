#include "NativeWindowHelpers.h"

#if defined (_WIN32)

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstdlib>
#include <vector>

#pragma comment (lib, "comctl32.lib")
#pragma comment (lib, "dwmapi.lib")

namespace altra::native
{

void* getTopLevelWindow (void* window)
{
    auto hwnd = static_cast<HWND> (window);
    return hwnd != nullptr && IsWindow (hwnd) ? GetAncestor (hwnd, GA_ROOT) : nullptr;
}

bool getWindowPosition (void* window, int& x, int& y)
{
    auto hwnd = static_cast<HWND> (window);
    RECT r {};

    if (hwnd == nullptr || ! IsWindow (hwnd) || ! GetWindowRect (hwnd, &r))
        return false;

    x = r.left;
    y = r.top;
    return true;
}

void moveWindowIfOnScreen (void* window, int x, int y)
{
    auto hwnd = static_cast<HWND> (window);

    if (hwnd == nullptr || ! IsWindow (hwnd))
        return;

    // Somewhere in the title bar has to be on a monitor, or the user can't drag it back.
    const POINT titleBar { x + 60, y + 10 };

    if (MonitorFromPoint (titleBar, MONITOR_DEFAULTTONULL) == nullptr)
        return;

    SetWindowPos (hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void* getMonitorOfWindow (void* window)
{
    auto hwnd = static_cast<HWND> (window);
    return hwnd != nullptr && IsWindow (hwnd) ? MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
}

namespace
{
    constexpr const wchar_t* barProperty = L"AltraPlugHost.CollapsedBar";

    // What the user sees of a window. On Windows 10/11, GetWindowRect also includes an invisible
    // resize border, which would leave gaps between bars and at the screen edge.
    RECT getVisibleBounds (HWND hwnd)
    {
        RECT r {};

        if (FAILED (DwmGetWindowAttribute (hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof (r))))
            GetWindowRect (hwnd, &r);

        return r;
    }

    struct BarSearch
    {
        HWND self;
        DWORD process;
        std::vector<RECT> bars;
    };

    BOOL CALLBACK collectBar (HWND hwnd, LPARAM data)
    {
        auto& search = *reinterpret_cast<BarSearch*> (data);
        DWORD process = 0;
        GetWindowThreadProcessId (hwnd, &process);

        if (hwnd != search.self && process == search.process && IsWindowVisible (hwnd)
            && GetPropW (hwnd, barProperty) != nullptr)
            search.bars.push_back (getVisibleBounds (hwnd));

        return TRUE;
    }
}

void keepWindowOnScreen (void* window)
{
    auto hwnd = static_cast<HWND> (window);
    MONITORINFO info {};
    info.cbSize = sizeof (info);

    if (hwnd == nullptr || ! IsWindow (hwnd) || IsIconic (hwnd) || IsZoomed (hwnd)
        || ! GetMonitorInfoW (MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST), &info))
        return;

    RECT outer {};
    GetWindowRect (hwnd, &outer);
    const auto visible = getVisibleBounds (hwnd);
    const auto& work = info.rcWork;

    int dx = 0, dy = 0;

    if (visible.right > work.right)   dx = work.right - visible.right;
    if (visible.left + dx < work.left) dx = work.left - visible.left;
    if (visible.bottom > work.bottom) dy = work.bottom - visible.bottom;
    if (visible.top + dy < work.top)   dy = work.top - visible.top;

    if (dx != 0 || dy != 0)
        SetWindowPos (hwnd, nullptr, outer.left + dx, outer.top + dy, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void placeInRow (void* window, void* monitor, bool atTop)
{
    auto hwnd = static_cast<HWND> (window);
    MONITORINFO info {};
    info.cbSize = sizeof (info);

    if (hwnd == nullptr || ! IsWindow (hwnd) || monitor == nullptr
        || ! GetMonitorInfoW (static_cast<HMONITOR> (monitor), &info))
        return;

    RECT outer {};
    GetWindowRect (hwnd, &outer);
    const auto visible = getVisibleBounds (hwnd);
    const int width = visible.right - visible.left, height = visible.bottom - visible.top;
    const auto& work = info.rcWork;

    if (width <= 0 || height <= 0)
        return;

    BarSearch search { hwnd, GetCurrentProcessId(), {} };
    EnumWindows (collectBar, reinterpret_cast<LPARAM> (&search));

    const int perRow = std::max (1, (int) (work.right - work.left) / width);
    constexpr int tolerance = 8;

    for (int slot = 0;; ++slot)
    {
        const int x = work.left + (slot % perRow) * width;
        const int row = slot / perRow;
        const int y = atTop ? work.top + row * height : work.bottom - (row + 1) * height;

        if (y < work.top || y + height > work.bottom)
            return; // the monitor is full of bars: leave this one where it is

        const auto taken = std::any_of (search.bars.begin(), search.bars.end(), [&] (const RECT& r)
        {
            return std::abs (r.left - x) < tolerance && std::abs (r.top - y) < tolerance;
        });

        if (taken)
            continue;

        SetPropW (hwnd, barProperty, reinterpret_cast<HANDLE> (1));
        SetWindowPos (hwnd, nullptr, x - (visible.left - outer.left), y - (visible.top - outer.top), 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
}

void removeFromRow (void* window)
{
    auto hwnd = static_cast<HWND> (window);

    if (hwnd != nullptr && IsWindow (hwnd))
        RemovePropW (hwnd, barProperty);
}

void beginWindowDrag (void* window)
{
    auto hwnd = static_cast<HWND> (window);

    if (hwnd == nullptr || ! IsWindow (hwnd))
        return;

    ReleaseCapture();
    SendMessageW (hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

void closeWindow (void* window)
{
    auto hwnd = static_cast<HWND> (window);

    if (hwnd != nullptr && IsWindow (hwnd))
        PostMessageW (hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
}

//==============================================================================
struct FramelessWindow::State
{
    HWND hwnd = nullptr;
    LONG_PTR savedStyle = 0;
    int width = 0, height = 0;

    static constexpr LONG_PTR frameStyles = WS_CAPTION | WS_THICKFRAME;
    static constexpr UINT_PTR subclassId = 0x416c7466; // 'Altf'

    static LRESULT CALLBACK proc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR, DWORD_PTR refData)
    {
        auto* state = reinterpret_cast<State*> (refData);

        if (message == WM_STYLECHANGING && (int) wParam == GWL_STYLE)
        {
            // The host putting its title bar back.
            reinterpret_cast<STYLESTRUCT*> (lParam)->styleNew &= ~(DWORD) frameStyles;
        }
        else if (message == WM_WINDOWPOSCHANGING)
        {
            // The host sizing the window for its frame: keep it the size of the bar.
            auto* pos = reinterpret_cast<WINDOWPOS*> (lParam);

            if ((pos->flags & SWP_NOSIZE) == 0)
            {
                pos->cx = state->width;
                pos->cy = state->height;
            }
        }
        else if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass (hwnd, proc, subclassId);
            state->hwnd = nullptr;
        }

        return DefSubclassProc (hwnd, message, wParam, lParam);
    }
};

FramelessWindow::FramelessWindow (void* window)
    : state (std::make_unique<State>())
{
    auto hwnd = static_cast<HWND> (window);

    if (hwnd == nullptr || ! IsWindow (hwnd))
        return;

    RECT client {};
    GetClientRect (hwnd, &client);
    POINT origin { 0, 0 };
    ClientToScreen (hwnd, &origin);

    state->width = client.right - client.left;
    state->height = client.bottom - client.top;
    state->savedStyle = GetWindowLongPtrW (hwnd, GWL_STYLE);

    if (state->width <= 0 || state->height <= 0
        || ! SetWindowSubclass (hwnd, State::proc, State::subclassId, reinterpret_cast<DWORD_PTR> (state.get())))
        return;

    state->hwnd = hwnd;

    // The content stays where it is on screen; the frame around it goes.
    SetWindowLongPtrW (hwnd, GWL_STYLE, state->savedStyle & ~State::frameStyles);
    SetWindowPos (hwnd, nullptr, origin.x, origin.y, state->width, state->height,
                  SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
}

FramelessWindow::~FramelessWindow()
{
    auto hwnd = state->hwnd;

    if (hwnd == nullptr || ! IsWindow (hwnd))
        return;

    RemoveWindowSubclass (hwnd, State::proc, State::subclassId);

    RECT current {};
    GetWindowRect (hwnd, &current);

    // Put the frame back around the content, which stays where it is. The host resizes the
    // window for its content again as soon as our editor changes size.
    RECT framed { 0, 0, state->width, state->height };
    AdjustWindowRectEx (&framed, (DWORD) state->savedStyle, FALSE, (DWORD) GetWindowLongPtrW (hwnd, GWL_EXSTYLE));

    SetWindowLongPtrW (hwnd, GWL_STYLE, state->savedStyle);
    SetWindowPos (hwnd, nullptr, current.left + framed.left, current.top + framed.top,
                  framed.right - framed.left, framed.bottom - framed.top,
                  SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
}

//==============================================================================
struct DestroyWatcher::State
{
    HWND hwnd = nullptr;
    std::function<void()> onDestroy;

    static constexpr UINT_PTR subclassId = 0x416c7472; // 'Altr'

    static LRESULT CALLBACK proc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR, DWORD_PTR refData)
    {
        auto* state = reinterpret_cast<State*> (refData);

        if (message == WM_DESTROY)
        {
            // Copied first: the callback may delete the DestroyWatcher, and with it this state.
            auto callback = state->onDestroy;
            state->onDestroy = nullptr;

            if (callback)
                callback();
        }
        else if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass (hwnd, proc, subclassId);
            state->hwnd = nullptr;
        }

        return DefSubclassProc (hwnd, message, wParam, lParam);
    }
};

DestroyWatcher::DestroyWatcher (void* window, std::function<void()> onDestroy)
    : state (std::make_unique<State>())
{
    auto hwnd = static_cast<HWND> (window);

    if (hwnd == nullptr || ! IsWindow (hwnd))
        return;

    state->onDestroy = std::move (onDestroy);

    if (SetWindowSubclass (hwnd, State::proc, State::subclassId, reinterpret_cast<DWORD_PTR> (state.get())))
        state->hwnd = hwnd;
}

DestroyWatcher::~DestroyWatcher()
{
    // The window may outlive us; it must not call into freed state afterwards.
    if (state->hwnd != nullptr && IsWindow (state->hwnd))
        RemoveWindowSubclass (state->hwnd, State::proc, State::subclassId);
}

void* DestroyWatcher::getWindow() const noexcept
{
    return state->hwnd;
}

//==============================================================================
struct TitleWatcher::State
{
    HWND hwnd = nullptr;
    std::function<std::wstring (const std::wstring&)> makeTitle;
    std::wstring hostTitle;
    bool applying = false;

    static constexpr UINT_PTR subclassId = 0x416c7474; // 'Altt'

    void apply()
    {
        if (hwnd == nullptr || ! makeTitle)
            return;

        const auto title = makeTitle (hostTitle);
        applying = true;
        SetWindowTextW (hwnd, title.c_str());
        applying = false;
    }

    static LRESULT CALLBACK proc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR, DWORD_PTR refData)
    {
        auto* state = reinterpret_cast<State*> (refData);

        if (message == WM_SETTEXT && ! state->applying && state->makeTitle)
        {
            // The host is setting its title: remember it, and show ours in its place.
            state->hostTitle = lParam != 0 ? reinterpret_cast<const wchar_t*> (lParam) : L"";
            const auto title = state->makeTitle (state->hostTitle);
            return DefSubclassProc (hwnd, message, wParam, reinterpret_cast<LPARAM> (title.c_str()));
        }

        if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass (hwnd, proc, subclassId);
            state->hwnd = nullptr;
        }

        return DefSubclassProc (hwnd, message, wParam, lParam);
    }
};

TitleWatcher::TitleWatcher (void* window, std::function<std::wstring (const std::wstring&)> makeTitle)
    : state (std::make_unique<State>())
{
    auto hwnd = static_cast<HWND> (window);

    if (hwnd == nullptr || ! IsWindow (hwnd))
        return;

    wchar_t buffer[512] {};
    GetWindowTextW (hwnd, buffer, (int) std::size (buffer));
    state->hostTitle = buffer;
    state->makeTitle = std::move (makeTitle);

    if (SetWindowSubclass (hwnd, State::proc, State::subclassId, reinterpret_cast<DWORD_PTR> (state.get())))
    {
        state->hwnd = hwnd;
        state->apply();
    }
}

TitleWatcher::~TitleWatcher()
{
    if (state->hwnd != nullptr && IsWindow (state->hwnd))
    {
        RemoveWindowSubclass (state->hwnd, State::proc, State::subclassId);

        // Give the window back the host's own title.
        state->applying = true;
        SetWindowTextW (state->hwnd, state->hostTitle.c_str());
    }
}

void* TitleWatcher::getWindow() const noexcept    { return state->hwnd; }
std::wstring TitleWatcher::getHostTitle() const    { return state->hostTitle; }
void TitleWatcher::reapply()                       { state->apply(); }

} // namespace altra::native

#else

namespace altra::native
{
    void* getTopLevelWindow (void*)          { return nullptr; }
    bool getWindowPosition (void*, int&, int&) { return false; }
    void moveWindowIfOnScreen (void*, int, int) {}
    void* getMonitorOfWindow (void*)         { return nullptr; }
    void placeInRow (void*, void*, bool)     {}
    void keepWindowOnScreen (void*)          {}
    void removeFromRow (void*)               {}
    void beginWindowDrag (void*)             {}
    void closeWindow (void*)                 {}

    struct FramelessWindow::State {};
    FramelessWindow::FramelessWindow (void*) : state (std::make_unique<State>()) {}
    FramelessWindow::~FramelessWindow() = default;

    struct DestroyWatcher::State {};
    DestroyWatcher::DestroyWatcher (void*, std::function<void()>) : state (std::make_unique<State>()) {}
    DestroyWatcher::~DestroyWatcher() = default;
    void* DestroyWatcher::getWindow() const noexcept { return nullptr; }

    struct TitleWatcher::State {};
    TitleWatcher::TitleWatcher (void*, std::function<std::wstring (const std::wstring&)>) : state (std::make_unique<State>()) {}
    TitleWatcher::~TitleWatcher() = default;
    void* TitleWatcher::getWindow() const noexcept { return nullptr; }
    std::wstring TitleWatcher::getHostTitle() const { return {}; }
    void TitleWatcher::reapply() {}
}

#endif
