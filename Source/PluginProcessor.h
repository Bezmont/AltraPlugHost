#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "HostedEditorWindow.h"
#include "ParameterBank.h"
#include "MidiBus.h"

namespace altra
{

/** Where a collapsed bar goes: a row along the bottom or top of the screen, or (off) floating,
    back where the user last left it. */
enum class SnapMode { off, bottom, top };

/** The wrapper plug-in. It hosts one VST3 plug-in and owns that plug-in's editor.

    The hosted editor is built once and then only moved, never rebuilt: docked inside
    AltraPlugHost's own window in Live, or parked in a hidden window of ours while Live's window
    is closed or collapsed. Moving it is cheap because JUCE only reparents the native window the
    plug-in drew into; the plug-in isn't told.

    Threading: the hosted instance is created, replaced and deleted only on the message thread.
    `audioLock` guards it against processBlock (which only try-locks and outputs silence if it
    can't get the lock); `stateLock` guards it against state calls that may come from another
    thread, and is never taken on the audio thread.
*/
class WrapperProcessor final : public juce::AudioProcessor,
                               private juce::AudioProcessorListener,
                               private juce::AsyncUpdater,
                               private juce::Timer
{
public:
    WrapperProcessor();
    ~WrapperProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                     { return true; }

    const juce::String getName() const override         { return JucePlugin_Name; }
    bool acceptsMidi() const override                   { return true; }
    bool producesMidi() const override                  { return true; }
    bool isMidiEffect() const override                  { return false; }
    double getTailLengthSeconds() const override        { return tailSeconds.load(); }

    int getNumPrograms() override                       { return 1; }
    int getCurrentProgram() override                    { return 0; }
    void setCurrentProgram (int) override               {}
    const juce::String getProgramName (int) override    { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    void updateTrackProperties (const TrackProperties&) override;

    //==============================================================================
    // Message thread only.

    /** Lists the plug-ins inside a .vst3 bundle (usually just one). */
    juce::OwnedArray<juce::PluginDescription> findPluginsInFile (const juce::File&);

    /** Loads a plug-in, replacing the current one. Returns an error message, or empty on success. */
    juce::String loadPlugin (const juce::PluginDescription&, const juce::MemoryBlock& stateToRestore = {});
    void unloadPlugin();

    bool isPluginLoaded() const noexcept                { return hosted != nullptr; }
    juce::String getHostedPluginName() const;
    juce::String getMissingPluginName() const;
    int getHostedLatencySamples() const;

    /** How many of the hosted plug-in's parameters the host can automate, and how many didn't fit. */
    int getNumAutomatableParameters() const noexcept    { return parameterBank.getNumMapped(); }
    int getNumParametersNotMapped() const noexcept      { return parameterBank.getNumUnmapped(); }

    /** Builds the hosted editor if it doesn't exist yet. This is the slow part, paid once per
        plug-in instance. Returns false if the plug-in has no editor we can build. */
    /** Builds the hosted editor straight into `slot` (docked) if given, else into the hidden
        window. Straight into the visible slot matters: some plug-ins (JUCE ones, which in Live
        can't report their size before they're attached) settle at their minimum size when
        first attached to a hidden window. */
    bool buildHostedEditor (juce::Component* slot);
    bool hasHostedEditorBeenBuilt() const noexcept      { return hostedEditor != nullptr; }
    bool didHostedEditorFail() const noexcept           { return hostedEditorFailed; }
    juce::AudioProcessorEditor* getHostedEditor() const noexcept { return hostedEditor.get(); }

    /** Moves the (already built) hosted editor into a component of AltraPlugHost's own editor. */
    void dockHostedEditor (juce::Component& slot);

    /** Moves the hosted editor out of AltraPlugHost's editor into our hidden window. Must be
        called before that editor is destroyed. */
    void parkHostedEditor();
    bool isHostedEditorDocked() const noexcept          { return dockSlot != nullptr; }

    /** Collapsed, AltraPlugHost's window is only a small bar and the hosted editor waits in the
        hidden window. Opening the window always expands it again. */
    void setCollapsed (bool);
    bool isCollapsed() const noexcept                   { return collapsed; }

    /** Where Live's window was, expanded and collapsed, so each state returns to its own place.
        Screen position in physical pixels. Saved with the Set. */
    std::optional<juce::Point<int>> expandedPosition, collapsedPosition;

    /** Where the current collapse put the bar: in a row along the bottom or top of the screen,
        or (off) floating, back where the user last left it. Chosen by the collapse button used. */
    SnapMode collapseMode = SnapMode::bottom;

    /** On: the collapsed bar has no title bar (Live's frame is removed while collapsed). */
    bool slimCollapsed = true;

    /** The track we're on, if the host has told us (VST3 hosts may, through IInfoListener). */
    juce::String getTrackName() const;
    std::optional<juce::Colour> getTrackColour() const;

    /** Called on the message thread when the plug-in or its window changes. */
    std::function<void()> onStatusChanged;

private:
    //==============================================================================
    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float newValue) override;
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override;
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index) override;
    void audioProcessorChanged (juce::AudioProcessor*, const juce::AudioProcessorListener::ChangeDetails&) override;
    void handleAsyncUpdate() override;
    void timerCallback() override;   // keeps our names on the MIDI bus current

    static BusesProperties makeBuses();
    void configureBuses (juce::AudioPluginInstance&) const;
    void prepareHosted (juce::AudioPluginInstance&, double sampleRate, int maxBlockSize);
    void destroyHostedEditor();
    void ensureParkingWindow();
    void applyPendingState();
    void notifyStatusChanged();

    //==============================================================================
    juce::AudioPluginFormatManager formatManager;
    ParameterBank parameterBank { *this };
    midibus::Publisher midiPublisher; // the hosted plug-in's MIDI output, for receivers
    juce::String busTrackName, busPluginName; // what the bus shows, to update only on change

    std::unique_ptr<juce::AudioPluginInstance> hosted;
    juce::CriticalSection audioLock, stateLock;
    juce::AudioBuffer<float> scratch;

    std::unique_ptr<juce::AudioProcessorEditor> hostedEditor;
    std::unique_ptr<HostedEditorWindow> editorWindow; // parking place, and the popped-out window
    juce::Component::SafePointer<juce::Component> dockSlot;
    bool hostedEditorFailed = false; // so a plug-in without a usable editor isn't retried endlessly
    bool collapsed = false;

    std::atomic<double> tailSeconds { 0.0 };
    bool prepared = false;
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;

    // State from setStateInformation that is waiting for the message thread.
    std::unique_ptr<juce::XmlElement> pendingState;
    juce::String trackName;
    std::optional<juce::Colour> trackColour;
    juce::CriticalSection pendingLock;
    std::atomic<bool> latencyDirty { false }, trackDirty { false }, parameterNamesDirty { false };

    // If the saved plug-in can't be loaded, keep its state so saving the Set doesn't lose it.
    std::unique_ptr<juce::XmlElement> missingPluginState;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WrapperProcessor)
};

} // namespace altra
