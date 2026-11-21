#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace altra
{

/** A fixed set of parameters ("slots") that AltraPlugHost reports to the host and that stand in
    for the hosted plug-in's parameters, so the host can automate them.

    VST3 hosts read a plug-in's parameter list once, so the number of slots never changes. When a
    plug-in is loaded, its automatable parameters are mapped onto the slots in order and the slots
    take their names; unused slots are named "Unused N".

    Host -> plug-in: the host sets a slot (automation, Configure, a controller) and the value goes
    to the hosted parameter.
    Plug-in -> host: when the user moves a control in the hosted editor, the hosted plug-in reports
    the change and the gesture, and we report the same on the slot, so the host can record it.

    Threading: the mapping changes only on the message thread, under `lock`. Other threads (the
    audio thread) only try-lock it, and drop the change if the mapping is being swapped. */
class ParameterBank
{
public:
    static constexpr int numSlots = ALTRA_PARAMETER_SLOTS; // set in CMakeLists.txt

    /** Creates the slots and adds them to the processor. Call from its constructor. */
    explicit ParameterBank (juce::AudioProcessor& owner);

    /** Message thread. Maps the slots onto this plug-in's parameters, or clears them (nullptr).
        Must be called with the previous plug-in still alive. Tells the host about the new names
        and values. */
    void attach (juce::AudioPluginInstance*);

    /** Message thread. Re-reads names after the hosted plug-in changed its parameter info. */
    void refreshNames();

    int getNumMapped() const noexcept                   { return numMapped; }
    int getNumUnmapped() const noexcept                 { return numUnmapped; }

    // Called from the hosted plug-in's listener callbacks, on any thread.
    void hostedValueChanged (juce::AudioProcessor*, int hostedIndex, float newValue);
    void hostedGesture (juce::AudioProcessor*, int hostedIndex, bool starting);

private:
    class Slot;

    Slot* slotForHosted (juce::AudioProcessor*, int hostedIndex) const; // call with lock held

    juce::AudioProcessor& owner;
    std::vector<Slot*> slots;                            // owned by the processor

    juce::SpinLock lock;
    juce::AudioProcessor* attached = nullptr;            // guarded by lock
    std::vector<int> slotForHostedIndex;                 // guarded by lock; -1 = not mapped
    int numMapped = 0, numUnmapped = 0;                  // message thread

    JUCE_DECLARE_NON_COPYABLE (ParameterBank)
};

} // namespace altra
