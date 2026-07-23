# DendroLog — Quick Help

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
- **Text Filters** panel — Include/Exclude rules with AND/OR, per-rule case sensitivity and regex, optionally bound to a field. See **[Text Filters and search](#text-filters-and-search)** below for details. Filters apply to the **active tab** with **Apply** / **Reset**.

## Text Filters and search

The **Text Filters** panel is a set of rules; each rule is one card:

- **Contains / Not contains** — the row must (Include) or must not (Exclude) contain the text.
- **AND / OR** — how the rule links to the previous one (the first rule has no link).
- **Field** — `(entire row)` by default; can be limited to a specific Log Fields column (active when **Filter blocks** is on).
- **⚙ (gear)** — per-rule options: **Case sensitive** and **Regular expression**.
- **Colour swatch** — left-click picks the highlight colour for this rule's matches; right-click toggles this rule's highlighting on/off. The colour is auto-picked to contrast with the theme, but you can override it.

### How rules combine (AND / OR)

Rules combine with boolean logic, **identically in both modes** (below) — only the destination of the result differs:

- **AND** — a row must satisfy **both** adjacent rules at once. Example: `Contains "Timeout"` **AND** `Contains "Disk"` — only rows where a single line contains *both* “Timeout” *and* “Disk”.
- **OR** — a row only needs to satisfy **either** rule. Example: `Contains "Timeout"` **OR** `Contains "Disk"` — all rows with “Timeout” **plus** all rows with “Disk”.
- Precedence: **AND binds tighter than OR**, so `A AND B OR C` reads as `(A AND B) OR C`.
- **Not contains** pairs well with AND: `Contains "error"` **AND** `Not contains "timeout"` — errors except timeouts.

> If several rules give an empty result, it is almost always because they are joined by **AND** while the texts live on **different** rows (no intersection). Switch the link to **OR** to see the union.

### Two modes: Filter and Search

The **Non-destructive search** switch at the top of the panel:

- **Off — Filter (default):** **Apply** *hides* rows in the main view that fail the rules. Destructive to the display (not the file) — a quick “keep only what matters”.
- **On — Search:** **Search** keeps the main view **complete** and lists matches in a separate **Search Results** panel (bottom; opens from `View`, or by itself when you press Search). Clicking a result jumps to that row in the main view. Nothing is hidden.

AND/OR work the **same** in both modes. One convenience: a rule added *in Search mode* defaults its link to **OR** (to “show anything any rule matches”), while in Filter mode it defaults to **AND** (narrow down). You can always change the link by hand.

- **Highlight in main view** — also highlight the matched text in the main view (works in both modes; it never hides rows).

### Regular expressions

The **Regular expression** checkbox treats the rule text as a **Perl/PCRE-style regex**, not a command-line wildcard:

- `*` repeats the previous character — it does **not** mean “any characters”. For “any characters”, write `.*`.
- `.` is any single character; `\d` is a digit; `\d+` is one or more digits.
- A rule matches if the expression is found **anywhere** in the row (no anchor needed).
- Examples: `entry number \d+` matches “entry number 42”; `Log entry number .*` matches any such entry; `(WARN|ERROR)` matches rows with either level.
- An **invalid expression** is flagged: the field gets a red border and the error (with position) appears beneath it. While invalid, the rule takes no part in the search.

### Profiles

The **Profile** combo + **⋯** button save named rule sets (**Save**, **Save as new…**, **Rename…**, **Delete**). Switching profiles with unsaved edits prompts whether to keep them.

## Highlighting (Row Highlighters)

- *Row Highlighters* panel — colour whole rows that match a pattern, non-destructively (rows are **not** hidden). Apply/Reset act on the active tab.

## Field schemas (Log Fields)

- *Log Fields* panel — define a **schema** (ordered blocks: timestamp, level, integer, text, regex, remainder…) via **Manage…** (auto-detect from a sample line or import a Grok expression).
- Tick **Filter blocks** to show only selected blocks; the selection is reflected in **Save View As** and in field-bound text filters.

## Quick find (toolbar)

- The toolbar search box is a one-off text find (not to be confused with the **Text Filters** panel above). `Ctrl+F` focuses it; `Enter` / `F3` finds next, `Shift+F3` finds previous. The match row expands and the term is highlighted.
- For multi-criteria non-destructive search with a results list, use the **Search** mode of the **Text Filters** panel (see above).

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
