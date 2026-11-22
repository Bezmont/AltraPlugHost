#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "MidiBus.h"

namespace altra
{

/** AltraPlugHost Receiver: outputs one MIDI channel of one AltraPlugHost instance's hosted
    plug-in, so Live can record each channel of a multi-channel MIDI source on its own track.

    It goes in a MIDI track's instrument slot (Live takes plug-in MIDI output only from there),
    or in a chain of an Instrument Rack so one track can hold several. Another track records it
    with "MIDI From" set to this track and receiver. This track's own "MIDI From" should be the
    server's track: that makes Live process the server first, so the MIDI arrives in the same
    block. The MIDI that comes in that way (all channels merged) is ignored. */
class ReceiverProcessor final : public juce::AudioProcessor,
                                private juce::Timer
{
public:
    ReceiverProcessor();
    ~ReceiverProcessor() override;

    void prepareToPlay (double, int) override       {}
    void releaseResources() override                {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                 { return true; }

    const juce::String getName() const override     { return JucePlugin_Name; }
    bool acceptsMidi() const override               { return true; }
    bool producesMidi() const override              { return true; }
    bool isMidiEffect() const override              { return false; }
    double getTailLengthSeconds() const override    { return 0.0; }

    int getNumPrograms() override                   { return 1; }
    int getCurrentProgram() override                { return 0; }
    void setCurrentProgram (int) override           {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Message thread.
    void subscribe (uint64_t serverId, int channel);
    uint64_t getServerId() const noexcept           { return subscriber.getServerId(); }
    int getChannel() const noexcept                 { return subscriber.getChannel(); }

    /** One line for the editor: connected or not, and a warning if the MIDI arrives late. */
    juce::String getStatus() const                  { return status; }
    bool isStatusAWarning() const noexcept          { return statusIsWarning; }

private:
    void timerCallback() override;
    void updateStatus();

    midibus::Subscriber subscriber;

    // Message thread: what was last logged and shown.
    bool wasConnected = false;
    midibus::Subscriber::Timing timing = midibus::Subscriber::Timing::unknown;
    juce::String status;
    bool statusIsWarning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReceiverProcessor)
};

} // namespace altra
