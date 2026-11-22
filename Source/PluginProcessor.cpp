#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Diagnostics.h"

namespace altra
{

namespace
{
    constexpr auto stateTag       = "AltraPlugHost";
    constexpr auto hostedStateTag = "HostedState";
    constexpr auto windowTag      = "Window";

    std::atomic<int> liveInstances { 0 };
}

juce::AudioProcessor::BusesProperties WrapperProcessor::makeBuses()
{
       #if ALTRA_IS_INSTRUMENT
        return BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true);
       #else
        return BusesProperties().withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
       #endif
}

//==============================================================================
WrapperProcessor::WrapperProcessor()
    : AudioProcessor (makeBuses())
{
    juce::addDefaultFormatsToManager (formatManager);
    diagnostics::log ("instance created (" + juce::String (++liveInstances) + " alive)");
    startTimer (1000);
}

WrapperProcessor::~WrapperProcessor()
{
    const diagnostics::ScopedStep step ("instance deleting, hosting " + getHostedPluginName().quoted());

    cancelPendingUpdate();
    stopTimer();
    onStatusChanged = nullptr;

    // The editor must go before the plug-in it belongs to.
    destroyHostedEditor();

    if (hosted != nullptr)
    {
        hosted->removeListener (this);
        parameterBank.attach (nullptr);

        const diagnostics::ScopedStep deleting ("deleting hosted " + hosted->getName().quoted());
        hosted.reset();
    }

    diagnostics::log ("instance deleted (" + juce::String (--liveInstances) + " alive)");
}

//==============================================================================
bool WrapperProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

   #if ALTRA_IS_INSTRUMENT
    return layouts.getMainInputChannelSet().isDisabled();
   #else
    return layouts.getMainInputChannelSet() == out;
   #endif
}

void WrapperProcessor::configureBuses (juce::AudioPluginInstance& instance) const
{
    // Match the hosted plug-in's main buses to ours. Extra buses (sidechains, multi-outs) keep
    // their defaults and get silence or are dropped for now.
    auto layout = instance.getBusesLayout();

    if (! layout.inputBuses.isEmpty() && getBusCount (true) > 0)
        layout.inputBuses.getReference (0) = getChannelLayoutOfBus (true, 0);

    if (! layout.outputBuses.isEmpty() && getBusCount (false) > 0)
        layout.outputBuses.getReference (0) = getChannelLayoutOfBus (false, 0);

    if (! instance.setBusesLayout (layout))
    {
        DBG ("Hosted plug-in refused our bus layout; using its default layout.");
    }
}

void WrapperProcessor::prepareHosted (juce::AudioPluginInstance& instance, double sampleRate, int maxBlockSize)
{
    instance.setRateAndBufferSizeDetails (sampleRate, maxBlockSize);
    instance.setNonRealtime (isNonRealtime());
    instance.prepareToPlay (sampleRate, maxBlockSize);

    const auto channels = std::max ({ instance.getTotalNumInputChannels(), instance.getTotalNumOutputChannels(),
                                      getTotalNumInputChannels(), getTotalNumOutputChannels(), 2 });
    scratch.setSize (channels, maxBlockSize, false, true, false);
}

void WrapperProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    const juce::ScopedLock sl (audioLock);

    currentSampleRate = sampleRate;
    currentBlockSize = maximumExpectedSamplesPerBlock;
    prepared = true;

    if (hosted != nullptr)
    {
        prepareHosted (*hosted, sampleRate, maximumExpectedSamplesPerBlock);
        setLatencySamples (hosted->getLatencySamples());
    }
}

void WrapperProcessor::releaseResources()
{
    const juce::ScopedLock sl (audioLock);
    prepared = false;

    if (hosted != nullptr)
        hosted->releaseResources();
}

void WrapperProcessor::reset()
{
    const juce::ScopedLock sl (audioLock);

    if (hosted != nullptr)
        hosted->reset();
}

void WrapperProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midiPublisher.beginBlock();
    const juce::ScopedTryLock tl (audioLock);

    const auto numSamples = buffer.getNumSamples();
    const auto ourOuts = getTotalNumOutputChannels();

    if (! tl.isLocked() || hosted == nullptr || ! prepared)
    {
        // The plug-in is being swapped (silence for a block), or nothing is loaded
        // (effects pass audio through, instruments are silent).
       #if ALTRA_IS_INSTRUMENT
        buffer.clear();
       #else
        if (! tl.isLocked())
            buffer.clear();
       #endif
        midi.clear();
        midiPublisher.publish (midi, numSamples);
        return;
    }

    auto& plugin = *hosted;
    const auto innerIns  = plugin.getTotalNumInputChannels();
    const auto innerOuts = plugin.getTotalNumOutputChannels();
    const auto innerChannels = std::max (innerIns, innerOuts);

    if (scratch.getNumChannels() < innerChannels || scratch.getNumSamples() < numSamples)
        scratch.setSize (std::max (innerChannels, scratch.getNumChannels()),
                         std::max (numSamples, scratch.getNumSamples()), false, true, true); // shouldn't happen

    juce::AudioBuffer<float> work (scratch.getArrayOfWritePointers(), innerChannels, numSamples);
    work.clear();

    for (int ch = 0; ch < std::min (innerIns, getTotalNumInputChannels()); ++ch)
        work.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    plugin.setPlayHead (getPlayHead());
    plugin.processBlock (work, midi);
    midiPublisher.publish (midi, numSamples); // midi now holds the plug-in's output

    for (int ch = 0; ch < ourOuts; ++ch)
    {
        if (ch < innerOuts)
            buffer.copyFrom (ch, 0, work, ch, 0, numSamples);
        else if (innerOuts == 1)
            buffer.copyFrom (ch, 0, work, 0, 0, numSamples); // mono plug-in on a stereo track
        else
            buffer.clear (ch, 0, numSamples);
    }
}

//==============================================================================
juce::OwnedArray<juce::PluginDescription> WrapperProcessor::findPluginsInFile (const juce::File& file)
{
    JUCE_ASSERT_MESSAGE_THREAD
    juce::OwnedArray<juce::PluginDescription> results;

    for (auto* format : formatManager.getFormats())
        if (format->fileMightContainThisPluginType (file.getFullPathName()))
            format->findAllTypesForFile (results, file.getFullPathName());

    return results;
}

juce::String WrapperProcessor::loadPlugin (const juce::PluginDescription& description,
                                           const juce::MemoryBlock& stateToRestore)
{
    JUCE_ASSERT_MESSAGE_THREAD

    double sampleRate;
    int maxBlockSize;
    bool wasPrepared;

    {
        const juce::ScopedLock sl (audioLock);
        sampleRate = currentSampleRate;
        maxBlockSize = currentBlockSize;
        wasPrepared = prepared;
    }

    juce::String error;
    std::unique_ptr<juce::AudioPluginInstance> instance;

    {
        const diagnostics::ScopedStep step ("loading " + description.name.quoted());
        instance = formatManager.createPluginInstance (description, sampleRate, maxBlockSize, error);
    }

    if (instance == nullptr)
        return error.isNotEmpty() ? error : "Couldn't load " + description.name;

    configureBuses (*instance);

    if (! stateToRestore.isEmpty())
        instance->setStateInformation (stateToRestore.getData(), (int) stateToRestore.getSize());

    if (wasPrepared)
        prepareHosted (*instance, sampleRate, maxBlockSize);

    // The old editor must go before the old plug-in.
    destroyHostedEditor();

    std::unique_ptr<juce::AudioPluginInstance> old;

    {
        const juce::ScopedLock sl1 (stateLock);
        const juce::ScopedLock sl2 (audioLock);

        // If the host re-prepared us while the plug-in was loading, catch up.
        if (prepared && (! wasPrepared || sampleRate != currentSampleRate || maxBlockSize != currentBlockSize))
            prepareHosted (*instance, currentSampleRate, currentBlockSize);

        old = std::exchange (hosted, std::move (instance));
        missingPluginState.reset();
    }

    hosted->addListener (this);
    parameterBank.attach (hosted.get()); // before the old plug-in goes: the slots may point into it
    setLatencySamples (hosted->getLatencySamples());
    tailSeconds = hosted->getTailLengthSeconds();

    if (old != nullptr)
    {
        const diagnostics::ScopedStep step ("deleting hosted " + old->getName().quoted());
        old->removeListener (this);
        old->releaseResources();
        old.reset();
    }

    notifyStatusChanged();
    return {};
}

