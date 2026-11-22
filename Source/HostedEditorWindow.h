#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace altra
{

/** The processor's hidden window, where the hosted plug-in's editor is parked while
    AltraPlugHost's window in Live is closed or collapsed. The editor survives Live destroying
    that window because it lives here meanwhile. It is never shown. It never owns the editor:
    the processor does, and moves it between this window and AltraPlugHost's window in Live.
*/
class HostedEditorWindow final : public juce::DocumentWindow
{
public:
    /** Not on the desktop yet: call addToDesktop(). */
    HostedEditorWindow();
    ~HostedEditorWindow() override;

    /** Takes the editor in (without owning it), resizing the window to fit. */
    void hold (juce::AudioProcessorEditor&);

    /** Lets go of the editor so it can be placed somewhere else. Doesn't delete it. */
    void release();

    void closeButtonPressed() override {}

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostedEditorWindow)
};

} // namespace altra
