#include "MidiBus.h"

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace altra::midibus
{

namespace
{
    //==============================================================================
    // The shared layout. Every DLL that maps it must agree on it exactly, so the magic number
    // includes its size; a mismatch (an old and a new build loaded together) leaves the bus off.
    // Plain integers, accessed through std::atomic_ref where another DLL may touch them.

    struct Event
    {
        uint64_t block;
        uint32_t offset;
        uint8_t size;
        uint8_t data[3];
    };

    enum : uint64_t { slotFree = 0, slotClaiming = 1, slotLive = 2 };

    struct Slot
    {
        uint64_t state;
        uint64_t id;
        uint64_t nameVersion;               // odd while the names are being written
        uint16_t track[nameLength], plugin[nameLength];
        uint64_t writeCount;                // events ever written
        uint64_t blocksDone;                // sequence number of the newest complete block
        uint64_t lastBlockStart;            // QueryPerformanceCounter at the newest block's start
        Event ring[ringSize];
    };

    struct Bus
    {
        uint64_t magic;
        Slot slots[maxServers];
    };

    constexpr uint64_t busMagic = (uint64_t { 0x414C5452 } << 32) | (uint64_t) sizeof (Bus);

    static_assert ((ringSize & (ringSize - 1)) == 0);
    static_assert (std::atomic_ref<uint64_t>::is_always_lock_free);

    inline std::atomic_ref<uint64_t> at (uint64_t& v) noexcept       { return std::atomic_ref<uint64_t> (v); }
    inline uint64_t load (uint64_t& v) noexcept                       { return at (v).load (std::memory_order_acquire); }
    inline void store (uint64_t& v, uint64_t x) noexcept              { at (v).store (x, std::memory_order_release); }

    Bus* openBus()
    {
       #if defined (_WIN32)
        const auto name = L"Local\\AltraPlugHost.MidiBus." + std::to_wstring (GetCurrentProcessId());
        auto* mapping = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                            0, (DWORD) sizeof (Bus), name.c_str());

        if (mapping == nullptr)
            return nullptr;

        // Never unmapped: other DLLs may still be using it when ours unloads, and the process
        // ending releases it.
        auto* bus = static_cast<Bus*> (MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (Bus)));

        if (bus == nullptr)
            return nullptr;

        uint64_t expected = 0;
        at (bus->magic).compare_exchange_strong (expected, busMagic);

        if (expected != 0 && expected != busMagic)
            return nullptr; // another build with a different layout got here first

        return bus;
       #else
        return nullptr;
       #endif
    }

    /** First called on the message thread (constructors, subscribe), so the audio thread never
        creates the mapping. */
    Bus* getBus()
    {
        static Bus* const bus = openBus();
        return bus;
    }

    int64_t now() noexcept
    {
       #if defined (_WIN32)
        LARGE_INTEGER t;
        QueryPerformanceCounter (&t);
        return t.QuadPart;
       #else
        return 0;
       #endif
    }

    int64_t ticksPerSecond() noexcept
    {
       #if defined (_WIN32)
        LARGE_INTEGER f;
        QueryPerformanceFrequency (&f);
        return f.QuadPart;
       #else
        return 1;
       #endif
    }

    bool isLive (Slot& s) noexcept   { return load (s.state) == slotLive; }

    int findSlot (Bus& bus, uint64_t id)
    {
        for (int i = 0; i < maxServers; ++i)
            if (isLive (bus.slots[i]) && load (bus.slots[i].id) == id)
                return i;

        return -1;
    }

    uint64_t makeId()
    {
        uint64_t id = 0;

        while (id == 0)
            id = (uint64_t) juce::Random::getSystemRandom().nextInt64();

        return id;
    }

    void writeName (uint16_t* dest, const juce::String& name)
    {
        const auto utf16 = name.toUTF16();
        int i = 0;

        for (auto* p = utf16.getAddress(); *p != 0 && i < nameLength - 1; ++p)
            dest[i++] = (uint16_t) *p;

        dest[i] = 0;
    }

    juce::String readName (const uint16_t* source)
    {
        uint16_t copy[nameLength];
        std::memcpy (copy, source, sizeof (copy));
        copy[nameLength - 1] = 0;
        return juce::String (juce::CharPointer_UTF16 (reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (copy)));
    }
}

