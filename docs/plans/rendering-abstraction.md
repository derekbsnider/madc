# Rendering Abstraction Layer Plan

Research performed 2026-05-24.

## Vision

A unified UI abstraction for madc that targets:
1. **Plain text terminal** — simple print output
2. **Curses/ncurses console** — interactive TUI apps
3. **Web apps** — server-side rendering with thin browser client
4. **Native GUI** — desktop windows via SDL/GTK/etc.

Think "QML/JSX but in C" — using madc as the scripting language instead
of JavaScript. No new markup language — C syntax with struct initializers,
function calls, and lambdas is sufficient.

## Industry Research

### What works (proven patterns)

| Framework | Language | Approach | Key Insight |
|-----------|----------|----------|-------------|
| **Clay** | C99 | Declarative macros → render commands | Flexbox in C, single header, arena alloc |
| **Dear ImGui** | C++ | Immediate mode → draw lists | `if (Button("x"))` pattern for events |
| **ImTui** | C++ | ImGui API → ncurses output | Same API, terminal backend |
| **Nuklear** | C89 | Immediate mode → command buffer | No malloc in hot path, 18K LOC |
| **MicroUI** | C | Immediate mode → command list | 1,100 LOC, minimal viable UI |
| **Textual** | Python | Widget tree → ANSI or HTML/WebSocket | Terminal + web from same code |
| **Slint** | Rust/C++ | Compiled .slint markup → native code | QML-inspired, 3 rendering backends |
| **FTXUI** | C++ | Functional composition → terminal | React-inspired TUI |

### The universal pattern

Every successful cross-target UI library uses a **render command list**:

```
UI code → render command list → backend driver
                                 ├── terminal (ANSI)
                                 ├── curses (ncurses)
                                 ├── web (WebSocket → DOM)
                                 └── native (SDL/OpenGL/GTK)
```

The command list is the abstraction boundary. UI code produces abstract
commands (draw rect, draw text, set clip). Backend drivers consume them.

## Design for madc

### Architecture: `ui::` namespace with render command list

```
madc script
    │
    ▼ ui::button(), ui::text(), ui::panel(), etc.
    │
    ▼ Render command list (tagged union array)
    │
    ├──► Terminal driver: ANSI escape codes / plain text
    ├──► Curses driver: ncurses calls via #load
    ├──► Web driver: WebSocket server → thin JS client
    └──► Native driver: SDL2/GTK via #load
```

### What declarative UI looks like in madc

**Approach A: Immediate mode (recommended for Phase 1)**
```c
ui::begin_panel("main", { .direction = UI_VERTICAL });
    ui::text("Hello World");
    if (ui::button("Click me")) {
        counter++;
    }
    ui::text(format("Count: %d", counter));
ui::end_panel();
```

Simplest to implement. The `if (button())` pattern handles events
elegantly. No tree construction needed. Same pattern as Dear ImGui,
Nuklear, MicroUI.

**Approach B: Function-call tree (for retained mode / web)**
```c
ui::element *root = ui::column(
    ui::text("Hello", { .font_size = 24 }),
    ui::button("Click me", [&]() { counter++; }),
    ui::text(format("Count: %d", counter))
);
ui::render(root);
```

Returns a tree of element nodes. Enables diffing for efficient web DOM
updates. More complex but needed for the web backend.

**Approach C: Elm/MVU architecture (cleanest separation)**
```c
struct Model { int counter; };

void update(Model *m, int msg) {
    if (msg == MSG_INCREMENT) m->counter++;
}

ui::element *view(Model *m) {
    return ui::column(
        ui::text(format("Count: %d", m->counter)),
        ui::button("Increment", MSG_INCREMENT)
    );
}

int main() {
    Model m = { .counter = 0 };
    ui::run(&m, update, view);
}
```

Pure `view` function, centralized `update`. Maps cleanly to madc's
structs and function pointers.

### madc features that enable this

| Feature | UI Role |
|---------|---------|
| Structs with initializers | Component configuration |
| Lambdas `[&]() {}` | Event handlers |
| Namespaces (`ui::`) | API surface |
| `#load` | Load ncurses, SDL, GTK at runtime |
| `std::string` | Text content |
| Function pointers | Callbacks, view functions |
| Multi-return | Error handling from UI operations |
| JIT compilation | Hot-reload of UI code |

### Missing language feature

**Tagged unions / sum types** — needed for polymorphic render command
lists and element trees. Currently simulated with `struct { int type;
union { ... } data; }`. Could be a language addition (C++17
`std::variant` equivalent, or Rust-style `enum`).

## Render Command Structure

