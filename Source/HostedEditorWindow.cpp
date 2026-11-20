#include "HostedEditorWindow.h"
#include "NativeWindowHelpers.h"

namespace altra
{

HostedEditorWindow::HostedEditorWindow (const juce::String& title,
                                        std::unique_ptr<juce::AudioProcessorEditor> editor)
    : DocumentWindow (title, juce::Colours::black, DocumentWindow::closeButton | DocumentWindow::minimiseButton,
                      false)
{
    // Everything that would recreate the native window happens before addToDesktop(), so the
    // owner we set afterwards isn't lost.
    setUsingNativeTitleBar (true);
    setResizable (editor->isResizable(), false);
    setContentOwned (editor.release(), true);
}

HostedEditorWindow::~HostedEditorWindow()
{
    // Delete the editor while the window still exists, before the hosted plug-in goes away.
    clearContentComponent();
}

void HostedEditorWindow::closeButtonPressed()
{
    // The whole point of the project: closing hides, it never destroys the editor.
    setVisible (false);
}

void HostedEditorWindow::attachToHostWindow (juce::Component* componentInsideHost)
{
    auto* peer = getPeer();

    if (peer == nullptr)
        return;

    void* hint = nullptr;

    if (componentInsideHost != nullptr)
        if (auto* hostPeer = componentInsideHost->getPeer())
            hint = hostPeer->getNativeHandle();

    const auto ourWindow = peer->getNativeHandle();
    const auto currentOwner = native::getTopLevelOwner (ourWindow);

    // Without a hint we can only guess, so keep an owner we already found.
    if (currentOwner != nullptr && hint == nullptr)
        return;

    if (auto* hostWindow = native::findHostMainWindow (hint, ourWindow))
        if (hostWindow != currentOwner)
            native::setTopLevelOwner (ourWindow, hostWindow);
}

void HostedEditorWindow::visibilityChanged()
{
    DocumentWindow::visibilityChanged();
    juce::NullCheckedInvocation::invoke (onVisibilityChanged);
}

} // namespace altra