//==============================================================================
Publisher::Publisher()
{
    id = makeId();
    claimSlot();
}

Publisher::~Publisher()
{
    releaseSlot();
}

void Publisher::claimSlot()
{
    auto* bus = getBus();

    if (bus == nullptr)
        return;

    for (int i = 0; i < maxServers; ++i)
    {
        auto& s = bus->slots[i];
        uint64_t expected = slotFree;

        if (at (s.state).compare_exchange_strong (expected, slotClaiming))
        {
            store (s.id, id);
            store (s.writeCount, 0);
            store (s.blocksDone, 0);
            store (s.nameVersion, 0);
            s.track[0] = 0;
            s.plugin[0] = 0;
            store (s.state, slotLive);
            slot = i;
            return;
        }
    }
}

void Publisher::releaseSlot()
{
    if (auto* bus = getBus(); bus != nullptr && slot >= 0)
        store (bus->slots[slot].state, slotFree);

    slot = -1;
}

void Publisher::setId (uint64_t newId)
{
    if (newId == 0 || newId == id)
        return;

    auto* bus = getBus();

    // A duplicated track brings the original's ID; the copy keeps its own.
    if (bus != nullptr && findSlot (*bus, newId) >= 0)
        return;

    id = newId;

    if (bus != nullptr && slot >= 0)
        store (bus->slots[slot].id, id);
}

void Publisher::setNames (const juce::String& track, const juce::String& plugin)
{
    auto* bus = getBus();

    if (bus == nullptr || slot < 0)
        return;

    auto& s = bus->slots[slot];
    const auto version = load (s.nameVersion);

    store (s.nameVersion, version + 1);
    writeName (s.track, track);
    writeName (s.plugin, plugin);
    store (s.nameVersion, version + 2);
}

void Publisher::beginBlock() noexcept
{
    blockStart = now();
}

void Publisher::publish (const juce::MidiBuffer& midi, int numSamples)
{
    auto* bus = getBus();

    if (bus == nullptr || slot < 0)
        return;

    auto& s = bus->slots[slot];
    const auto block = load (s.blocksDone) + 1;
    auto written = load (s.writeCount);
    int thisBlock = 0;

    for (const auto metadata : midi)
    {
        // Channel messages only (they all fit in three bytes); SysEx and the like are skipped.
        if (metadata.numBytes < 1 || metadata.numBytes > 3 || metadata.data[0] < 0x80 || metadata.data[0] >= 0xf0)
            continue;

        if (++thisBlock > ringSize / 2)
            break; // a flood; keep the readers from losing everything

        auto& e = s.ring[written & (ringSize - 1)];
        e.block = block;
        e.offset = (uint32_t) juce::jlimit (0, std::max (0, numSamples - 1), metadata.samplePosition);
        e.size = (uint8_t) metadata.numBytes;
        std::memcpy (e.data, metadata.data, (size_t) metadata.numBytes);
        ++written;
    }

    store (s.writeCount, written);
    store (s.lastBlockStart, (uint64_t) blockStart);
    store (s.blocksDone, block);
}

//==============================================================================
juce::String ServerInfo::getDisplayName() const
{
    if (track.isNotEmpty() && plugin.isNotEmpty())
        return track + " - " + plugin;

    return track.isNotEmpty() ? track : plugin.isNotEmpty() ? plugin : juce::String ("AltraPlugHost");
}

juce::Array<ServerInfo> listServers()
{
    juce::Array<ServerInfo> result;
    auto* bus = getBus();

    if (bus == nullptr)
        return result;

    for (auto& s : bus->slots)
    {
        if (! isLive (s))
            continue;

        ServerInfo info { load (s.id), {}, {} };

        for (int attempt = 0; attempt < 10; ++attempt)
        {
            const auto before = load (s.nameVersion);

            if ((before & 1) != 0)
                continue;

            auto track = readName (s.track);
            auto plugin = readName (s.plugin);

            if (load (s.nameVersion) == before)
            {
                info.track = track;
                info.plugin = plugin;
                break;
            }
        }

        result.add (info);
    }

    return result;
}

