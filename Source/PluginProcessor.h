#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "HostedEditorWindow.h"

namespace altra
{

/** The wrapper plug-in. It hosts one VST3 plug-in and owns that plug-in's editor window.

    Threading: the hosted instance is created, replaced and deleted only on the message thread.
    `audioLock` guards it against processBlock (which only try-locks and outputs silence if it
    can't get the lock); `stateLock` guards it against state calls that may come from another
    thread, and is never taken on the audio thread.
*/
class WrapperProcessor final : public juce::AudioProcessor,
                               private juce::AudioProcessorListener,
                               private juce::AsyncUpdater
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
    bool producesMidi() const override                  { return false; }
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

    /** Shows the hosted editor, building it the first time. The component helps us find the
        host's main window; it can be nullptr. */
    void showHostedEditor (juce::Component* componentInsideHost);
    void hideHostedEditor();
    bool isHostedEditorVisible() const;
    bool hasHostedEditorBeenBuilt() const noexcept      { return editorWindow != nullptr; }

    void setHostedWindowAlwaysOnTop (bool);
    bool isHostedWindowAlwaysOnTop() const noexcept     { return alwaysOnTop; }

    /** Called on the message thread when the plug-in or its window changes. */
    std::function<void()> onStatusChanged;

private:
    //==============================================================================
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged (juce::AudioProcessor*, const juce::AudioProcessorListener::ChangeDetails&) override;
    void handleAsyncUpdate() override;

    static BusesProperties makeBuses();
    void configureBuses (juce::AudioPluginInstance&) const;
    void prepareHosted (juce::AudioPluginInstance&, double sampleRate, int maxBlockSize);
    void destroyEditorWindow();
    void applyPendingState();
    void notifyStatusChanged();
    juce::String getWindowTitle() const;

    //==============================================================================
    juce::AudioPluginFormatManager formatManager;

    std::unique_ptr<juce::AudioPluginInstance> hosted;
    juce::CriticalSection audioLock, stateLock;
    juce::AudioBuffer<float> scratch;

    std::unique_ptr<HostedEditorWindow> editorWindow;
    juce::Rectangle<int> savedWindowBounds;
    bool alwaysOnTop = false;

    std::atomic<double> tailSeconds { 0.0 };
    bool prepared = false;
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;

    // State from setStateInformation that is waiting for the message thread.
    std::unique_ptr<juce::XmlElement> pendingState;
    juce::String trackName;
    juce::CriticalSection pendingLock;
    std::atomic<bool> latencyDirty { false }, titleDirty { false };

    // If the saved plug-in can't be loaded, keep its state so saving the Set doesn't lose it.
    std::unique_ptr<juce::XmlElement> missingPluginState;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WrapperProcessor)
};

} // namespace altra
