# madc IDE & Editor Plan

Created 2026-05-24. Depends on rendering abstraction Phases 1-2.

## Two Concrete Goals

### Goal 1: Simple Terminal App Framework (Rendering Phase 1)

A dumb-terminal application abstraction: scrolling text output, simple
command menus, and an input line. Think: the MUD client, a CLI dashboard,
or a REPL with menus.

```c
#include <madcui.h>

int main() {
    render {
        output { /* scrolling text area */ }
        status { "Connected to MadSMAUG — Room 109" }
        menu { "look", "north", "south", "inventory", "quit" }
        input { prompt: "> " }
    }
}
```

This is the simplest possible rendering target — Level 0/1. Proves the
abstraction works before building anything complex.

**Deliverable:** `libmadcui` providing `output`, `status`, `menu`, `input`
semantic blocks that render to plain text (Level 0) or basic terminal
with line editing (Level 1).

---

### Goal 2: Curses-Based madc IDE (`madcide`)

A Turbo-C style IDE for madc, built IN madc, using the rendering
abstraction at Level 1 (curses). Includes:

- Source editor with syntax highlighting
- File browser / project tree
- Build output pane
- Error navigation (click error → jump to source)
- Integrated terminal / program output
- Debugger integration (future)

The editor component is a **separate reusable library** (`libmadcedit`)
that can be embedded in any madc program.

## `libmadcedit` — The Editor Library

### Architecture

```
libmadcedit (reusable text editor component)
    │
    ├── Buffer model (gap buffer or piece table)
    ├── Cursor / selection management
    ├── Undo/redo (operation log)
    ├── Syntax highlighting engine
    ├── Keybinding dispatch (pluggable)
    └── Render interface (emits semantic IR)
         └── Consumed by any Level 1+ renderer

madcide (the IDE application)
    │
    ├── Uses libmadcedit for the editor pane
    ├── File browser pane
    ├── Build system integration (make -C src)
    ├── Error parser + jump-to-source
    ├── Tabbed/split editor views
    └── Renders via curses (Level 1)
```

### Pluggable Keybinding System

The editor does NOT hardcode keybindings. A keybinding is a map from
key sequences to editor commands. Users select a preset or define custom
bindings.

**Preset keybinding profiles:**

| Profile | Era | Key Characteristics |
|---------|-----|---------------------|
| **Turbo-C** | 1987 | F-key driven. F2=Save, F3=Open, F5=Run, F9=Build. Block select via Ctrl+K. Menu bar via F10/Alt. |
| **Microsoft Edit** | 1991 | Simple. Arrow keys, Ctrl+C/V/X (CUA). F1=Help. Menus via Alt. |
| **WordStar** | 1978 | Ctrl+K block commands, Ctrl+Q cursor movement. The original "moded" editor. Influenced Turbo Pascal/C. |
| **WordPerfect DOS** | 1982 | F-key heavy. F7=Exit, Shift+F7=Print. Reveal Codes (F11/Alt+F3). Two-key combos everywhere. |
| **Joe's Own Editor** | 1991 | WordStar-compatible by default. Ctrl+K H for help. Clean, modal-optional. |
| **Vim** | 1991 | Modal: Normal/Insert/Visual/Command. h/j/k/l navigation. :w save, :q quit. Composable commands (d2w = delete 2 words). |
| **Emacs** | 1976 | Ctrl+X prefix. Meta (Alt) modifier. Ctrl+X Ctrl+S save. Ctrl+X Ctrl+C quit. Kill ring for clipboard. |
| **Nano/Pico** | 1989 | Simple. Ctrl+O save, Ctrl+X exit. Bottom help bar. Beginner-friendly. |
| **CUA (Common User Access)** | 1987 | Ctrl+C/V/X/Z/A/S. Shift+arrows for selection. The "modern" standard that Windows/Mac/Linux GUIs follow. |

**Implementation:**

```c
struct KeyBinding {
    const char *key_sequence;   // "ctrl+s", "f2", "ctrl+k b", "dd"
    const char *command;        // "save", "open", "block_begin", "delete_line"
    const char *context;        // "normal", "insert", "visual", "any"
};

struct KeybindingProfile {
    const char *name;           // "turbo-c", "vim", "emacs"
    KeyBinding *bindings;       // array of bindings
    bool        modal;          // vim-style modes?
    const char *default_mode;   // "normal" for vim, "any" for non-modal
};
```

The editor core exposes ~80 commands (move_left, move_right, move_word,
delete_char, delete_line, select_begin, select_end, copy, paste, undo,
redo, save, open, find, replace, goto_line, etc.). Keybindings map
key sequences to these commands. The profile is loaded at startup.

**Vim-specific:** Vim's composable commands (operator + motion, like
`d2w`) require a mini command parser in the keybinding layer. The
dispatch becomes: key → check if it's an operator → wait for motion →
compose and execute. This is the most complex profile but it's isolated
in the Vim keybinding handler, not in the editor core.

### Syntax Highlighting Engine

