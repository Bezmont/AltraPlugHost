# AltraPlugHost — Design Notes

Background and decisions from the initial feasibility discussion (2026-11-19).

## Problem

Some VST plugin GUIs are very expensive to open in Ableton Live. Opening one locks Live's
UI for a noticeable time. When the user closes the plugin window in Live, Live destroys the
plugin's editor (the plugin instance itself stays loaded). Reopening the window rebuilds the
editor from scratch, so the user pays that cost and the UI freeze every time.

**Goal:** let the user load plugins into Live and keep the plugin windows alive. The editors
are never destroyed: the user can hide, show and minimize them, and showing them again is
instant. The hosted plugins should keep as much of their normal behaviour in Live as possible
(automation, mapping, delay compensation, state saving).

## Approaches considered

### 1. Max for Live device hosting `[vst~]` (rejected)

- Hosting works, and chains of `[vst~]` objects work.
- Probably no gain on its own: closing the `[vst~]` editor window most likely destroys the
  editor too. Max has no built-in way to hide that window, so it would need a custom C external
  using Win32 calls.
- Significant limitations compared with Live's own device chain:
  - Parameters reach Live only through a fixed set of pre-built `live.*` parameter slots.
  - Hosted plugin latency isn't reported to Live, as far as we know, so there's no delay
    compensation.
  - Stereo only. Sidechain input is limited.
  - Adds one Max signal vector of latency and Max's own CPU overhead.
  - Saving plugin state with the Live Set needs extra work.

### 2. JUCE wrapper plugin (chosen)

AltraPlugHost is a VST3 plugin built with JUCE. Live loads it like any other plugin, and it
hosts the real plugin(s) inside it.

## Core architecture: why the hosted editor survives

Live **will** destroy AltraPlugHost's own editor when the user closes its window. A plugin
can't refuse or override that teardown, so we never fight it.

Instead, the hosted plugin's editor goes in a **separate top-level window owned by the
`AudioProcessor`**, not the `AudioProcessorEditor`, and is never placed inside Live's window.
Live doesn't know that window exists, so it never destroys it.

```
Live
 └─ AltraPlugHost AudioProcessor        (alive as long as the device is on the track)
     ├─ hosted plugin instance          (loaded once)
     ├─ hosted editor window            ← owned by the processor, separate top-level window
     │                                     closing it HIDES it (setVisible(false)), never deletes the editor
     └─ AltraPlugHost's small editor    ← Live creates and destroys this freely; it's cheap
          └─ Load plugin… / Show / Hide controls
```

- The hosted plugin's window is destroyed only when the wrapper processor is destroyed:
  deleting the device, deleting the track, or closing the Set.
- Idea: a "Show editor" parameter, mappable to MIDI, Push or keys, that toggles the hosted
  window without opening the wrapper's own editor.

## Known caveats to handle

1. **First open still freezes Live.** Plugin editors are built on Live's UI thread. Possible
   mitigation: build the editor right after the plugin loads and keep it hidden, so the cost
   is paid upfront.
2. **We manage the window ourselves.** Live's behaviours (hiding plugin windows when switching
   tracks, the "multiple plug-in windows" preference, stacking) don't apply to our window.
   On Windows, make Live's main window the owner, so ours stays above it, minimizes with it,
   and stays out of the taskbar. Offer always-on-top as an option.
3. **Keyboard focus.** Live forwards some keystrokes from its plugin windows; ours won't by
   default.
4. **Hidden editors may still redraw** and use CPU or GPU. Offer a per-plugin option to destroy
   and recreate the editor instead of hiding it.
5. **Live 12 plug-in sandboxing (running plugins out of process).** Test with it on and off.
6. **Parameter exposure.** The VST3 parameter count is fixed when the plugin loads. Plan a large
   fixed bank of proxy parameters mapped to the hosted plugin's parameters.

## Wrapper responsibilities (pass-through to Live)

- Audio and MIDI pass-through. Multiple buses and sidechain later.
- Report the hosted plugin's latency to Live (`setLatencySamples`) so Live can compensate.
- Save and restore the hosted plugin's state through `getStateInformation` and
  `setStateInformation`: plugin description plus the hosted plugin's state blob.
- Expose parameters through a proxy bank, after the proof of concept.
- Later: chains of multiple hosted plugins.

## Proof-of-concept success criteria

1. Load a known-heavy VST3 inside AltraPlugHost in Live.
2. Open its editor (expected: one-time freeze).
3. Hide and show it repeatedly. **Showing again must be instant.**
4. Close and reopen AltraPlugHost's own window in Live; the hosted window and plugin are unaffected.
5. Save and reload the Live Set; the hosted plugin's state is restored.
6. Null test against the same plugin loaded natively, using a lookahead or linear-phase plugin,
   to confirm the latency is reported and compensated.

## PoC results (2026-11-19, tested by the user in Live with Komplete Kontrol)

Komplete Kontrol was chosen because its GUI is the slowest to open of the user's plugins.

- Criteria 1–4 pass. The first open is slow, as expected. After that, Hide → close
  AltraPlugHost's window in Live → reopen it (fast) → Show brings the hosted window back
  **instantly**. Closing AltraPlugHost's window in Live leaves the hosted window open and working.
- MIDI from the track reaches the hosted instrument (only lightly tested).
- Criterion 6 passes. Ozone 11 EQ in Digital (linear-phase) mode reports 3070 samples, and Live
  shows the same 3070 for AltraPlugHost. No audible delay or doubling. The null test leaves the
  same small residue whether Ozone is hosted in AltraPlugHost or loaded natively, so the residue
  comes from Ozone, not from us.
- Criterion 5 passes. After saving and reloading the Set, Komplete Kontrol came back with the
  changes made to the plugin loaded inside it (Komplete Kontrol is itself a plugin host), and
  its window reopened in the saved position.
- **All six PoC criteria pass.**

### Problem found: minimising

The hosted window is owned by Live's main window, so it has no taskbar button. Windows
minimises owned windows the old way: to a small title bar in the bottom-left corner of the
screen. That bar is too narrow for its restore button to work; the user has to open the window
menu and choose Restore.

To fix in the real implementation. Options:

- **Taskbar button while owned:** keep `WS_EX_APPWINDOW` on the owned window. It then minimises
  to the taskbar like a normal window and still stays above Live. Downside: a taskbar button for
  every hosted window.
- **Minimise means hide:** catch the minimise (`WM_SYSCOMMAND` / `SC_MINIMIZE`) and hide the
  window instead. It comes back through AltraPlugHost's Show button or the planned "show editor"
  parameter (MIDI, Push or keys). No taskbar clutter, but it must be easy to bring back.
- Make it a per-window or global setting.

### Found in code review: hosted plug-in can re-prepare itself off our lock

When a hosted VST3 calls `restartComponent (kIoChanged)`, JUCE's VST3 host calls
`releaseResources()` and `prepareToPlay()` on the instance from the message thread
(`VST3HostContextHeadless::restartComponentOnMessageThread` in JUCE). That doesn't take our
`audioLock`, so it can run while our `processBlock` is inside the plug-in. Before the real
implementation, handle `kIoChanged` ourselves (or wrap it) under `audioLock`. Latency changes
(`kLatencyChanged`) are fine: they reach us through `audioProcessorChanged` and are forwarded
to Live.
