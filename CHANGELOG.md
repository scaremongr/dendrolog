# Changelog

All notable changes to DendroLog are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- **Previous session and closed tabs.** On a normal exit the main window
  remembers its tabs (`last-session.json` in the settings folder; a window
  closed without tabs keeps the previous one). **Settings → General → Sessions
  → Restore the tabs of the previous session on startup** (off by default)
  reopens them at startup before the command-line files; when it is off, the
  start screen offers **Restore previous session**. Windows opened with
  `--new-instance` or reading stdin neither restore nor overwrite it.
  **`Ctrl+Shift+T`** (**File → Reopen Closed Tab**) brings back the last closed
  tabs (up to 10) at their place with files, filters, highlighters,
  bookmarks, position and encoding choice; missing or rewritten files are
  reported in the status bar. A tab opened from a session whose files are all
  missing is no longer taken over by the next file opened.
- **Sessions.** **File → Save Session As…** writes the whole window to a
  `*.dlsession` file: every tab with its files (a merged tab keeps all of
  them), filters, row highlighters, bookmarks with notes, the selected line
  and its place on screen, auto-reload and a hand-picked encoding, plus the
  active tab, the field schema and the column selection. **File → Open
  Session…** (or a `.dlsession` passed on the command line, sent to the open
  window or dropped into it) adds the session's tabs next to the open ones.
  A session moved together with its logs to another folder or machine finds
  them by their path relative to the session file. Opening asks nothing: a
  missing file is skipped, a changed one opens clean with its bookmarks
  greyed out, and a status-bar link opens the **Session Report**, where
  **Locate File…** points a missing file at its new place (only the same file
  is accepted). The session's schema is selected if the schema folder has it,
  otherwise added as “(from session)”. Standard input is not saved.
- **File memory.** A file opened again gets back its bookmarks with notes, row
  highlighters and position (the selected line at the same place on screen, or
  following the tail), auto-reload and an encoding chosen by hand — however it
  is opened: command line, drag and drop, Recent Files, Directory Scanner. Its
  filters are not applied by themselves: a bar above the log offers
  **Restore Filters** for one click. A file added to a tab as the second one
  gets only its bookmarks. A rotated, moved or unpacked-elsewhere file
  (`app.log` → `app.log.1`) finds its memory by its content; a rewritten file
  opens clean, its old bookmarks greyed out in the Bookmarks panel. The memory
  is written when a tab closes, on exit and 2 seconds after a bookmark edit;
  it lives in the `memory` folder of the settings folder (paths of the logs and
  fragments of bookmarked lines are stored there), keeps up to 200 files
  (files with bookmarks are never dropped) and can be turned off or cleared in
  **Settings → General → File memory**. Two windows on the same file merge
  their bookmarks instead of wiping each other's.
- **Navigation history.** `Alt+←` (`View → Navigate Back`) returns to the
  place you jumped from — by Go to, the timeline, Statistics, a Search result,
  the quick search, next error, a bookmark or the "↓ N new lines" badge;
  `Alt+→` goes forward again, as in a browser. The mouse side buttons work too.
  A run of `F3` (or `F8`) presses without moving in between is one step back.
  Each tab keeps its own history of up to 100 places; it survives filters
  (a hidden place leads to the nearest shown line), tail updates, a reload of
  the same content and the switch to the large-file mode, skips places whose
  line is gone, and is dropped with the tab.
- A newly added default shortcut no longer takes away a key you already
  assigned to another command: the key stays with your command.
- **Marks on the scrollbar.** The vertical scrollbar of a log shows where the
  shown lines with errors (Error and Fatal) and warnings are, where the
  bookmarks are and, on the tab the Search panel searches, where its matches
  are — without scrolling. A level mark stands for the first line of an entry,
  the line `F8` goes to; a line on screen has its mark inside the scrollbar
  handle. Marks follow filters, tail updates, reloads, added files, window
  resizing and bookmark changes. They are computed in the background, so a
  huge or fast-growing log stays as responsive as before.
