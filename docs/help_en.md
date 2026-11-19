# DendroLog — Quick Help

A fast viewer for large log files: multi-file tabs, structured field extraction,
filtering, highlighting and live reload.

> Tip: every keyboard shortcut below is configurable in **Tools → Settings → Shortcuts**.

---

## Opening logs

- **Open log file(s)** — `File → Open` (`Ctrl+O`) or the folder button on the toolbar. Multiple files open into one tab and are merged by timestamp; such a tab is named after the first file, e.g. `app.log +2`.
- **Drop files** anywhere in the window to open them. **Drop a folder** to open it in the Directory Scanner: the panel comes up and the scan starts. Files and folders dropped together: the files open, the folder is scanned. The scanner shows one folder at a time, so of several folders the first one is scanned.
- **Command line** — `DendroLog app.log other.log` opens the files, `DendroLog C:\logs` scans the folder; the same rules as for a drop. `program | DendroLog -` reads the program's output as a growing log. `--encoding <name>` sets the encoding of the files and the stream (see "Encodings").
- **Start screen** — while no tab is open, the window shows buttons to open files or scan a folder and the recent files as links.
- **Directory Scanner** panel — scan a folder for log files by extension and open one or many at once (`File → Scan Directory…`).
- **Recent Files** — `File → Recent Files` keeps the last 10 files; **Clear Recent Files** empties the list. A piped stream (`program | DendroLog -`) is not remembered: it does not exist after the window closes.
- **Close Tab** — `Ctrl+W`, the tab's cross or the middle mouse button.
- **Save View As** — `File → Save View As…` (`Ctrl+Shift+S`). Writes exactly what the view currently shows (active filters **and** the Log Fields selection) to a **new** file. Open files are never overwritten.
- The view is saved in the background: the window stays usable, the status bar shows the progress, and **Cancel Save** next to it stops the export. The rows are taken as they were when saving started, so filtering or scrolling meanwhile does not change the file. A cancelled export leaves an existing destination file as it was.
- Export also protects open files accessed through another path or a filesystem link. The destination is replaced only after the entire export is written successfully; if saving fails, an error is shown and any previous destination file is preserved.
- The file-type list (e.g. `log, txt`) is shared between Open, Save and the Directory Scanner — set it in **Settings → General**.

## Reading the view

- **Tabs** — each tab is an independent view; filters/markers are per-tab.
- **Gutter marker `›`** — start of a logical entry (without a schema: a line that opens with a timestamp, or a line with a timestamp and a level word); painted green for lines added by the last auto-reload.
- **Badges on the right:**
  - File badge — colour-coded source file (only when a tab holds several files).
  - `+N` / `−` — the line is longer than the visible width; click to expand/collapse it.
- **Marks on the scrollbar** — short strokes on the vertical scrollbar show where the shown lines are: errors (Error and Fatal, red) and warnings (yellow) in the right half, bookmarks in the middle, matches of the running Search panel query on the left (on the tab it searches). A level mark stands for the first line of an entry, the same line `F8` goes to. When a line is on screen, its mark is inside the scrollbar handle, so dragging the handle to a mark brings that place into view. Marks follow filters, tail updates, reloads and added files; on a very large log they appear a moment after loading, computed in the background.
- **Syntax highlighting** — strings, numbers, hex, URLs, file paths, GUIDs, timestamps, matching brackets.
- **Word wrap** — toolbar button, `View → Word Wrap`, or `Alt+Z` (global default in **Settings → Appearance**).
- **Font size** — `Ctrl` + mouse wheel over the log, `Ctrl++` / `Ctrl+-`, or `View → Larger Font / Smaller Font`; `Ctrl+0` returns to the default size. The size is remembered.
- **Status bar** — the current line and the number of visible lines; when filters hide lines, how many the document has in total (`Line 12 of 5 000 (filtered from 116 384)`).

## Selection, copy & context actions

