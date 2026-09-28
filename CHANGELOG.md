# Changelog

All notable changes to DendroLog are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed
- Search Results now searches every visible row of large indexed documents,
  including matches beyond the former 200,000-row input limit. Search runs in
  the background, keeps text on disk, and reports progress and the search scope.
  Replacing a query, clearing results or switching tabs discards stale results.
- Save View As protects open log files by filesystem identity, including
  differently cased paths, symbolic links, Windows shortcuts and hard links.
  The destination is checked again before publishing the export.
- Save View As writes to a temporary file and only replaces the destination
  after successful completion. Write and commit failures are reported instead
  of displaying a misleading success message; failed exports preserve an
  existing destination.
- Changing the field schema or turning field extraction off while a large tab
  was still filtering in the background could crash the application.
- Search Results over a large indexed log honours the text cache budget from
  Settings instead of always using 256 MB.
- File system errors in Save View As are shown in the system language instead
  of garbled characters on non-English Windows.

## [0.3.0] — 2026-09-19

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
- Large logs: a tab that received several files at once — opening them
  together, merging tabs, or adding a large file to a tab that already had
  files — **showed no lines at all**. It now shows every file merged by time.
- Large logs: a tab merging several files is now **ordered correctly by time**
  even when a file starts with a header without timestamps or has lines out of
  time order; lines used to land in arbitrary places.
- Large logs whose lines are not in time order: after a filter change the view
  now finds the right row for the previously selected entry instead of losing it
  or landing on a neighbouring row.
- **Filtering no longer throws the view back to the top of the file.** When a
  filter hid the selected entry and the window then regained focus — Alt+Tab, a
  closed dialog, focus returning from a panel — Qt quietly made a row near the
  top of the list current without selecting it, and the view mistook that for
  your choice: the next filter change restored *that* row and scrolled to it.
  The same happened when you scrolled through a log without selecting anything
  at all. The view now ignores a current row it did not get from you, so your
  entry (or, with nothing selected, your scroll position) survives filtering.
- Switching which Log Fields columns are shown keeps the viewport where it was
  instead of scrolling to a row you never selected.
- The first keyboard move in a freshly opened log starts at the first line
  rather than skipping to the second.

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
