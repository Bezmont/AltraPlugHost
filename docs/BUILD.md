# Building AltraPlugHost

## Requirements

- Visual Studio 2022 with the "Desktop development with C++" workload (MSVC 14.4x, Windows SDK 10.0.26100).
- CMake 3.22 or later. The copy bundled with Visual Studio works:
  `C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
- Git, for the JUCE submodule (`external/JUCE`, pinned to tag 9.0.6).

## Build

```
git submodule update --init
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

Or open the folder in Visual Studio ("Open a local folder") and let it use CMake directly.

The build produces two plugins, both copied to `build/VST3/`:

| Plugin                     | Use it for                       |
|----------------------------|----------------------------------|
| `AltraPlugHost.vst3`       | hosting audio effects            |
| `AltraPlugHost Instrument.vst3` | hosting instruments (Live only puts instruments in the instrument slot) |

Close Live before rebuilding; Windows won't overwrite a plugin DLL that Live has loaded.

## Loading in Live

Live allows only one custom VST3 folder, so `C:\Program Files\Common Files\VST3` contains
directory junctions pointing at the build output (set up once, 2026-11-19, with admin rights):

```
C:\Program Files\Common Files\VST3\AltraPlugHost.vst3            -> D:\git\AltraPlugHost\build\VST3\AltraPlugHost.vst3
C:\Program Files\Common Files\VST3\AltraPlugHost Instrument.vst3 -> D:\git\AltraPlugHost\build\VST3\AltraPlugHost Instrument.vst3
```

A Release build updates what Live loads, with no copying. To recreate them, in an elevated PowerShell:

```
$vst3 = 'C:\Program Files\Common Files\VST3'; $build = 'D:\git\AltraPlugHost\build\VST3'
foreach ($n in 'AltraPlugHost.vst3', 'AltraPlugHost Instrument.vst3') {
    New-Item -ItemType Junction -Path "$vst3\$n" -Target "$build\$n"
}
```

If you delete the `build` folder, the junctions point nowhere until the next Release build.
Rescan plug-ins in Live after the first install.

## PoC test checklist

See "Proof-of-concept success criteria" in `DESIGN.md`. Also check:

- Close the AltraPlugHost window in Live, then use the hosted window: it must still work.
- Minimise Live: the hosted window should hide with it, and come back with it.
- The hosted window should not have its own taskbar button.
- Delete the device and undo the deletion; close the Set with the hosted window open.
- With Live 12 plug-in sandboxing on and off.