```c
struct SyntaxRule {
    const char *pattern;        // regex or keyword list
    const char *token_type;     // "keyword", "string", "comment", "number", "type", "function"
    int         color_fg;       // foreground color (from theme)
    int         style;          // BOLD, ITALIC, UNDERLINE
};

struct SyntaxDefinition {
    const char *language;       // "mad", "c", "cpp", "python"
    SyntaxRule *rules;
    const char *line_comment;   // "//"
    const char *block_comment_start; // "/*"
    const char *block_comment_end;   // "*/"
    const char *string_delimiters;   // "\"'"
};
```

Built-in syntax definitions for:
- **madc/C** (primary — keywords, types, preprocessor, strings, comments)
- **C++** (extends C with class/template/namespace keywords)
- Generic fallback (strings and comments only)

The highlighting runs on visible lines only (not the whole file).
Re-highlights incrementally when the user edits a line.

### Buffer Model

**Recommendation: Piece table.** Used by VS Code, TextEdit, and others.

A piece table represents the document as a sequence of "pieces" — each
piece is a span in either the original file buffer or an append-only
add buffer. Insertions append to the add buffer and split/add pieces.
Deletions just adjust piece boundaries.

Advantages over gap buffer:
- Undo is trivial (restore previous piece table)
- Multiple cursors are natural (each cursor tracks a piece position)
- Large files are efficient (original file is mmap'd, never copied)
- Read-only files have zero overhead (one piece = whole file)

### Undo/Redo

Operation log model: each edit is an `EditOp` (insert/delete with
position and content). Undo replays in reverse. Redo replays forward.
Operations are grouped by "edit session" (typing a word is one undo
unit, not one per character).

### Editor API (libmadcedit)

```c
// Create editor with buffer content
MadcEdit *madcedit_new(const char *content, size_t len);
MadcEdit *madcedit_open(const char *filepath);

// Keybinding profile
void madcedit_set_keybindings(MadcEdit *ed, const char *profile); // "vim", "emacs", etc.
void madcedit_bind_key(MadcEdit *ed, const char *key, const char *command);

// Syntax highlighting
void madcedit_set_syntax(MadcEdit *ed, const char *language); // "c", "mad", "python"

// Core operations
void madcedit_insert(MadcEdit *ed, const char *text);
void madcedit_delete(MadcEdit *ed, int count);
void madcedit_move(MadcEdit *ed, MoveDir dir, int count);
void madcedit_select(MadcEdit *ed, int start, int end);
void madcedit_undo(MadcEdit *ed);
void madcedit_redo(MadcEdit *ed);

// Query
const char *madcedit_get_line(MadcEdit *ed, int line);
int madcedit_cursor_line(MadcEdit *ed);
int madcedit_cursor_col(MadcEdit *ed);
int madcedit_line_count(MadcEdit *ed);

// Rendering — emits semantic IR for the visible region
void madcedit_render(MadcEdit *ed, RenderTarget *target, int width, int height);

// Event handling — processes keyboard input
bool madcedit_handle_key(MadcEdit *ed, int key, int modifiers);
```

The editor is a **component** that can be embedded in any madc application:
the IDE, a mail client, a config editor, or the SMAUG online creation
system (OLC). Same library, different containers.

## `madcide` — The IDE Application

### Layout

```
┌─ madcide ──────────────────────────────────────────────┐
│ File  Edit  Search  Build  Debug  Options  Help        │ ← menu bar
├────────────┬───────────────────────────────────────────┤
│ Files      │ src/compiler.cpp                    [x]   │ ← tab bar
│  ▸ src/    │─────────────────────────────────────────  │
│    lexer.  │ 1  #include <stdio.h>                     │
│    parser. │ 2  #include <asmjit/x86.h>                │
│    compil. │ 3  #include "datadef.h"                   │
│    typesa. │ 4  #include "tokens.h"                    │
│  ▸ include/│ 5  #include "madc.h"                      │
│  ▸ tests/  │ 6                                         │
│            │ 7  using namespace std;                   │ ← editor pane
│            │ 8  using namespace asmjit;                │    (libmadcedit)
│            │ 9                                         │
│            │10  static DataDef *get_complex...          │
├────────────┴──────────────────┬────────────────────────┤
│ Build Output                  │ Errors                 │
│ make -C src                   │ compiler.cpp:42: error │
│ g++ -c compiler.cpp...        │ parser.cpp:108: warn   │ ← output/errors
│ Build succeeded (0 errors)    │                        │
├───────────────────────────────┴────────────────────────┤
│ compiler.cpp  Ln 7, Col 1  │ CUA │ UTF-8 │ madc       │ ← status bar
└────────────────────────────────────────────────────────┘
```

### IDE Features

| Feature | Implementation |
|---------|---------------|
| File browser | Tree view of project directory. Expand/collapse. Open on Enter. |
| Tabbed editors | Multiple open files, switchable. Ctrl+Tab or mouse click. |
| Split view | Horizontal/vertical split. Same or different files. |
| Build integration | Run `make -C src` or custom build command. Capture stdout/stderr. |
| Error parsing | Parse GCC/madc error format (`file:line:col: error:`). Click to jump. |
| Find/Replace | In-file and across-project. Regex support. |
| Go to line | Ctrl+G / :N (vim) / Meta+G (emacs) |
| Go to definition | Parse tags or use madc's own symbol table |
| Syntax highlighting | Via libmadcedit's syntax engine |
| Auto-indent | Language-aware. Match previous line's indent. |
| Bracket matching | Highlight matching `{}`/`()`/`[]` |
| Line numbers | Toggleable. Relative numbers for vim mode. |
| Word wrap | Toggleable. Soft wrap (display only) or hard wrap. |

### Configuration

```c
// ~/.madcide/config.mad (madc script!)
madcide::set_keybindings("turbo-c");
madcide::set_theme("monokai");
madcide::set_font_size(1);  // 1 = normal, 2 = large
madcide::set_tab_width(8);
madcide::set_build_command("make -C src");
madcide::map_key("f5", "build_and_run");
madcide::map_key("ctrl+shift+f", "find_in_project");
```

Config supports two formats:

**TOML** (simple, declarative — for most users):
```toml
[editor]
keybindings = "turbo-c"
theme = "monokai"
tab_width = 8

[build]
command = "make -C src"

[keys]
f5 = "build_and_run"
"ctrl+shift+f" = "find_in_project"
```

**madc script** (programmable — for power users):
```c
// ~/.madcide/config.mad
madcide::set_keybindings("turbo-c");
if (env("TERM") == "xterm-256color")
    madcide::set_theme("monokai");
else
    madcide::set_theme("basic16");
```

TOML is the default. If `config.mad` exists alongside `config.toml`,
the script runs after TOML is loaded (can override). The madc script
approach gives Emacs/Vim-level programmability — conditional keybindings,
custom commands, macros — all JIT-compiled.

## Implementation Phases

### Phase 1: libmadcedit core (3-4 weeks)
- Piece table buffer model
- Cursor management (single cursor)
- Insert/delete/move operations
- Undo/redo with operation grouping
- CUA keybinding profile (Ctrl+C/V/X/Z/S)
- Plain text rendering to Level 0 (stdout)
- Gate: edit a file, save it, undo works

### Phase 2: libmadcedit curses rendering (2-3 weeks)
- Level 1 rendering: curses-based editor view
- Line numbers, status bar, cursor highlighting
- Scrolling (vertical and horizontal)
- Mouse support (click to position cursor)
- Gate: usable text editor in a terminal

### Phase 3: Syntax highlighting + more keybindings (2-3 weeks)
- Syntax highlighting engine with madc/C definition
- Vim keybinding profile (modes, composable commands)
- Emacs keybinding profile (prefix keys, kill ring)
- Turbo-C keybinding profile (F-keys, menus)
- Gate: switch between keybinding profiles at runtime

### Phase 4: madcide shell (3-4 weeks)
- IDE layout: file tree + editor + output + status
- Tabbed editor panes
- Build command execution with output capture
- Error parsing and jump-to-source
- Gate: build madc projects from within madcide

### Phase 5: Advanced editor features (ongoing)
- Find/replace with regex
- Split views
- Bracket matching, auto-indent
- Go to definition (using madc's symbol table)
- Multiple cursors
- Nano, WordStar, WordPerfect, Joe keybinding profiles

## Library Structure

```
libmadcedit.so              ← reusable editor component
  ├── buffer (piece table)
  ├── cursor
  ├── undo
  ├── keybindings/
  │   ├── cua.mad           ← CUA (Ctrl+C/V/X) — default
  │   ├── vim.mad           ← Vim modal editing
  │   ├── emacs.mad         ← Emacs prefix keys
  │   ├── turbo-c.mad       ← Turbo C F-key driven
  │   ├── wordstar.mad      ← WordStar Ctrl+K commands
  │   ├── nano.mad          ← Nano/Pico simple
  │   └── custom.mad        ← user-defined
  ├── syntax/
  │   ├── c.mad             ← C/madc highlighting
  │   ├── cpp.mad           ← C++ highlighting
  │   └── generic.mad       ← strings + comments
  └── render (semantic IR output)

madcide                      ← the IDE application
  ├── uses libmadcedit
  ├── file browser
  ├── build integration
  ├── error navigation
  └── config system
```

## Prerequisites

- Rendering abstraction Phase 1 (Level 0 — semantic IR + text output)
- Rendering abstraction Phase 2 (Level 1 — curses backend)
- Track 2.1 (constructors/destructors — for editor object lifecycle)
- libmadc (for config file JIT compilation)

## The SMAUG Connection

SMAUG's OLC (Online Creation) system lets builders edit room descriptions,
mob programs, and area files. Currently this uses a primitive line editor.
`libmadcedit` could provide a full in-game editor with syntax highlighting
for MobProg scripts — same editor library used in madcide AND inside the
running MUD.
