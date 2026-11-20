#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace altra
{

namespace
{
    constexpr auto stateTag       = "AltraPlugHost";
    constexpr auto hostedStateTag = "HostedState";
    constexpr auto windowTag      = "Window";
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
}

WrapperProcessor::~WrapperProcessor()
{
    cancelPendingUpdate();
    onStatusChanged = nullptr;

    // The editor must go before the plug-in it belongs to.
    destroyEditorWindow();

    if (hosted != nullptr)
    {
        hosted->removeListener (this);
        hosted.reset();
    }
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
    auto instance = formatManager.createPluginInstance (description, sampleRate, maxBlockSize, error);

    if (instance == nullptr)
        return error.isNotEmpty() ? error : "Couldn't load " + description.name;

    configureBuses (*instance);

    if (! stateToRestore.isEmpty())
        instance->setStateInformation (stateToRestore.getData(), (int) stateToRestore.getSize());

    if (wasPrepared)
        prepareHosted (*instance, sampleRate, maxBlockSize);

    // The old editor must go before the old plug-in.
    destroyEditorWindow();
    savedWindowBounds = {};

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
    setLatencySamples (hosted->getLatencySamples());
    tailSeconds = hosted->getTailLengthSeconds();

    if (old != nullptr)
    {
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

    destroyEditorWindow();
    savedWindowBounds = {};

    std::unique_ptr<juce::AudioPluginInstance> old;

    {
        const juce::ScopedLock sl1 (stateLock);
        const juce::ScopedLock sl2 (audioLock);
        old = std::move (hosted);
        missingPluginState.reset();
    }

    if (old != nullptr)
    {
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
}

//==============================================================================
void WrapperProcessor::showHostedEditor (juce::Component* componentInsideHost)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (hosted == nullptr)
        return;

    if (editorWindow == nullptr)
    {
        // This is the expensive part, paid once per plug-in instance.
        std::unique_ptr<juce::AudioProcessorEditor> editor (hosted->hasEditor()
                                                                ? hosted->createEditorAndMakeActive()
                                                                : new juce::GenericAudioProcessorEditor (*hosted));
        if (editor == nullptr)
            return;

        editorWindow = std::make_unique<HostedEditorWindow> (getWindowTitle(), std::move (editor));
        editorWindow->setAlwaysOnTop (alwaysOnTop);

        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* display = displays.getDisplayForPoint (savedWindowBounds.getPosition().toFloat());

        if (! savedWindowBounds.isEmpty() && display != nullptr
            && display->userBounds.toNearestInt().contains (savedWindowBounds.getPosition()))
            editorWindow->setTopLeftPosition (savedWindowBounds.getPosition());
        else
            editorWindow->centreAroundComponent (componentInsideHost, editorWindow->getWidth(), editorWindow->getHeight());

        editorWindow->addToDesktop();

        editorWindow->onVisibilityChanged = [this]
        {
            if (editorWindow != nullptr && ! editorWindow->isVisible())
                savedWindowBounds = editorWindow->getBounds();

            notifyStatusChanged();
        };
    }

    editorWindow->attachToHostWindow (componentInsideHost);
    editorWindow->setVisible (true);
    editorWindow->toFront (true);
    notifyStatusChanged();
}

void WrapperProcessor::hideHostedEditor()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (editorWindow != nullptr)
        editorWindow->setVisible (false);
}

bool WrapperProcessor::isHostedEditorVisible() const
{
    return editorWindow != nullptr && editorWindow->isVisible();
}

void WrapperProcessor::setHostedWindowAlwaysOnTop (bool shouldBeOnTop)
{
    alwaysOnTop = shouldBeOnTop;

    if (editorWindow != nullptr)
        editorWindow->setAlwaysOnTop (shouldBeOnTop);
}

void WrapperProcessor::destroyEditorWindow()
{
    if (editorWindow == nullptr)
        return;

    savedWindowBounds = editorWindow->getBounds();
    editorWindow->onVisibilityChanged = nullptr;
    editorWindow.reset();
}

juce::String WrapperProcessor::getWindowTitle() const
{
    auto title = getHostedPluginName();

    const juce::ScopedLock sl (pendingLock);

    if (trackName.isNotEmpty())
        title << " - " << trackName;

    return title;
}

void WrapperProcessor::updateTrackProperties (const TrackProperties& properties)
{
    {
        const juce::ScopedLock sl (pendingLock);
        trackName = properties.name.value_or (juce::String());
    }

    titleDirty = true;
    triggerAsyncUpdate();
}

//==============================================================================
void WrapperProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (juce::MessageManager::existsAndIsCurrentThread() && editorWindow != nullptr && editorWindow->isVisible())
        savedWindowBounds = editorWindow->getBounds();

    const juce::ScopedLock sl (stateLock);

    juce::XmlElement root (stateTag);
    root.setAttribute ("version", 1);

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
    window->setAttribute ("x", savedWindowBounds.getX());
    window->setAttribute ("y", savedWindowBounds.getY());
    window->setAttribute ("w", savedWindowBounds.getWidth());
    window->setAttribute ("h", savedWindowBounds.getHeight());
    window->setAttribute ("alwaysOnTop", alwaysOnTop);

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

    if (auto* window = xml->getChildByName (windowTag))
    {
        savedWindowBounds = { window->getIntAttribute ("x"), window->getIntAttribute ("y"),
                              window->getIntAttribute ("w"), window->getIntAttribute ("h") };
        setHostedWindowAlwaysOnTop (window->getBoolAttribute ("alwaysOnTop"));
    }

    const auto restoredBounds = savedWindowBounds;
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
    savedWindowBounds = restoredBounds;

    if (error.isNotEmpty())
    {
        DBG ("Couldn't restore hosted plug-in: " << error);
        unloadPlugin();
        missingPluginState = std::move (xml);
        savedWindowBounds = restoredBounds;
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

    if (titleDirty.exchange (false) && editorWindow != nullptr)
        editorWindow->setName (getWindowTitle());
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
