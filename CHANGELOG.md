# Changelog

All notable changes to DendroLog are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.4.0] — 2026-09-28

### Added
- A start screen replaces the empty frame while no tab is open: buttons to open
  files or scan a folder, a drop hint and the recent files as links.
- The log's context menu acts on selected text: **Find**, **Show Only Lines
  with…**, **Hide Lines with…** and **Highlight Lines with…**. Filters narrow
  what the tab shows now; rules the Text Filters panel holds but has not
  applied to the tab are neither applied nor overwritten.
- **Use as Time Filter Start / End** works without selecting a timestamp: it
  takes the time of the line under the cursor.
- Font size from the keyboard and mouse: `Ctrl` + wheel over the log,
  `Ctrl++`, `Ctrl+-`, and `Ctrl+0` for the default size (`View` menu).
- Quick search can match case (**Aa** next to the search box).
- **Find All**: the **All** button next to the toolbar find box
  (`Ctrl+Shift+F`) and **Find All “…”** in the log's context menu list every
  line with the text in the Search panel.
- Menu commands and shortcuts: **Close Tab** (`Ctrl+W`), **Exit** (`Ctrl+Q`),
  **Scan Directory…**, **Auto-reload This Tab** (`Ctrl+Shift+F5`), **Follow
  Tail** in the View menu, **Show All Levels** (`Alt+0`), **Reset Filters of
  This Tab** (`Ctrl+Shift+R`), **Field Schemas…**, and `Alt+1…Alt+6` for the
  level buttons. All of them are configurable in Settings → Shortcuts.
- **Whole Log** in the Time Range Filter panel, which also shows the time span
  of the log and whether the filter is on.
- Recent files keep 10 entries and can be cleared.

### Changed
- Text Filters is split in two panels instead of one panel with a
  "Non-destructive search" switch that was easy to miss: **Text Filters** hides
  the lines that do not match (Apply / Reset), **Search** finds lines and lists
  them while the log stays complete (Search / Clear). Each panel says in one
  line what it does and keeps its own profiles; both can be used together, and
  each one's Highlight colours its matches in the log. Rules saved by the
  previous version are available in both panels.
- The Search panel holds the query and the results together (the former Search
  Results panel): rules on the left and results on the right when docked at the
  bottom or floating, stacked when docked at a side. It takes the place of the
  Search Results panel in a saved layout, and tells how to start a search when
  none is running.
- A denser, uniform layout: one set of margins and spacings for the whole
  window instead of Fusion's roomy defaults. The log reaches the toolbar and
  the panels (the gap to a docked panel went from ~15 to 4 px), toolbars are
  27 px high instead of 35 with 16 px icons, tabs are lower, text buttons are
  as wide as their text rather than at least 80 px, and panels, cards,
  Statistics, Entry Details and the dialogs use the same tighter spacing.
- The toolbars are regrouped: open, reload, auto-reload, follow tail and word
  wrap; find; log levels; filter indicators. They now fit into one row of a
  default-sized window instead of pushing the filter indicators behind `»`.
  Buttons show icons and their tooltips show the current shortcut.
- Level buttons carry the level's colour, and a pressed one looks like a
  coloured chip, readable in the dark theme too.
- Auto-reload has its own toolbar toggle (right-clicking Reload still works),
  and tabs with auto-reload on are marked with an icon.
- The Time Range Filter fields start at the time span of the active tab's log
  instead of "yesterday–today", show seconds and milliseconds, and a bound left
  at the edge of the log stays open, so a growing log keeps showing new lines.
- The status bar says "Line 12 of 5 000 (filtered from 116 384)" with digit
  grouping, and "No lines match the filters" when everything is hidden.
- A tab with several files is named after the first one ("app.log +2").
- Long lists in the Text Filters, Row Highlighters and Log Fields panels scroll
  instead of stretching the panel past the window. A new rule or marker is
  scrolled into view with the cursor in its text field.
- Row Highlighters uses the same header as Text Filters and explains itself
  while empty; Log Fields puts the schema choice at the top.
