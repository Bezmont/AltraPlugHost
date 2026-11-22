# AltraPlugHost

A VST3 plug-in host for **Ableton Live on Windows** that keeps heavy plug-in windows alive, and
splits multi-channel MIDI so each channel can be recorded on its own track.

## Why

Some plug-ins (Komplete Kontrol, Kontakt, big synths) take seconds to open their window, and Live
freezes while they do. Live destroys a plug-in's window every time you close it, so you pay that
cost again on every reopen.

AltraPlugHost is a plug-in that hosts another plug-in. You load AltraPlugHost in Live and pick
the plug-in inside it. When you close the window in Live, the hosted plug-in's window isn't
destroyed: AltraPlugHost parks it in a hidden window of its own. Reopening is instant.

## Features

### Instant reopening
- The hosted plug-in's window is built once and then kept. Closing and reopening the device
  window in Live is instant, and the plug-in never redraws from scratch.
- Two variants, because Live keeps instruments and effects in different slots:
  **AltraPlugHost** (effects) and **AltraPlugHost Instrument** (instruments).

### Behaves like the plug-in itself
- **Audio and MIDI** pass through to the hosted plug-in, and its MIDI output is passed on to Live.
- **Latency** is reported to Live, so delay compensation works.
- **State** is saved with the Live Set and restored on load. If a hosted plug-in can't be found
  when the Set opens, its state is kept so saving the Set doesn't lose it.
- **Automation:** AltraPlugHost exposes 4096 parameter slots that take the hosted plug-in's
  parameter names, values and units. Recording automation by turning knobs in the plug-in's own
  window, playback and Live's Configure all work.
- **Window title:** Live's window shows "*hosted plug-in* / *track*" instead of AltraPlugHost.

### Compact windows
- **Collapse** the window to a small bar showing the track colour, the plug-in and track name,
  and Restore:
  - **Up arrow:** snaps the bar into a row along the top of the screen.
  - **Down arrow:** snaps it into a row along the bottom of the screen.
  - **Left arrow:** a floating bar that remembers where you put it.

  Several collapsed plug-ins line up side by side in the row. Expanded and collapsed positions
  are each remembered with the Set.
- **Slim bar** (in the ☰ menu, on by default): the collapsed bar has no Windows title bar, just
  the bar itself. Drag it by its background.
- Reopening a closed window always opens it expanded, and fully on screen.

