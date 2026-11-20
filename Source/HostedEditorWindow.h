#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace altra
{

/** A top-level window holding the hosted plug-in's editor.

    It is owned by the processor, not by our AudioProcessorEditor, so the host can destroy our
    editor without touching it. Closing it only hides it; the hosted editor is deleted only
    when this window is deleted.
*/
class HostedEditorWindow final : public juce::DocumentWindow
{
public:
    /** Takes ownership of the editor. The window is not on the desktop yet: call addToDesktop(). */
    HostedEditorWindow (const juce::String& title, std::unique_ptr<juce::AudioProcessorEditor> editor);
    ~HostedEditorWindow() override;

    void closeButtonPressed() override;

    /** Makes the host's main window our owner, if we can find it. Safe to call repeatedly. */
    void attachToHostWindow (juce::Component* componentInsideHost);

    std::function<void()> onVisibilityChanged;

private:
    void visibilityChanged() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostedEditorWindow)
};

} // namespace altra