void WrapperProcessor::unloadPlugin()
{
    JUCE_ASSERT_MESSAGE_THREAD

    destroyHostedEditor();

    std::unique_ptr<juce::AudioPluginInstance> old;

    {
        const juce::ScopedLock sl1 (stateLock);
        const juce::ScopedLock sl2 (audioLock);
        old = std::move (hosted);
        missingPluginState.reset();
    }

    parameterBank.attach (nullptr);

    if (old != nullptr)
    {
        const diagnostics::ScopedStep step ("deleting hosted " + old->getName().quoted());
        old->removeListener (this);
        old->releaseResources();
        old.reset();
    }

    setLatencySamples (0);
    tailSeconds = 0.0;
    notifyStatusChanged();
}

juce::String WrapperProcessor::getHostedPluginName() const
{
    return hosted != nullptr ? hosted->getName() : juce::String();
}

juce::String WrapperProcessor::getMissingPluginName() const
{
    if (missingPluginState != nullptr)
        if (auto* plugin = missingPluginState->getChildByName ("PLUGIN"))
            return plugin->getStringAttribute ("name");

    return {};
}

int WrapperProcessor::getHostedLatencySamples() const
{
    return hosted != nullptr ? hosted->getLatencySamples() : 0;
}

void WrapperProcessor::audioProcessorChanged (juce::AudioProcessor* processor, const juce::AudioProcessorListener::ChangeDetails& details)
{
    // May arrive on any thread, including the audio thread: hand it to the message thread.
    if (details.latencyChanged && processor == hosted.get())
    {
        latencyDirty = true;
        triggerAsyncUpdate();
    }

    if (details.parameterInfoChanged && processor == hosted.get())
    {
        parameterNamesDirty = true;
        triggerAsyncUpdate();
    }
}

// The hosted plug-in's parameters, passed on to the host through the parameter bank. Any thread.
void WrapperProcessor::audioProcessorParameterChanged (juce::AudioProcessor* processor, int index, float newValue)
{
    parameterBank.hostedValueChanged (processor, index, newValue);
}

void WrapperProcessor::audioProcessorParameterChangeGestureBegin (juce::AudioProcessor* processor, int index)
{
    parameterBank.hostedGesture (processor, index, true);
}

void WrapperProcessor::audioProcessorParameterChangeGestureEnd (juce::AudioProcessor* processor, int index)
{
    parameterBank.hostedGesture (processor, index, false);
}

//==============================================================================
void WrapperProcessor::ensureParkingWindow()
{
    if (editorWindow != nullptr)
        return;

    editorWindow = std::make_unique<HostedEditorWindow>();
    editorWindow->addToDesktop();   // hidden: a parked editor still needs a native window to live in
}

bool WrapperProcessor::buildHostedEditor (juce::Component* slot)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (hostedEditor != nullptr)
        return true;

    if (hosted == nullptr)
        return false;

    // This is the expensive part, paid once per plug-in instance.
    const diagnostics::ScopedStep step ("opening editor of " + hosted->getName().quoted());
    hostedEditor.reset (hosted->hasEditor() ? hosted->createEditorAndMakeActive()
                                            : new juce::GenericAudioProcessorEditor (*hosted));

    if (hostedEditor == nullptr)
    {
        hostedEditorFailed = true;
        return false;
    }

    ensureParkingWindow();

    if (slot != nullptr)
    {
        slot->addAndMakeVisible (*hostedEditor);
        dockSlot = slot;
    }
    else
    {
        editorWindow->hold (*hostedEditor);
    }

    return true;
}