### Plug-in browser
- **Browse** opens a screenshot grid of your plug-ins, with search and filters by type, vendor and
  category. It reads the catalog exported by
  [PlugBrowser](https://github.com/Bezmont/PlugBrowser), a separate plug-in scanner and catalog
  app. PlugBrowser does the scanning in separate processes, so a plug-in that crashes while
  being scanned can't take Live down, and AltraPlugHost can warn about plug-ins that failed.
- Without PlugBrowser's catalog, Browse falls back to a menu of the plug-ins in the standard VST3
  folders, plus "Browse..." for picking any `.vst3` file.

### Multi-channel MIDI: AltraPlugHost Receiver
Live merges all MIDI channels of a plug-in's output into one, and clips don't keep channels. So a
plug-in that plays several parts on different MIDI channels (for example Maschine, with each
group sent to its own channel) can't be recorded part by part.

**AltraPlugHost Receiver** is a third plug-in that solves this. It picks up the MIDI that a
hosted plug-in sends, directly from AltraPlugHost (channels intact, without going through Live),
and outputs just one channel. Notes and CCs are both passed on.

Setup, per channel you want to record:

1. **Track A:** AltraPlugHost Instrument hosting the multi-channel plug-in.
2. **Receiver track:** a MIDI track with **AltraPlugHost Receiver**. In its window pick track A's
   AltraPlugHost and a channel. Set this track's **MIDI From** to track A, and monitoring to
   **In**. This makes Live process track A first, so the MIDI arrives in the same audio block.
   If you forget, the Receiver warns you in orange that its MIDI arrives one block late.
3. **Recording track:** a MIDI track with **MIDI From** set to the receiver track (and the
   receiver in the sub-menu), with whatever instrument you want. Arm and record.

Several receivers can share one track: put them in the chains of an **Instrument Rack** (show the
chain list and drop each receiver into it). The recording tracks' MIDI From then lists each
receiver in the rack's sub-menu. Set the rack track's MIDI From to track A, as above.

## Requirements

- Windows 10 or 11, 64-bit.
- Ableton Live (developed and tested with Live 12 Suite). Other VST3 hosts may work, but the window
  features are built around Live.
- Hosts **VST3** plug-ins (64-bit).
- Optional: [PlugBrowser](https://github.com/Bezmont/PlugBrowser) for the screenshot browser.

Nothing else needs installing: the Visual C++ runtime is built in.

## Install

1. Get the three `.vst3` bundles: from this repository's Releases when available, or by building
   them (below).
2. Copy them into `C:\Program Files\Common Files\VST3`:
   - `AltraPlugHost.vst3`
   - `AltraPlugHost Instrument.vst3`
   - `AltraPlugHost Receiver.vst3` (only needed for multi-channel MIDI)
3. In Live, rescan plug-ins (Settings > Plug-Ins > Rescan).

## Use

1. Drop **AltraPlugHost Instrument** on a MIDI track (or **AltraPlugHost** on an audio or MIDI
   track for an effect) and open its window.
2. Click **Browse** and pick a plug-in.
3. Use it as normal. Close and reopen the window as often as you like; it's instant.

The toolbar: **Browse** (change plug-in), the three collapse arrows, and **☰** (options). Hover
over the empty left part of the toolbar to see the plug-in's latency and how many of its
parameters are automatable.

## Known limitations

- Windows only for now. A Mac version would need Mac-specific window code and a Mac to test on.
- Only the hosted plug-in's main stereo input and output are connected: no sidechain or
  multi-output yet.
- Automation slots are assigned in the hosted plug-in's parameter order, and aren't remapped when
  you switch plug-ins: existing automation lanes then control whatever the new plug-in has in
  those slots. Plug-ins with more than 4096 automatable parameters get only the first 4096.
- Live may hang on exit when Kontakt 8 is loaded. This happens without AltraPlugHost too: it's in
  Kontakt 8 (Native Instruments), not something AltraPlugHost can fix.
- Maschine's own CC output can be uneven (stray values from other groups on channel 1). That's
  in Maschine's output, not added by AltraPlugHost.

## Building

Needs Visual Studio 2022 ("Desktop development with C++"), CMake 3.22 or later, and Git.

```
git clone --recursive https://github.com/Bezmont/AltraPlugHost.git
cd AltraPlugHost
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

The plug-ins end up in `build/VST3/`. Close Live before rebuilding: Windows won't overwrite a
plug-in that Live has loaded. More detail, including developer tools, is in
[docs/BUILD.md](docs/BUILD.md).

## How it works

- **Keeping windows alive:** the hosted plug-in's editor belongs to the plug-in instance
  (AltraPlugHost's processor), not to AltraPlugHost's window in Live. While Live's window is open,
  the editor is docked inside it; just before Live destroys that window, the editor is moved to a
  hidden window of AltraPlugHost's own. Moving a plug-in window only reparents its native window,
  and the plug-in isn't told, so nothing is rebuilt.
- **Multi-channel MIDI:** each AltraPlugHost publishes its hosted plug-in's MIDI output into a
  shared-memory ring inside Live's process. Receivers read it with their own position, keep one
  channel, and send note-offs when they change channel or lose their source.
- The full design, the approaches that were rejected, and test results are in
  [docs/DESIGN.md](docs/DESIGN.md).

## Licence

AltraPlugHost is licensed under the **GNU Affero General Public License v3.0**; see
[LICENSE](LICENSE).

It's built with [JUCE](https://juce.com) (AGPLv3 / commercial), which is included as a submodule
and includes Steinberg's VST3 SDK. VST is a registered trademark of Steinberg Media Technologies
GmbH. Ableton Live, Maschine, Kontakt and Komplete Kontrol are trademarks of their respective
owners; AltraPlugHost isn't affiliated with them.
