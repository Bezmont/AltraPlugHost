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
- Done: git repo, JUCE 9.0.6 submodule, CMake with two variants (FX `AltraPlugHost`, instrument
  `AltraPlugHost Instrument`), load menu, Show/Hide, always-on-top, audio/MIDI pass-through,
  latency reporting, state save/restore (keeps the state of a plugin that fails to load).
- All six PoC criteria pass, including latency compensation and Set save/reload. MIDI only
  lightly tested.
- Next for the real implementation: fix the `kIoChanged` re-prepare race and minimise behaviour
  (both in DESIGN.md), then: pre-build the editor on load
  (option), "show editor" parameter, parameter proxy bank, sidechain/multi-bus.
- Repo will become a **private** GitHub repo named `AltraPlugHost`. Confirm with the user before
  running `gh repo create --private`.

## Code map

- `Source/PluginProcessor.*`: `WrapperProcessor`. Owns the hosted instance and its window.
  Threading rules are in the class comment.
- `Source/HostedEditorWindow.*`: processor-owned `DocumentWindow`; closing hides it.
- `Source/NativeWindowHelpers.*`: Win32 owner window (sets Live's main window as owner).
- `Source/PluginEditor.*`: the small wrapper editor Live creates and destroys.

## Gotchas

- **Don't use JUCE 9's `TopLevelWindow::setAssociatedComponent()` with our own editor.** It makes
  the window's owner the top-level window containing that component, which is Live's plug-in
  window. Windows destroys owned windows with their owner, so closing the wrapper in Live
  would destroy the hosted editor. We set Live's *main* window as owner manually instead.
- CMake isn't on PATH. Use the copy bundled with VS 2022 (path in `docs/BUILD.md`).

## Testing

- Test plugin: Komplete Kontrol (the slowest GUI the user has). The user tests in Live.
- Live loads our builds through junctions in `C:\Program Files\Common Files\VST3` that point at
  `build/VST3` (Live allows only one custom VST3 folder). See `docs/BUILD.md`.

## Conventions

- Platform: Windows 11. Shells: PowerShell and Git Bash.
- Commit or push only when the user asks.
