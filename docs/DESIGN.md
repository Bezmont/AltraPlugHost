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

### Observation: either variant can host an instrument

Both variants host instruments and sound the same, because both accept MIDI and the hosted
plugin's audio replaces ours. The difference is how Live classifies them: the FX variant
declares itself an audio effect, so Live puts it after an empty instrument slot ("Drop an
instrument here") on a MIDI track.

Keep both variants, and use the Instrument variant for instruments so Live treats the track
normally. Untested risks of hosting an instrument in the FX variant: anything in Live that
depends on the track having an instrument, and a real instrument dropped into that slot feeding
audio into our effect. Idea: the FX variant could show a hint when the loaded plugin is an
instrument (`PluginDescription::isInstrument`).

## Plugin browser: PlugBrowser's catalog (decided 2026-11-19)

Picking a plugin uses the catalog from [PlugBrowser](https://github.com/Bezmont/PlugBrowser), the
user's own scanner and catalog app. PlugBrowser stays the scanner and catalog provider;
AltraPlugHost only reads what it exports.

- **The contract** is PlugBrowser's export file, `%LOCALAPPDATA%\PlugBrowser\export\plugins.json`,
  specified in PlugBrowser's `docs/catalog-export.md` (format version 1). AltraPlugHost never
  reads PlugBrowser's SQLite database, which is a private cache PlugBrowser rebuilds whenever its
  schema changes.
- **Why not scan ourselves:** scanning loads plugins, and a plugin that crashes or hangs during a
  scan would take Live down. PlugBrowser scans in separate processes with timeouts, and it records
  which plugins failed, so AltraPlugHost can warn about them.
- **The browser** (`Source/PluginBrowser.*`) is a screenshot grid built from PlugBrowser's
  320 px thumbnails, with search and type, vendor and category filters. The type filter defaults
  to the variant: instruments in AltraPlugHost Instrument, effects in AltraPlugHost.
  - Shown: 64-bit VST3 only. Left out: blacklisted, unsupported, and AltraPlugHost itself.
    Plugins that failed or hung in PlugBrowser's scan are shown with a warning badge, because
    those failures are often intermittent.
  - Loading uses the catalog's path, then picks the class by name; only a few catalog entries
    have a VST3 class ID.
  - Thumbnails are decoded on the message thread in small slices (about 8 ms per tick), because
    JUCE 9 images may be GPU-backed on Windows. They're freed when the browser closes.
- **Fallback:** without a readable export (missing, or a format version we don't support), "Load
  plugin…" shows the old menu listing the standard VST3 folders.

## Open issue: Live hung on exit (2026-11-19)

After the user closed Live, two Live processes stayed alive with no windows, marked "Not
Responding", and had to be ended in Task Manager. Both had AltraPlugHost (both variants) and
Komplete Kontrol loaded. One had used about 58 minutes of CPU time, so it may have been hung for
a long time, possibly since an earlier session. Windows' process APIs could no longer see them
while `tasklist` still could, which usually means a process stuck while exiting.

Cause unknown; no debugger or symbols were available. To narrow it down:

- Does Live exit cleanly with Komplete Kontrol loaded natively (no AltraPlugHost)?
- Does it exit cleanly with AltraPlugHost loaded but the hosted window never opened? And with
  the window open?
- Suspects on our side: the hosted window owned by Live's main window being destroyed in the
  wrong order at exit, and plugins being deleted outside the message thread.
- For next time: install WinDbg or the Windows SDK debugging tools, and build RelWithDebInfo, so
  a hung process can be inspected.

**Update 2026-11-20:** it keeps happening now and then (not every exit). Once, Live was started
while a hung one was still running and a second Live started. Since the hosted window is no longer
owned by Live's main window (it's a hidden parking window now), the first suspect above is gone.

What we know of how hosted plugins are unloaded: JUCE keeps each hosted plugin's DLL loaded
(reference-counted) while any instance of it exists, and calls its `ExitDll` and `FreeLibrary`
when the last instance is deleted. That happens on the message thread when Live deletes our
instance, not while our own DLL is unloading, so the classic loader-lock deadlock shouldn't apply.

To find out whether we're involved, AltraPlugHost now writes a lifecycle log (`Diagnostics.*`) to
`%LOCALAPPDATA%\AltraPlugHost\lifecycle.log`: module loaded/unloading, each instance created and
deleted (with a count of instances alive), each hosted plugin's load and deletion, and its editor
opening and deletion. Steps are logged before and after ("...", "done (N ms)"), so a step that
hangs is the line with no "done". After a hung exit, the end of the log says which case it is:

- **Ends with "module unloading":** every instance was deleted and our DLL unloaded cleanly. The
  hang is elsewhere: Live itself, or another plugin.
- **Ends with a step that has no "done":** that step hangs. For example, "deleting hosted
  "Komplete Kontrol"..." would point at the plugin's own shutdown.
- **Ends with instances still alive and no deletion:** Live never deleted our instances. It hung
  before getting to them, or deleted them on a path we don't see.

If it's inside a hosted plugin's shutdown, the next step is a memory dump of the hung process
(Task Manager: right-click, Create memory dump file) and WinDbg to see the threads.

## Revised architecture: dock the hosted editor in Live's window (prototype, 2026-11-19)

Proposed by the user after the PoC: make AltraPlugHost's own window in Live the wrapper for the
hosted plugin's GUI, with a small toolbar on top, and keep instant show/hide.

**Why it can work.** In JUCE 9, a hosted VST3 editor draws into a JUCE-created native child
window (`VST3PluginWindow::ViewComponent`, an `HWNDComponent`). `IPlugView::attached()` is
called once, when the editor is first shown, and `componentPeerChanged()` is empty: moving the
editor to another window only reparents that child window (`SetParent` in
`juce_HWNDComponent_windows.cpp`). The plugin isn't told, so its GUI isn't rebuilt.

**How it works.** The processor owns the hosted `AudioProcessorEditor` and a hidden top-level
window (`HostedEditorWindow`). The editor is always in exactly one place:

- **Docked** in AltraPlugHost's editor in Live, below the toolbar. AltraPlugHost's window sizes
  itself to the plugin plus the toolbar.
- **Parked** in the hidden window while Live's window is closed or the browser is showing.
  `WrapperEditor`'s destructor parks it before Live destroys the window.
- **Popped out:** the same window, shown as a floating window (the PoC behaviour).

Live's own window management applies again when docked, so the minimise problem and most of
caveats 2 and 3 above go away for the docked case.

**Risk to test first.** This relies on Live calling `IPlugView::removed()` (which deletes our
editor and so parks the hosted one) *before* destroying its window, as the VST3 spec requires.
If Live destroyed the window first, Windows would destroy the plugin's child window with it.

**Found in testing: parking in the destructor is too late.** Reopening showed a black area
of the right size. JUCE's VST3 wrapper (`juce_audio_plugin_client_VST3.cpp`, `removed()`) calls
`removeFromDesktop()` on its component, destroying our native window, *before* deleting our
editor, and Windows destroys child windows with their parent: the hosted plugin's window was
destroyed.

First fix, parking on `WM_DESTROY` (sent before child windows are destroyed), kept the window
alive but the plugin then drew only white once docked again. `tools/DockTest --raw` showed why:
moving a plugin's editor between windows works in every way tried (through JUCE, raw
`SetParent`, via a hidden or visible parking window), *except* moving it while the old window is
already being destroyed.

Working fix: park *before* the window is destroyed. `Component::removeFromDesktop()` calls
`CachedComponentImage::releaseResources()` on every component inside before it deletes the
native window, so `WrapperEditor::TeardownSentinel`, an invisible child with its own
`CachedComponentImage`, parks the hosted editor from there. The same call also happens when a
parent component is hidden or removed; the editor then docks again asynchronously if it is still
showing. The `WM_DESTROY` watcher stays as a last resort. Verified with DockTest: Filter MS-20 and
Komplete Kontrol draw correctly after four close/reopen cycles, with the same plugin window
(same HWND) every time.

**Not done yet:** resizing AltraPlugHost's window by dragging (it follows the plugin's size, but
the user can't resize it), and DPI changes when the docked window moves between monitors.

Slow work (loading a plugin, building its editor) shows "Loading…" or "Opening…" and the wait
cursor first. It still freezes Live, because both must run on the message thread.

## Experiment: showing the hosted plugin's name in Live (2026-11-19)

A VST3 can't change the device title Live shows, and Live doesn't let the user rename plugin
devices. Experiment: AltraPlugHost reports two "programs", both named after the hosted plugin
("No plugin loaded" or "Missing: …" otherwise). Two, because JUCE only creates the program
parameter when there is more than one. After every load, unload or failed restore,
`HostNotifications` uses the host's `IComponentHandler` (from `VST3ClientExtensions`) to send
`IUnitHandler::notifyProgramListChange` (JUCE never sends it) and
`restartComponent (kParamValuesChanged)`. Verified with DockTest: JUCE's host re-reads the names.

**Result in Live 12 (2026-11-20): failed, removed.** Live showed the program parameter, but its
value stayed "No plugin loaded" (the name read at insert time) whatever was loaded. Live appears
to read program names once and to ignore `notifyProgramListChange`. The only visible effect was an
extra "Program" automation parameter, so the code was removed. Workaround: put AltraPlugHost in a
Group (Audio/Instrument Rack) and rename the rack.

Also seen: turning a knob in the hosted plugin while arming automation creates no lane. Expected:
Live only sees AltraPlugHost's own parameters, and there were none yet. Now addressed by the
parameter bank below.

## Parameter bank (automation) (2026-11-20, works in Live)

`ParameterBank` gives AltraPlugHost a fixed number of parameters ("slots", IDs `slot1`, `slot2`, …),
set by the CMake cache variable `ALTRA_PARAMETER_SLOTS` (4096 since 2026-11-20; it was 2048).
A VST3 host reads the parameter list once, so the count can't follow the hosted plugin.

- On load, the hosted plugin's automatable parameters (minus its bypass) are mapped onto the slots
  in order. Each slot takes its parameter's name, unit, value text and default value. Unused slots are
  named "Unused N". Then `updateHostDisplay (parameterInfoChanged)` makes JUCE send
  `restartComponent (kParamTitlesChanged)`. Live renames the slots right away (verified).
- Host → plugin: `Slot::setValue` forwards to the hosted parameter (automation playback, Configure).
- Plugin → host: the hosted plugin's value changes and gestures (begin/end edit) arrive through our
  `AudioProcessorListener` and are re-sent on the slot, which JUCE turns into
  `beginEdit`/`performEdit`/`endEdit` to Live, so Live can record automation and Configure can learn.
  An echo of a value we just set is ignored, so automation playback isn't reported back to Live as
  a user edit.
- Threading: the mapping changes on the message thread under a `SpinLock`. Other threads only
  try-lock it and drop the change during a plugin swap. Names and value texts come from the
  hosted plugin only on the message thread (JUCE's hosted `getText` takes the message manager lock).
- The mapping isn't saved: it's recomputed from the plugin's parameter order, which is stable for a
  given plugin version. Changing the hosted plugin leaves existing automation lanes pointing at
  whatever the new plugin puts in those slots.
- Slot count: automatable parameter counts measured with `DockTest --count`: Filter MS-20 36,
  Replika XT 56, Analog Lab V 304, Jup-8 V4 361, Massive X 488, Guitar Rig 7 512, Omnisphere 513,
  MS-20 V 690, FM8 1093, Komplete Kontrol 1135, Kontakt 8 / Battery 4 2065, Pigments 2265.
  Kontakt itself reports over 4000 parameters to Live. With 4096 slots all of these fit; the
  editor's name tooltip says how many.
- MIDI CC is separate: JUCE's VST3 wrapper already turns Live's MIDI CCs into MIDI messages in
  `processBlock` (`JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS`), and they pass to the hosted plugin.

Verified in DockTest: the slots carry the hosted names and value texts (e.g. "Master Volume" =
-8.50 dB on MS-20 V).

**Verified in Live 12 (2026-11-20):** recording automation by turning knobs in the hosted editor,
playback, Configure, and slot names matching the hosted plugin all work. After changing the
hosted plugin, existing lanes stay and control whatever the new plugin has in those slots, as
designed.

Live renames the slots as soon as the hosted plugin changes: it honours
`restartComponent (kParamTitlesChanged)`, unlike program-name changes. That makes a later option
possible: assigning slots when a hosted parameter is first touched, instead of in order. That would
be for plugins with more than 2048 parameters (Pigments), and for keeping lanes attached when
switching plugins.

### Slot count: what more slots cost (2026-11-20)

Measured with `DockTest --timing` (JUCE's VST3 host, 5 rounds, the Instrument variant hosting Replika XT, then
switching to MS-20 V). The wrapper reports the slots plus about 2080 more: JUCE's 2048 hidden MIDI CC
parameters and a few of its own.

| Slots | Parameters reported | Create (insert) | Load into empty | Switch plugin | Save | Delete |
|------:|--------------------:|----------------:|----------------:|--------------:|-----:|-------:|
| 2048  | 4129  | 41 ms  | 117 ms | 215 ms | 31 ms | 126 ms |
| 4096  | 6177  | 61 ms  | 116 ms | 214 ms | 29 ms | 136 ms |
| 8192  | 10273 | 113-121 ms | 118-131 ms | 220 ms | 29 ms | 138 ms |

Loading Replika XT directly takes 181 ms and MS-20 V 264 ms, so the hosted plugin dominates every load.
Only creating the wrapper grows with the slot count: about 10 ms more per 1000 parameters, paid
once per device when it's inserted or when the Set loads.

**Judged in Live 12 (2026-11-20), settled on 4096:** with several instances, inserting devices
and saving and reopening the Set felt no slower than normal.

Live's control chooser doesn't list all the slots. For a plugin with this many parameters it
lists only the ones added with Configure, plus the last one touched, so the "Unused" slots never
show. An empty lane added for the last-touched parameter falls back to "Device On" when another
parameter is touched. Lanes with automation, and parameters added with Configure, stay. This is
how Live treats any large plugin (Kontakt too), not something AltraPlugHost can change.

Raising the count later keeps saved automation (the IDs don't change). Lowering it breaks lanes
on the removed slots, so start low enough.

## Collapse instead of pop-out (2026-11-20, not yet tested in Live)

The pop-out window is removed: the user found no use for it once the plugin docks in
AltraPlugHost's own window. `HostedEditorWindow` is now only the hidden parking window. The
Win32 owner code that kept the popped-out window above Live (`findHostMainWindow`,
`setTopLevelOwner`) went with it. Old Sets' window position in the state is ignored.

Live's plugin window belongs to Live: a plugin can't add buttons to its title bar or change what
its close button does. A plugin also can't ask Live to reopen a window it closed. So a separate
"mini window" whose Restore reopens Live's window can't be done. Instead **Collapse** (toolbar)
parks the hosted editor, exactly as closing the window does, and shrinks AltraPlugHost's editor
to the toolbar alone: the plugin and track names and a Restore button. Live's window follows,
because VST3 plugins may resize their window. Restore docks the editor again (instant). The
collapsed state is part of AltraPlugHost's state, so it survives closing the window and
reloading the Set.

**Track name and colour.** VST3 hosts may send them (`IInfoListener`, which JUCE passes to
`updateTrackProperties`). It isn't known yet whether Live does: the lifecycle log records it
("track properties: ..."). If Live sends a colour, the toolbar shows it as a strip on the left.

**Window title.** Live titles the window "<device>/<track>", e.g. "AltraPlugHost
Instrument/1-AltraPlugHost Instrument". (Live names a track after its first device until the user
renames it, which is why our name appears twice.) We subclass Live's window (`native::TitleWatcher`)
and replace the device part with the hosted plugin's name: "Kontakt 8/1-AltraPlugHost Instrument".
Each time Live sets the title (e.g. after the track is renamed), we keep Live's version and
show ours. The track part also feeds the toolbar when the host doesn't send the track name. When
our editor closes, Live's own title is put back. To check in Live: whether Live redraws or resets
the title in ways we don't see.

**Tested in Live 12 (2026-11-20):** Collapse/Restore works, and the title shows
"<hosted plugin>/<track>" and follows track renames. Live does send track info, but the name
and the colour arrive in separate `updateTrackProperties` calls, each with the other field empty,
so only the fields present are applied. After that test:

- Collapsed, the bar shows only the track colour (a square) and Restore; Live's title already
  names the plugin and track.
- Each state returns to where it was last: Collapse remembers the expanded window's position and
  moves to the last collapsed position; Restore does the reverse. Positions are screen
  coordinates (physical pixels) from Live's window (`native::getWindowPosition`), kept in the
  saved state (`Window` element: `expandedX/Y`, `collapsedX/Y`), and only applied if the title bar
  would land on a monitor.

## Exit hang: cause found (2026-11-20)

A hung Live (one thread left, no windows, "Responding" but not exiting) was attached to
non-invasively with cdb (WinDbg, `cdb -pv -p <pid>`). The last thread, Live's main thread, was
blocked here:

```
ucrtbase!common_exit → KERNEL32!ExitProcess → ntdll!LdrShutdownProcess
  → ni_platform.dll's atexit handler (C:\Program Files\Common Files\Native Instruments\NTK\ni-platform.dll)
    → WS2_32!WSAPoll → NtWaitForSingleObject      (waits forever)
```

At process exit, Windows stops every other thread first and then runs each DLL's cleanup. NI's
platform library waits on a socket for a thread that no longer exists. Every hang so far had an
NI plugin loaded (Komplete Kontrol; Kontakt 8 and Massive). The one logged clean exit had only
Arturia.

AltraPlugHost's part checked out:
- The lifecycle log showed all four instances and their hosted plugins deleted normally
  (~2 s) before the hang.
- Under cdb, JUCE called Massive's `InitDll`/`ExitDll` exactly once per load, as the VST3 module
  rules require.
- Many plugins pin themselves (Kontakt 8, Jup-8000 V, Filter MS-20 stay loaded after
  `FreeLibrary` 50 times; `DockTest --count` lists what stays loaded), so their cleanup runs at
  process exit in any host, Live included.

Still unknown: whether the same hang happens with NI plugins hosted by Live directly. The user
doesn't remember seeing it before AltraPlugHost, but the hang is silent (a new Live starts
normally), so it may have gone unnoticed. Test: NI plugins loaded directly in Live, editors
opened, quit several times, then check Task Manager.

**Second hang, same day (Komplete Kontrol in FX AltraPlugHost + Jup-8000 V in the Instrument, an
older Set).** Same stack: ni-platform's atexit handler in `WSAPoll`. The socket's far end is NI's
**NTKDaemon** process (localhost ports 5563 and 5146, both connections still Established): the
cleanup waits for the daemon to answer and it never does. Our log was clean again: both instances
deleted, hosted plugins deleted. Komplete Kontrol stays loaded (pinned).
The user's comparison: NI instruments loaded directly in Live, about 10 quits, no hang. DockTest
(load KK, delete, exit) 8 times: no hang. So AltraPlugHost may change something NI depends on.
Suspect: JUCE calls the plugin's `ExitDll` when its last instance is deleted, which Live may not do
for natively hosted plugins.

**Resolved: not AltraPlugHost (2026-11-20).** Komplete Kontrol loaded directly in Live, with no
AltraPlugHost in the process, and a Kontakt-based instrument loaded inside KK (that is what loads
Kontakt 8 and ni-platform.dll). On quit Live hung with the identical stack: ni-platform's atexit
handler in `WSAPoll`. The debugger also showed Live itself calls KK's `ExitDll` on quit, and KK
calls Kontakt 8's, so the `ExitDll` suspect above is wrong: JUCE does the same as Live.
The user's earlier "native" tests didn't hang because ni-platform.dll was never loaded (KK
without a Kontakt instrument, or no NI plugins). It's an NI bug (ni-platform / NTKDaemon), triggered
by Kontakt 8 running inside Komplete Kontrol. Nothing to fix in AltraPlugHost; the lifecycle log
can stay as a diagnostic.
Confirmed by the user without the debugger: same native setup, same hang.

**Collapse follow-up (2026-11-20).** Position memory works in Live (expanded and collapsed each
return to their own place). Change after testing: opening the window always shows it expanded.
Closing while collapsed records the bar's position; the next open expands and moves Live's window
to the expanded position (Live would otherwise reopen it where the bar was). The saved
`collapsed` flag now only means "was closed collapsed".
Also hangs with Kontakt 8 alone (no KK) hosted in AltraPlugHost with an instrument loaded: same ni-platform stack. ni-platform.dll is NI's shared library that both KK and Kontakt load, so any plugin that loads it can trigger it.
User confirmed natively in Live: Kontakt 8 with an instrument hangs Live on exit every time; Kontakt 7 never does. A Kontakt 8 / NI bug; nothing for AltraPlugHost to fix.

**Snap to a bottom row (2026-11-20, not yet tested in Live).** Toolbar checkbox "Snap", on by
default, saved per instance. On: Collapse moves the bar into the first free slot along the bottom
of the work area of the monitor the plug-in window was on, left to right, then rows above. Off:
the old remembered-position behaviour. Collapsed bars have a fixed width (250) so they line up and
Live's title mostly fits. Instances find each other's bars through a window property
(`AltraPlugHost.CollapsedBar`) on Live's window, so the FX and Instrument DLLs share one row. A
slot is taken when a tagged window's visible top-left (DWM frame bounds, without the invisible
resize border) is there: restoring or dragging a bar away frees its slot, and the others stay put.

