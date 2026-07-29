# subtitletool-qt

A personal Qt6/C++ subtitle editor for Windows. Edits subtitle tables stored as
`.xlsx` (Start/End/Speaker/Source/Target), with a waveform/spectrogram view
driven by sibling audio files, and exports to SRT.

This is a personal tool — small, opinionated, not meant for general use.

## Features

- Editable table of subtitle rows with computed Duration / Gap / CPS / MaxLen
  columns. Cells turn red when limits (duration, gap, CPS, max line length) are
  exceeded.
- Multi-file workspace with a file list; each file remembers its own state.
- Waveform and spectrogram rendering of the matching audio file (auto-detected
  by leading number in the xlsx name, e.g. `06_...xlsx` → `../m4a/06.m4a`).
- Audio playback with cursor sync.
- Find/Replace, rule checks, time shift, duration auto-fix, split/merge rows.
- Export SRT (single file, silent) and Export SRT All (one `.srt` per open file,
  reports the count). Both write next to the source `.xlsx`.
- Load/save dialogs remember their last-used directory (persisted in the
  registry via `QSettings`).

## Build (Windows, MSVC + vcpkg)

```
x64build.bat
```

`x64build.bat` initializes the MSVC x64 environment, ensures Qt6 Multimedia is
installed via vcpkg (one-time, slow), configures CMake with the `default`
preset (Ninja + vcpkg toolchain, static triplet, Release), and builds
`build/subtitletool.exe`.

Prerequisites:
- Visual Studio 2026 (v18) with C++ (MSVC) and the Ninja bundled under
  `Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja`.
- vcpkg at `C:\PIG\vcpkg` with Qt6 Widgets + Multimedia, xlnt, and ffmpeg
  (libav) available for the `x64-windows-static` triplet. The project-local
  overlay triplets in `cmake/triplets/` force `VCPKG_BUILD_TYPE=release` to
  skip the Debug variant (the ffmpeg Debug backend fails under this toolchain).

See `CMakePresets.json` for the configured presets.

## Layout

```
src/
  main.cpp, MainWindow.{h,cpp}   app shell, toolbar, table view, shortcuts
  models/                         SubtitleRow/Model, FileTab, AudioData
  helpers/                        Decimal, CharCount, ExcelHelper (xlnt), etc.
  dialogs/                        Find/Replace, RuleCheck, ShiftTime, Settings
  widgets/                        WaveformWidget
cmake/triplets/                   vcpkg overlay triplets (release-only)
```

## Notes

- SRT export writes UTF-8 (no BOM) with CRLF line endings.
- Settings (last-used dirs) live in the registry under `HKCU\Software\subtitletool\subtitletool`.
