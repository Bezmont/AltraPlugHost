#pragma once

// Small platform layer for things JUCE doesn't do for us. Handles are passed as void* so this
// header doesn't pull <windows.h> into JUCE code.
namespace altra::native
{
    /** Finds the host's main window: the window our hosted editor window should be owned by.

        hintWindow is any native window inside the host (for example our own editor's peer), or
        nullptr. We never return the host's plug-in window, because the host destroys it when the
        user closes it, and Windows destroys owned windows along with their owner.
    */
    void* findHostMainWindow (void* hintWindow, void* windowToIgnore);

    /** Makes ownerWindow the owner of a top-level window: it then stays above the owner,
        minimises with it and is left off the taskbar. Passing nullptr clears the owner.
    */
    void setTopLevelOwner (void* window, void* ownerWindow);

    void* getTopLevelOwner (void* window);
}