**Slim collapsed bar (2026-11-20, not yet tested in Live).** Toolbar checkbox "Slim", on by
default, saved per instance. While collapsed, `native::FramelessWindow` strips `WS_CAPTION |
WS_THICKFRAME` from Live's plug-in window and sizes it to exactly our 300x34 bar. It subclasses the
window to block Live putting the frame back (`WM_STYLECHANGING`) or resizing it
(`WM_WINDOWPOSCHANGING`). The bar shows swatch, "plugin - track" (fixed font, ellipsis), Restore
and a close cross (`WM_SYSCOMMAND SC_CLOSE`, as Live's close button). Pressing the bar starts a
window move (`WM_NCLBUTTONDOWN HTCAPTION`). Restore puts the frame back before expanding, so Live
sizes the expanded window with its frame. To check in Live: that Live doesn't re-frame or
misplace the window, and that the close cross behaves exactly like Live's close button.
Slim bar works in Live (2026-11-20). Snap is now a drop-down: Snap bottom (default), Snap top (row along the top of the work area, further rows below), No snap. Saved as `snapMode`; older Sets' `snap` on/off maps to bottom/off.
Snap mode is now one global setting (all instances, both DLLs), kept in
`%APPDATA%\AltraPlugHost\AltraPlugHost.settings` (JUCE `PropertiesFile`, read fresh each time) until
the user picks another; no longer saved per instance in the Set. Also: whenever the expanded window
changes size (plug-in loaded, restored, reopened), it is moved just enough to be fully inside the
monitor's work area (`native::keepWindowOnScreen`), since Live grows windows right and down.

**Fit button (2026-11-20, not yet tested in Live).** Some plug-ins (Moog, activation screen on first
load) change size without calling `IPlugFrame::resizeView`, so the window stays at the old size.
Fit calls `setScaleFactor (1.0f)` on the hosted editor: JUCE's `VST3PluginWindow` then re-reads
`IPlugView::getSize` (`resizeToFit`), and our window follows. Logged as "fit to plugin size: A -> B".
If a plug-in doesn't report its new size through `getSize` either, this does nothing (B == A) and
the fallback would be measuring the plug-in's own child window, or rebuilding its editor.
Fit result in Live with Moog MF-105S: `getSize` still reported the old 384x334, so re-reading does
nothing. (In DockTest, standalone and inside AltraPlugHost, the activation screen opens correctly at
768x592; in Live the plug-in reported its normal UI size when the editor was built and grew later.)
Non-resizable VST3 views can't be enlarged from outside: JUCE's `VST3PluginWindow` snaps back to
`getSize`. So Fit now falls back to rebuilding the hosted editor (`rebuildHostedEditor`) when the
re-read size is unchanged; a new editor reports the size the plug-in needs now. Default window
width raised to 720 so the toolbar fits.

**Moog activation screen cut off: cause found (2026-11-20).** Not a sizing bug of ours. Moog's
plug-ins (JUCE) check which program they run in, and in "Ableton Live 12 Suite.exe" they make
their window 384x334 while the activation screen is laid out for 768x592; the plug-in's own
innermost window in Live is 384x334. Proof: DockTest hosting AltraPlugHost + MF-101S gets 768x592;
the same DockTest.exe copied as "Ableton Live 12 Suite.exe" gets 384x334. Fit can't help (the
plug-in reports 384x334, also when rebuilt). Possible fix: make hosted plug-ins see AltraPlugHost
rather than Live as their host (patch their `GetModuleFileNameW` import), which is arguably right
because we, not Live, host them, but it would also disable Live-specific behaviour in every hosted
plug-in. Not done; workaround is to activate the plug-in once outside AltraPlugHost.

