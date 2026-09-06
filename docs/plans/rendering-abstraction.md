# Universal Rendering Abstraction Plan

Revised 2026-05-24 after extensive research into 60 years of UI/UX history
and modern framework analysis.

> **2026-08-20:** Track 7 was revisited and merged with the data-substrate
> crossover into the APPROVED design
> [2026-08-20-data-hub-projection-rendering.md](2026-08-20-data-hub-projection-rendering.md).
> This doc remains the reference for the capability levels, the three-way
> negotiation, the WCAG mapping, and the UI-history lessons. Superseded by
> that design: render blocks as the primary authoring surface (Phase 1 is
> library-surface only), per-BINARY capability resolution (now
> per-connection), and hard C types in the semantic IR (now value-first:
> content = `madc::value`, classification = registry-interned ids).
>
> **2026-08-24:** the interaction layer above the hub (Context, Affordance,
> Invocation, Projection roles) is now defined by this doc's successor,
> [universal-application-interaction-rendering-abstraction.md](universal-application-interaction-rendering-abstraction.md);
> the execution plan is
> [2026-08-24-ui-interaction-rework-and-texteditor.md](2026-08-24-ui-interaction-rework-and-texteditor.md).
>
> **2026-09-06 (owner ruling):** Phase 5 (web backend) and Phase 6 (native
> GUI) are ONE target — HTML/CSS through the platform's own webview
> (WebView2 / WKWebView / WebKitGTK / Android WebView), with a browser over a
> socket as the same page on another transport. Design, recon and slices:
> [2026-09-06-ui-web-target-and-madcide-gui.md](2026-09-06-ui-web-target-and-madcide-gui.md)
> (madcide GUI mode is the first customer; `import` replaces `#load` as the
> platform-agnostic module binding). The GTK4/SDL2 native-widget phase below
> is superseded.

## Vision

A single semantic rendering abstraction that adapts to ANY display target —
from a dumb scrolling teletype to Unreal Engine — with the JIT compiler
resolving capabilities at compile time for zero-runtime-overhead adaptation.

The app developer writes to ONE API. The JIT emits specialized code for
whatever the target can do.

## Design Decisions (Confirmed)

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Declaration style | Code-first declarative | Industry consensus (SwiftUI, Compose, QML). madc controls the parser. |
| Description level | Semantic-first | Roles/labels/states/actions. Proven by accessibility trees. Degrades to any target. |
| JIT advantage | Compile-time capability resolution | Dead-code-eliminate unsupported paths. No other framework can do this. |
| Capability model | 5-level layered | Teletype → curses → 2D graphics → widgets → GPU/3D |
| Reactivity | One-way flow, compiler-tracked deps | Svelte approach at JIT time. No GC. Predictable. Universal. |

## The 5 Capability Levels

Each level is a superset of the previous. A renderer declares its level.
The JIT emits code for that level, dead-code-eliminating higher levels.

### Level 0: Sequential Text Stream
**Targets:** stdout, pipes, log files, dumb teletypes, voice synthesis
**Capabilities:** Emit characters. Line feed. That's it.
**What renders:** Role-based linearization. A `nav` becomes a text list.
A `button` becomes `[Label]`. A `heading` becomes `=== Title ===`.
**Historical model:** ASR-33 teletype (1963)

### Level 1: 2D Character Grid with Attributes
**Targets:** VT100+, xterm, curses/ncurses, SSH sessions
**Capabilities:** Cursor positioning, color, bold/underline, box drawing,
mouse (optional), alternate screen buffer.
**What renders:** Full TUI. Panels, borders, scrollable regions, menus.
**Historical model:** VT100 (1978), curses (1980), TERMINFO capability DB

### Level 2: 2D Geometric Primitives + Text Layout
**Targets:** Canvas 2D, Cairo, Skia, PDF, SVG, PostScript, printers
**Capabilities:** Lines, rectangles, arcs, fills, gradients, font rendering,
anti-aliasing, transparency, image blitting.
**What renders:** Custom-drawn widgets, charts, diagrams, rich text.
**Historical model:** PostScript (1982), Display PostScript (NeXT), Cairo

