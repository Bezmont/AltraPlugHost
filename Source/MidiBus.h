#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cstdint>

/** Passes MIDI from AltraPlugHost (the server) to AltraPlugHost Receiver instances in the same
    process, with the MIDI channel intact.

    Live merges the channels of a plug-in's MIDI output when another track takes it as input, so
    multi-channel MIDI can't reach separate tracks through Live. The server publishes the hosted
    plug-in's MIDI output into shared memory; each receiver reads one server's stream, keeps one
    channel and outputs that to Live.

    The bus is a named shared-memory block per process, so the separate plug-in DLLs (effect,
    instrument, receiver) all see the same one. Each server owns a slot with a ring of events
    that it writes without ever blocking; receivers read it with their own position.

    Timing: Live runs tracks in parallel, so a receiver may run before or after its server in an
    audio cycle. Setting the receiver track's "MIDI From" to the server's track makes Live run
    the server first; the receiver then gets each block's events in the same block. Otherwise it
    gets them one block late, and says so (see Subscriber::takeTiming). */
namespace altra::midibus
{

constexpr int maxServers = 32;
constexpr int ringSize = 4096;      // events per server; a power of two
constexpr int nameLength = 96;      // UTF-16 code units per name, including the terminator

/** Server side, owned by WrapperProcessor. */
class Publisher
{
public:
    Publisher();
    ~Publisher();

    /** The ID receivers subscribe to. Saved with the Set so they find us again after reloading. */
    uint64_t getId() const noexcept     { return id; }
    void setId (uint64_t);

    /** Shown in the receivers' server menu. Message thread. */
    void setNames (const juce::String& track, const juce::String& plugin);

    /** Audio thread, at the very start of processBlock: notes when the block started. */
    void beginBlock() noexcept;

    /** Audio thread. `midi` is the hosted plug-in's output for this block. */
    void publish (const juce::MidiBuffer& midi, int numSamples);

private:
    void claimSlot();
    void releaseSlot();

    uint64_t id = 0;
    int slot = -1;
    int64_t blockStart = 0;
};

struct ServerInfo
{
    uint64_t id;
    juce::String track, plugin;

    /** "<track> - <plug-in>", or whichever is known. */
    juce::String getDisplayName() const;
};

/** The servers currently on the bus. Message thread. */
juce::Array<ServerInfo> listServers();

/** Receiver side. */
class Subscriber
{
public:
    /** Message thread. Server 0 = none. Takes effect at the next read. */
    void subscribe (uint64_t serverId, int channel /* 1-16 */);
    uint64_t getServerId() const noexcept   { return wantedId.load(); }
    int getChannel() const noexcept         { return wantedChannel.load(); }
    bool isConnected() const noexcept       { return connected.load(); }

    /** Audio thread. Replaces `midi` with the subscribed channel's events since the last call. */
    void read (juce::MidiBuffer& midi, int numSamples, double sampleRate);

    enum class Timing { unknown, sameBlock, oneBlockLate };

    /** Message thread: whether the server mostly ran before us (same block) or after us (its
        events reach us a block late) since the last call. */
    Timing takeTiming();

private:
    void endHeldNotes (juce::MidiBuffer&, int samplePosition);

    std::atomic<uint64_t> wantedId { 0 };
    std::atomic<int> wantedChannel { 1 };
    std::atomic<bool> connected { false };
    std::atomic<uint32_t> blocksOnTime { 0 }, blocksLate { 0 };

    // Audio thread only.
    uint64_t currentId = 0;
    int currentChannel = 1;
    int slot = -1;
    uint64_t readPosition = 0, lastBlock = 0;
    bool held[128] {};
};

} // namespace altra::midibus
