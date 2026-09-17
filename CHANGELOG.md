# Changelog

All notable changes to DendroLog are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Directory Scanner: a **⟳ refresh button** that re-reads the scanned directory
  without rescanning it from scratch — only new and modified files are analysed
  again, a log that merely grew is read from where the last scan stopped, and
  deleted files leave the tree.
- Directory Scanner: **three refresh modes** on that button's context menu —
  *manual* (the button lights up when the directory changed on disk),
  *automatic* (a file-system watcher plus a configurable period; deferred while
  the panel is hidden) and *off*. The choice persists between sessions.

### Changed
- Directory Scanner sorts file and directory names **case-insensitively** and
  naturally, so `Alpha.log` no longer jumps above every lower-case name and
  `log2` sorts before `log10`.
- Directory Scanner content filter: files are read in chunks instead of line by
  line, on a dedicated I/O thread pool that no longer queues behind the file
  statistics analysis. Progress is now measured **in bytes**, so the bar tracks
  the real work rather than jumping when a file finishes, and cancellation takes
  effect mid-file.
- Directory Scanner content filter skips files the date filter already rejects
  and caches per-file verdicts, so re-applying the same query — or applying it
  again after a refresh — only reads what actually changed.
- Associated `.log` files now show their own document icon — a sheet of paper
  badged with the ring emblem — instead of the application icon, so log files
  are no longer indistinguishable from the DendroLog shortcut in Explorer.

### Fixed
- The generated `.ico` files again contain the 20 and 40 px images (the shell
  sizes used at 125 % display scaling); the ICO writer had been silently
  dropping them.
- Large logs (above the indexed-store threshold, 512 MB by default):
  **Find Previous** no longer re-reads up to 8 MB of the file for every line it
  steps back over, so searching backwards is as fast as searching forwards.

## [0.2.0] — 2026-07-17

### Added
- Large-file support: an indexed log store keeps multi-gigabyte logs on disk
  (~10 bytes of RAM per line) while scrolling, filtering and search stay
  responsive.
- Entry Details panel is now configurable and stateful: choose which sections
  to show (Header / Fields / Message / JSON); the choice persists.
- `project_configure.bat` auto-detects the installed Qt (override with
  `QT_PATH`).
- README: screenshot, directory scanner and large-file documentation.

### Changed
- Improved Log Fields parsing and reworked the schema editor.
- Double-click selection no longer treats quoted strings as a single token.

### Fixed
- Keyboard-navigation selection in the log list.

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
