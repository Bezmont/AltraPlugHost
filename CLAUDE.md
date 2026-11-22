# AltraPlugHost

A JUCE-based VST3 wrapper plugin for Ableton Live (Windows first). It hosts other VST3
plugins and keeps their editor windows alive (hidden or shown, never destroyed). This avoids
the expensive editor rebuild and UI freeze that happen every time a heavy plugin's window is
reopened in Live.

**Read `docs/DESIGN.md` first.** It records the problem, the rejected Max for Live approach,
and the core architecture: the hosted editor lives in a top-level window owned by the
`AudioProcessor`, not the `AudioProcessorEditor`.

## Status

- **PoC proven in Live (2026-11-19)** with Komplete Kontrol: the hosted window survives closing
  the wrapper, and Show after Hide is instant. Results and the minimise problem are in
  `docs/DESIGN.md` ("PoC results"). Build steps: `docs/BUILD.md`.
- Done: git repo, JUCE 9.0.6 submodule, CMake with two variants (FX `AltraPlugHost FX`, instrument
  `AltraPlugHost Instrument`), load menu, Show/Hide, always-on-top, audio/MIDI pass-through,
  latency reporting, state save/restore (keeps the state of a plugin that fails to load).
- All six PoC criteria pass, including latency compensation and Set save/reload. MIDI only
  lightly tested.
- In progress (not yet tested in Live): screenshot plugin browser built on PlugBrowser's catalog
  export, with the old folder menu as fallback. See "Plugin browser" in DESIGN.md. The export
  side is on PlugBrowser's `catalog-export` branch (uncommitted).
- Works in Live (2026-11-20): hosted editor docked inside AltraPlugHost's own window in Live,
  with a toolbar, parked in a hidden window when Live closes ours (repeated close/reopen with
  Komplete Kontrol is instant and never blank).
- Works in Live (2026-11-20): pop-out removed. Toolbar: Browse, three collapse buttons (up/down
  snap the bar into a row at the top/bottom of the screen, left = floating bar with remembered
  position), options menu (Slim bar). Collapsed: slim bar without Live's title bar (swatch, name,
  Restore, close). Live's window title shows "<plugin>/<track>". Window kept on screen. New editors
  are built straight into the visible window (fixes Moog plugins shrinking in Live). See DESIGN.md
  from "Collapse instead of pop-out" on.
- Works in Live (2026-11-21): multi-channel MIDI. AltraPlugHost passes the hosted plugin's MIDI
  out to Live and onto a shared-memory bus; **AltraPlugHost Receiver** (third plugin) outputs one
  channel of one AltraPlugHost, so each channel can be recorded on its own track (Live merges
  channels otherwise). Receivers can share a track in an Instrument Rack. The receiver track's
  MIDI From must be the AltraPlugHost track (else one block late; the receiver warns). See
  "Multi-channel MIDI (receivers)" in DESIGN.md. Maschine's own CC output is uneven (spikes on
  channel 1); traced to Maschine, not ours.
- Live hanging on exit is a Kontakt 8 / NI bug (ni-platform.dll), not ours; confirmed natively.
  The lifecycle log (`Diagnostics.*`) stays as a diagnostic.
- Next for the real implementation: fix the `kIoChanged` re-prepare race and minimise behaviour
  (both in DESIGN.md), then: pre-build the editor on load
  (option), "show editor" parameter, sidechain/multi-bus.
- Works in Live (2026-11-20): parameter bank, 4096 automation slots (`ALTRA_PARAMETER_SLOTS`; don't lower it, saved lanes would break) mapped onto the hosted
  plugin's parameters (record, playback, Configure, names). See "Parameter bank" in DESIGN.md.
- GitHub repo `Bezmont/AltraPlugHost` (`origin`), pushed 2026-11-21 to become public, under AGPLv3 (JUCE's
  open-source licence). Public-facing docs: `README.md`. Everything committed is public.
- Next: GitHub Actions build that produces downloadable VST3s. Windows (x64) only until the user
  has a Mac.

## Code map

- `Source/PluginProcessor.*`: `WrapperProcessor`. Owns the hosted instance and its window.
  Threading rules are in the class comment.
- `Source/HostedEditorWindow.*`: the processor's hidden window where the hosted editor is parked
  while AltraPlugHost's window is closed or collapsed.
- `Source/NativeWindowHelpers.*`: Win32 helpers on Live's plug-in window: destruction watcher,
  title replacement, positions, bottom/top rows of collapsed bars, frameless (slim) bar, keeping
  the window on screen.
- `Source/Diagnostics.*`: lifecycle log, `%LOCALAPPDATA%\AltraPlugHost\lifecycle.log`.
- `Source/PluginEditor.*`: AltraPlugHost's window in Live: toolbar plus the docked hosted
  editor, the browser, or a status message. Parks the hosted editor in its destructor.
- `Source/PluginCatalog.*`: reads PlugBrowser's catalog export (`plugins.json`).
- `Source/PluginBrowser.*`: screenshot grid for picking a plugin, and the thumbnail cache.
- `Source/ParameterBank.*`: the fixed automation slots that stand in for the hosted plugin's
  parameters.
- `Source/MidiBus.*`: shared-memory MIDI bus between AltraPlugHost (publisher) and receivers.
- `Source/Receiver.*`: the AltraPlugHost Receiver plugin (separate CMake target).
- `tools/MidiBusTest`: console test of the bus, built with `ALTRA_BUILD_DOCKTEST`.

## Gotchas

- **Don't use JUCE 9's `TopLevelWindow::setAssociatedComponent()` with our own editor.** It makes
  the window's owner the top-level window containing that component, which is Live's plug-in
  window. Windows destroys owned windows with their owner, so closing the wrapper in Live
  would destroy the hosted editor. The parking window has no owner.
- **Build a new hosted editor straight into the visible slot, not the hidden parking window.**
  JUCE plug-ins can't report their size in Live until attached; attached hidden first, resizable
  ones settle at their minimum size (Moog).
- **Park the hosted editor before our native window is destroyed, never during.** Parking on
  `WM_DESTROY` leaves plugins drawing white. See `TeardownSentinel` in `PluginEditor.h`.
- Test docking changes with `tools/DockTest` (see `docs/BUILD.md`) before asking the user to
  test in Live.
- CMake isn't on PATH. Use the copy bundled with VS 2022 (path in `docs/BUILD.md`).

## Testing

- Test plugin: Komplete Kontrol (the slowest GUI the user has). The user tests in Live.
- Live loads our builds through junctions in `C:\Program Files\Common Files\VST3` that point at
  `build/VST3` (Live allows only one custom VST3 folder). See `docs/BUILD.md`.

## Conventions

- Platform: Windows 11. Shells: PowerShell and Git Bash.
- Commit or push only when the user asks.
