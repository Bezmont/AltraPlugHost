#include "Receiver.h"
#include "Diagnostics.h"

namespace altra
{

namespace
{
    constexpr auto receiverTag = "AltraPlugHostReceiver";

    juce::String describe (const midibus::ServerInfo& s)       { return s.getDisplayName().quoted(); }

    std::optional<midibus::ServerInfo> findServer (uint64_t id)
    {
        for (const auto& s : midibus::listServers())
            if (s.id == id)
                return s;

        return std::nullopt;
    }

    /** Pick an AltraPlugHost instance and a channel; shows the receiver's status. */
    class ReceiverEditor final : public juce::AudioProcessorEditor,
                                 private juce::Timer
    {
    public:
        explicit ReceiverEditor (ReceiverProcessor& p)
            : AudioProcessorEditor (p), processor (p)
        {
            addAndMakeVisible (serverBox);
            addAndMakeVisible (channelBox);
            addAndMakeVisible (statusLabel);

            serverBox.setTextWhenNothingSelected ("Choose an AltraPlugHost...");
            serverBox.setTextWhenNoChoicesAvailable ("No AltraPlugHost loaded");
            serverBox.setTooltip ("The AltraPlugHost whose plug-in's MIDI this receiver passes on");
            serverBox.onChange = [this] { apply(); };

            for (int ch = 1; ch <= 16; ++ch)
                channelBox.addItem ("Channel " + juce::String (ch), ch);

            channelBox.setSelectedId (processor.getChannel(), juce::dontSendNotification);
            channelBox.setTooltip ("The MIDI channel this receiver passes on");
            channelBox.onChange = [this] { apply(); };

            statusLabel.setJustificationType (juce::Justification::topLeft);
            statusLabel.setFont (juce::FontOptions (13.0f));
            statusLabel.setMinimumHorizontalScale (1.0f);

            refresh();
            setResizable (true, false);
            setResizeLimits (300, 90, 1200, 300);
            setSize (440, 100);
            startTimer (500);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff2b2b2b));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (8);
            auto row = area.removeFromTop (26);
            channelBox.setBounds (row.removeFromRight (110));
            row.removeFromRight (6);
            serverBox.setBounds (row);
            area.removeFromTop (8);
            statusLabel.setBounds (area);
        }

    private:
        void timerCallback() override   { refresh(); }

        void refresh()
        {
            if (! serverBox.isPopupActive())
                refreshServers();

            statusLabel.setText (processor.getStatus(), juce::dontSendNotification);
            statusLabel.setColour (juce::Label::textColourId, processor.isStatusAWarning() ? juce::Colour (0xffffb347)
                                                                                           : juce::Colours::lightgrey);
        }

        void refreshServers()
        {
            const auto servers = midibus::listServers();
            const auto wanted = processor.getServerId();

            juce::StringArray names;
            juce::Array<uint64_t> ids;

            for (const auto& s : servers)
            {
                names.add (s.getDisplayName());
                ids.add (s.id);
            }

            if (wanted != 0 && ! ids.contains (wanted))
            {
                names.add ("(not loaded)");
                ids.add (wanted);
            }

            if (names == shownNames && ids == shownIds)
                return;

            shownNames = names;
            shownIds = ids;
            serverBox.clear (juce::dontSendNotification);

            for (int i = 0; i < names.size(); ++i)
                serverBox.addItem (names[i], i + 1);

            const auto index = shownIds.indexOf (wanted);
            serverBox.setSelectedId (index >= 0 ? index + 1 : 0, juce::dontSendNotification);
        }

        void apply()
        {
            const auto index = serverBox.getSelectedId() - 1;
            const auto id = juce::isPositiveAndBelow (index, shownIds.size()) ? shownIds[index] : processor.getServerId();
            processor.subscribe (id, juce::jmax (1, channelBox.getSelectedId()));
            refresh();
        }

        ReceiverProcessor& processor;
        juce::ComboBox serverBox, channelBox;
        juce::Label statusLabel;
        juce::TooltipWindow tooltipWindow { this, 600 };
        juce::StringArray shownNames;
        juce::Array<uint64_t> shownIds;
    };
}

//==============================================================================
ReceiverProcessor::ReceiverProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    subscriber.subscribe (0, 1); // opens the bus on the message thread
    updateStatus();
    startTimer (500);
}

ReceiverProcessor::~ReceiverProcessor()
{
    stopTimer();
}

bool ReceiverProcessor::isBusesLayoutSupported (const BusesLayout& layout) const
{
    const auto out = layout.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void ReceiverProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    buffer.clear();
    subscriber.read (midi, buffer.getNumSamples(), getSampleRate());
}

juce::AudioProcessorEditor* ReceiverProcessor::createEditor()
{
    return new ReceiverEditor (*this);
}

void ReceiverProcessor::subscribe (uint64_t serverId, int channel)
{
    if (serverId == subscriber.getServerId() && channel == subscriber.getChannel())
        return;

    subscriber.subscribe (serverId, channel);
    timing = midibus::Subscriber::Timing::unknown;
    subscriber.takeTiming(); // forget the old server's timing

    const auto server = findServer (serverId);
    diagnostics::log ("receiver: channel " + juce::String (channel) + " of "
                      + (server.has_value() ? describe (*server) : juce::String::toHexString ((juce::int64) serverId)));
    updateStatus();
}

void ReceiverProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement root (receiverTag);
    root.setAttribute ("version", 1);
    root.setAttribute ("server", juce::String::toHexString ((juce::int64) subscriber.getServerId()));
    root.setAttribute ("channel", subscriber.getChannel());
    copyXmlToBinary (root, destData);
}

void ReceiverProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes); xml != nullptr && xml->hasTagName (receiverTag))
        subscriber.subscribe ((uint64_t) xml->getStringAttribute ("server").getHexValue64(),
                              xml->getIntAttribute ("channel", 1));
}

void ReceiverProcessor::timerCallback()
{
    const auto connected = subscriber.isConnected();

    if (connected != wasConnected)
    {
        wasConnected = connected;
        diagnostics::log (juce::String ("receiver: ") + (connected ? "connected" : "disconnected"));
    }

    // Keep the last known timing while the server isn't processing (no new blocks).
    if (const auto t = subscriber.takeTiming(); t != midibus::Subscriber::Timing::unknown && t != timing)
    {
        timing = t;
        diagnostics::log (juce::String ("receiver: ")
                          + (t == midibus::Subscriber::Timing::sameBlock ? "MIDI arrives in the same block"
                                                                         : "MIDI arrives one block late"));
    }

    updateStatus();
}

void ReceiverProcessor::updateStatus()
{
    const auto id = subscriber.getServerId();
    const auto server = id != 0 ? findServer (id) : std::nullopt;
    const auto channel = juce::String (subscriber.getChannel());

    statusIsWarning = false;

    if (id == 0)
        status = "Choose the AltraPlugHost and the MIDI channel to pass on.";
    else if (! server.has_value())
        status = "Waiting: that AltraPlugHost isn't loaded.";
    else if (! subscriber.isConnected())
        status = "Connecting to " + describe (*server) + "...";
    else if (timing == midibus::Subscriber::Timing::oneBlockLate)
    {
        status = "MIDI arrives one block late. Set this track's MIDI From to "
               + (server->track.isNotEmpty() ? server->track.quoted() : juce::String ("the AltraPlugHost's track")) + ".";
        statusIsWarning = true;
    }
    else
        status = "Passing on channel " + channel + " of " + describe (*server) + ".";
}

} // namespace altra

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new altra::ReceiverProcessor();
}
