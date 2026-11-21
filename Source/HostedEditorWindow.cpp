#include "HostedEditorWindow.h"

namespace altra
{

HostedEditorWindow::HostedEditorWindow()
    : DocumentWindow ("AltraPlugHost", juce::Colours::black, 0, false)
{
    // Settings that would recreate the native window are made before addToDesktop().
    setUsingNativeTitleBar (true);
    setResizable (false, false);
}

HostedEditorWindow::~HostedEditorWindow()
{
    release();
}

void HostedEditorWindow::hold (juce::AudioProcessorEditor& editor)
{
    // Changing this recreates the native window, so only when it actually changes.
    if (isResizable() != editor.isResizable())
        setResizable (editor.isResizable(), false);

    setContentNonOwned (&editor, true);
}

void HostedEditorWindow::release()
{
    clearContentComponent(); // non-owned content: removed, not deleted
}

} // namespace altra
