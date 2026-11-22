#include "ParameterBank.h"

namespace altra
{

namespace
{
    bool isMessageThread()
    {
        return juce::MessageManager::existsAndIsCurrentThread();
    }

    /** Takes the lock fully on the message thread (it's only held briefly elsewhere), but only
        tries on other threads, so the audio thread never waits for the message thread. */
    struct BankLock
    {
        explicit BankLock (juce::SpinLock& l)
            : lock (l), locked (isMessageThread() ? (l.enter(), true) : l.tryEnter()) {}
        ~BankLock()                     { if (locked) lock.exit(); }
        explicit operator bool() const  { return locked; }

        juce::SpinLock& lock;
        const bool locked;
    };
}

//==============================================================================
class ParameterBank::Slot final : public juce::HostedAudioProcessorParameter
{
public:
    Slot (juce::SpinLock& l, int i) : lock (l), index (i) {}

    /** Call with the lock held. */
    void setTarget (juce::AudioProcessorParameter* p) { target = p; }
    juce::AudioProcessorParameter* getTargetOnMessageThread() const { return target; }

    /** Message thread. Copies the hosted parameter's value and default; tells the host if the
        value changed. */
    void syncFromTarget()
    {
        const auto newDefault = target != nullptr ? target->getDefaultValue() : 0.0f;
        const auto newValue   = target != nullptr ? target->getValue() : 0.0f;
        defaultValue = newDefault;

        if (! juce::exactlyEqual (value.exchange (newValue), newValue))
            sendValueChangedMessageToListeners (newValue);
    }

    /** The hosted parameter changed; returns true if that is news (not our own value echoed). */
    bool takeValueFromTarget (float newValue)
    {
        return ! juce::approximatelyEqual (value.exchange (newValue), newValue);
    }

    //==============================================================================
    juce::String getParameterID() const override        { return "slot" + juce::String (index + 1); }

    float getValue() const override                     { return value.load(); }

    void setValue (float newValue) override
    {
        value = newValue;

        if (BankLock bl { lock })
            if (target != nullptr)
                target->setValue (newValue);
    }

    float getDefaultValue() const override              { return defaultValue.load(); }

    juce::String getName (int maximumStringLength) const override
    {
        if (isMessageThread() && target != nullptr)
            return target->getName (maximumStringLength);

        return juce::String ("Unused " + juce::String (index + 1)).substring (0, maximumStringLength);
    }

    juce::String getLabel() const override
    {
        return isMessageThread() && target != nullptr ? target->getLabel() : juce::String();
    }

    juce::String getText (float v, int maximumStringLength) const override
    {
        // The hosted plug-in's text conversion may lock the message thread, so only ask there.
        if (isMessageThread() && target != nullptr)
            return target->getText (v, maximumStringLength);

        return juce::String (v, 3).substring (0, maximumStringLength);
    }

    float getValueForText (const juce::String& text) const override
    {
        if (isMessageThread() && target != nullptr)
            return target->getValueForText (text);

        return juce::jlimit (0.0f, 1.0f, text.getFloatValue());
    }

private:
    juce::SpinLock& lock;
    const int index;
    juce::AudioProcessorParameter* target = nullptr; // written on the message thread under lock
    std::atomic<float> value { 0.0f }, defaultValue { 0.0f };
};

//==============================================================================
ParameterBank::ParameterBank (juce::AudioProcessor& o) : owner (o)
{
    slots.reserve (numSlots);

    for (int i = 0; i < numSlots; ++i)
    {
        auto slot = std::make_unique<Slot> (lock, i);
        slots.push_back (slot.get());
        owner.addParameter (slot.release()); // the processor owns its parameters
    }
}

void ParameterBank::attach (juce::AudioPluginInstance* plugin)
{
    JUCE_ASSERT_MESSAGE_THREAD

    std::vector<juce::AudioProcessorParameter*> targets ((size_t) numSlots, nullptr);
    std::vector<int> reverse;
    int mapped = 0, unmapped = 0;

    if (plugin != nullptr)
    {
        const auto& params = plugin->getParameters();
        auto* bypass = plugin->getBypassParameter(); // the host's own bypass covers this
        reverse.assign ((size_t) params.size(), -1);

        for (int i = 0; i < params.size(); ++i)
        {
            auto* p = params[i];

            if (p == bypass || ! p->isAutomatable())
                continue;

            if (mapped == numSlots)
            {
                ++unmapped;
                continue;
            }

            targets[(size_t) mapped] = p;
            reverse[(size_t) i] = mapped++;
        }
    }

    {
        const juce::SpinLock::ScopedLockType sl (lock);
        attached = plugin;
        slotForHostedIndex = std::move (reverse);

        for (size_t i = 0; i < slots.size(); ++i)
            slots[i]->setTarget (targets[i]);
    }

    numMapped = mapped;
    numUnmapped = unmapped;

    for (auto* slot : slots)
        slot->syncFromTarget();

    owner.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withParameterInfoChanged (true));
}

void ParameterBank::refreshNames()
{
    JUCE_ASSERT_MESSAGE_THREAD
    owner.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withParameterInfoChanged (true));
}

ParameterBank::Slot* ParameterBank::slotForHosted (juce::AudioProcessor* plugin, int hostedIndex) const
{
    if (plugin != attached || ! juce::isPositiveAndBelow (hostedIndex, (int) slotForHostedIndex.size()))
        return nullptr;

    const auto slot = slotForHostedIndex[(size_t) hostedIndex];
    return slot >= 0 ? slots[(size_t) slot] : nullptr;
}

void ParameterBank::hostedValueChanged (juce::AudioProcessor* plugin, int hostedIndex, float newValue)
{
    Slot* slot = nullptr;

    if (BankLock bl { lock })
        slot = slotForHosted (plugin, hostedIndex);

    // Slots live as long as the processor, so it's safe to use one after unlocking.
    if (slot != nullptr && slot->takeValueFromTarget (newValue))
        slot->sendValueChangedMessageToListeners (newValue);
}

void ParameterBank::hostedGesture (juce::AudioProcessor* plugin, int hostedIndex, bool starting)
{
    Slot* slot = nullptr;

    if (BankLock bl { lock })
        slot = slotForHosted (plugin, hostedIndex);

    if (slot == nullptr)
        return;

    if (starting)
        slot->beginChangeGesture();
    else
        slot->endChangeGesture();
}

} // namespace altra