//==============================================================================
void Subscriber::subscribe (uint64_t serverId, int channel)
{
    getBus(); // create the mapping here, not on the audio thread
    wantedChannel = juce::jlimit (1, 16, channel);
    wantedId = serverId;
}

Subscriber::Timing Subscriber::takeTiming()
{
    const auto onTime = blocksOnTime.exchange (0);
    const auto late = blocksLate.exchange (0);

    if (onTime + late == 0)
        return Timing::unknown;

    return late > onTime ? Timing::oneBlockLate : Timing::sameBlock;
}

void Subscriber::endHeldNotes (juce::MidiBuffer& midi, int samplePosition)
{
    for (int note = 0; note < 128; ++note)
    {
        if (held[note])
        {
            midi.addEvent (juce::MidiMessage::noteOff (currentChannel, note), samplePosition);
            held[note] = false;
        }
    }
}

void Subscriber::read (juce::MidiBuffer& midi, int numSamples, double sampleRate)
{
    const auto ourStart = now();
    midi.clear();

    const auto id = wantedId.load();
    const auto channel = wantedChannel.load();

    if (id != currentId || channel != currentChannel)
    {
        endHeldNotes (midi, 0);
        currentId = id;
        currentChannel = channel;
        slot = -1;
    }

    auto* bus = getBus();

    if (bus == nullptr || currentId == 0)
    {
        connected = false;
        return;
    }

    if (slot < 0 || ! isLive (bus->slots[slot]) || load (bus->slots[slot].id) != currentId)
    {
        slot = findSlot (*bus, currentId);

        if (slot < 0)
        {
            endHeldNotes (midi, 0); // the server went away
            connected = false;
            return;
        }

        readPosition = load (bus->slots[slot].writeCount);
        lastBlock = load (bus->slots[slot].blocksDone);
    }

    connected = true;

    auto& s = bus->slots[slot];
    const auto newest = load (s.blocksDone);

    // Did the server run before us in this audio cycle? Then its newest block started moments
    // ago; if it runs after us, its newest block is from the previous cycle, about one block
    // period back. Measured in Live: 0.6-2 ms against 4.5-6.5 ms for 5.3 ms blocks.
    if (newest != lastBlock && sampleRate > 0.0 && numSamples > 0)
    {
        static const auto ticks = ticksPerSecond();
        const auto sinceServerStart = (double) (ourStart - (int64_t) load (s.lastBlockStart)) / (double) ticks;
        const auto blockPeriod = numSamples / sampleRate;

        if (sinceServerStart < blockPeriod * 0.5)
            blocksOnTime.fetch_add (1, std::memory_order_relaxed);
        else
            blocksLate.fetch_add (1, std::memory_order_relaxed);
    }

    lastBlock = newest;

    const auto written = load (s.writeCount);

    if (written - readPosition > (uint64_t) ringSize)
        readPosition = written - ringSize; // we fell too far behind; the oldest events are gone

    for (; readPosition < written; ++readPosition)
    {
        const auto e = s.ring[readPosition & (ringSize - 1)];

        if (load (s.writeCount) - readPosition > (uint64_t) ringSize)
            continue; // overwritten while we read it

        if ((e.data[0] & 0x0f) + 1 != currentChannel)
            continue;

        // Events from the newest block keep their position; older ones are already late and go
        // at the start.
        const auto position = e.block == newest ? juce::jlimit (0, std::max (0, numSamples - 1), (int) e.offset) : 0;
        const auto status = e.data[0] & 0xf0;

        if (status == 0x90 && e.size == 3)
            held[e.data[1] & 0x7f] = e.data[2] != 0;
        else if (status == 0x80 && e.size >= 2)
            held[e.data[1] & 0x7f] = false;
        else if (status == 0xb0 && e.size == 3 && (e.data[1] == 120 || e.data[1] == 123))
            std::fill (std::begin (held), std::end (held), false);

        midi.addEvent (e.data, e.size, position);
    }
}

} // namespace altra::midibus
