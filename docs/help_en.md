# DendroLog — Quick Help

A fast viewer for large log files: multi-file tabs, structured field extraction,
filtering, highlighting and live reload.

> Tip: every keyboard shortcut below is configurable in **Tools → Settings → Shortcuts**.

---

## Opening logs

- **Open log file(s)** — `File → Open` (`Ctrl+O`) or the folder button on the toolbar. Multiple files open into one tab and are merged by timestamp; such a tab is named after the first file, e.g. `app.log +2`.
- **Drop files** anywhere in the window to open them.
- **Start screen** — while no tab is open, the window shows buttons to open files or scan a folder and the recent files as links.
- **Directory Scanner** panel — scan a folder for log files by extension and open one or many at once (`File → Scan Directory…`).
- **Recent Files** — `File → Recent Files` keeps the last 10 files; **Clear Recent Files** empties the list.
- **Close Tab** — `Ctrl+W`, the tab's cross or the middle mouse button.
- **Save View As** — `File → Save View As…` (`Ctrl+Shift+S`). Writes exactly what the view currently shows (active filters **and** the Log Fields selection) to a **new** file. Open files are never overwritten.
- The view is saved in the background: the window stays usable, the status bar shows the progress, and **Cancel Save** next to it stops the export. The rows are taken as they were when saving started, so filtering or scrolling meanwhile does not change the file. A cancelled export leaves an existing destination file as it was.
- Export also protects open files accessed through another path or a filesystem link. The destination is replaced only after the entire export is written successfully; if saving fails, an error is shown and any previous destination file is preserved.
- The file-type list (e.g. `log, txt`) is shared between Open, Save and the Directory Scanner — set it in **Settings → General**.

## Reading the view

- **Tabs** — each tab is an independent view; filters/markers are per-tab.
- **Gutter marker `›`** — start of a logical entry; painted green for lines added by the last auto-reload.
- **Badges on the right:**
  - File badge — colour-coded source file (only when a tab holds several files).
  - `+N` / `−` — the line is longer than the visible width; click to expand/collapse it.
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

## Filtering

