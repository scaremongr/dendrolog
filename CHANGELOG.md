# Changelog

All notable changes to DendroLog are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.0] — 2026-07-10

First public release under the new name **DendroLog** (previously an unnamed
"log viewer").

### Added
- Multi-file tabs merged by timestamp, directory scanner, recent files.
- Field schemas with auto-detection and Grok import; per-field filters.
- Include/Exclude filter builder, log-level and time-range filters.
- Row highlighters, timeline histogram, statistics and entry-details panels.
- Non-destructive search results panel; syntax highlighting; smart selection.
- Live reload (manual and per-tab auto-reload); Save View As.
- Light/dark OS theme, configurable shortcuts, EN/RU built-in help.
- Application icon, Help menu with About dialog and update check.
- Single-instance mode: opening a file re-uses the running window
  (`--new-instance` opts out); drag & drop files onto the window.
- Portable mode (`portable` marker next to the exe); settings otherwise live
  in the user profile and migrate automatically from old `LogViewer.ini`.
- Windows installer (Inno Setup), portable ZIP and Linux AppImage built by CI.