- A not-found quick search turns the search box red.
- Settings: the Font and View tabs are merged into **Appearance**; the dialog
  is larger; shortcuts can be cleared with a button in the field, and a
  combination assigned to two commands is marked and must be resolved before
  OK.

### Fixed
- The Help tab of the field schema editor showed garbled characters (`â€”`)
  instead of dashes and symbols.
- Settings (`Ctrl+,`) had no shortcut on Windows although the help listed one.
- A level button click filtered the tab twice, and switching between tabs with
  different level filters filtered the shown tab again — on a large indexed log
  each of these is a full background pass.
- Applying a time range whose From is later than To silently removed the time
  filter; now nothing changes and the panel explains why.
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
- Filtering, statistics or the timeline running while a new field schema is
  being applied in the background could read fields in the middle of being
  rewritten. New fields are now computed aside and put in place at once;
  cancelling a schema switch (for example, by picking another schema) no
  longer leaves the entries half in the old schema and half in the new one.
- Search Results over a large indexed log honours the text cache budget from
  Settings instead of always using 256 MB.
- File system errors in Save View As are shown in the system language instead
  of garbled characters on non-English Windows.
- Timestamps with an explicit time zone are read as the moment they denote:
  `Z`, `+02:00`, `+0200` or `+02` right after the time, and `+02:00`, `+0200`,
  `UTC` or `GMT` after a space. The zone used to be ignored and the time taken
  as local, which misordered merged files from different zones and shifted the
  time filter and the timeline. Timestamps without a zone are still local time.
- Fractional seconds are read as fractions: `,7` is 700 ms, not 7 ms.
  Timestamps with microseconds or nanoseconds are recognised (lines with more
  than three fraction digits used to get no timestamp at all), and
  `dd.MM.yyyy` / `dd/MM/yyyy` timestamps keep their fraction and zone too.
- A schema Timestamp field now includes a zone glued to the time, so lines
  such as `2026-09-20T10:00:00Z INFO …` match the schema.
- Search Results keeps up with a growing log: appended lines are searched as
  they arrive and added to the results, instead of every appended batch
  starting the whole search over — on a large growing log the panel could stay
  "Searching" forever. A line that was still being written is checked again
  once it is complete. Changing the Log Fields selection updates the results
  panel at once.
- Lines appended to a merged indexed tab without filters are inserted in
  place instead of resetting the view on every batch, so the selection and
  scroll position survive a growing log.
- Quick search (`F3` / `Shift+F3`) runs in the background: the window no longer
  freezes while a huge log without matches is read from disk. A long search
  shows its progress in the status bar, `Esc` in the search box cancels it, and
  a term that is not found is reported there.
- Save View As writes in the background: the window no longer freezes while a
  large view is saved. The status bar shows the progress, **Cancel Save** stops
  the export and keeps an existing destination file as it was, and the saved
  rows are the view as it was when saving started.
- With word wrap on, Down, PageDown and End keep the current line on screen,
  and the last line can be scrolled into view: the view no longer positions
  itself by estimated line heights that change once the lines are drawn.
- A text filter rule bound to a Log Fields column that is not in the current
  schema no longer silently searches the whole row: it is ignored, does not
  highlight, and its card explains why. With Filter blocks off, a card bound to
  a column notes that it searches the entire row.
- A log that grows while open (below the indexed threshold) now reads its tail
  like a large one: a multi-line record written in parts stays one record, so
  its continuation lines keep the record's level and time and Entry Details
  shows it whole; line numbers continue instead of restarting at 1 for every
  appended batch; a line the writer has not finished yet is replaced by its full
  text once completed instead of being split in two; blank appended lines are
  kept; and lines written while the log was being read are neither skipped nor
  read twice.
- Adding a second file to a tab with a large log whose lines are not in time
  order is much faster: the first file is reordered record by record from a
  lock-free snapshot of its index instead of comparing every pair of lines
  under the index lock.
- Lines appended to a large (indexed) log no longer show up empty when the end
  of the file had already been displayed: the cached last block of the file is
  read again once the file grows.

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