- **Drag** to select text; **double-click** selects a smart token.
- **Double-click recognises:** quoted strings, timestamps, URLs, file paths, hex literals, IP addresses, filenames, numbers, words. Consecutive separators are selected as a group; clicking whitespace selects only the whitespace (no jump to the next word).
- **Extend selection with the keyboard:** `Shift+←/→` by character, `Ctrl+Shift+←/→` by token/block.
- **Copy** — `Ctrl+C`.
- **`Space`** — expand/collapse the current line (same as the right-hand badge).
- **Right-click → context menu:**
  - **Copy** the selection; **Copy Whole Line** under the cursor.
  - With a short piece of text selected on one line:
    - **Find “…”** — the quick find of the toolbar (jumps to the next match).
    - **Find All “…”** — every line with it, listed in the **Search** panel.
    - **Show Only Lines with “…”** / **Hide Lines with “…”** — adds a Contains / Not contains rule and filters the tab. The rule narrows what the tab shows now: if the Text Filters panel describes this tab's filter (or both are empty), the rule is added to the panel as if you pressed Apply; if the panel holds other rules that are not applied to this tab, they are left alone and only the tab's filter gets the rule.
    - **Highlight Lines with “…”** — adds a marker to Row Highlighters and colours such lines.
  - **Word Wrap (this line)** for an expandable line.
  - **Open Link** when a URL is selected.
  - **Open File / Open Containing Folder** when a file path is selected.
  - **Use as Time Filter Start / End** — the selected timestamp or, with nothing selected, the time of the line under the cursor.
  - **Add Bookmark / Remove Bookmark** and **Add Bookmark with Note… / Edit Bookmark Note…** for the line under the cursor (see Bookmarks).

## Filtering

