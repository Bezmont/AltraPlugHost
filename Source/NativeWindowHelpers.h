#pragma once

#include <functional>
#include <memory>
#include <string>

// Small platform layer for things JUCE doesn't do for us. Handles are passed as void* so this
// header doesn't pull <windows.h> into JUCE code.
namespace altra::native
{
    /** The top-level window containing this native window (the host's plug-in window). */
    void* getTopLevelWindow (void* window);

    /** A top-level window's position on screen, in physical pixels. False if there's no window. */
    bool getWindowPosition (void* window, int& x, int& y);

    /** Moves a top-level window (without resizing or activating it), unless its title bar would
        end up off every monitor, for example after a monitor was unplugged. */
    void moveWindowIfOnScreen (void* window, int x, int y);

    /** Moves a top-level window just enough that all of it is inside the work area of the
        monitor it is mostly on (or as much as fits, top-left first). Doesn't resize it. */
    void keepWindowOnScreen (void* window);

    /** The monitor a window is mostly on (an HMONITOR), or nullptr. */
    void* getMonitorOfWindow (void* window);

    /** Moves a collapsed bar into the first free slot of a row along the bottom (or top) of a
        monitor's work area (inside the taskbar), filling left to right, then the next row up (or
        down). Bars are tagged,
        so instances of both AltraPlugHost DLLs in this process see each other's. A slot counts as
        taken when a tagged window's top-left is there, so dragging a bar away frees its slot.
        Call after the window has its collapsed size. */
    void placeInRow (void* window, void* monitor, bool atTop);

    /** Removes the bar tag (on restore). */
    void removeFromRow (void* window);

    /** Starts moving a top-level window with the mouse, as if its title bar had been pressed.
        Call from a mouse-down. Returns when the user lets go. */
    void beginWindowDrag (void* window);

    /** Closes the host's plug-in window the way its own close button does. */
    void closeWindow (void* window);

    /** While this exists, the host's plug-in window has no title bar or border: it is exactly
        the size its content (our editor) had when this was created. The host's attempts to put
        the frame back or resize the window are overridden. Deleting it puts the frame back.
        Message thread only. */
    class FramelessWindow
    {
    public:
        explicit FramelessWindow (void* window);
        ~FramelessWindow();

        struct State;

    private:
        std::unique_ptr<State> state;
    };

    /** Calls a function when a native window starts being destroyed: on WM_DESTROY, which
        Windows sends before it destroys the window's child windows. That is the last moment a
        child window can be moved somewhere else and survive.

        Stops watching when this object is deleted or the window is gone, whichever comes first.
        Message thread only. */
    class DestroyWatcher
    {
    public:
        DestroyWatcher (void* window, std::function<void()> onDestroy);
        ~DestroyWatcher();

        void* getWindow() const noexcept;

        struct State;

    private:
        std::unique_ptr<State> state;
    };

    /** Lets us choose the title of the host's plug-in window.

        Each time the host sets the title (it does when the track is renamed, for example), we
        keep what it wanted and show `makeTitle (hostTitle)` instead. `reapply()` shows our title
        again after something we put in it changed. Removes itself when deleted or when the window
        is destroyed. Message thread only. */
    class TitleWatcher
    {
    public:
        TitleWatcher (void* window, std::function<std::wstring (const std::wstring& hostTitle)> makeTitle);
        ~TitleWatcher();

        void* getWindow() const noexcept;
        std::wstring getHostTitle() const;
        void reapply();

        struct State;

    private:
        std::unique_ptr<State> state;
    };
}