void WrapperProcessor::dockHostedEditor (juce::Component& slot)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (hostedEditor == nullptr || dockSlot == &slot)
        return;

    editorWindow->release();
    slot.addAndMakeVisible (*hostedEditor); // only reparents the plug-in's native window
    dockSlot = &slot;
}

void WrapperProcessor::parkHostedEditor()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (hostedEditor == nullptr || dockSlot == nullptr)
        return;

    dockSlot->removeChildComponent (hostedEditor.get());
    dockSlot = nullptr;
    editorWindow->hold (*hostedEditor);
}

void WrapperProcessor::setCollapsed (bool shouldBeCollapsed)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (collapsed == shouldBeCollapsed)
        return;

    collapsed = shouldBeCollapsed;
    notifyStatusChanged();
}

void WrapperProcessor::destroyHostedEditor()
{
    if (dockSlot != nullptr && hostedEditor != nullptr)
        dockSlot->removeChildComponent (hostedEditor.get());

    dockSlot = nullptr;

    if (editorWindow != nullptr)
        editorWindow->release();

    // The editor goes before the plug-in it belongs to, and before the window it was parked in.
    if (hostedEditor != nullptr)
    {
        const diagnostics::ScopedStep step ("deleting hosted editor");
        hostedEditor.reset();
    }

    editorWindow.reset();
    hostedEditorFailed = false;
}

juce::String WrapperProcessor::getTrackName() const
{
    const juce::ScopedLock sl (pendingLock);
    return trackName;
}

std::optional<juce::Colour> WrapperProcessor::getTrackColour() const
{
    const juce::ScopedLock sl (pendingLock);
    return trackColour;
}

void WrapperProcessor::updateTrackProperties (const TrackProperties& properties)
{
    // May come from any thread.
    {
        const juce::ScopedLock sl (pendingLock);
        // Live sends the name and the colour in separate calls, so only change what's given.
        if (properties.name.has_value())
            trackName = *properties.name;

        if (properties.colourARGB.has_value())
            trackColour = juce::Colour (*properties.colourARGB);
    }

    juce::String message ("track properties: name ");
    message << (properties.name.has_value() ? properties.name->quoted() : juce::String ("(none)"));

    if (properties.colourARGB.has_value())
        message << ", colour " << juce::Colour (*properties.colourARGB).toDisplayString (false);

    diagnostics::log (message);
    trackDirty = true;
    triggerAsyncUpdate();
}

//==============================================================================
void WrapperProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const juce::ScopedLock sl (stateLock);

    juce::XmlElement root (stateTag);
    root.setAttribute ("version", 1);
    root.setAttribute ("busId", juce::String::toHexString ((juce::int64) midiPublisher.getId()));

    if (hosted != nullptr)
    {
        root.addChildElement (hosted->getPluginDescription().createXml().release());

        juce::MemoryBlock hostedState;
        hosted->getStateInformation (hostedState);
        root.createNewChildElement (hostedStateTag)->addTextElement (hostedState.toBase64Encoding());
    }
    else if (missingPluginState != nullptr)
    {
        // Pass the unloadable plug-in's state through untouched.
        for (auto* child : missingPluginState->getChildIterator())
            if (! child->hasTagName (windowTag))
                root.addChildElement (new juce::XmlElement (*child));
    }

    auto* window = root.createNewChildElement (windowTag);
    window->setAttribute ("collapsed", collapsed);
    window->setAttribute ("slim", slimCollapsed);

    const auto savePosition = [window] (const char* prefix, const std::optional<juce::Point<int>>& position)
    {
        if (position.has_value())
        {
            window->setAttribute (juce::String (prefix) + "X", position->x);
            window->setAttribute (juce::String (prefix) + "Y", position->y);
        }
    };

    savePosition ("expanded", expandedPosition);
    savePosition ("collapsed", collapsedPosition);

    copyXmlToBinary (root, destData);
}

void WrapperProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (stateTag))
        return;

    {
        const juce::ScopedLock sl (pendingLock);
        pendingState = std::move (xml);
    }

    // Plug-ins must be created on the message thread.
    if (juce::MessageManager::existsAndIsCurrentThread())
        applyPendingState();
    else
        triggerAsyncUpdate();
}

void WrapperProcessor::applyPendingState()
{
    JUCE_ASSERT_MESSAGE_THREAD

    std::unique_ptr<juce::XmlElement> xml;

    {
        const juce::ScopedLock sl (pendingLock);
        xml = std::move (pendingState);
    }

    if (xml == nullptr)
        return;

    if (xml->hasAttribute ("busId"))
        midiPublisher.setId ((uint64_t) xml->getStringAttribute ("busId").getHexValue64());

    // Older Sets also stored the popped-out window's position here; it's ignored now.
    if (auto* window = xml->getChildByName (windowTag))
    {
        collapsed = window->getBoolAttribute ("collapsed");
        // Older builds saved a snap setting per instance ("snap", "snapMode"); it's global now.
        slimCollapsed = window->getBoolAttribute ("slim", true);

        const auto loadPosition = [window] (const char* prefix) -> std::optional<juce::Point<int>>
        {
            const auto x = juce::String (prefix) + "X", y = juce::String (prefix) + "Y";

            if (! window->hasAttribute (x) || ! window->hasAttribute (y))
                return std::nullopt;

            return juce::Point<int> (window->getIntAttribute (x), window->getIntAttribute (y));
        };

        expandedPosition = loadPosition ("expanded");
        collapsedPosition = loadPosition ("collapsed");
    }

    auto* pluginXml = xml->getChildByName ("PLUGIN");
    juce::PluginDescription description;

    if (pluginXml == nullptr || ! description.loadFromXml (*pluginXml))
    {
        unloadPlugin();
        return;
    }

    juce::MemoryBlock hostedState;
    hostedState.fromBase64Encoding (xml->getChildElementAllSubText (hostedStateTag, {}));

    // Same plug-in already loaded: restore its state in place and keep its editor.
    if (hosted != nullptr && hosted->getPluginDescription().isDuplicateOf (description))
    {
        if (! hostedState.isEmpty())
            hosted->setStateInformation (hostedState.getData(), (int) hostedState.getSize());

        notifyStatusChanged();
        return;
    }

    const auto error = loadPlugin (description, hostedState);

    if (error.isNotEmpty())
    {
        DBG ("Couldn't restore hosted plug-in: " << error);
        unloadPlugin();
        missingPluginState = std::move (xml);
        notifyStatusChanged();
    }
}

void WrapperProcessor::handleAsyncUpdate()
{
    applyPendingState();

    if (latencyDirty.exchange (false) && hosted != nullptr)
    {
        setLatencySamples (hosted->getLatencySamples());
        tailSeconds = hosted->getTailLengthSeconds();
        notifyStatusChanged();
    }

    if (parameterNamesDirty.exchange (false))
        parameterBank.refreshNames();

    if (trackDirty.exchange (false))
        notifyStatusChanged();
}

void WrapperProcessor::timerCallback()
{
    const auto track = getTrackName();
    const auto plugin = isPluginLoaded() ? getHostedPluginName() : juce::String();

    if (track != busTrackName || plugin != busPluginName)
    {
        busTrackName = track;
        busPluginName = plugin;
        midiPublisher.setNames (track, plugin);
    }
}

void WrapperProcessor::notifyStatusChanged()
{
    juce::NullCheckedInvocation::invoke (onStatusChanged);
}

juce::AudioProcessorEditor* WrapperProcessor::createEditor()
{
    return new WrapperEditor (*this);
}

} // namespace altra

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new altra::WrapperProcessor();
}