### Level 3: Widget-Level Declarative UI
**Targets:** Native GUI toolkits (GTK, Qt, Cocoa, WinUI), web DOM, mobile
**Capabilities:** Platform-native controls (buttons, text fields, lists,
scroll views, tabs), layout engines (flexbox/grid), system themes,
accessibility APIs, clipboard, drag-and-drop, IME.
**What renders:** Full native-feeling application UI.
**Historical model:** NeXTSTEP (1988), HTML/CSS (1995), SwiftUI (2019)

### Level 4: GPU-Accelerated / 3D / Spatial
**Targets:** WebGPU, Metal, Vulkan, DirectX, Unreal, Unity, Vision Pro
**Capabilities:** Shader programs, 3D geometry, lighting, physics,
spatial positioning, eye/hand tracking, haptics.
**What renders:** Games, simulations, data visualization, XR/spatial UI.
**Historical model:** OpenGL (1992), DirectX (1995), WebGPU (2024)

## Architecture

```
madc source code
    │
    ▼ Parser: render { } blocks → Semantic IR tree
    │
    ▼ Compiler: query RenderCaps → JIT-specialize
    │
    ├──► Level 0: emit sequential text generation
    ├──► Level 1: emit ANSI/curses grid operations
    ├──► Level 2: emit 2D draw commands (Skia/Cairo)
    ├──► Level 3: emit native widget construction
    └──► Level 4: emit GPU/scene graph operations
```

### Semantic IR Node

The core abstraction — what every render block produces:

```c
struct UINode {
    Role     role;        // NAVIGATION, CONTENT, ACTION, INPUT, HEADING,
                          // LIST, ITEM, IMAGE, SEPARATOR, GROUP, ...
    char    *label;       // human-readable text
    State    state;       // NORMAL, DISABLED, SELECTED, EXPANDED, ...
    Action  *actions;     // what the user can do (ACTIVATE, INPUT, SCROLL)
    UINode  *children;    // nested elements
    Hints    hints;       // optional: preferred size, color, direction, weight
};
```

Each renderer interprets this tree to its maximum capability:
- Level 0: linearizes roles and labels into text
- Level 1: maps roles to TUI widgets (borders, menus, scrollable panes)
- Level 2: draws custom-styled controls
- Level 3: maps to native platform controls
- Level 4: maps to 3D scene elements

### Three-Way Negotiation: Hardware × User × Accessibility

Rendering is NOT determined by hardware alone. Three inputs combine:

1. **Hardware capabilities:** What the display CAN do (detected)
2. **User preferences:** What the user WANTS (configured)
3. **Accessibility requirements:** What the user NEEDS (WCAG mandated)

User preferences can override hardware in BOTH directions:
- **Down:** "I have a GPU but prefer clean terminal output"
- **Up:** "My terminal is basic but give me fancy box-drawing and color"
- **Sideways:** "I have full GUI but need high-contrast, large text, no animation"

The effective render level is: `min(hardware_cap, user_preferred_max)`
unless the user explicitly requests higher (up-rendering on limited hardware).

```c
struct RenderProfile {
    // Hardware (detected at startup)
    RenderCaps  hardware;

    // User preferences (from config file, env, or runtime API)
    uint8_t     preferred_level;    // user may prefer Level 1 on Level 3 hardware
    bool        prefer_simplicity;  // clean/minimal over feature-rich
    float       font_scale;         // 1.0 = default, 2.0 = double size
    bool        high_contrast;      // WCAG AAA contrast ratios
    bool        reduce_motion;      // no animations, no transitions
    bool        reduce_transparency;
    bool        screen_reader;      // optimize for assistive technology
    bool        keyboard_only;      // no mouse/touch assumed
    ColorMode   color_mode;         // FULL, HIGH_CONTRAST, MONOCHROME, CUSTOM
    char       *custom_theme;       // user-specified color theme
};
```

The JIT compiler resolves the effective profile at compile time.
Both hardware caps AND user prefs are compile-time constants:

