// Checks the MIDI bus without Live: a server publishing notes and CCs on channels 1-3 and a
// receiver keeping channel 2, with the server running before the receiver, after it, and
// alternating; plus server listing, note-offs on channel change, overrun, and timing detection.
#include "../../Source/MidiBus.h"
#include <cstdio>

using namespace altra::midibus;

namespace
{
    int failures = 0;

    void check (bool ok, const char* what)
    {
        std::printf ("%s: %s\n", ok ? "ok  " : "FAIL", what);
        failures += ok ? 0 : 1;
    }

    juce::MidiBuffer makeBlock (int block)
    {
        juce::MidiBuffer m;
        for (int ch = 1; ch <= 3; ++ch)
        {
            m.addEvent (juce::MidiMessage::noteOn (ch, 60 + (block % 12), (juce::uint8) 100), 10 * ch);
            m.addEvent (juce::MidiMessage::controllerEvent (ch, 1, (block * ch) % 128), 5 * ch);
        }
        m.addEvent (juce::MidiMessage::noteOff (2, 60 + ((block + 11) % 12)), 5);
        return m;
    }

    struct Result { int events = 0, wrongChannel = 0, onTime = 0, atZero = 0, ccs = 0, ccValueWrong = 0; };

    Result run (Publisher& server, Subscriber& receiver, int blocks, int order /* 0 server first, 1 receiver first, 2 alternate */)
    {
        Result r;
        for (int b = 0; b < blocks; ++b)
        {
            const bool serverFirst = order == 0 || (order == 2 && b % 2 == 0);
            juce::MidiBuffer out;
            auto publish = [&] { server.beginBlock(); server.publish (makeBlock (b), 64); };
            auto read = [&] { receiver.read (out, 64, 48000.0); };
            if (serverFirst) { publish(); read(); } else { read(); publish(); }

            const int expectedBlock = serverFirst ? b : b - 1;

            for (const auto md : out)
            {
                ++r.events;
                const auto msg = md.getMessage();
                if (msg.getChannel() != 2) ++r.wrongChannel;
                if (msg.isNoteOn() && md.samplePosition == 20) ++r.onTime;
                if (md.samplePosition == 0) ++r.atZero;
                if (msg.isController() && msg.getControllerNumber() == 1)
                {
                    ++r.ccs;
                    if (msg.getControllerValue() != (expectedBlock * 2) % 128 && order != 2) ++r.ccValueWrong;
                }
            }
        }
        return r;
    }

    Subscriber::Timing timingFor (Publisher& server, Subscriber& receiver, bool serverFirst)
    {
        // Blocks of 48 samples at 1 kHz: 48 ms each, slow enough to measure with real sleeps.
        receiver.takeTiming();

        for (int b = 0; b < 8; ++b)
        {
            juce::MidiBuffer out;
            if (serverFirst) { server.beginBlock(); server.publish ({}, 48); receiver.read (out, 48, 1000.0); }
            else             { receiver.read (out, 48, 1000.0); server.beginBlock(); server.publish ({}, 48); }
            juce::Thread::sleep (48);
        }

        return receiver.takeTiming();
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    Publisher server;
    server.setNames ("1-Maschine", "Maschine 3");
    const auto servers = listServers();
    check (servers.size() == 1 && servers[0].id == server.getId(), "server listed");
    check (servers.size() == 1 && servers[0].getDisplayName() == "1-Maschine - Maschine 3", "server names");

    Subscriber receiver;
    receiver.subscribe (server.getId(), 2);

    {
        juce::MidiBuffer out; receiver.read (out, 64, 48000.0); // connect
        check (receiver.isConnected(), "connected");
    }

    auto r = run (server, receiver, 100, 0);
    check (r.wrongChannel == 0 && r.onTime == 100 && r.ccs == 100 && r.ccValueWrong == 0, "server first: everything on time, CC values intact");

    r = run (server, receiver, 100, 1);
    check (r.wrongChannel == 0 && r.ccs >= 99 && r.ccValueWrong == 0, "receiver first: one block late, nothing lost");

    r = run (server, receiver, 100, 2);
    check (r.wrongChannel == 0 && r.ccs == 100, "alternating: nothing lost");

    check (timingFor (server, receiver, true) == Subscriber::Timing::sameBlock, "timing: server first detected as same block");
    check (timingFor (server, receiver, false) == Subscriber::Timing::oneBlockLate, "timing: receiver first detected as one block late");

    // Changing channel ends the held note on the old one.
    {
        juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (2, 100, (juce::uint8) 90), 0);
        server.beginBlock(); server.publish (m, 64);
        juce::MidiBuffer out; receiver.read (out, 64, 48000.0);
        receiver.subscribe (server.getId(), 3);
        out.clear(); receiver.read (out, 64, 48000.0);
        bool offFor100 = false;
        for (const auto md : out) offFor100 |= md.getMessage().isNoteOff() && md.getMessage().getNoteNumber() == 100 && md.getMessage().getChannel() == 2;
        check (offFor100, "note-off for the held note when the channel changes");
    }

    // Overrun: far more events than the ring holds between two reads; the reader keeps going.
    {
        for (int b = 0; b < 3000; ++b) { server.beginBlock(); server.publish (makeBlock (b), 64); }
        juce::MidiBuffer out; receiver.read (out, 64, 48000.0);
        int count = 0;
        for (const auto md : out) { juce::ignoreUnused (md); ++count; }
        check (count > 0 && count < ringSize, "overrun: reader skips to the newest events");
    }

    std::printf (failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