- **Log level** — the coloured toolbar buttons (Fatal…Trace), the `Filters` menu or `Alt+1…Alt+6`. A pressed button means “show only this level”; several can be combined; with none pressed, every level is shown. **Show All Levels** (`Alt+0`) releases them all.
- **Reset Filters of This Tab** — `Filters` menu or `Ctrl+Shift+R`: turns the level, time and text filters of the active tab off at once.
- **Time range** — *Time Range Filter* panel: set From/To and Apply. The fields start at the time span of the active tab's log (seconds and milliseconds included) and the panel shows that span and whether the filter is on. **Whole Log** puts the span back. A bound left at the edge of the log stays open, so lines appended to a growing log keep showing up. If From is later than To, nothing is applied and the panel says why. A line's time can be sent here from its context menu.
- **How line times are read.** A timestamp without a time zone is taken as local time on this computer. An explicit zone is honoured: `Z`, `+02:00`, `+0200`, `+02` right after the time, and `+02:00`, `+0200`, `UTC`, `GMT` after a single space (for example, `2026-09-20 10:00:00 +0200`). So `10:00:00Z` and `12:00:00+02:00` are the same moment, and files from different zones merge correctly in one tab. Fractional seconds of any length are truncated to milliseconds (`.1` is 100 ms, `.123456` is 123 ms). Recognised forms: `2026-11-18 13:55:36` (also with `T`), `2026/11/18 13:55:36` (nginx error) and `18/Nov/2026:13:55:36 +0000` (nginx / Apache access log) anywhere in the line; `18/11/2026 13:55:36`, `11/18/2026 13:55:36`, `18.11.2026 13:55:36` and the syslog form `Nov 18 13:55:36` (also `Nov  8 …`) only at the very start of the line. If a line has a timestamp at its start, that one counts. Month names are English abbreviations, in any case. A syslog timestamp has no year: it gets the latest year in which that day is not later than the day after the file was last modified, so a file that runs over New Year reads correctly, and lines appended after New Year get the new year. A time without a date (`[13:55:36 INF]`) is not read yet. The time filter, the timeline and the details panel show moments in local time; the line text is not changed.
- **Text Filters** panel — hides the lines that do not match its rules (Include/Exclude, AND/OR, case sensitivity and regex per rule, optionally bound to a field). It acts on the **active tab** with **Apply** / **Reset**. To *find* lines without hiding the rest, use the **Search** panel — see **[Text Filters and Search](#text-filters-and-search)** below.

### Which word means which level

The level of an entry comes from the **leftmost** word of the table below in the line the entry starts with. The word must stand on its own (`[ERROR]`, `ERROR:`, `level=error` count; `ERRORS`, `error_code` do not); case does not matter. Continuation lines (a stack trace) take the level of their entry. Without a field schema an entry starts at a line that opens with a timestamp: one at the very start of the line (`2026-11-18 13:55:36 …`, `Nov 18 13:55:36 …`), right after a `[` at the start (`[2026-11-18 13:55:36] …`), or an access-log timestamp in brackets after the client address (`10.0.0.1 - - [18/Nov/2026:13:55:36 +0000] …`). No level word is needed for that, so access logs, syslog and plain "timestamp message" logs split into entries without a schema. A line that has both a timestamp (anywhere) and such a word starts an entry too. A line whose timestamp is in the middle or indented — a JSON or SQL body, the text of an exception — stays a continuation: it takes the moment and level of its entry, not its own.

| Level | Words |
|---|---|
| **Fatal** | `FATAL`, `CRITICAL`, `CRIT`, `SEVERE`, `EMERG`, `FTL` |
| **Error** | `ERROR`, `ERR` |
| **Warn** | `WARN`, `WARNING`, `WRN` |
| **Info** | `INFO`, `INF`, `NOTICE` |
| **Debug** | `DEBUG`, `DBG` |
| **Trace** | `TRACE`, `VERBOSE`, `VRB` |

The level buttons, the line colour, the `F7` / `F8` jumps and the counters in Statistics all use this level; the directory scanner counts Fatal, Error and Warn over the entries of that level; the **Level** block of a field schema accepts the same words. Only a word written in capitals is coloured inside the text. `ALERT`, `PANIC`, `FAIL`, `FINE`, `FINER`, `FINEST`, `CONFIG` and one-letter levels (`E`, `W` in logcat) are not treated as levels: they are ordinary message words, and the leftmost word of the table decides. For the same reason a component name that equals a table word and stands before the level (`[notice-worker] ERROR …`) gives the level of that word.

## Text Filters and Search

Two panels work with text, and they differ in **what happens to the log**:

| | **Text Filters** | **Search** |
|---|---|---|
| What it does | **hides** the lines that do not match | **lists** the matching lines; the log stays complete |
| Main button | **▶ Apply** (or `Enter` in a rule) | **▶ Search** (or `Enter` in a rule) |
| Undo button | **⟲ Reset** — the tab shows all lines again | **✕ Clear** — the results list empties |
| Where the result is | in the log itself | in the list inside the Search panel; a click jumps to the line in the log |
| Scope | the active tab; each tab keeps its own filter | the visible lines of the active tab |

Both panels use the same rules and can be used together: filter the log down, then search in what is left. **Highlight** in each panel also colours the matched text in the log. The rules stay in a panel after Reset / Clear, so you can apply them again.

### Rules

Each rule is one card:

- **Contains / Not contains** — the line must (Include) or must not (Exclude) contain the text.
- **AND / OR** — how the rule links to the previous one (the first rule has no link).
- **Field** — `(entire row)` by default; can be limited to a specific Log Fields column (active when **Filter blocks** is on). If the chosen column is not in the current schema (for example, after switching schemas), the rule is ignored and its card says so — it does not quietly search the whole row; the binding comes back with the column. With **Filter blocks** off, a rule bound to a column searches the entire row, and its card notes that.
- **⚙ (gear)** — per-rule options: **Case sensitive** and **Regular expression**.
- **Colour swatch** — left-click picks the highlight colour for this rule's matches; right-click toggles this rule's highlighting on/off. The colour is auto-picked to contrast with the theme, but you can override it.

### How rules combine (AND / OR)

Rules combine with boolean logic, the same way in both panels:

- **AND** — a line must satisfy **both** adjacent rules at once. Example: `Contains "Timeout"` **AND** `Contains "Disk"` — only lines that contain *both* “Timeout” *and* “Disk”.
- **OR** — a line only needs to satisfy **either** rule. Example: `Contains "Timeout"` **OR** `Contains "Disk"` — all lines with “Timeout” **plus** all lines with “Disk”.
- Precedence: **AND binds tighter than OR**, so `A AND B OR C` reads as `(A AND B) OR C`.
- **Not contains** pairs well with AND: `Contains "error"` **AND** `Not contains "timeout"` — errors except timeouts.
- A new rule links with **AND** in Text Filters (narrow down) and with **OR** in Search (find this too). You can always change the link by hand.

> If several rules give an empty result, it is almost always because they are joined by **AND** while the texts live on **different** lines (no intersection). Switch the link to **OR** to see the union.

### The Search panel

The panel holds the query and the results side by side. Docked at the bottom (the default) or floating, the rules are on the left and the results on the right; docked at a side, where the panel is narrow, the results go under the rules.

- Search covers **all currently visible lines** of the active tab, including the end of large files; level, time and text filters of the tab still limit its scope. Large indexed logs are searched in the background without loading their text into memory. The list shows progress while searching and the match count when complete.
- Clicking a result (or moving through the list with the arrow keys) jumps to that line in the log; the focus stays in the list.
- Results follow the active tab live: lines appended to a growing log are searched as they arrive and added to the list, without searching the whole file again; a line that was still being written is checked once it is complete. Changes to the visible lines themselves — level, time or text filters, a reloaded file — run the search again. Switching tabs runs it on the new tab. While the panel is hidden, such full searches wait until it is shown.
- **Find All** sends a single term here: the **All** button next to the toolbar find box (`Ctrl+Shift+F`) or **Find All “…”** in the log's context menu replaces the rules with one `Contains` rule and searches. `Ctrl+Shift+F` with an empty find box just opens the panel with the cursor in the first rule.

### Regular expressions

The **Regular expression** checkbox treats the rule text as a **Perl/PCRE-style regex**, not a command-line wildcard:

- `*` repeats the previous character — it does **not** mean “any characters”. For “any characters”, write `.*`.
- `.` is any single character; `\d` is a digit; `\d+` is one or more digits.
- A rule matches if the expression is found **anywhere** in the line (no anchor needed).
- Examples: `entry number \d+` matches “entry number 42”; `Log entry number .*` matches any such entry; `(WARN|ERROR)` matches lines with either level.
- An **invalid expression** is flagged: the field gets a red border and the error (with position) appears beneath it. While invalid, the rule takes no part in filtering or search.

### Profiles

The **Profile** combo + **⋯** button save named rule sets (**Save**, **Save as new…**, **Rename…**, **Delete**). Text Filters and Search keep separate profiles. Switching profiles with unsaved edits prompts whether to keep them.

## Highlighting (Row Highlighters)

- *Row Highlighters* panel — colour whole rows that match a pattern, non-destructively (rows are **not** hidden). Apply/Reset act on the active tab.

## Field schemas (Log Fields)

- *Log Fields* panel — define a **schema** (ordered blocks: timestamp, level, integer, text, regex, remainder…) via **Manage…** or `Tools → Field Schemas…` (auto-detect from a sample line or import a Grok expression).
- **Built-in schemas** for common formats are in the list from the first start; pick one in the combo box of the *Log Fields* panel (nothing is selected by default):

| Schema | A line it reads |
| --- | --- |
| Logback / Log4j | `2026-11-18 13:55:36.123 [main] INFO  com.example.App - Started` |
| Spring Boot | `2026-11-18T13:55:36.130Z  INFO 34786 --- [main] o.s.b.App : Started` |
| nginx / Apache access (combined) | `203.0.113.42 - - [18/Nov/2026:13:55:36 +0000] "GET / HTTP/1.1" 200 64 "-" "curl/8.5.0"` (the common format too) |
| nginx error | `2026/11/18 13:55:36 [error] 1234#1234: *99 open() failed` |
| Syslog (RFC 3164) | `Nov 18 13:55:36 web01 sshd[2231]: Accepted publickey` |
| Syslog / journalctl (ISO timestamp) | `2026-11-18T13:55:38+0100 web01 systemd[1]: Starting nginx` |
| Serilog (console) | `[13:55:36 INF] Starting web host` |
| Serilog (file, with date) | `2026-11-18 13:55:36.123 +01:00 [INF] Starting web host` |
| Python logging | `2026-11-18 13:55:36,123 - myapp.db - INFO - Connected` |
| Python logging (basicConfig) | `WARNING:root:Watch out!` |

  Stack traces and Python tracebacks stay inside their entry. Built-in schemas are ordinary files in the `patterns` folder next to your settings and can be edited, renamed or deleted like your own: an edited one is never overwritten, a deleted one does not come back, and a schema of yours with the same file name or name is left as it is. A new version of DendroLog updates only built-in schemas you have not changed. To get deleted built-in schemas back, delete the `.builtin-schemas` file in that folder and restart.
- Each schema can have a **Description** (a line under its name in `Tools → Field Schemas…`): it is shown as the tooltip of the schema in the editor's list and in the *Log Fields* schema box, and is saved in the schema file. Built-in schemas come with one. Editing only the description does not count as changing a built-in schema — a new version may replace its text; changing the blocks or the name does. Keys of a schema file that DendroLog does not know are kept when the schema is saved.
- The time filter, the timeline and the merge of several files use the moment read from the line itself, by the rules in *How line times are read* above — with or without a schema. A time without a date (Serilog console, `[13:55:36 INF]`) is shown in the field but does not position the entry in time yet.
- Tick **Filter blocks** to show only selected blocks; the selection is reflected in **Save View As** and in field-bound text filters.

## Quick find (toolbar)

- The toolbar search box is a one-off text find (not to be confused with the **Text Filters** panel above). `Ctrl+F` focuses it; `Enter` / `F3` finds next, `Shift+F3` finds previous (also the arrow buttons next to it). Three toggles next to the box refine it (see below). The search starts from the selected line (from its place if a filter hides it; with nothing selected, from the top, or from the end for `Shift+F3`) and wraps around. The match row expands, becomes the selected one and the term is highlighted; Follow tail is switched off, so a growing log does not pull the view back to its end. The search runs in the background, so the window stays responsive even on a huge log without matches: a long search shows its progress in the status bar, `Esc` in the search box cancels it, and a term that is not found turns the box red and is reported in the status bar.
- The toggles: **Aa** (`Alt+C`) — match case; **ab** underlined (`Alt+W`) — whole word: the match may not touch a letter, digit or `_` on either side, so `error` skips `errors` and `my_error` but finds `[error]`; **.\*** (`Alt+R`) — regular expression, the same syntax as in Text Filters (see *Regular expressions* above: found anywhere in the line). Whole word works with a regex too: `\d+` then finds a number that is not glued to letters. An **invalid expression** gives the box a red border, its tooltip and the status bar say what is wrong and at which position, and nothing is searched until it is fixed. The toggles are remembered between runs.
- **Recent searches:** the down arrow in the box (or the arrow button at its right edge) lists the last 20 searches, newest first; picking one runs it again. A query is remembered when it is actually searched (`Enter`, `F3`, `Shift+F3`, the arrow buttons, **All**), not while typing; only the text is kept, the toggles stay as they are. **Clear search history** at the bottom of the list forgets them.
- To see every line with the term at once, press **All** next to the box (`Ctrl+Shift+F`): the **Search** panel lists them without hiding anything (see above). Match case, whole word and regex carry over: the panel gets one rule with the same case setting, and a regex or whole-word query becomes a **Regular expression** rule (whole word — the text wrapped in word-boundary checks). The Search panel also takes several rules at once.

## Go to line or time

- `Ctrl+G` or `View → Go to Line / Time…` opens one input box. Digits are a **line number of the file** — the same number Entry Details shows. Anything else is a **time**: `10:02`, `10:02:03.250`, a date `2026-03-05`, or both — `2026-03-05 10:02`. A time without a date takes the date of the selected line, or of the start of the log when nothing is selected. A zone may follow (`Z`, `+03:00`, `UTC`); without one the time is local, as in the log itself.
- The line under the box says where the jump lands before you press **Go**. If the line is hidden by the filters, the jump goes to the nearest shown line — the next one after its place, or the last one. A number past the end of the file goes to its last line. A time goes to the first shown line at or after it; a time later than the whole log — to the last line with a timestamp.
- In a tab with several files a line number belongs to one file: the **Line of file** list picks it and starts on the file of the selected line. It does not affect a time.
- The line becomes the selected one, so it stays in place when filters change afterwards.
- A large file opened alone is shown in file order; if its timestamps are not in order, a jump to a time is approximate there.

## Navigation history

- `Alt+←` (`View → Navigate Back`) returns to the place you jumped from; `Alt+→` (`View → Navigate Forward`) goes forward again, as in a browser. The side buttons of the mouse ("back" / "forward") over the log do the same.
- Every jump to a line counts: Go to, a click on the timeline or in Statistics, a Search result, the quick search (`F3`), next error (`F8` / `F7`), bookmarks (`F2`, the Bookmarks panel) and the **↓ N new lines** badge. The history keeps the selected line you left (or, with nothing selected, the first line on screen) and the line you came to. Clicks, arrow keys and scrolling are not jumps and are not recorded.
- A run of jumps of one kind with no move in between — `F3`, `F3`, `F3` through matches, `F8` after `F8`, arrowing through Search results — is one step: a single `Alt+←` brings you back to where you were reading before it. A new jump after going back drops the "forward" part, as in a browser. The history keeps the last 100 places.
- Going back selects the line and switches Follow tail off, like any jump, and adds nothing to the history. A place hidden by filters leads to the nearest shown line. Places stay through tail updates, a reload of the same content and the switch to the large-file mode; a place whose line is gone (the file was rewritten or removed) is skipped.
- Each tab has its own history; it is not saved and goes away when the tab is closed.

## Next and previous error

- `F8` / `Shift+F8` (`View → Next Error` / `Previous Error`) jump from the selected line to the nearest entry of level **Error** or **Fatal**; `F7` / `Shift+F7` do the same for **Warn**. With nothing selected the search starts from the top (or, backwards, from the bottom).
- A multi-line entry (a stack trace) counts once: the jump goes to its first shown line, not through every line of the trace.
- Only the lines the tab shows are considered: what the filters hide is skipped.
- Past the last such line the jump continues from the other end of the tab; the status bar says so. It also says when there are no such lines at all, or when the selected line is the only one.
- The line becomes the selected one and Follow tail is switched off, as with Go to.
- On a very large log the lookup runs in the background (the level is taken from the line index, the text is not read), so the window stays responsive; a long lookup shows its progress in the status bar, and pressing the key again starts over. If the shown lines change while it runs (a filter finishes, the file is reloaded), the status bar asks to press the key again.

## Bookmarks

- `Ctrl+F2` (`View → Add / Remove Bookmark`) bookmarks the selected line or removes its bookmark; the context menu of a line does the same and can ask for a note. A bookmarked line has a ribbon in the left margin.
- `F2` / `Shift+F2` (`View → Next Bookmark` / `Previous Bookmark`) jump from the selected line to the next / previous bookmark among the shown lines; past the last one the jump continues from the other end of the tab, as with `F8`. Bookmarks on lines hidden by filters are skipped.
- A bookmark belongs to one physical line, not to the whole entry: a bookmark on a line of a stack trace stays on that line.
- The **Bookmarks** panel (`Ctrl+F11`) lists the bookmarks of the current tab: file and line number, the start of the line's text and a note you can edit right there. Click the file and line to go to the bookmark, ✕ removes it, **Clear All** removes every bookmark of the tab.
- A bookmark on a line hidden by the filters is marked **hidden by filter**; clicking it goes to the nearest shown line. When the filter is removed, the margin mark is back.
- Bookmarks stay through tail updates, a full reload of the file and the switch of a growing file to the large-file mode. If the file is rewritten and the line is gone or has different text, the bookmark stays in the panel with its text and note, marked **line not found**, and is not moved to another line. Bookmarking that line again reattaches the bookmark and keeps its note. While the file is being read again the bookmark is marked **line not loaded**. A bookmark is found again by its line number and text, also when a filter hides that line at the moment.
- Bookmarks live until the tab is closed. Merging tabs moves the bookmarks of the merged tab, with their notes, to the receiving tab.

## Reloading

- **Reload** — `F5` or the toolbar button: re-reads appended content.
- **Auto-reload** — the toggle next to Reload, `File → Auto-reload This Tab` or `Ctrl+Shift+F5` (right-clicking Reload still works); per tab. Tabs with auto-reload on carry a small icon. The interval is set in **Settings → General**.
- **Follow tail** — the toggle with the down arrow or `Shift+F5`: keeps the newest lines in view; scrolling up turns it off, and so does any jump to a line (Go to, a search match, a Search result, the timeline, Statistics, next error).
- **↓ N new lines** — while you read further up with Follow tail off, lines appended below the screen are counted in a small badge at the bottom of the view. Only lines the tab's filters show are counted; above 99,999 the number is shortened (`120K`, `3.4M`), the tooltip has the exact one. Click the badge, or press `Alt+End` (`View → Go to New Lines`), to jump to the first new line: it becomes the selected line, Follow tail stays off, and lines that come on screen drop out of the count, so pressing again pages through the rest. The badge goes away when you scroll to the bottom or turn Follow tail on. A filter change or a reload of a rewritten file starts the count afresh; each tab keeps its own.

## Encodings

- **Detected by itself.** UTF-8, Windows-1251, IBM866 (cp866), KOI8-R and Windows-1252 — each file has its own encoding, a merged tab included. While a file holds only ASCII there is nothing to decide: the text is the same in any encoding. The first byte outside ASCII decides, wherever it is — on the third gigabyte or in an appended tail: bytes that look like UTF-8 give UTF-8, anything else the fallback encoding (among the Cyrillic ones, Windows-1251, IBM866 and KOI8-R are told apart by the bytes). A file with a UTF-8 byte-order mark is read as UTF-8. When there are few non-ASCII bytes so far (a word or two), the decision is **provisional**: it is checked again as more bytes arrive, and if it changes, the file is re-read without losing your place.
- **The indicator** on the right of the status bar shows the encoding of the current line's file and where it comes from: `Windows-1251 (auto)`, `… (auto, provisional)`, `UTF-8 (BOM)`, `KOI8-R (manual)` — chosen by you, `ASCII (auto)` — only ASCII so far. In a merged tab it is the encoding of the selected line's file.
- **Choose by hand** — click the indicator: UTF-8, Windows-1251, IBM866, KOI8-R, Windows-1252. The file is shown in the new encoding at once — a large file is not re-indexed; the current line, the selection and the place on screen stay, and search, filters, bookmarks and the navigation history work with the new text. The choice lasts while the tab is open: through reloads, a rewritten file and merging tabs. **Detect automatically** drops the choice and brings back the automatic decision. You can choose even while the file is still all ASCII.
- **Fallback encoding** — **Settings → General → Non-UTF-8 files**: what to use for a file that is not UTF-8. The default follows the system language (Cyrillic — Windows-1251, otherwise Windows-1252). A change applies to new files and to open files whose decision is not final yet; decided files and manual choices stay as they are.
- **Command line** — `DendroLog --encoding cp1251 app.log` opens the files in that encoding (as a manual choice); `program | DendroLog --encoding cp866 -` does the same for a stream. Names: `utf-8`, `windows-1251`/`cp1251`, `ibm866`/`cp866`, `koi8-r`, `windows-1252`/`cp1252`. An unknown name shows a message in the window, and the files open with automatic detection. The argument also works when the files are passed to an already open window (a second launch).
- **Save View As** always writes UTF-8: a tab may hold files in different encodings, and UTF-8 keeps all of them intact.
- **Directory Scanner** searches file contents in the same encoding a tab would use: by the byte-order mark (UTF-8, UTF-16, UTF-32), otherwise by the same detection rule with the same fallback encoding. A query that finds a line in a tab finds the file in the scanner. Changing the fallback encoding re-checks only the files that depend on it.
- **Limits.** UTF-16 and UTF-32 are recognised by the byte-order mark only and are read whole in the regular mode: a large such file is not indexed, its encoding cannot be changed, and it does not open in a tab with other files — open it in a tab of its own. A block of garbage at the start of a file (hundreds of non-ASCII bytes that do not look like text) can give a wrong final decision — correct it by choosing by hand.

## Directory Scanner

- The folder button picks a directory and scans it for files with the configured extensions; the tree shows entry counts, the time span, W/E/F counters and size. Subdirectories are listed on demand.
- **Rotated logs are listed too**: besides `app.log`, the tree shows `app.log.1`, `app.log.10`, `app.log-20260101`, `app.log.2026-01-01` and `app-2026-01-01.log` — a configured extension followed by nothing but a rotation number or date (digits joined by `.`, `-` or `_`). Backups and editor files (`app.log.bak`, `app.log.swp`, `app.log~`) and compressed rotations (`app.log.gz`, `app.log.1.gz`) are not listed. Rotated files sort right after the main one, by number: `app.log`, `app.log.1`, `app.log.2`, `app.log.10`. The **Open** dialog's “Log files” filter lists plain extensions only; pick “All files” there to open a rotated file, or open it from the scanner.
- **Entries are counted the way an opened tab counts them**: an entry starts the same way as in a tab without a schema (see "Which word means which level"), and the lines after it (a stack trace, a multi-line message) belong to it. The W/E/F counters count such entries, the From / To span comes from their timestamps. A field schema does not change the count: the scanner counts the way a tab does without field extraction. A file in UTF-16 is recognised by its byte-order mark, and a growing log whose last line was still being written is counted again correctly once the line is finished.
- Names sort **case-insensitively** and naturally: `log2` comes before `log10`. Clicking a column header re-sorts (name ascending, numeric columns largest-first).
- **The ⟳ button refreshes.** It re-reads the directory but not the files: only new and modified ones are analysed again, and a log that merely grew is read from where the previous scan stopped (unless it was rewritten). Deleted files leave the tree.
- **Right-click ⟳** to pick the refresh mode:
  - **Manual** (default) — the tree never changes on its own, but the button lights up when something changed on disk.
  - **Automatic** — changes are pulled in by themselves; the period is set from “Auto-refresh interval…” in the same menu. While the panel is hidden the refresh is deferred until it reappears.
  - **Off** — nothing is watched; the button still refreshes on demand.
- **⚙ opens the filters.** The content filter reads files in the background and reports progress **in bytes**, so the bar tracks the real work even when one file dwarfs the rest. Re-applying the same query is served from cache (instant), and an active date filter narrows down which files are read at all. A file's encoding is detected the way a tab detects it (see "Encodings"): Cyrillic in Windows-1251, IBM866 and KOI8-R and text in UTF-16 are found, case-sensitive or not, as plain text or a regular expression; an appended or rotated file is checked again in its own encoding.

## Panels & layout

- Toggle docks from the **View** menu or with `Ctrl+F1`, `Ctrl+F3…F11` (Text Filters `Ctrl+F1`, Time Filter `F3`, Log Fields `F4`, Row Highlighters `F5`, Timeline `F6`, Search `F7`, Entry Details `F8`, Statistics `F9`, Directory Scanner `F10`, Bookmarks `F11`; `Ctrl+F2` is the bookmark key). Dock positions are remembered between sessions; `View → Reset Panel Layout` restores the default arrangement.
- Long lists in the Text Filters, Row Highlighters and Log Fields panels scroll instead of stretching the panel past the window.
- The **Search** panel shows its rules and results side by side when docked at the bottom and stacked when docked at a side.

## Settings & theme

- **Tools → Settings** (`Ctrl+,`):
  - **General** — scan extensions, auto-reload interval, large files, fallback encoding.
  - **Appearance** — window theme, monospaced font family and size (live preview), default word wrap.
  - **Colors** — log-level, syntax, selection and UI colours.
  - **Shortcuts** — rebind any command; the ⊗ button in a field removes its shortcut, **Restore Defaults** brings them back. A combination given to two commands is marked red, and OK waits until the clash is resolved.
- **Theme** (**Settings → Appearance → Theme**): **Same as system** follows the light or dark theme of the operating system and switches with it (the default); **Light** and **Dark** keep the window that way whatever the system uses. The choice applies at once, without a restart, and is remembered. The colours of the **Colors** tab are one set shared by both themes.

---

## Keyboard shortcuts (defaults)

| Action | Shortcut |
|---|---|
| Open log file(s) | `Ctrl+O` |
| Save View As | `Ctrl+Shift+S` |
| Close tab | `Ctrl+W` |
| Exit | `Ctrl+Q` |
| Reload file | `F5` |
| Auto-reload this tab | `Ctrl+Shift+F5` |
| Follow tail | `Shift+F5` |
| Settings | `Ctrl+,` |
| Focus search field | `Ctrl+F` |
| Search next / previous | `F3` / `Shift+F3` |
| Find all (Search panel) | `Ctrl+Shift+F` |
| Quick find: match case / whole word / regex | `Alt+C` / `Alt+W` / `Alt+R` |
| Go to line or time | `Ctrl+G` |
| Navigate back / forward | `Alt+←` / `Alt+→`, mouse side buttons |
| Next / previous error (Error, Fatal) | `F8` / `Shift+F8` |
| Next / previous warning | `F7` / `Shift+F7` |
| Add / remove bookmark | `Ctrl+F2` |
| Next / previous bookmark | `F2` / `Shift+F2` |
| Go to new lines (below the screen) | `Alt+End` |
| Toggle word wrap | `Alt+Z` |
| Larger / smaller / default font | `Ctrl++` / `Ctrl+-` / `Ctrl+0`, `Ctrl` + wheel |
| Show only Fatal … Trace (toggle) | `Alt+1` … `Alt+6` |
| Show all levels | `Alt+0` |
| Reset filters of this tab | `Ctrl+Shift+R` |
| Copy selection | `Ctrl+C` |
| Expand/collapse current line | `Space` |
| Extend selection by character | `Shift+←/→` |
| Extend selection by token | `Ctrl+Shift+←/→` |
| Show/Hide panels | `Ctrl+F1`, `Ctrl+F3…F11` |