- **Bookmarks with notes.** `Ctrl+F2` bookmarks the selected line (or use
  **Add Bookmark** in a line's context menu); a ribbon in the left margin marks
  it. `F2` / `Shift+F2` jump to the next / previous bookmark among the shown
  lines, continuing from the other end of the tab. The **Bookmarks** panel
  (`Ctrl+F11`, `View` menu) lists the bookmarks of the current tab with file,
  line, the start of the line's text and a note you can edit in place; click a
  bookmark to go to it, remove one with ✕ or all with **Clear All**. Bookmarks
  stay through filters (a bookmark on a hidden line is marked "hidden by
  filter" and leads to the nearest shown line), through tail updates, a full
  reload of the file and the switch of a growing file to the large-file mode.
  If the file is rewritten and the line is gone, the bookmark stays in the
  panel with its text and note, marked "line not found", instead of moving to
  another line. Bookmarks are kept between runs (see *File memory*), so
  **Clear All** now asks for confirmation.
- **Logs not in UTF-8 are readable, and the encoding can be chosen by hand.**
  A log in Windows-1251, IBM866 (cp866), KOI8-R or Windows-1252 is now shown
  with its real text instead of `�`, and Cyrillic in it is found by quick
  find, Text Filters and the Search panel, case-insensitive too. This works the
  same for small files and for files above the large-file threshold, and a
  growing log keeps its encoding when it crosses the threshold. The encoding is
  detected by the file itself at its first non-ASCII line, wherever that is,
  also when it arrives as an appended tail. While a file has only a little
  non-ASCII text so far (a stray broken byte, a single word), the guess stays
  open: when more text further on or in the appended tail shows another
  encoding, the file is reread in it, keeping the selected line and its place
  on screen — a UTF-8 log that starts with one broken byte is no longer shown
  as garbage. Every file of a tab with several files keeps its own encoding.
  The **status bar** shows the encoding of the current line's file and where
  it comes from (auto, provisional, byte-order mark, manual); click it to
  choose UTF-8, Windows-1251, IBM866, KOI8-R or Windows-1252, or **Detect
  automatically** to go back to the detected one. A large file is not
  re-indexed for that, and the selected line, its place on screen, bookmarks
  and the navigation history stay. The choice lasts while the tab is open,
  also through reloads and merging tabs. **Settings → General → Non-UTF-8
  files** sets the fallback encoding (by default, by the system language).
  `--encoding <name>` (`cp1251`, `cp866`, `koi8-r`, …) opens the files of the
  command line or a piped stream in that encoding, also through an already
  open window; an unknown name is reported instead of being read as UTF-8.
  **Save View As** writes the text as shown, in UTF-8.
  The **Directory Scanner** content filter reads each file in the same
  encoding a tab would: Cyrillic in Windows-1251, IBM866, KOI8-R and any text
  in UTF-16 is found now (before, files were searched as UTF-8 only), with the
  same fallback encoding as tabs; changing it re-checks only the files that
  depend on it. Searching a directory of UTF-8 logs is no slower.

### Changed
- **Show/Hide Directory Scanner panel** moved from `Ctrl+F2` to `Ctrl+F10`:
  `Ctrl+F2` now adds or removes a bookmark. A shortcut you set yourself in
  Settings is kept.

### Fixed
- Timestamps before 1970 behave the same in large files as in small ones.
  Above the large-file threshold (and in tabs that crossed it) a record dated
  before 1 January 1970 — or one whose time zone puts it just before that
  moment — was treated as having no timestamp: it sorted to the end of a
  merged tab, disappeared under a time filter, was skipped by Go to Time and
  left out of the timeline and statistics. Such records now keep their time
  on both paths, and the timeline, statistics and Go to Time also handle
  them in small files. Dates from year 0001 to 9999 work.
- **Find shortcuts with the Find toolbar hidden.** Hiding the Find toolbar
  (right-click on the toolbar area) also switched off **F3**, **Shift+F3**,
  **Ctrl+Shift+F** and the **Alt+C / Alt+W / Alt+R** search toggles; they now
  work whatever toolbars are shown and bring the hidden Find toolbar back, and
  **Ctrl+F** does the same with the cursor in the search field.
- Files opened from the **Directory Scanner** now appear in **Recent Files**,
  like files opened any other way.
- Regular expressions understand letters and digits of any alphabet: `\w`,
  `\b`, `\s` and classes like `[[:alpha:]]` now work on Cyrillic and other
  non-Latin text, whatever the file's encoding — `пользовател\w+` finds
  "пользователя", `\bошибка\b` finds the word. This applies everywhere a
  regex is typed: Text Filters, the Search panel, the toolbar find box and
  Find All, Row Highlighters, match highlighting and the Directory Scanner
  content filter, and they all agree. `\d` now also accepts non-Latin digits
  (such as Arabic-Indic). On plain English (ASCII) text queries find exactly
  what they found before. Accented Latin letters (`é`, `ü`) now count as
  letters too, so `\bcaf\b` no longer matches inside "café" and `\W`, `\D`
  no longer match non-Latin letters.
- Merging tabs (dragging one tab onto another, **Merge All Tabs**) no longer
  loses the bookmarks of the merged tab: they move to the receiving tab with
  their notes and find their lines as the files load.
- When a growing log crosses the large-file threshold, the selected line and
  its place on screen (or, without a selection, the top of the view) are kept.
  Before, the selection was dropped and the view jumped to the top of the file.
- A UTF-16 or UTF-32 file added to a tab with other files (or to a large-file
  tab) now says why it is not shown and what to do — open it in a tab of its
  own — instead of a bare "Failed to parse".
- A blank last line of a file is now shown in tabs below the large-file
  threshold, as it already was above it. Before, the line was dropped, and
  lines appended to the file after it got line numbers one too low. A lone
  carriage return inside a line is no longer removed from its text.
- An unfinished last line that ends with a carriage return (half of a
  `\r\n` not yet written) no longer shows the stray `\r` in large-file tabs:
  both modes show it the same way.
- A bookmark or a navigation-history place whose line was hidden by a filter
  when a rewritten file finished reloading, or when the file's encoding was
  changed, is found again by its line number and text, instead of becoming
  "line not found".
- A tab with level, time or text filters that switches to the large-file mode
  (a growing file passing the threshold) keeps filtering the lines it reads
  again; before, every line was shown until the filter was applied again.

## [0.5.0] — 2026-11-18

### Added
- **Go to Line / Time** (`Ctrl+G`, `View` menu): one box takes a line number
  of the file or a time (`10:02`, `10:02:03.250`, `2026-03-05 10:02`) and shows
  where the jump lands before you confirm. A line hidden by the filters leads
  to the nearest shown one; in a tab with several files you pick the file the
  line number belongs to.
- **Next / Previous Error** (`F8` / `Shift+F8`) and **Next / Previous
  Warning** (`F7` / `Shift+F7`), `View` menu: jump from the selected line to
  the nearest shown entry of level Error or Fatal (or Warn). A multi-line
  entry such as a stack trace counts once. Past the last one the jump
  continues from the other end of the tab and says so in the status bar.
  Lines hidden by filters are skipped. On a huge log the lookup runs in the
  background and reads no text from disk.
- **Whole word, regular expression and history in quick find.** Two toggles
  next to **Aa** in the toolbar find box: **ab** (`Alt+W`) finds the text only
  as a whole word, **.\*** (`Alt+R`) treats it as a regular expression with the
  same syntax as Text Filters; `Alt+C` toggles **Aa**. An invalid expression
  marks the box red and says where the error is. The toggles, **Aa** included,
  are remembered between runs, and **All** (`Ctrl+Shift+F`) carries them over
  to the Search panel. The down arrow in the find box (or the arrow at its
  right edge) lists the last 20 searches; picking one runs it again, and
  **Clear search history** forgets them.
- **↓ N new lines.** When the log grows while you read further up with Follow
  tail off, a badge at the bottom of the view counts the lines appended below
  the screen (only those the tab's filters show). Click it, or press `Alt+End`
  (`View → Go to New Lines`), to jump to the first new line; lines you scroll
  past drop out of the count, and the badge goes away at the bottom or when
  Follow tail is turned on.
- **Theme** in `Settings → Appearance`: **Same as system** (the default, follows
  the OS as before), **Light** or **Dark**. The choice applies at once to the
  whole window and is remembered.
- **Built-in field schemas** for common formats, in the list from the first
  start: Logback / Log4j, Spring Boot, nginx / Apache access (combined),
  nginx error, Syslog (RFC 3164), Syslog / journalctl (ISO timestamp),
  Serilog (console and file), Python logging (default and `basicConfig`).
  Stack traces stay inside their entry. They are ordinary schema files: an
  edited one is never overwritten, a deleted one does not come back, and a
  schema of yours with the same name is left alone; a new version updates
  only the built-in schemas you have not changed. Nothing is selected by
  default — the schema box now says `(none)` instead of showing the first
  schema while no schema is active.
- **Schema descriptions.** The field schema editor (`Tools → Field Schemas…`)
  has a Description line; the description is the tooltip of the schema in the
  editor's list and in the *Log Fields* schema box. Built-in schemas come with
  one.
- **A folder dropped on the window opens in the Directory Scanner**: the panel
  comes up and the scan starts. The same goes for a folder on the command line
  (`DendroLog C:\logs`) and one handed over to the already open window. Files
  dropped together with folders open as before; of several folders the first
  one is scanned.
- **Rotated logs show up in the Directory Scanner**: `app.log.1`,
  `app.log.10`, `app.log-20260101`, `app.log.2026-01-01` — a configured
  extension followed only by a rotation number or date. They sort right after
  the main file. Backups and editor files (`app.log.bak`, `app.log.swp`) and
  compressed rotations (`app.log.gz`) stay out.

### Changed
- **A line that opens with a timestamp starts a new entry, even without a
  level word.** Access logs (nginx / Apache), syslog and plain
  “timestamp message” logs used to become one huge entry without a field
  schema: every line after the first was glued to it as a continuation, so
  lines had no time of their own, the timeline was empty, the time filter and
  the merge by time did not work, and the Directory Scanner showed one entry.
  Now an entry starts at a line whose timestamp stands at the very start, right
  after a `[` at the start (`[2026-11-18 13:55:36] …`), or — for access logs —
  in brackets after the client address. A timestamp in the middle of a line or
  in an indented line (a JSON or SQL body, the text of an exception) still
  keeps the line inside its entry, and lines with a timestamp and a level word
  start an entry as before. Small and huge files and the Directory Scanner
  split entries the same way.
- **More timestamp forms are read: syslog, access logs and nginx error
  logs.** Lines with `Nov 18 13:55:36` (syslog, no year),
  `18/Nov/2026:13:55:36 +0000` (nginx / Apache access) and
  `2026/11/18 13:55:36` (nginx error) now get their moment: they merge by time
  with other files, and the time filter, the timeline, Go to Time and the
  Directory Scanner's From / To work for them. A syslog line takes the latest
  year that is not after the file's last change, so a file that runs over New
  Year reads correctly. The built-in Syslog (RFC 3164) and access schemas read
  their time field as a timestamp; if you have not edited them, they update by
  themselves. A time without a date (`[13:55:36 INF]`) is not read yet.
- **More level words are recognised.** `CRITICAL`, `CRIT`, `SEVERE`, `EMERG`
  and `FTL` count as Fatal; `ERR` as Error; `WRN` as Warn;
  `INF` and `NOTICE` as Info; `DBG` as Debug; `VERBOSE` and `VRB` as Trace.
  Lines with these words used to have no level: they were not coloured, the
  level buttons, `F7` / `F8` and the error counters ignored them, and without
  a field schema they were glued to the previous entry. The **Level** block of
  a field schema accepts the same words. The full table is in Quick Help
  (`F1`), “Which word means which level”.
- A jump — a click on the timeline, a jump from Statistics, a result picked in
  the Search panel, a quick-search match (`F3`) — now selects the line and
  switches Follow tail off, as Go to Line / Time does, so the next appended
  lines no longer pull the view back to the end of the log.
- Quick search (`F3` / `Shift+F3`) starts from the line you selected. When a
  filter hides that line, the search continues from its place instead of from
  the top of the log, also when the filter is applied while a search is
  running; a line that only got keyboard focus is no longer taken for the
  selected one.

### Fixed
- **The Directory Scanner counts entries, not lines.** A stack trace or any
  other multi-line message used to add one entry per line; now the Entries
  column equals the number of entries of the opened tab, and W/E/F and the
  From / To span agree with it: a level word in a continuation line (a stack
  trace, a dumped message) no longer adds to the counters. Lines in a CRLF
  file are split the same way. The appended part of a **UTF-16** log is read
  correctly (it was decoded as UTF-8 and lost its counts), and a log whose
  last line was still being written no longer gains an extra entry when the
  line is finished.
- **UTF-16 logs that grow.** Lines appended to an open UTF-16 file (and
  UTF-32) used to show as garbage: the new part was read as UTF-8. It is now
  read in the file's encoding, a line written in several parts stays one
  line, and a line continuing a multi-line entry stays in that entry. A UTF-16
  file larger than the indexed-backend threshold is no longer read again from
  the start on every auto-reload tick and every `F5`; only the appended part
  is read.
- **Switching between the light and dark theme repaints everything.** Rule
  and marker cards in Text Filters, Search and Row Highlighters, the header
  cards of Entry Details and the Directory Scanner kept the colours of the
  theme the window was opened in — white boxes and black labels on a dark
  window, or the reverse — until a restart, and lines of an open log kept the
  previous theme's text colour. The same happened when the OS theme changed
  while DendroLog was running. The section toggles of Entry Details, the muted
  captions, the Statistics panel, the scanner's refresh icon and the
  "not found" tint of the quick-search box now follow the theme as well. The
  dark theme looks the same after a switch from light as on a start under a
  dark OS.
- A jump to a line near the end of a growing log (Go to, timeline, Search,
  quick search, `F8`) no longer slides to the tail on the next appended lines
  when Follow tail is off. The view follows the tail again once you scroll
  down to it yourself or switch Follow tail on.
- A jump or a filter applied right after a tab was shown or the window was
  resized no longer loses the scroll position: the view used to return to
  where it stood at the moment of the resize, leaving the selected line off
  screen.
- Saving a schema in the editor no longer drops its description and the keys
  of the schema file DendroLog does not know (also on import and export).
  Editing or renaming a schema saves it to the same file, so an edited or
  renamed built-in schema no longer looks deleted; changing only its
  description keeps it eligible for updates by a new version.
- A piped stream (`program | DendroLog -`) no longer leaves a dead entry in
  **Recent Files**: its temporary file is gone once the window closes. Such
  entries left by earlier versions are dropped on start. The **Open** dialog no
  longer starts in the temporary folder after a piped stream either.
- A relative path passed on the command line while a window is already open
  is resolved against the folder the command was run in, not the folder of
  the open window.

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