**Moog cut-off: real cause and fix (2026-11-20).** The user found Moog opens correctly directly in
Live, so it was us. JUCE plug-ins (the plug-in side, `juce_audio_plugin_client_VST3.cpp`) return
`kResultFalse` from `IPlugView::getSize` in Live until they are attached to a window, and cache the
first size they report. We built the hosted editor into the hidden parking window first and then
docked it; attached there, the resizable Moog editor settled at its minimum size (384x334). DockTest
(as "Ableton Live 12 Suite.exe", `--raw ... probe-park`) reproduced it: hidden first = 384x334,
straight into a visible window = 768x592. Fix: `buildHostedEditor (slot)` builds the editor straight
into the visible slot; the parking window is only used for parking afterwards. Verified in DockTest
as Live (768x592) and KK docking (3 cycles). Fit button and `rebuildHostedEditor` removed.

**Toolbar redesign (2026-11-20).** Expanded toolbar: track swatch, (empty label carrying the
latency/parameter tooltip), Browse, three arrow icon buttons (up: collapse to the top row, down:
bottom row, left: floating bar with remembered position), and a burger menu whose only item is
"Slim collapsed bar". Unload, the snap drop-down and the Slim checkbox are gone. The snap position
is now chosen per collapse by the button used (`WrapperProcessor::collapseMode`), so the global
snap setting file is gone too. Collapsed bar: swatch, name, Restore, close. Minimum width 260.

