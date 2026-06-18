# LogViewer — Quick Help

A fast viewer for large log files: multi-file tabs, structured field extraction,
filtering, highlighting and live reload.

> Tip: every keyboard shortcut below is configurable in **Tools → Settings → Shortcuts**.

---

## Opening logs

- **Open log file(s)** — `File → Open` (`Ctrl+O`). Multiple files open into one tab and are merged by timestamp.
- **Directory Scanner** panel — scan a folder for log files by extension and open one or many at once.
- **Recent Files** — `File → Recent Files`.
- **Save View As** — `File → Save View As…` (`Ctrl+Shift+S`). Writes exactly what the view currently shows (active filters **and** the Log Fields selection) to a **new** file. Open files are never overwritten.
- The file-type list (e.g. `log, txt`) is shared between Open, Save and the Directory Scanner — set it in **Settings → General**.

## Reading the view

- **Tabs** — each tab is an independent view; filters/markers are per-tab.
- **Gutter marker `›`** — start of a logical entry; painted green for lines added by the last auto-reload.
- **Badges on the right:**
  - File badge — colour-coded source file (only when a tab holds several files).
  - `+N` / `−` — the line is longer than the visible width; click to expand/collapse it.
- **Syntax highlighting** — strings, numbers, hex, URLs, file paths, GUIDs, timestamps, matching brackets.
- **Word wrap** — toolbar button, `View → Word Wrap`, or `Alt+Z` (global default in **Settings → View**).

## Selection, copy & context actions

- **Drag** to select text; **double-click** selects a smart token.
- **Double-click recognises:** quoted strings, timestamps, URLs, file paths, hex literals, IP addresses, filenames, numbers, words. Consecutive separators are selected as a group; clicking whitespace selects only the whitespace (no jump to the next word).
- **Extend selection with the keyboard:** `Shift+←/→` by character, `Ctrl+Shift+←/→` by token/block.
- **Copy** — `Ctrl+C`.
- **`Space`** — expand/collapse the current line (same as the right-hand badge).
- **Right-click → context menu:**
  - **Copy** the selection.
  - **Word Wrap (this line)** for an expandable line.
  - **Open Link** when a URL is selected.
  - **Open File / Open Containing Folder** when a file path is selected.
  - **Use as Time Filter Start / End** when a timestamp is selected.

## Filtering

- **Log level** — toolbar buttons (Trace…Fatal) or the `Filters → Log Level` menu.
- **Time range** — *Time Range Filter* panel: set From/To and Apply. A selected timestamp can be sent here from the context menu.
- **Text Filters** panel — build Include/Exclude rules with AND/OR (AND binds tighter), per-rule case sensitivity and regex, optionally bound to a specific field. Filters apply to the **active tab** with **Apply** / **Reset**.

## Highlighting (Row Highlighters)

- *Row Highlighters* panel — colour whole rows that match a pattern, non-destructively (rows are **not** hidden). Apply/Reset act on the active tab.

## Field schemas (Log Fields)

- *Log Fields* panel — define a **schema** (ordered blocks: timestamp, level, integer, text, regex, remainder…) via **Manage…** (auto-detect from a sample line or import a Grok expression).
- Tick **Filter blocks** to show only selected blocks; the selection is reflected in **Save View As** and in field-bound text filters.

## Search

- Search box in the toolbar. `Ctrl+F` focuses it; `Enter` / `F3` finds next, `Shift+F3` finds previous. The match row expands and the term is highlighted.

## Reloading

- **Reload** — `F5` or the toolbar button: re-reads appended content.
- **Auto-reload** — right-click the reload button to toggle it per tab; interval is set in **Settings → General**.

## Panels & layout

- Toggle docks from the **View** menu or with `Ctrl+F1…F5` (Text Filters, Directory Scanner, Time Filter, Log Fields, Row Highlighters). Dock positions are remembered between sessions.

## Settings & theme

- **Tools → Settings** (`Ctrl+,`):
  - **General** — scan extensions, auto-reload interval.
  - **Font** — monospaced family and size (live preview).
  - **Colors** — log-level, syntax, selection and UI colours.
  - **View** — default word wrap.
  - **Shortcuts** — rebind any command (Restore Defaults available).
- The window follows the Windows light/dark theme automatically.

---

## Keyboard shortcuts (defaults)

| Action | Shortcut |
|---|---|
| Open log file(s) | `Ctrl+O` |
| Save View As | `Ctrl+Shift+S` |
| Reload file | `F5` |
| Settings | `Ctrl+,` |
| Focus search field | `Ctrl+F` |
| Search next / previous | `F3` / `Shift+F3` |
| Toggle word wrap | `Alt+Z` |
| Copy selection | `Ctrl+C` |
| Expand/collapse current line | `Space` |
| Extend selection by character | `Shift+←/→` |
| Extend selection by token | `Ctrl+Shift+←/→` |
| Show/Hide panels | `Ctrl+F1…F5` |