```c
enum RenderCommandType {
    RC_RECT, RC_TEXT, RC_LINE, RC_CLIP, RC_UNCLIP,
    RC_SCROLL, RC_IMAGE, RC_INPUT
};

struct RenderCommand {
    int type;
    int x, y, w, h;          // bounding box
    int color_fg, color_bg;   // colors
    int style;                // bold, italic, underline
    const char *text;         // for RC_TEXT
    void *userdata;           // backend-specific
};
```

Arena-allocated per frame (reset each render cycle). No per-command
malloc. This is the MicroUI/Nuklear pattern.

## Backend Drivers

### Terminal (Phase 1) — simplest
- Map `RC_TEXT` → `printf()` with ANSI color codes
- Map `RC_RECT` → box-drawing characters
- Map `RC_CLIP` → track viewport bounds
- ~200 lines of C code

### Curses (Phase 2) — interactive TUI
- `#load "libncursesw.so" as curses;`
- Map `RC_TEXT` → `mvaddstr(y, x, text)`
- Map `RC_RECT` → `mvhline()`/`mvvline()`
- Input via `getch()` → event loop
- ~500 lines

### Web (Phase 3) — server-side rendering
- madc runs a WebSocket server (via `#load "libwebsockets.so"`)
- Serialize render commands as JSON
- Thin JS client (~100 lines) interprets commands → DOM updates
- Same app code, different backend — Textual proves this works
- ~1,000 lines (server + client)

### Native GUI (Phase 4) — desktop
- `#load "libSDL2.so" as sdl;` or `#load "libgtk-4.so" as gtk;`
- Map `RC_RECT` → `SDL_RenderFillRect()` or `gtk_drawing_area`
- Map `RC_TEXT` → `SDL_RenderCopy()` with font texture or Pango
- ~1,500 lines per backend

## Layout Engine

**Recommendation: Adopt Clay.** Clay is C99, single-header (~5,300 LOC),
arena-allocated, flexbox-inspired, and outputs a render command list.
It solves the hard layout problems (text wrapping, scrolling, flexbox
alignment) so madc doesn't have to.

Clay can be:
- Compiled directly into madc (single header include)
- Or loaded via `#load` as a shared library
- Its `CLAY()` macro syntax maps naturally to madc's C syntax

## Implementation Phases

### Phase 1: Render commands + terminal backend (1-2 weeks)
- Define `RenderCommand` struct and command list
- Implement `ui::text()`, `ui::rect()`, `ui::newline()`
- Terminal driver: ANSI output
- Gate: "Hello World" TUI renders to terminal

### Phase 2: ncurses backend + input (2-3 weeks)
- `#load "libncursesw.so"` integration
- Event loop with keyboard/mouse input
- `ui::button()` with `if (button())` returns
- Gate: interactive counter app

### Phase 3: Immediate-mode API (2-3 weeks)
- `ui::begin_panel()` / `ui::end_panel()`
- `ui::text()`, `ui::button()`, `ui::input()`, `ui::checkbox()`
- Layout: integrate Clay or build minimal stack-based layout
- Gate: multi-panel app with input

### Phase 4: Web backend (3-4 weeks)
- WebSocket server in madc
- JSON-serialized render commands
- Thin JS client for DOM updates
- Gate: same app runs in terminal AND browser

### Phase 5: Retained mode + diffing (2-3 weeks)
- Element tree construction via function calls
- Tree diffing for efficient updates (React-style)
- Needed for web backend efficiency
- Gate: 60fps updates without full re-render

### Phase 6: Native GUI backend (3-4 weeks)
- SDL2 or GTK4 driver
- Font rendering (SDL_ttf or Pango)
- Window management, resize handling
- Gate: desktop app with native look

## What NOT To Do

- **No JS engine.** madc IS the scripting engine. No V8, no QJSEngine.
- **No custom renderer.** Use existing renderers via `#load`. Don't
  build Impeller/Skia.
- **No markup language.** C syntax is the markup. Struct initializers
  and function calls are the declaration syntax.
- **No standalone web apps.** Server-side rendering with thin client.
  madc runs on the server; the browser is a terminal.
- **No reactive binding system.** Start with immediate mode (explicit
  re-render). Add reactivity only if a use case demands it.

## Relationship to Other Plans

- **cpp-support.md:** Classes with methods would make component
  abstraction cleaner (stateful widgets as class instances)
- **precompiled-headers.md:** UI headers would benefit from PCH
- **macos-arm64-port.md:** SDL2 backend works on macOS natively
- **libmadc-phase4.md:** UI apps embedding madc could use the
  library API for hot-reload

## The SMAUG Connection

SMAUG is a MUD — a text-based multiplayer game. Its output is currently
`printf` to a socket. A rendering abstraction would let SMAUG use:
- Plain text for traditional telnet clients
- Curses for local play
- Web for browser-based MUD clients
- Native GUI for a graphical MUD client

Same game code, four rendering targets. This is the vision.