```c
// User prefers terminal even though hardware supports GUI
render {
    chart(data)    // JIT sees: user.preferred_level < 2 → emit table instead
    button("OK")   // JIT sees: user.keyboard_only → ensure focus order
}
```

### WCAG Compliance by Design

The semantic IR naturally satisfies WCAG because it IS an accessibility tree:

| WCAG Principle | How the Semantic IR Satisfies It |
|----------------|----------------------------------|
| **Perceivable** | Every element has a `role` and `label`. No information conveyed by color alone (hints are optional, roles are not). |
| **Operable** | Every interactive element has `actions`. Focus order derived from tree structure. Keyboard navigation is implicit. |
| **Understandable** | Roles carry semantic meaning. A `ROLE_NAVIGATION` is always navigation regardless of visual presentation. |
| **Robust** | The IR is consumed by renderers, screen readers, and test harnesses identically. One tree, many consumers. |

Specific WCAG features built into the render profile:
- **Focus management:** Tree order defines tab order. `ROLE_ACTION` elements are focusable. Skip-navigation via `ROLE_MAIN`.
- **Color independence:** `Hints.color` is OPTIONAL. Renderers that ignore color still convey all information via roles and labels.
- **Text alternatives:** Every `ROLE_IMAGE` requires a `label` (alt text). The IR rejects images without labels at compile time.
- **Motion control:** `user.reduce_motion` is a JIT-time constant. Animation code is dead-code-eliminated entirely — not just paused, REMOVED from the binary.
- **Font scaling:** `user.font_scale` flows through the layout engine. No clipping, no overflow — layout adapts.
- **Screen reader mode:** When `user.screen_reader` is true, the renderer emits ARIA attributes (web) or native accessibility API calls (desktop) directly from the semantic IR.

### Capability Negotiation at JIT Time

```c
struct RenderCaps {
    uint8_t  level;         // 0-4
    bool     color;
    bool     truecolor;
    bool     mouse;
    bool     keyboard;
    bool     touch;
    bool     hover;
    bool     gpu;
    bool     spatial_3d;
    uint16_t width, height; // in logical units
    // ...
};
```

The JIT compiler treats `RenderCaps` fields as compile-time constants.
Branches conditioned on capabilities are resolved at JIT time:

```c
render {
    heading("Dashboard")
    if (caps.level >= 2) {
        chart(data)                    // only emitted for 2D+ targets
    } else {
        table(data)                    // text/grid fallback
    }
    button("Refresh", on_refresh)      // semantic — all levels handle it
}
```

The `if (caps.level >= 2)` is evaluated at JIT compile time. The terminal
build has no chart code in the binary. The GPU build has no table code.
Zero runtime overhead.

### Reactivity: Compiler-Tracked Dependencies

The JIT compiler analyzes which state variables each render block reads,
then emits targeted re-render code — no runtime dependency graph, no
observer pattern, no GC.

```c
int counter = 0;

render {
    text(format("Count: %d", counter))  // compiler tracks: reads 'counter'
    button("+", [&]() { counter++; })
}
```

When `counter` changes, only the `text` node is re-evaluated. The `button`
node is unchanged and skipped. The compiler determined this statically.

Arena allocation for the view tree: each render cycle allocates from a
fresh arena, previous arena freed in bulk after diff is applied. No
per-node malloc/free.

## Syntax: Native `render` Blocks

Since madc controls the parser, `render` blocks are a first-class language
construct. Semantic roles are inferred from block names:

```c
render(target) {
    nav {
        item("File", show_file_menu)
        item("Edit", show_edit_menu)
    }
    main {
        heading("Welcome")
        if (logged_in) {
            content {
                text(user.bio)
                list(user.posts, [](Post p) {
                    item(p.title, [&]() { open_post(p); })
                })
            }
        } else {
            action("Log In", open_login)
        }
    }
    status {
        text(format("%d items", item_count))
    }
}
```