## Multi-channel MIDI (receivers)

**Problem.** When a Live track takes another track's plug-in as "MIDI From", Live merges all MIDI
channels into one, and clips store no channel anyway. So a plug-in that sends MIDI on several
channels (Maschine, set up to) can't be recorded channel by channel on separate tracks. Confirmed
by the user: there is only a device choice, no channel choice, and the channel doesn't survive.

**Design.** AltraPlugHost (the server) publishes the hosted plug-in's MIDI output, channels
intact, to a new plug-in, **AltraPlugHost Receiver**. Each receiver subscribes to one server and
one channel and outputs only that channel. A recording track takes "MIDI From" the receiver.
Per channel that is a receiver plus a recording track (Live records a track's input, not its own
plug-in's output).

- **Transport:** `Source/MidiBus.*`. A named shared-memory block per process
  (`Local\AltraPlugHost.MidiBus.<pid>`), so the separate DLLs share it. 32 server slots, each
  with a ring of 4096 channel messages (SysEx skipped). The server never blocks; receivers keep
  their own read position and detect overruns. The layout size is part of the magic number, so
  mismatched builds leave the bus off instead of misreading it.
- **Identity:** each server has a random 64-bit ID saved with the Set (`busId`); receivers save
  the server ID and channel and reconnect whenever both exist. A duplicated track keeps its own
  new ID. The receiver's menu shows "<hosted plug-in> / <track>".
- **Ordering, the hard part:** Live runs tracks in parallel, so a receiver may run before or after
  its server in an audio cycle. Plan: set the receiver track's "MIDI From" to the server track so
  Live must process the server first (the merged MIDI arriving that way is ignored); the receiver
  then reads the same block with no delay. If that doesn't hold, deliver consistently one block
  late instead of wobbling. Events from a block older than the newest go at offset 0.
- **Note hygiene:** the receiver tracks held notes and sends note-offs when its server or channel
  changes or the server disappears.
- Both AltraPlugHost variants now also output the hosted plug-in's MIDI to Live
  (`NEEDS_MIDI_OUTPUT`), which also makes them selectable as "MIDI From".

**Timing warning.** The receiver compares the time since the server's newest block started with
the block period: under half a period means the server ran first in this cycle (same block),
otherwise the events reach us one block late. It reports the majority each half second; when late,
the receiver's window shows "MIDI arrives one block late. Set this track's MIDI From to <server
track>." Connects, disconnects and timing changes go to the lifecycle log; nothing per block.
`tools/MidiBusTest` checks the bus without Live (server first, receiver first, alternating, the
timing detection with real sleeps, note-off on channel change, overrun); built with
`ALTRA_BUILD_DOCKTEST`.

Questions the test build (2026-11-21, with per-block logging, since removed) answered:
1. Does Maschine's multi-channel output reach us with its channels?
2. Is the order stable, with and without the receiver track's "MIDI From" set to the server track?
   Does Live give a host time, the same for every plug-in in a cycle?
3. Does "MIDI From" list receivers inside one Instrument Rack's chains separately (one hub track)?
4. Does Live accept CC output from a VST3 (JUCE sends CCs as `LegacyMIDICCOutEvent`)?

**Results in Live (2026-11-21), Maschine 3 at 48 kHz / 256 samples.** Setup: track A AltraPlugHost
Instrument (Maschine), tracks B and D a receiver each (channels 3 and 2), MIDI From track A;
tracks C and E record MIDI From B and D, each with an instrument. Works.
1. Maschine's channels arrive intact (1, 2 and 3 seen separately); each receiver passed only its
   own. No events late, no overruns.
2. With the receiver track's MIDI From set to the server track, the receiver ran 0.6-2 ms after
   the server's block start, in the same cycle: on time. Without it, about 5 ms after (one block):
   it ran before the server every cycle, so one block late, but stable. Make the routing the
   documented setup; Live doesn't promise the unrouted order.
3. Receivers can share one hub track: put them in parallel chains of an Instrument Rack (expand
   the chain list and drop each into it). A recording track's MIDI From lists the rack, with each
   receiver in its submenu. Tested with three receivers (channels 1-3): works. In that test the
   racked receivers ran one block late (stable): the hub track's own MIDI From hadn't been set to
   the server track (confirmed by the user). Easy to forget, so the receiver should warn about it.
4. Live gives no host time (`kSystemTimeValid` never set), so it can't identify the cycle.
5. CCs reach Live from both the server and receivers. Maschine's mod wheel recorded in 1/16 steps
   either way. The log showed Maschine sends one value of a controller at most every 6,000 samples
   (125 ms = 1/16 at 120 BPM, at 48 kHz). A test sweep from the receiver (a value every 64
   samples) records smoothly in Live. So the steps are Maschine's output rate, not ours or Live's.
6. Spikes in Maschine's recorded mod wheel (values from the other groups' curves on channel 1,
   identical every loop; follows Group A when moved to channel 4). Traced: the spikes are already
   in Maschine's raw VST3 output events (traced inside JUCE's hosting, before conversion), with no
   CC going into Maschine; JUCE's conversion, the bus and the receivers pass every event unchanged
   (compared event by event). A VCV Rack 2 LFO on channel 1 records as a clean sine through the
   same path. Others report inconsistent CC output from Maschine. Not ours.
7. Earlier sparse/swapped-looking CCs with receivers in an Instrument Rack were already sparse in
   the server's input from Maschine (that Maschine project's state), not caused by the rack.