- **Log level** — the coloured toolbar buttons (Fatal…Trace), the `Filters` menu or `Alt+1…Alt+6`. A pressed button means “show only this level”; several can be combined; with none pressed, every level is shown. **Show All Levels** (`Alt+0`) releases them all.
- **Reset Filters of This Tab** — `Filters` menu or `Ctrl+Shift+R`: turns the level, time and text filters of the active tab off at once.
- **Time range** — *Time Range Filter* panel: set From/To and Apply. The fields start at the time span of the active tab's log (seconds and milliseconds included) and the panel shows that span and whether the filter is on. **Whole Log** puts the span back. A bound left at the edge of the log stays open, so lines appended to a growing log keep showing up. If From is later than To, nothing is applied and the panel says why. A line's time can be sent here from its context menu.
- **How line times are read.** A timestamp without a time zone is taken as local time on this computer. An explicit zone is honoured: `Z`, `+02:00`, `+0200`, `+02` right after the time, and `+02:00`, `+0200`, `UTC`, `GMT` after a single space (for example, `2026-09-20 10:00:00 +0200`). So `10:00:00Z` and `12:00:00+02:00` are the same moment, and files from different zones merge correctly in one tab. Fractional seconds of any length are truncated to milliseconds (`.1` is 100 ms, `.123456` is 123 ms). The time filter, the timeline and the details panel show moments in local time; the line text is not changed.
- **Text Filters** panel — hides the lines that do not match its rules (Include/Exclude, AND/OR, case sensitivity and regex per rule, optionally bound to a field). It acts on the **active tab** with **Apply** / **Reset**. To *find* lines without hiding the rest, use the **Search** panel — see **[Text Filters and Search](#text-filters-and-search)** below.

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
- Tick **Filter blocks** to show only selected blocks; the selection is reflected in **Save View As** and in field-bound text filters.

## Quick find (toolbar)

- The toolbar search box is a one-off text find (not to be confused with the **Text Filters** panel above). `Ctrl+F` focuses it; `Enter` / `F3` finds next, `Shift+F3` finds previous (also the arrow buttons next to it). **Aa** makes the search case-sensitive. The match row expands and the term is highlighted. The search runs in the background, so the window stays responsive even on a huge log without matches: a long search shows its progress in the status bar, `Esc` in the search box cancels it, and a term that is not found turns the box red and is reported in the status bar.
- To see every line with the term at once, press **All** next to the box (`Ctrl+Shift+F`): the **Search** panel lists them without hiding anything (see above). The Search panel also takes several rules at once.

## Reloading

- **Reload** — `F5` or the toolbar button: re-reads appended content.
- **Auto-reload** — the toggle next to Reload, `File → Auto-reload This Tab` or `Ctrl+Shift+F5` (right-clicking Reload still works); per tab. Tabs with auto-reload on carry a small icon. The interval is set in **Settings → General**.
- **Follow tail** — the toggle with the down arrow or `Shift+F5`: keeps the newest lines in view; scrolling up turns it off.

## Directory Scanner

- The folder button picks a directory and scans it for files with the configured extensions; the tree shows entry counts, the time span, W/E/F counters and size. Subdirectories are listed on demand.
- Names sort **case-insensitively** and naturally: `log2` comes before `log10`. Clicking a column header re-sorts (name ascending, numeric columns largest-first).
- **The ⟳ button refreshes.** It re-reads the directory but not the files: only new and modified ones are analysed again, and a log that merely grew is read from where the previous scan stopped (unless it was rewritten). Deleted files leave the tree.
- **Right-click ⟳** to pick the refresh mode:
  - **Manual** (default) — the tree never changes on its own, but the button lights up when something changed on disk.
  - **Automatic** — changes are pulled in by themselves; the period is set from “Auto-refresh interval…” in the same menu. While the panel is hidden the refresh is deferred until it reappears.
  - **Off** — nothing is watched; the button still refreshes on demand.
- **⚙ opens the filters.** The content filter reads files in the background and reports progress **in bytes**, so the bar tracks the real work even when one file dwarfs the rest. Re-applying the same query is served from cache (instant), and an active date filter narrows down which files are read at all.

## Panels & layout

- Toggle docks from the **View** menu or with `Ctrl+F1…F9` (Text Filters, Directory Scanner, Time Filter, Log Fields, Row Highlighters, Timeline, Search, Entry Details, Statistics). Dock positions are remembered between sessions; `View → Reset Panel Layout` restores the default arrangement.
- Long lists in the Text Filters, Row Highlighters and Log Fields panels scroll instead of stretching the panel past the window.
- The **Search** panel shows its rules and results side by side when docked at the bottom and stacked when docked at a side.

## Settings & theme

- **Tools → Settings** (`Ctrl+,`):
  - **General** — scan extensions, auto-reload interval, large files.
  - **Appearance** — monospaced font family and size (live preview), default word wrap.
  - **Colors** — log-level, syntax, selection and UI colours.
  - **Shortcuts** — rebind any command; the ⊗ button in a field removes its shortcut, **Restore Defaults** brings them back. A combination given to two commands is marked red, and OK waits until the clash is resolved.
- The window follows the Windows light/dark theme automatically.

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
| Toggle word wrap | `Alt+Z` |
| Larger / smaller / default font | `Ctrl++` / `Ctrl+-` / `Ctrl+0`, `Ctrl` + wheel |
| Show only Fatal … Trace (toggle) | `Alt+1` … `Alt+6` |
| Show all levels | `Alt+0` |
| Reset filters of this tab | `Ctrl+Shift+R` |
| Copy selection | `Ctrl+C` |
| Expand/collapse current line | `Space` |
| Extend selection by character | `Shift+←/→` |
| Extend selection by token | `Ctrl+Shift+←/→` |
| Show/Hide panels | `Ctrl+F1…F9` |