Block names map to roles:
- `nav` → ROLE_NAVIGATION
- `main` → ROLE_MAIN
- `heading` → ROLE_HEADING
- `content` → ROLE_CONTENT
- `action` / `button` → ROLE_ACTION
- `item` → ROLE_ITEM
- `list` → ROLE_LIST
- `input` → ROLE_INPUT
- `status` → ROLE_STATUS
- `group` → ROLE_GROUP

Structural hints via modifiers (optional, renderer may ignore):

```c
render {
    main {
        text("Hello") .size(24) .color(0xFF0000) .weight(BOLD)
        button("Click") .width(200) .padding(8)
    } .direction(HORIZONTAL) .gap(16)
}
```

## Lessons from 60 Years of UI History

| Historical Lesson | How We Apply It |
|-------------------|-----------------|
| TERMCAP/TERMINFO capability DB (1978) | RenderCaps struct, probed at JIT time |
| Curses retained-mode + diff (1980) | Arena-allocated view tree with differential updates |
| VT100 backward compatibility | Level 0 always works; higher levels add, never break |
| NAPLPS resolution-independent coords (1980) | Logical units throughout, not physical pixels |
| RIPscrip/Flash/Silverlight death | No proprietary format. No plugin. Open. |
| HTML's success (1995) | Semantic base. Progressive enhancement. Zero-install via web. |
| HyperCard's lost vision (1987) | Blur user/developer line via JIT hot-reload |
| Java AWT/Swing failure | "Same everywhere" fails; "appropriate everywhere" succeeds |
| X11 network transparency failure | No serialization boundaries in render pipeline |
| React's virtual DOM (2013) | UI = f(state). One-way flow. Diff-based updates. |
| SwiftUI/Compose convergence (2019-21) | Code-first declarative is the consensus |
| Accessibility trees | Semantic IR is the proven universal contract |
| HTMX/LiveView thin-client renaissance | Support both server-side and client-side rendering |
| Microsoft UI framework churn | Don't couple to platform strategy. Be platform-neutral. |
| Svelte compiler-tracked reactivity | JIT-time dep tracking. No runtime observer overhead. |

## Implementation Phases

### Phase 1: Semantic IR + Level 0 (2-3 weeks)
- Define `UINode` struct and `Role` enum
- `render { }` block parsing in parser.cpp
- Level 0 renderer: linearize semantic tree to stdout
- Gate: render block prints formatted text

### Phase 2: Level 1 — Terminal/Curses (3-4 weeks)
- ncurses backend via `#load`
- Map roles to TUI widgets
- Input event loop, differential updates
- Gate: interactive TUI app

### Phase 3: Reactivity + State (2-3 weeks)
- Compiler-tracked dependencies in render blocks
- Arena-allocated view trees
- One-way data flow cycle
- Gate: only changed nodes re-render

### Phase 4: Level 2 — 2D Graphics (3-4 weeks)
- Skia or Cairo backend via `#load`
- Gate: charts alongside text UI

### Phase 5: Level 3 — Web Backend (4-6 weeks)
- WebSocket server + thin JS client
- Semantic IR diffs → DOM updates
- Gate: same app in terminal AND browser

### Phase 6: Level 3 — Native GUI (4-6 weeks)
- GTK4 or SDL2 backend via `#load`
- Gate: desktop app with native look

### Phase 7: Level 4 — GPU/3D (future)
- WebGPU/Metal/Vulkan via `#load`
- Gate: 3D visualization or spatial UI

## The SMAUG Connection

Same MUD game code, five experiences:
- **Level 0:** Traditional telnet (scrolling text)
- **Level 1:** Curses TUI (split-pane, status bar)
- **Level 2:** Graphical map overlay (RIPscrip's dream, done right)
- **Level 3:** Web-based MUD client in any browser
- **Level 4:** 3D rendered rooms and characters

## What We Do NOT Build

- No JS engine — madc IS the scripting engine
- No custom renderer — use Skia/Cairo/WebGPU via `#load`
- No markup language — C syntax IS the declaration
- No plugin/runtime — JIT compiles to native
- No reactive binding system — compiler tracks deps statically
- No layout engine from scratch — adopt Clay/Yoga via library
