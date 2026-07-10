<p align="center">
  <img src="resources/icons/dendrolog.svg" width="96" alt="DendroLog icon"/>
</p>

<h1 align="center">DendroLog</h1>

<p align="center">
  <b>A fast desktop viewer for log files</b> — multi-file tabs, structured field
  extraction, filtering, highlighting and live reload.<br/>
  <i>Dendrochronology reads time from tree rings; DendroLog reads it from your logs.</i>
</p>

<p align="center">
  <a href="https://github.com/scaremongr/dendrolog/actions/workflows/ci.yml"><img src="https://github.com/scaremongr/dendrolog/actions/workflows/ci.yml/badge.svg" alt="CI"/></a>
  <a href="https://github.com/scaremongr/dendrolog/releases/latest"><img src="https://img.shields.io/github/v/release/scaremongr/dendrolog" alt="Latest release"/></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT license"/></a>
</p>

<!-- TODO: screenshot/GIF — docs/screenshot.png -->

## Features

- **Multi-file tabs** — open several files into one tab, merged by timestamp.
- **Field schemas** — split entries into structured fields (timestamp, level,
  thread, message…); auto-detect from a sample line or import a **Grok** expression.
- **Filtering** — log level, time range, and an Include/Exclude rule builder
  with AND/OR, regex, per-field binding.
- **Row highlighters** — colour matching rows without hiding anything.
- **Timeline histogram** — entry density over time; click to jump, drag to filter.
- **Statistics panel** — level counts, top repeated messages, rate, anomalies.
- **Live reload** — re-read appended lines manually (`F5`) or automatically.
- **Smart text handling** — syntax highlighting, token-aware double-click
  selection, open URLs/paths from the log, per-line word wrap.
- **Save View As** — export exactly what you see (filters + field selection).
- Light/dark theme following the OS, fully configurable shortcuts, EN/RU help.

## Install

**Windows**
- Installer: grab `DendroLog-<version>-setup.exe` from the
  [latest release](https://github.com/scaremongr/dendrolog/releases/latest).
- Portable: unzip `DendroLog-<version>-win64-portable.zip` anywhere and run
  `DendroLog.exe` — settings stay next to the exe.

**Linux**
- Download `DendroLog-<version>-x86_64.AppImage`, `chmod +x`, run.

**macOS** — not packaged yet; builds from source with Qt 6.5+.

## Build from source

Requires CMake 3.16+ and Qt 6.5+ (Widgets, Concurrent, Svg, Network).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release   # smoke tests
```

On Windows, run `windeployqt` on the built `DendroLog.exe` to collect Qt DLLs.

## Portable mode

DendroLog stores settings in the user profile
(`%LOCALAPPDATA%/DendroLog` on Windows, `~/.config/DendroLog` on Linux).
If a file named `portable` (or an existing `DendroLog.ini`) sits next to the
executable, settings are kept there instead.

## License

[MIT](LICENSE) © Anton Petrov. Built with [Qt](https://www.qt.io) (LGPLv3,
dynamically linked).
