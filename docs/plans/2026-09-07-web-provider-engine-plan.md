# Web Provider — Engine Half Implementation Plan (slice 2 of the web-target arc)

> **For agentic workers:** execute task-by-task, one gated commit each, in
> order. Steps use checkbox (`- [ ]`) syntax for tracking. Every `src/` /
> `include/` commit carries the four rule trailers (`Hypothesis:` / `Layer:` /
> `Searched:` / `Oracle:`). Targeted tests per task; the FULL battery runs
> once, at the merge wave (Task 12).

**Goal:** a madc program renders the same value tree it renders on the
terminal into a window through the platform webview, receives the same
semantic events back, and vised.mad edits a file in that window — with the
compiler's `import` binding finishing its lane coverage (objects carry their
module list) and madc's resource guards defaulting off.

**Architecture:** the Level-1 pair (model + target) repeated at the TREE
level instead of the grid level. `web_model` (dependency-free C++,
`include/madcdis/web_model.h`) turns the `uinode` tree into keyed DOM
operations as JSON and page input back into the SAME `tui_event` objects
the TUI emits; the chord/key owner and the focus/navigation owner are
factored OUT of `tui_model` into shared headers so both models run one
implementation. The web TARGET is madc source: the embedded fragment
`<ns_ui_web>` does `import madcwebview;` over the typed upstream C header
(Astra's slice-2 build half) and hosts ONE embedded page (JS applier +
CSS). `src/ns_ui.cpp` gains a target-generic session (`ui::open(target)`)
with two frontends — grid (today's code, moved) and DOM — and the `tui_*`
names become the `term` target's spellings. The engine's C++ never names a
platform library.

**Tech stack:** C++11 (madcdis headers, doctest), madc dialect (the fragment,
value-first), one JS + one CSS file embedded like the headers, webview/webview
0.12.0 through `libmadcwebview` (already built by `make -C src libmadcwebview`),
the `gui` stage of `scripts/remote_build.sh` (Xvfb).

**Spec:** `docs/plans/2026-09-06-ui-web-target-and-madcide-gui.md` §3.2–§3.7,
§3.9, §5 slice 2, plus the owner rulings of 2026-09-07 recorded in the KG:
`Decision resource_guards_default_off`, `Gap obj_lane_module_dependency`
(DECIDED), `Gap gui_memory_guard_policy` (DECIDED), and the approved order
(key owner out → `web_model` → fragment + page → `ui::` surface → gate).
Astra's build half: `docs/building-webview.md`, `include/madc/webview.h`.

## Global constraints

- **Rule #7 / one owner:** no second copy of chord resolution, focus
  navigation, key spelling, or the event value shape. Task 1 and Task 2 leave
  gates behind (`scripts/check-one-key-owner.sh`).
- **Value-first madc code** (`.claude/rules/value-first.md`): the fragment
  carries ZERO includes, spells the carrier `var`, prints through bare
  `println`; handles are `webview_t` / `void *`, NEVER `var`.
- **Dialect-lean** (`.claude/rules/dialect-lean.md`): `<ns_ui_web>` uses only
  `const char *`, integers, pointers and the webview C types; the gate
  `check-dialect-lean.sh` runs in fulltest.
- **No key→action logic in JavaScript; no editor knowledge in the page**
  (design §3.2, §3.9). The page sends raw key spellings in the TUI vocabulary
  (`tui_key_name`: `"^k"`, `"up"`, `"enter"`, printable runs).
- **Thread contract** (design §3.7): a web frontend is confined to the thread
  that opened it; `webview_dispatch` is the only cross-thread door and this
  slice does not use it.
- **Every task's tests run on the CONTAINER** (`scripts/remote_build.sh sync
  build tests-all` with `TESTS=`; unit tests via `unittest`; the GUI battery
  via `gui`). The NAS never builds or tests. One heavy container job at a
  time. No `&&` chains in shell calls; script files are fine.
- **Trailers on every src/include commit**; `Oracle:` for a pure refactor is
  `n/a — behaviour-preserving, suite is the oracle`.
- **Branch:** `feature/web-provider-engine-claude` off `develop` (≥ 2272cff1).
  Merge wave = the four develop lanes + the `gui` stage (Task 12).

## Facts the executor needs (recon 2026-09-07, verified in source)

- `include/madcdis/tui_model.h` (1492 lines, `namespace madc::hub`): keys at
  436–530 (`tui_key`, `tui_keyev`, `tui_key_name`, `tui_key_from_name`),
  `tui_bindings` 532–659, `tui_keyparse` (terminal byte→key adapter) 660–785,
  events 786–825 (`tui_event_kind`, `tui_event`), `tui_model` 826–1489 with
  `_bindings`/`_pending` at 851–852, `set_bindings`/`pending_chord` at
  1306–1311, `apply_keys` at 1322–1487 (chord resolution 1329–1372, printable
  coalescing 1373–1385, head-binding lookup 1386–1409, focus/choice navigation
  1410–1486 over `_focusables`, `_focus`, `_selection`, `selection_of`).
- `src/ns_ui.cpp`: `ui_session` (verbs, world) ~96–112; `ui_tui` 130–140
  (`tui_target *target; tui_model model; tui_grid painted; queue; rows,
  cols`); `ui_tuis()` handle table; `tui_open` 962–980 calls
  `register_builtin_tui_targets()` + `create_tui_target(NULL)`; `tui_render`
  1010–1020 (`model.compose(s->r, value_to_uinode(s->w, tree), rows, cols)`
  then `target->paint`); `tui_bind_keys` 1107–1120 (`table_to_bindings`);
  `tui_event` 1149–1217 (the event → value object switch); `tui_pending`
  1219. `include/madc/ns_ui` declares the `tui_*` publics at 276–298.
- `include/madcdis/tui_provider.h`: `tui_target` (open/close/paint/read_keys/
  suspend/resume/size), `register_tui_target`, `create_tui_target`,
  `register_builtin_tui_targets` (defined `src/ui_term.cpp:829`, registers
  `"term"`).
- `include/madcdis/uinode.h`: `struct uinode { name_id role; ...; content;
  children; hints }`, `value_to_uinode(const world &, const value &)` at 150,
  `roles` (`heading`, `status`, `content`, `edit`, `choice`, …) — the
  vocabulary `tui_model::compose` switches on (908–1030).
- Auto-include: `src/lexer.cpp:1239 auto_include_header_for_identifier` maps
  a namespace identifier to its fragment (`{"ui", "ns_ui"}`); a new row
  `{"ui_web", "ns_ui_web"}` makes any mention of `ui_web::` pull the web
  fragment. Embedded files come from `include/madc/**` via
  `scripts/gen_embedded_headers.sh` (any file, key = path relative to
  `include/madc/`); C++ reads one with `find_embedded_header(name)`
  (`include/madc.h:1693`).
- webview 0.12.0 run loops are RE-ENTRANT after `terminate`: GTK
  `run_impl` resets `m_stop_run_loop` then iterates (`webview.h:1951–1960`),
  Cocoa `[NSApp run]` / `stop_run_loop` (2312–2320), Win32 `GetMessageW` /
  `PostQuitMessage` (4060–4087). So "run until the page posts one event, then
  terminate" is a valid pull-based loop on all three.
- Object lane: `src/madc_cir.cpp:2236 madc_cir_run_object` →
  `MIR_object_load(bytes, cir_run_object_resolve)`; symbols of a loaded image
  via `MIR_object_loaded_sym(lo, name)` (`third_party/mir/mir-debug.h:409`);
  `madc_cir_run_objects` 2415 (multi-object merge). Module spellings live in
  `Program::module_link_libs` (`include/madc.h:4224`; filled by
  `bind_module_namespace`, `src/lexer.cpp:1724`), consumed by `src/madc.cpp:
  1430` (single TU → `cc_link_args`) and `src/madc_cir.cpp:6412` (project).
  The CIR builder's unit-level extern flush is the `m_output_externs` pass
  (`src/cir_builder.cpp` ~30120–30200); the slot declaration in
  `dyn_module_callee` (28630–28652) shows the `N_SPEC_DECL` operand layout
  (specs, declarator, then three operands, the last the initializer —
  `ignore()` when absent); initializer lists are `N_INIT` rows (8942, 9015).
- Guards: `src/madc.cpp:140 install_resource_guards(project_tus, cfg)` —
  `env_rlim` (85) reads an env knob as a number, CPU arm 156–170 (opt-in),
  memory arm 176–200 sets `rlim_cur = rlim_max` (HARD) on `RLIMIT_AS`;
  `--help` text 515–530; `madc.ini` keys `cpu-limit` / `mem-limit`
  (`src/madc_config.cpp:41`, `include/madc_config.h:48`). The runner
  invokes tests at `scripts/run_tests.sh:264/266/292/294` (JIT), 384/386
  (obj), 427/429 (exe). The `gui` stage: `scripts/remote_build.sh:249–257`
  (exports `MADC_MEM_LIMIT=$gui_mem`, default 0).
- Module rows: `include/madc_modules.h` `MadcModuleSpec{name, interface,
  posix, darwin, windows}`, table in `src/madc_modules.cpp:14–18` (rows `c`,
  `m`, `madcwebview`), unit test `tests/unit/test_modules.cpp` (8 cases).
- GUI tests: `tests/gui/webview.mad` (+`.expect`, `.timeout`) is the pattern —
  the bound callback prints, `.expect` lines must appear; run by
  `remote_build.sh gui` (JIT + EXE under `xvfb-run`).
- vised: `tools/texteditor/vised.mad` → `vised_core.inc:135–149`
  (`ui::tui_open()`, loop `tui_render` / `tui_event`, `tui_close`).

---

### Task 1: the shared key owner — `madcdis/keys.h`

**Files:**
- Create: `include/madcdis/keys.h`
- Modify: `include/madcdis/tui_model.h` (delete the moved declarations;
  `apply_keys` calls the resolver)
- Create: `tests/unit/test_keys.cpp` (the four key/binding/chord cases move
  here from `tests/unit/test_tui_model.cpp`: lines 818–1062 "key spelling —
  one owner, both directions", "bindings — build validation is loud and
  whole-table", "chords — a ctrl-held continuation completes the chord
  (JOE)", "chords — resolve, coalesce around, miss, cancel, persist")
- Modify: `tests/unit/test_tui_model.cpp` (remove the moved cases; keep
  "chords — bindings win over navigation; a swap restores it" — it exercises
  the model's integration)
- Modify: `src/Makefile` (add `test_keys` beside `test_tui_model` in the unit
  test list — grep `test_tui_model` for the three places)
- Create: `scripts/check-one-key-owner.sh`; Modify: `src/Makefile` fulltest
  (line after `check-one-module-member-owner.sh`)

**Interfaces:**
- Produces (namespace `madc::hub`, all moved verbatim from `tui_model.h`):
  `enum class tui_key`, `struct tui_keyev`, `tui_key_name(const tui_keyev &)`,
  `tui_key_from_name(const std::string &, tui_keyev &)`, `class tui_bindings`
  (unchanged API: `bind`, `finalize`, `bound`, `prefix`, `action_of`,
  `seq_spelling`, `cont_spelling`, `empty`).
- Produces NEW: the chord resolver — the one owner of chord-pending state:

```cpp
// include/madcdis/keys.h  (namespace madc::hub)
// One key through the installed bindings. The caller (a model) hands
// every non-printable key here FIRST; printable runs it coalesces itself
// (the §7.5 rule), except that a PENDING chord consumes printables too.
struct key_step
{
    enum class kind : unsigned char
    {
	passthrough,	// not bound, no chord pending: the model's own rules apply
	pending,	// a chord started or extended: repaint (status echoes it)
	cancelled,	// esc cancelled a pending chord: repaint
	action,		// a bound sequence completed: action_name (empty = miss), seq
	transparent	// resize/wake mid-chord: pass through, chord untouched
    };
    kind k;
    std::string action_name, seq;
    key_step() : k(kind::passthrough) {}
};

class key_resolver
{
    tui_bindings _bindings;
    std::string _pending;	// chord so far (canonical)
public:
    void set_bindings(const tui_bindings &b) { _bindings = b; _pending.clear(); }
    const tui_bindings &bindings() const { return _bindings; }
    const std::string &pending() const { return _pending; }
    // True when this key was consumed or classified by the bindings —
    // exactly the decisions tui_model::apply_keys made at 1329–1409
    // (2026-09-07 line numbers), moved here unchanged. A printable key
    // with NO chord pending returns passthrough without touching state.
    key_step step(const tui_keyev &k);
};
```

- `tui_model` keeps `_focusables`, `_focus`, `_selection`, `selection_of`,
  and the navigation arms (1410–1486); its `_bindings` + `_pending` become
  ONE `key_resolver _keys;`; `set_bindings(b)` forwards; `pending_chord()`
  returns `_keys.pending()`; `apply_keys` becomes: for each key → if
  `_keys.step(k)` is `pending`/`cancelled` → push a `focus` event; `action`
  → push the action event; `transparent` → resize/wake event; `passthrough`
  → the existing printable-coalescing + navigation code, byte-for-byte.
- `tui_keyparse` (the VT byte adapter) STAYS in `tui_model.h` (terminal
  input, not key semantics).

- [ ] **Step 1: move the declarations.** Cut lines 436–659 of `tui_model.h`
      (keys + bindings) into `include/madcdis/keys.h` with the header
      guard `__MADCDIS_KEYS_H`, includes `<map> <set> <string> <vector>`,
      the thread-contract comment ("plain value objects; confined with the
      model that owns them"), and add `#include "madcdis/keys.h"` to
      `tui_model.h`. Build (`remote_build.sh sync build`) — must be green
      before any behaviour change.
- [ ] **Step 2: write the failing tests.** `tests/unit/test_keys.cpp` —
      preamble identical to `test_tui_model.cpp` lines 8–18 but including
      `madcdis/keys.h` only; move the four cases; add ONE new case:

```cpp
TEST_CASE("key_resolver — the chord state machine stands alone")
{
    using namespace madc::hub;
    tui_bindings b;
    std::string err;
    REQUIRE(b.bind("^k s", "save", err));
    REQUIRE(b.bind("^q", "quit", err));
    REQUIRE(b.finalize(err));
    key_resolver r;
    r.set_bindings(b);
    CHECK(r.step(tui_keyev(tui_key::ch, 'a')).k == key_step::kind::passthrough);
    key_step s1 = r.step(tui_keyev(tui_key::ctrl, 'k'));
    CHECK(s1.k == key_step::kind::pending);
    CHECK(r.pending() == "^k");
    key_step s2 = r.step(tui_keyev(tui_key::resize));
    CHECK(s2.k == key_step::kind::transparent);
    CHECK(r.pending() == "^k");
    key_step s3 = r.step(tui_keyev(tui_key::ch, 'S'));	// case-insensitive continuation
    CHECK(s3.k == key_step::kind::action);
    CHECK(s3.action_name == "save");
    CHECK(s3.seq == "^k s");
    CHECK(r.pending().empty());
    key_step s4 = r.step(tui_keyev(tui_key::ctrl, 'q'));
    CHECK(s4.k == key_step::kind::action);
    CHECK(s4.action_name == "quit");
    r.step(tui_keyev(tui_key::ctrl, 'k'));
    CHECK(r.step(tui_keyev(tui_key::esc)).k == key_step::kind::cancelled);
    r.step(tui_keyev(tui_key::ctrl, 'k'));
    key_step miss = r.step(tui_keyev(tui_key::ch, 'z'));
    CHECK(miss.k == key_step::kind::action);
    CHECK(miss.action_name.empty());
    CHECK(miss.seq == "^k z");
}
```

      Exact `tui_bindings` method names: read `keys.h` lines for `bind` /
      `finalize` signatures before writing (the four moved cases show them).
- [ ] **Step 3: run the unit build to see it fail** (`remote_build.sh sync
      unittest`): `key_resolver` undefined.
- [ ] **Step 4: implement `key_resolver::step`** by MOVING the code of
      `apply_keys` 1329–1409 into it (pending branch → `pending` /
      `cancelled` / `action` / `transparent`; head lookup → `action` /
      `pending`; else `passthrough`). Rewrite `apply_keys` to dispatch on
      the step kind and keep 1373–1385 and 1410–1486 verbatim. Delete the
      model's `_bindings` / `_pending` members.
- [ ] **Step 5: unit battery green** (`unittest`): test_keys 5 cases,
      test_tui_model 21 cases (25 − 4 moved), all other binaries unchanged.
- [ ] **Step 6: the gate.** `scripts/check-one-key-owner.sh` (shape of
      `check-one-module-member-owner.sh`): the pattern
      `std::string _pending|_bindings\.prefix\(|_bindings\.action_of\(`
      must appear in no `src/*.cpp`, `include/**/*.h` file except
      `include/madcdis/keys.h`; negative control = a temp file declaring
      `std::string _pending;`. Wire into `src/Makefile` fulltest after
      `check-one-module-member-owner.sh`. Run it locally (grep-only).
- [ ] **Step 7: targeted integration tests** on the container:
      `TESTS='testtui* testmadcide* testvised* testlined* testeditcheck
      testbindgate testreenter' remote_build.sh tests-all` — all green
      (JIT/exe/obj).
- [ ] **Step 8: commit** — `keys: the chord/key owner leaves tui_model —
      madcdis/keys.h (tui_key, tui_keyev, spelling, tui_bindings,
      key_resolver); tui_model consumes the resolver; gate
      check-one-key-owner.sh`. Trailers: Hypothesis = "the chord state
      machine is tui_model-private and web_model would need a copy";
      Layer = `ns_ui tui_event -> tui_model::apply_keys -> (chord
      resolution inline) — the resolution is a model-independent rule, so
      it moves one level down into its own owner; both models consume it`;
      Searched = `grep -n '_pending\|_bindings' include/madcdis src` (concept:
      who resolves a chord) → one site, tui_model; Oracle = `n/a —
      behaviour-preserving, suite is the oracle (test_tui_model 21/21 +
      test_keys 5/5 + the tui/madcide integration set)`.

### Task 2: the shared focus/navigation owner — `madcdis/ui_focus.h`

**Files:**
- Create: `include/madcdis/ui_focus.h`
- Modify: `include/madcdis/tui_model.h` (`_focusables`, `_focus`,
  `_selection`, `selection_of`, `struct focusable` and the navigation arms
  1410–1486 move; `compose` keeps DISCOVERING focusables, now into the
  shared object)
- Modify: `tests/unit/test_tui_model.cpp` (the two "events —" cases stay:
  they drive the model end-to-end); Create: cases in `tests/unit/test_keys.cpp`
  for the focus owner (rename the binary's source to `test_input.cpp`? NO —
  keep `test_keys.cpp`, add `#include "madcdis/ui_focus.h"`)
- Modify: `scripts/check-one-key-owner.sh` (second pattern:
  `option_count\b.*%|_selection\[` outside `ui_focus.h`)

**Interfaces:**

```cpp
// include/madcdis/ui_focus.h  (namespace madc::hub)
// Presentation state a frontend keeps between composes: which focusable
// has focus, each choice's live selection; and the navigation rules
// (tab cycles; arrows move a focused choice's selection; enter chooses;
// any other key rides through with the focused choice's selection) —
// moved from tui_model::apply_keys 1410–1486 (2026-09-07), unchanged.
struct focusable
{
    enum class kind : unsigned char { choice, edit };
    kind k;
    size_t option_count;
    std::vector<name_id> option_actions;
    focusable() : k(kind::choice), option_count(0) {}
};

class focus_state
{
    std::vector<focusable> _focusables;
    size_t _focus;
    std::map<size_t, size_t> _selection;
public:
    focus_state() : _focus(0) {}
    // compose() calls these in discovery order (the Phase-1 identity rule).
    void begin_compose();			// clears the list, keeps focus/selection
    size_t add(const focusable &f);		// returns the slot
    void end_compose();				// clamps _focus to the list
    size_t focus() const { return _focus; }
    bool has_focus_slot() const { return _focus < _focusables.size(); }
    const focusable &focused() const;		// precondition has_focus_slot()
    size_t selection_of(size_t slot) const;	// 0 when never moved
    bool on_choice() const;			// the `on_choice` predicate at 1410
    // The navigation step for a PASSTHROUGH key: fills `e`, returns true when
    // the key was consumed (focus/selection changed or a choose fired);
    // false = the application's key (e is filled as the key event, carrying
    // the focused choice's selection when on_choice()).
    bool navigate(const tui_keyev &k, tui_event &e);
};
```

- `tui_model` replaces its three members with `focus_state _focus_st;` and
  `compose` calls `begin_compose/add/end_compose` where it pushed into
  `_focusables` before; `apply_keys`'s tail becomes `if (!_focus_st.navigate
  (k, e)) {/* e is the key event */} out.push_back(e);` plus the resize/wake
  arms. Any accessor the model exposed for tests (`focus()`, selection) keeps
  its name, forwarding.

- [ ] **Step 1: failing test** in `test_keys.cpp`:

```cpp
TEST_CASE("focus_state — tab cycles, arrows select, enter chooses, keys ride through")
{
    using namespace madc::hub;
    focus_state f;
    f.begin_compose();
    focusable menu; menu.k = focusable::kind::choice; menu.option_count = 3;
    menu.option_actions.assign(3, (name_id)0); menu.option_actions[1] = 42;
    f.add(menu);
    focusable ed; ed.k = focusable::kind::edit; f.add(ed);
    f.end_compose();
    tui_event e;
    CHECK(f.on_choice());
    CHECK(f.navigate(tui_keyev(tui_key::right), e)); CHECK(e.kind == tui_event_kind::focus);
    CHECK(f.selection_of(0) == 1);
    CHECK(f.navigate(tui_keyev(tui_key::enter), e)); CHECK(e.kind == tui_event_kind::choose);
    CHECK(e.option == 1); CHECK(e.action == 42);
    CHECK(!f.navigate(tui_keyev(tui_key::del), e)); CHECK(e.kind == tui_event_kind::key);
    CHECK(e.choice_focused); CHECK(e.option == 1);
    CHECK(f.navigate(tui_keyev(tui_key::tab), e)); CHECK(f.focus() == 1); CHECK(!f.on_choice());
    CHECK(!f.navigate(tui_keyev(tui_key::up), e)); CHECK(e.kind == tui_event_kind::key);
}
```

- [ ] **Step 2: see it fail; Step 3: move the code; Step 4: unittest green**
      (test_tui_model unchanged 21/21 — its event cases are the oracle that
      the move preserved behaviour); **Step 5:** extend the gate's pattern;
      **Step 6:** the Task-1 integration set again (`tests-all`).
- [ ] **Step 7: commit** — `ui: the focus/navigation owner leaves tui_model
      — madcdis/ui_focus.h (focusable, focus_state::navigate); tui_model
      consumes it`. Trailers as Task 1 (Layer: "navigation rules are
      frontend-independent presentation state; a DOM frontend needs the same
      tab/arrow/enter contract — one owner, one level down"); Oracle `n/a —
      behaviour-preserving`.

### Task 3: `web_model` — tree → DOM operations, page input → events

**Files:**
- Create: `include/madcdis/web_model.h` (header-only like `tui_model.h`)
- Create: `tests/unit/test_web_model.cpp`; Modify: `src/Makefile` (unit list)

**Interfaces:**

```cpp
// include/madcdis/web_model.h  (namespace madc::hub) — dependency-free:
// uinode.h + keys.h + ui_focus.h + <string> <vector> <map>. No JSON library:
// the ops are written by one small escaper (web_json_escape) — the page
// parses them with JSON.parse.
//
// DOM OPERATIONS. compose() returns the full keyed tree every cycle (the
// compose+diff cadence the TUI runs; Track 7.3 refines it) as ONE JSON
// array the page applies with `madcApply(ops)`:
//   {"op":"root"}                                       reset marker
//   {"op":"node","key":"0.2","tag":"div","class":"heading",
//    "parent":"0","text":"..."}                          create-or-update
//   {"op":"node","key":"0.3","class":"edit","lines":[
//      {"t":"line text","s":[[start,len,"kw"],...]} ...],
//    "caret":{"line":3,"col":7},"sel":[[l,c],[l,c]]|null}
//   {"op":"node","key":"0.4","class":"choice","opts":["Save","Quit"],
//    "sel":1,"focus":true}
//   {"op":"end"}                                        prune unvisited keys
// Keys are node PATHS in the composed tree (child indices joined by '.'),
// so the applier reconciles by key: a node with an unchanged key updates
// in place; keys absent after "end" are removed.
//
// INPUT. The page posts ONE JSON object per event through the fragment's
// bound callback:
//   {"kind":"key","key":"^k"}         a non-printable in tui_key_name spelling
//   {"kind":"text","text":"abc"}      a printable run (the page coalesces
//                                     an `input` event's data)
//   {"kind":"resize","rows":40,"cols":120}  viewport FACTS in text cells
//   {"kind":"snapshot","text":"..."}  the test seam's DOM text (Task 7)
// apply_input() turns it into tui_event objects — the SAME kinds tui_model
// emits — through key_resolver (chords) and focus_state (navigation).
class web_model
{
    key_resolver _keys;
    focus_state _focus;
    size_t _rows, _cols;			// last reported viewport facts
    std::string _snapshot;			// last snapshot text (test seam)
public:
    web_model() : _rows(24), _cols(80) {}
    void set_bindings(const tui_bindings &b) { _keys.set_bindings(b); }
    const std::string &pending_chord() const { return _keys.pending(); }
    size_t rows() const { return _rows; }
    size_t cols() const { return _cols; }
    const std::string &last_snapshot() const { return _snapshot; }
    // The composed tree as DOM ops JSON. Roles: heading/status → a bar div
    // (label + content); content → wrapped paragraphs (the page wraps —
    // no wrap_text here; rows/cols are facts for the APPLICATION's
    // compose, as on the TUI); edit → the line-DOM (text split on '\n',
    // spans from hints["spans"] rows {s,e,c} as [start,len,class], caret /
    // sel_start / sel_end byte offsets → line/col); choice → options
    // (node_text of each child) with the live selection and focus flag.
    std::string compose(const roles &r, const uinode &tree);
    // One posted input object → zero or more semantic events.
    std::vector<tui_event> apply_input(const std::string &json);
};
```

- [ ] **Step 1: failing tests** (`test_web_model.cpp`, doctest, preamble as
      test_keys): (a) compose of a tree with heading + content + edit(text
      "ab\ncd", caret 3, spans [{s:0,e:2,c:"kw"}]) + choice(2 options)
      produces ops containing `"key":"0.0"`, `"class":"heading"`,
      `"class":"edit"`, `"lines":[{"t":"ab","s":[[0,2,"kw"]]},{"t":"cd","s":[]}]`,
      `"caret":{"line":1,"col":0}`, `"opts":[`, and ends with `{"op":"end"}`;
      JSON is well-formed (a 40-line hand parser in the test that counts
      brackets and validates string escapes is enough — or `CHECK` exact
      substrings); (b) `apply_input("{\"kind\":\"text\",\"text\":\"hi\"}")`
      → one text event "hi"; (c) with bindings `^k s`→save: key "^k" → focus
      event, key "s" → action save seq "^k s"; (d) after a compose with a
      choice, key "right" → focus, key "enter" → choose option 1; (e)
      `{"kind":"resize","rows":50,"cols":132}` → resize event, rows()==50;
      (f) a malformed object → no events, no throw.
- [ ] **Step 2: see them fail; Step 3: implement** (`web_json_escape`, the
      ops writer per role using `prose::text_of(n.content)` exactly as
      `tui_model::compose` 908–1030 reads nodes — copy the READS, not the
      grid layout; input parsing with a minimal tolerant scanner for the
      four fixed keys — no general JSON parser); **Step 4: unittest green**.
- [ ] **Step 5: commit** — `web_model: the value tree as keyed DOM
      operations, page input as the TUI's semantic events — the engine half
      of the web target, dependency-free`. Trailers: Hypothesis "a DOM
      frontend needs only the tree reads and the shared key/focus owners";
      Layer `ns_ui render/event -> web_model (new, beside tui_model) ->
      keys.h / ui_focus.h (shared) — nothing in tui_model changes`; Searched
      `grep -rn 'dom\|json' include/madcdis` (concept: an existing tree→JSON
      or DOM emitter) → none (render_text/render_tree are text renderers);
      Oracle `n/a — new engine, doctest is the oracle (the TUI's event
      cases pin the shared halves)`.

### Task 4: the target-generic session in `ns_ui.cpp` — `ui::open(target)`

**Files:**
- Modify: `src/ns_ui.cpp` (`ui_tui` → `ui_frontend` + `ui_grid_frontend`;
  new publics; `tui_*` become wrappers), `include/madc/ns_ui` (declarations)
- Create: `tests/testuiopenterm.mad` (+`.expect`, `.input`): opens
  `ui::open("term")` — with no tty the term target refuses; the test checks
  the refusal path AND that `ui::open("nosuch")` returns 0 with
  `ui::open: unknown target 'nosuch'` on stderr (`.expect_err`? no — the
  program prints the return values; stderr content is not asserted).

**Interfaces (C++, `src/ns_ui.cpp`):**

```cpp
// The target-generic session: one per ui::open. The grid frontend is
// today's ui_tui moved verbatim; the DOM frontend arrives in Task 6.
struct ui_frontend
{
    virtual ~ui_frontend() {}
    virtual bool open(size_t &rows, size_t &cols) = 0;	// false + stderr reason
    virtual void close() = 0;
    virtual void render(ui_session *s, madc::value &tree) = 0;
    virtual bool next_event(madc::hub::tui_event &e) = 0;	// blocks; false = input ended
    virtual void size(size_t &rows, size_t &cols) = 0;
    virtual void set_bindings(const madc::hub::tui_bindings &b) = 0;
    virtual std::string pending_chord() const = 0;
    virtual bool suspend() { return false; }
    virtual bool resume() { return false; }
    virtual void refresh() {}
};
struct ui_grid_frontend : ui_frontend { /* target, model, painted, queue, next_event, rows, cols — the ui_tui fields */ };
handle_table<ui_frontend> &ui_frontends();	// replaces ui_tuis()
// The event → value object shaping (the switch at tui_event 1163–1214)
// becomes ONE function both surfaces call:
madc::value ui_event_value(const madc::hub::tui_event &e, ui_session *s);
```

**Interfaces (script, `include/madc/ns_ui`, added after `tui_pending`):**

```c
    // ---- the target-generic surface (Level 3 arrives beside Level 1) ----
    // open(target): "term" (the grid frontend behind the tui_* names) or a
    // registered script-hosted target ("web", <ns_ui_web>). 0 + stderr
    // reason when the target is unknown, cannot serve, or is already open.
    // The tui_* functions are open("term")'s spellings — same handles.
    int64_t open(const char *target);
    void    close(int64_t t);
    int64_t rows(int64_t t);
    int64_t cols(int64_t t);
    void    render(int64_t t, int64_t w, value &tree);
    bool    bind_keys(int64_t t, value &table);
    bool    validate_keys(value &table);
    bool    event(value &out, int64_t t, int64_t w);
    bool    suspend(int64_t t);		// terminal-only capability: web answers false
    bool    resume(int64_t t);
    void    refresh(int64_t t);
    void    pending(value &out, int64_t t);
```

- [ ] **Step 1:** write `tests/testuiopenterm.mad`:

```c
int main()
{
    long bad = ui::open("nosuch");
    println("unknown target -> {}", bad);
    long t = ui::open("term");		// no tty under the runner: 0
    println("term without a tty -> {}", t);
    var table = { "^k s": "save" };
    println("validate -> {}", ui::validate_keys(table));
    return 0;
}
```

      `.expect`: `unknown target -> 0`, `term without a tty -> 0`,
      `validate -> true` (check how booleans print through `println` in an
      existing test — e.g. grep `println.*true` in tests/*.expect — and
      match). `.input` empty file (stdin not a tty in the runner anyway).
- [ ] **Step 2:** run it → fails (`ui::open` undeclared).
- [ ] **Step 3:** implement: move `ui_tui` into `ui_grid_frontend` (the
      bodies of `tui_open/close/rows/cols/render/event/suspend/resume/
      refresh/pending/bind_keys` become the frontend's methods + the
      generic publics); `tui_open()` = `return open("term");` etc. `open()`:
      `"term"` → `register_builtin_tui_targets(); create_tui_target("term")`
      wrapped in a grid frontend; any other name → the script-host registry
      of Task 5 (empty for now → "unknown target"). Keep every existing
      message text.
- [ ] **Step 4:** targeted tests: `TESTS='testuiopenterm testtui* testmadcide*
      testvised* testlined* testeditcheck testbindgate testreenter'
      tests-all` green; the doc-embedded `.expect` for testmadcide is
      byte-identical (the event value shape moved, not changed).
- [ ] **Step 5: commit** — `ns_ui: the target-generic surface — ui::open(target)
      / render / event / bind_keys / …; the tui_* names are the term
      target's spellings; one event-value owner`. Trailers (Oracle `n/a —
      behaviour-preserving for term; the new names are covered by
      testuiopenterm`).

### Task 5: the script-hosted target seam — `ui::register_host`

**Files:**
- Modify: `src/ns_ui.cpp`, `include/madc/ns_ui`
- Create: `tests/testuihostfake.mad` (+`.expect`): a FAKE host written in
  madc (no webview) registers as target `"fake"`, records the ops JSON
  `render` pushes and feeds canned events — proves the seam without a
  display, in every lane.

**Interfaces (script side, `include/madc/ns_ui`):**

```c
    // ---- script-hosted targets (the web target is one; a test fake is
    // another). A host is a table of C function pointers a madc fragment
    // fills and registers ONCE (dynamic init of a global is fine). The
    // engine calls them on the opening thread only.
    typedef void *(*ui_host_open_fn)(const char *title, const char *page, void *ctx);
    typedef void  (*ui_host_close_fn)(void *host);
    typedef long  (*ui_host_eval_fn)(void *host, const char *js);	// 0 = ok
    typedef long  (*ui_host_run_fn)(void *host);	// run the host's loop until ONE
						// event was posted (ui::post_event),
						// then return; nonzero = the host ended
    struct ui_host_ops {
	ui_host_open_fn  open;
	ui_host_close_fn close;
	ui_host_eval_fn  eval;
	ui_host_run_fn   run;
    };
    bool register_host(const char *target, const ui_host_ops *ops);
    // The host's ONE inbound door: the page's event object (JSON text)
    // for the session `ctx` that open() received. Callable from inside
    // run(); the engine queues it and run() returns.
    void post_event(void *ctx, const char *json);
```

**C++ side:** a `std::map<std::string, const ui_host_ops *> &ui_hosts()`
registry; `ui_dom_frontend : ui_frontend { web_model model; const
ui_host_ops *ops; void *host; std::deque<std::string> inbound; std::vector
<tui_event> queue; size_t next; }` — `open()` builds the page HTML (Task 6;
in this task an empty shell string), calls `ops->open(title, page, this)`;
`render()` = `ops->eval(host, "madcApply(" + model.compose(...) + ")")`;
`next_event()`: while `queue` drained: while `inbound` empty → `if
(ops->run(host)) return false;`; pop one inbound JSON →
`queue = model.apply_input(json)`; `post_event(ctx, json)` → the frontend's
`inbound.push_back`. `size()` = model.rows/cols. `open("web")` etc. resolve
through `ui_hosts()`. `register_host` refuses a duplicate name (false +
stderr).

- [ ] **Step 1:** the fake host test:

```c
// A script-hosted ui target with no display: records what the engine
// pushes and answers canned page events. Proves ui::register_host /
// post_event / open / render / event in every lane.
static var pushed;			// the ops JSON the engine evaluated
static int step = 0;
static void *fake_ctx;

void *fake_open(const char *title, const char *page, void *ctx) { fake_ctx = ctx; println("fake open: {}", title); return ctx; }
void fake_close(void *host) { println("fake close"); }
long fake_eval(void *host, const char *js) { pushed = js; return 0; }
long fake_run(void *host)
{
    step++;
    if ( step == 1 ) { ui::post_event(fake_ctx, "{\"kind\":\"text\",\"text\":\"hi\"}"); return 0; }
    if ( step == 2 ) { ui::post_event(fake_ctx, "{\"kind\":\"key\",\"key\":\"^k\"}"); return 0; }
    if ( step == 3 ) { ui::post_event(fake_ctx, "{\"kind\":\"key\",\"key\":\"s\"}"); return 0; }
    return 1;					// the host ended
}
static ui::ui_host_ops fake_ops = { fake_open, fake_close, fake_eval, fake_run };
static bool fake_registered = ui::register_host("fake", &fake_ops);

int main()
{
    long w = ui::world_new();
    long t = ui::open("fake");
    println("open -> {}", t > 0);
    var table = { "^k s": "save" };
    ui::bind_keys(t, table);
    var tree = { "role": "heading", "label": "Title", "content": "hello" };	// use the render_tree schema an existing tui test uses — copy from tests/testtuievents.mad or docs/language/ns-ui.md §Projection-as-data
    ui::render(t, w, tree);
    println("pushed heading: {}", php::str_contains(pushed, "\"class\":\"heading\""));
    var ev;
    while ( ui::event(ev, t, w) )
	println("event {} {}", ev["event"], ev["event"] == "text" ? ev["text"] : (ev["event"] == "action" ? ev["action"] : ""));
    ui::close(t);
    ui::world_close(w);
    return 0;
}
```

      `.expect`: `fake open:`, `open -> true`, `pushed heading: true`,
      `event text hi`, `event focus`, `event action save`, `fake close`.
      Verify the tree literal against the render_tree schema in
      `docs/language/ns-ui.md` §Projection-as-data before relying on it;
      value-first spelling (`var`, object literal) — if an object literal
      with string keys does not parse in the dialect, build it with
      `ui::set`-style calls the existing tui tests use.
- [ ] **Step 2:** fails (`register_host` undeclared). **Step 3:** implement
      per the interfaces. **Step 4:** `tests-all` for `testuihostfake
      testuiopenterm` + the Task-4 set — green in JIT/exe/obj (the fake host
      is why the seam is gated in every lane).
- [ ] **Step 5: commit** — `ns_ui: script-hosted targets — ui::register_host
      / post_event, the DOM frontend over web_model; a display-free fake host
      gates the seam in every lane`. Trailers (Oracle: the fake's printed
      transcript; Searched: `grep -n 'script_executor\|register_tui_target'`
      (concept: how a script provides a provider) → the verb executor is
      by-name source execution, the target registry is C++ factories —
      neither fits a per-event callback, hence the typed table).

### Task 6: the page and the web fragment — `<ns_ui_web>`

**Files:**
- Create: `include/madc/ui_web/page.js`, `include/madc/ui_web/page.css`
  (embedded automatically by `gen_embedded_headers.sh`; keys
  `ui_web/page.js`, `ui_web/page.css`)
- Create: `include/madc/ns_ui_web` (the fragment; namespace `ui_web`)
- Modify: `src/lexer.cpp:1239` map: add `{"ui_web", "ns_ui_web"}`
- Modify: `src/ns_ui.cpp`: `ui_dom_frontend::open` builds the page from
  `find_embedded_header("ui_web/page.js")` / `page.css`
- Modify: `scripts/check-dialect-lean.sh` ONLY if it rejects the new
  fragment for a legitimate reason (it must not: the fragment includes
  nothing and names no std type)

**The fragment (`include/madc/ns_ui_web`, dialect, value-first):**

```c
// madc embedded <ns_ui_web> — the WEB target host (design §3.2): the
// platform webview through the typed madcwebview interface. Pulled in by
// any mention of ui_web:: (auto-include); registers itself as ui target
// "web" at startup. The engine (ns_ui.cpp / web_model) never names a
// platform library — this fragment is the one place that does, through
// the module map. THREAD CONTRACT: confined to the opening thread; the
// page's bound callback runs inside webview_run on that same thread.
import madcwebview;

namespace ui_web {
    struct host {
	webview_t view;
	void *ctx;			// the engine session (ui::post_event's key)
	int   posted;			// events posted during this run()
    };

    // The ONE bound callback: the page calls window.madc(json) → here.
    void on_event(const char *id, const char *req, void *arg)
    {
	host *h = (host *)arg;
	// req is a JSON ARRAY of the call's arguments: ["{...}"] — unwrap
	// the one string argument (the page always sends exactly one).
	ui::post_event(h->ctx, ui_web::unwrap_arg(req));
	h->posted++;
	webview_return(h->view, id, 0, "null");
	webview_terminate(h->view);		// run() returns; the engine drains
    }

    void *open(const char *title, const char *page, void *ctx)
    {
	host *h = (host *)malloc(sizeof(host));
	h->view = webview_create(0, 0);
	if ( !h->view ) { println(stderr, "ui_web: webview_create failed (no display?)"); free(h); return 0; }
	h->ctx = ctx; h->posted = 0;
	webview_set_title(h->view, title);
	webview_set_size(h->view, 960, 640, WEBVIEW_HINT_NONE);
	webview_bind(h->view, "madc", on_event, h);
	webview_set_html(h->view, page);
	return h;
    }
    void close(void *hv) { host *h = (host *)hv; webview_unbind(h->view, "madc"); webview_destroy(h->view); free(h); }
    long eval(void *hv, const char *js) { return webview_eval(((host *)hv)->view, js); }
    long run(void *hv)
    {
	host *h = (host *)hv;
	h->posted = 0;
	if ( webview_run(h->view) != WEBVIEW_ERROR_OK ) return 1;
	return h->posted ? 0 : 1;		// the loop ended without an event = the window closed
    }
    const char *target() { return "web"; }	// the name a program hands ui::open
}
static ui::ui_host_ops __ui_web_ops = { ui_web::open, ui_web::close, ui_web::eval, ui_web::run };
static bool __ui_web_registered = ui::register_host("web", &__ui_web_ops);
```

  `ui_web::unwrap_arg(req)`: strips the outer `[` `]` and the JSON string
  quoting of the single argument (a 20-line dialect helper using `php::`
  string functions or `madc::` intrinsics — value-first). `println(stderr,
  ...)` is the wall-output form. The `webview_bind` callback signature is
  `void (*)(const char *id, const char *req, void *arg)` (the typed header).
  Verify: does the dialect's `namespace X { void f() {...} }` with a static
  global initialized by a call (`__madc_global_init`, cir_builder 9040) work
  in the exe/obj lanes? — `tests/testuihostfake.mad` (Task 5) already proves
  that exact shape before this task starts.

**The page (`ui_web/page.js`):** `madcApply(ops)` reconciles a `Map` of key
→ element under `#root`: `root` clears a `visited` set; `node` creates or
updates (`div.<class>`, text, `data-key`); `edit` renders `div.line` per
line with `span.c-<class>` for spans, `span.caret` at the caret line/col,
`.sel` on selected ranges; `choice` renders `span.opt` with `.sel` and
`.focus`; `end` removes elements whose key was not visited. Input: a hidden
`<input id="kb">` kept focused; `keydown` maps to the TUI vocabulary
(`Enter`→`enter`, `Tab`→`tab`, `Backspace`→`backspace`, `Escape`→`esc`,
arrows→`up/down/left/right`, `Home/End/PageUp/PageDown/Delete/Insert`,
ctrl+letter→`^<lower>`, ctrl+`\ ] ^ _`→ the four punctuation controls as
`tui_key_name` spells them — read `keys.h` for the exact strings) and posts
`{"kind":"key","key":...}`; the `input` event's `data` posts
`{"kind":"text","text":...}` (the page NEVER decides what a key means).
`ResizeObserver` on `#root` measures a `span.measure` (one `M`) and posts
`{"kind":"resize","rows","cols"}` once on load and on change. A global
`madcSnapshot()` posts `{"kind":"snapshot","text": root.innerText}` (the
test seam, Task 7). ~150 lines. `page.css`: monospace, dark default,
`.heading/.status` bars, `.edit .line` pre-wrap, `.caret` inverse block,
`.opt.sel` inverse, `.c-kw/.c-str/.c-cmt/...` a small default palette —
theme `@gui` sections are slice 3.

- [ ] **Step 1:** write `page.js`, `page.css`, `ns_ui_web`; add the lexer map
      row; the frontend assembles
      `<!doctype html><html><head><meta charset="utf-8"><style>CSS</style></head>
      <body><div id="root"></div><input id="kb" autofocus><script>JS</script></body></html>`.
- [ ] **Step 2:** build (`sync build`): the embedded map gains the two files;
      `check-dialect-lean.sh` and `check-ns-header-widths.sh` green (run
      `fulltest`'s gate scripts alone on the container: `cd /workspace/madc;
      bash scripts/check-dialect-lean.sh`).
- [ ] **Step 3:** the first window: `tests/gui/ui_web_hello.mad`:

```c
// The web target end to end under Xvfb: open, render a tree, take a DOM
// snapshot through the page's test seam, close. No editor, no keys.
int main()
{
    long w = ui::world_new();
    long t = ui::open(ui_web::target());
    if ( !t ) return 1;
    var tree = { "role": "heading", "label": "madc", "content": "hello from the web target" };
    ui::render(t, w, tree);
    ui::eval_page(t, "madcSnapshot()");		// Task 7 adds eval_page; for now use the snapshot the first resize/…
    var ev;
    while ( ui::event(ev, t, w) )
    {
	if ( ev["event"] == "snapshot" ) { println("GUI_SNAPSHOT {}", ev["text"]); break; }
    }
    ui::close(t);
    ui::world_close(w);
    return 0;
}
```

      → so Task 7's `eval_page` and the `snapshot` event must land in THIS
      commit too (fold Task 7 here, keeping its own tests): `ui::eval_page
      (t, js)` = `ops->eval(host, js)` on a DOM frontend (false on a grid
      frontend); `web_model::apply_input` turns `{"kind":"snapshot"}` into
      a `tui_event` of a NEW kind `snapshot` carrying `text` (add the enum
      value and `ui_event_value` arm: `{event:"snapshot", text}`).
      `.expect`: `GUI_SNAPSHOT madc hello from the web target` (innerText
      joins with newline/space — set the expectation from the FIRST real
      run's output after eyeballing it, then pin it). `.timeout` 20.
- [ ] **Step 4:** run the GUI stage: `remote_build.sh gui` — Astra's two
      tests + `ui_web_hello` 3/3 JIT + 3/3 EXE.
- [ ] **Step 5: commit** — `ui_web: the web target — <ns_ui_web> hosts the
      platform webview over the typed madcwebview interface; one embedded
      page (applier + CSS); ui::eval_page test seam; the first window under
      Xvfb`. Trailers (Layer: "the fragment is the ONE place a platform
      library is named — through the module map; the engine C++ reads the
      page text and speaks JSON only"; Oracle: `xvfb-run` snapshot equals the
      rendered heading text; a display-free run refuses with the
      webview_create reason).

### Task 7: keys and editing in the window — the vised gate

**Files:**
- Modify: `tools/texteditor/vised_core.inc:135–149` (`ui::open(target)`
  with `target` from an optional argv flag `--web`; `ui::render/event/close`
  generic names), `tools/texteditor/vised.mad` (usage line)
- Create: `tests/gui/ui_web_edit.mad` (+`.expect`, `.timeout`): a compact
  editor loop (NOT vised — vised's verb bodies load by relative path) over a
  tree with an `edit` node and a `choice` menu; drives itself through
  `ui::eval_page` synthetic key dispatch:

```c
// Synthetic keys through the page's real key path: the page's keydown
// handler is what runs, so key spelling stays the page's job and
// interpretation the engine's.
ui::eval_page(t, "document.getElementById('kb').dispatchEvent(new KeyboardEvent('keydown',{key:'k',ctrlKey:true}))");
ui::eval_page(t, "document.getElementById('kb').dispatchEvent(new KeyboardEvent('keydown',{key:'s'}))");
// expect: event action save seq "^k s"
```

  then a text insertion (`InputEvent` with `data:"hi"` on `#kb` →
  `{"event":"text","text":"hi"}`), a `tab` + `right` + `enter` → `choose`
  option 2, a snapshot showing the edit line-DOM carries the inserted text
  and the menu's selected option.
- [ ] **Step 1:** write the test with `.expect` lines
      `EDIT action save ^k s`, `EDIT text hi`, `EDIT choose 2`,
      `EDIT snapshot ...` (pin after the first run). **Step 2:** `gui`
      stage — fix what fails in `page.js`'s key mapping or `web_model`'s
      edit ops until green (each fix its own trailer'd commit if it touches
      src/include; page.js/ns_ui_web changes ride along).
- [ ] **Step 3:** vised: `run_visual(path, ro, target)`; `main` reads
      `--web` → `ui_web::target()` else `"term"`; the terminal path is
      byte-identical (`testvised*` green). Manual gate for the owner (not a
      suite step): `xvfb-run -a bin/madc tools/texteditor/vised.mad
      tmp/x.txt --web` on the container shows the file; the same command on
      madc-mac-x86 shows a window. Record the run in the commit message.
- [ ] **Step 4: commit** — `vised: edits in a window — the web target is
      one flag; tests/gui/ui_web_edit gates keys, text, choice navigation
      and the line-DOM under Xvfb`.

### Task 8: resource guards default off (`Decision resource_guards_default_off`)

**Files:**
- Modify: `src/madc.cpp` (`install_resource_guards` 140–200; `--help`
  515–530; `env_rlim` gains the `off` / `auto` spellings), `src/madc_config.cpp:41`
  + `include/madc_config.h:48` (`mem-limit` / `cpu-limit` accept `off|auto|N`),
  `include/madc_modules.h` + `src/madc_modules.cpp` (row flag
  `MADC_MODULE_GUI`), `src/lexer.cpp bind_module_namespace` (records a
  bound GUI module on the Program: `bool bound_gui_module`), `src/madc.cpp`
  after parse, before run (lift the soft guard when set)
- Modify: `scripts/run_tests.sh` (export `MADC_MEM_LIMIT=auto` for the JIT,
  exe and obj invocations — the ONE place: set it once near line 160 with
  `export MADC_MEM_LIMIT="${MADC_MEM_LIMIT:-auto}"`), `scripts/remote_build.sh`
  gui stage (drop `MADC_GUI_MEM_LIMIT` / `MADC_MEM_LIMIT=$gui_mem`: the lift
  handles it; keep the CPU/wall caps), `docs/building-webview.md` (the
  memory policy paragraph), `docs/language/*.md` wherever `MADC_MEM_LIMIT`
  is documented (`grep -rn MADC_MEM_LIMIT docs README.md`)
- Create: `tests/unit/test_modules.cpp` case "module rows: the GUI flag"
  (madcwebview has it; c/m do not)

**Behaviour:**
- Values: `off` (default for both guards) | `auto` (memory: 4096 + 128/TU;
  CPU: `auto` == off — there is no safe finite CPU default, keep the
  documented reasoning) | `<number>`. Precedence unchanged: flag > env >
  madc.ini > default. `0` stays a synonym for `off` (compat).
- An armed memory guard sets `rlim_cur` only (`rlim_max` untouched:
  `getrlimit` first, keep `rlim_max`). The trip message unchanged.
- After parse, before execution (single TU: where `cc_link_args` is
  extended at `madc.cpp:1430`; project: after all TUs parse), if the guard
  is armed and `prog->bound_gui_module` → `rlim_cur = RLIM_INFINITY`
  (soft), `madc_mem_guard_mb = 0`, and a DBG line names the module row.
- `MadcModuleSpec` gains `unsigned flags;` with `MADC_MODULE_GUI = 1u << 0`;
  the `madcwebview` row carries it; `madc_module_find` unchanged.

- [ ] **Step 1:** failing unit case (GUI flag) + a reducer
      `tests/testguardsoff.mad` (+`.flags`? no) that allocates and touches
      6 GB of address space with `malloc` + one byte per 1 GB page group
      (`.expect`: `guard off: ok`) — this FAILS today under the 4096 default
      and passes when the default is off; the runner's `auto` export would
      re-arm it, so the test carries `tests/testguardsoff.flags`? No flag
      exists — instead the runner honours a per-test fixture
      `tests/<base>.env` (whitespace-split `NAME=value` pairs prepended with
      `env`)? That is a new generic runner convention (test-fixtures.md) —
      add it: `tests/testguardsoff.env` = `MADC_MEM_LIMIT=off`. Document
      the fixture in `.claude/rules/test-fixtures.md` + `docs/rules/`.
- [ ] **Step 2:** implement; **Step 3:** `unittest` + `TESTS='testguardsoff
      testimport* testnoautoload' tests-all` + `gui` (the GUI tests now run
      with the runner's `auto` and the lift — `MADC_GUI_MEM_LIMIT` gone).
- [ ] **Step 4: commit** — `guards: madc arms no resource guard by default —
      off|auto|N via config/env; the runner asks for auto; an armed memory
      guard is a soft limit a GUI-flagged module lifts at run start (owner
      ruling 2026-09-07, recon in the KG Decision)`. Trailers (Oracle: gcc/
      clang arm no guards; PHP CLI has no time limit; the testguardsoff
      reducer before/after).

### Task 9: the object carries its module list (`Gap obj_lane_module_dependency`)

**Files:**
- Modify: `src/cir_builder.cpp` (unit-level emission of the table),
  `src/cir_builder.h`, `src/madc_cir.cpp` (`madc_cir_run_object` +
  `madc_cir_run_objects`: read the symbol, open each spelling), `docs/language/import.md`
  (the `-c` paragraph), `tests/testimport.mad` already has the alias form —
  add `tests/testimportobjdeps.mad` (+`.expect`): `import m;` + `sqrt` — the
  obj lane must run it WITHOUT `-lm` on the command line (libm is in the
  default scope on Linux, so use a library that is NOT: `import madcwebview;`
  is display-bound… use `import crypt;`? needs a row — instead build the
  test on `-l`-free `import c as libc; libc::abs(...)` PLUS a check that the
  emitted C carries the table: `tests/testimportobjdeps.mad` = `import m;`
  + `println("{}", sqrt(16.0))` with `.expect` `4`, and the obj lane
  additionally asserts via `--emit=c11 | grep __madc_module_deps` in a
  gate script `scripts/check-object-module-deps.sh` (fulltest): emits c11
  for testimportiface.mad and requires the array literal
  `"libm.so.6\0"`).

**Emission:** when `prog->module_link_libs` is non-empty, the CIR builder
appends to the unit (in the extern flush pass, once) —

```c
const char __madc_module_deps[] = "libm.so.6\0libmadcwebview.so\0";   /* NUL-separated, double-NUL terminated */
```

  as an `N_SPEC_DECL` (specs `N_CONST N_CHAR`; declarator `id` + array
  `N_ARR` with no size; initializer an `N_STR` whose length counts every
  byte incl. the terminating NULs — the `str(len)` contract) marked
  `synth_from_origin`. In `--emit=c11` it prints as the C line above. It is
  emitted in object mode AND exe mode (harmless data; the exe also has
  NEEDED).

**Loading:** in `madc_cir_run_object` after `MIR_object_load` and before
`cir_enter_loaded_main`: `const char *deps = (const char *)
MIR_object_loaded_sym(lo, "__madc_module_deps"); for each NUL-separated
spelling: std::string err; if (!madc_module_open(spelling, err)) { fprintf
(stderr, "madc: %s: import: cannot load '%s': %s\n", path, spelling, err.c_str
()); return 1; }`. BUT the resolver runs INSIDE `MIR_object_load` (imports
resolve at load) — so the deps must be opened BEFORE `MIR_object_load`: read
the symbol's bytes from the object first. The in-tree MIR builder API
(`third_party/mir/mir-debug.h:194–270`) has `MIR_object_read(obj, buf, size,
err, errlen)` and `MIR_object_find_symbol(obj, name, &sec, &value, &size)`
(value = the symbol's SECTION OFFSET, size its byte size) but NO accessor
for a section's bytes. Add ONE, beside `MIR_object_find_symbol` in
`third_party/mir/mir-object.c` + the header (madc source; its own trailer'd
commit): `extern int MIR_object_section_bytes (MIR_object_t obj, int sec,
const void **bytes, size_t *len);` (nonzero + the section's current
contents; the .rodata/.data section the table lives in). Loader flow:
`cir_read_objects(paths)` (already merges N objects into one builder) →
`MIR_object_find_symbol(obj, "__madc_module_deps", …)` →
`MIR_object_section_bytes` → walk `value .. value+size` as NUL-separated
spellings → `madc_module_open` each (stderr + return 1 on the first failure,
naming the object and the spelling) → then the existing emit/load. For the
single-object lane route `madc_cir_run_object` through the same reader
(read → open deps → `MIR_object_load` of the ORIGINAL bytes, so nothing
else changes). A merged multi-object image carries the UNION automatically
(each object's table is a distinct local symbol? NO — same name in N
objects collides on merge: emit the symbol LOCAL (`static`) in each object
and read every object's table BEFORE merging, one `MIR_object_read` per
path; the merged image keeps whichever local survives, unused at run
time). Explicit `-l` (`run_flags` in the runner, `link_libs`) stays as an
override and opens first.

- [ ] **Step 1:** failing reducer + gate script (negative control: a c11
      emission with no `import` must NOT carry the symbol). **Step 2:**
      emission; `--emit=c11` shows the array; **Step 3:** the loader; the
      obj lane of `testimportiface` (currently passes only because libm is
      default-scope) and the new test pass; run `tests-all` on
      `testimport* testnoautoload testdlopen testnsexternc`; **Step 4:**
      `tests/gui/webview*.mad` + `ui_web_*.mad` through the OBJ lane: the
      gui stage gains `--obj` (edit `remote_build.sh` gui: `run_tests.sh
      --exe --obj`); **Step 5: commit** — `import: the object carries its
      module list — __madc_module_deps (NUL-separated spellings) emitted by
      the builder, opened by the single-object loader before load; the obj
      lane needs no -l for an imported module`. Trailers (Layer: `madc -c ->
      module_link_libs -> [no consumer in the object capture] -> loader
      resolves imports blind — the list joins the IR as data, the loader
      reads it: both edits at the two ends of the one gap`; Oracle: g++
      `.o` has no NEEDED either — MSVC .drectve / Rust #[link] precedent;
      the reducer before: `unresolved symbol`, after: runs).

### Task 10: lazy (optional) module binding — `MADC_MODULE_LAZY`

*Severable: nothing in Tasks 1–9 needs it; slice 3 (madcide `--gui` on a
machine without libmadcwebview) does. Design §3.2: "a missing library makes
open refuse with the reason".*

**Files:** `include/madc_modules.h` / `src/madc_modules.cpp` (flag
`MADC_MODULE_LAZY = 1u << 1` on `madcwebview`), `src/lexer.cpp
bind_module_namespace` (lazy: do NOT open at parse, do NOT add to
`module_link_libs`; wrap the interface tokens in `#pragma madc module_begin
("<spelling>")` … `#pragma madc module_end` synthetic directives),
`src/parser.cpp` (the pragma handler sets `Program::lazy_module_spelling`;
parseFunction's declaration-only registration stamps `dyn_module_library` =
that spelling, `dyn_module_member` = source_id, `dyn_module_typed = true`),
`src/cir_builder.cpp dyn_module_callee` (when `dyn_module_typed`, cast the
slot to the FuncDef's REAL prototype instead of `long (*)()` — the same
shape the `__madc_dl_member` lowering already produces), `src/madc_mir_backend.cpp`
(`__madc_dl_member` keeps throwing on failure), `include/madc/ns_madc` +
`src/ns_madc.cpp` (`bool madc::module_available(const char *name)` = tries
`madc_module_open` of the row's spelling, no throw — the fragment's `open`
calls it first and refuses with the reason), `include/madc/ns_ui_web`
(refusal), `tests/testimportlazy.mad` (+`.expect`): `import madcwebview;`
compiles and runs `println(madc::module_available("madcwebview"))` on a box
with the library, and `tests/testimportlazy_missing.mad` with a row-less…
no: a test-only lazy row cannot exist (Rule #7) — instead the test sets
`MADC_TEST_HIDE_LIB=…`? No. Gate the missing case in the unit test of
`madc_module_open` on a nonexistent spelling (already `test_modules.cpp`
"open failure") plus the fragment's refusal path exercised by
`tests/gui/ui_web_hello.mad` under `DISPLAY=` unset (webview_create fails →
refusal text) — a `.gui_nodisplay` variant is a lane question for slice 3.

- [ ] Steps: failing tests → pragma + stamping → typed slot lowering →
      `module_available` → the fragment refuses cleanly → `tests-all` on the
      import set + `gui` → commit `import: lazy module rows bind at first
      call with typed prototypes; madc::module_available; the web target
      refuses without the library instead of failing at parse`.

### Task 11: docs, rules, mirrors

- `docs/language/ns-ui.md`: a "Level-3 web target" section (the generic
  `ui::open/…` surface, `ui_web::target()`, the event objects incl.
  `snapshot`, `eval_page` as a test seam, the thread contract) and a note
  that `tui_*` are the term target's spellings.
- `docs/language/import.md`: objects carry `__madc_module_deps`; lazy rows.
- `docs/building-webview.md`: the memory policy paragraph → the ruling.
- `.claude/rules/test-fixtures.md` + `docs/rules/test-fixtures.md`: `.env`.
- `AGENTS.md` rule #4 standing instances: chord/key = `key_resolver`
  (`keys.h`), focus = `focus_state` (`ui_focus.h`), gated by
  `check-one-key-owner.sh`.
- `CHANGELOG.md` Unreleased: "Web target (slice 2 engine half)", "Resource
  guards default off", "Objects carry their module list", "Lazy module rows".
- `docs/plans/ROADMAP.md` 7.5 status; design doc §5 slice 2 → landed.
- KG: Feature `ui_web_target` status; Decision `resource_guards_default_off`
  executed; Gaps `obj_lane_module_dependency` / `gui_memory_guard_policy`
  → fixed; new `DupFamily key_resolver_owner` (gated), `focus_state_owner`
  (gated).
- `claude_status.json` live_handoff; memory `project_ui_web_target.md`.

### Task 12: merge wave

- `/dupaudit` scoped to `src/ns_ui.cpp`, `include/madcdis/{keys,ui_focus,
  web_model,tui_model}.h`, `include/madc/ns_ui*`, `src/madc_cir.cpp` (loader),
  `src/madc.cpp` (guards).
- Battery on the container at the final content: `remote_build.sh sync
  battery` → `gui` (now JIT+EXE+OBJ) → `win release-win wine` +
  `verify_pe_release.sh` → `c_testsuite_lane.sh` → `release-macos`
  (macho gate inside). One chained background script, per-lane logs
  `tmp/logs/*-s16x.log`.
- Record the four develop lanes (`lane_ledger.sh record …`), test-status
  Current block, merge `--no-ff` to develop, push (the pre-push gate).
- The GUI lane on the Mac: `xvfb`-free run of `tests/gui/*.mad` on
  madc-mac-x86 by hand (the darwin `gui` lane is a slice-3 lane question);
  the arm64 Mac is Jane's — never.

---

## Self-review (2026-09-07)

- **Spec coverage:** §3.2 web_model (T3), key owner (T1), fragment + page
  (T6), line-DOM editor (T3/T6/T7), cadence (T3); §3.3 generic surface (T4);
  §3.6 doctest + gui stage + snapshot fixture (T3, T6, T7); §3.7 contract
  (headers' comments); §3.9 no JS key logic / no editor component / no
  per-platform code (T6). Rulings: guards (T8), object module list (T9),
  order (T1→T7). Missing-library refusal (§3.2) = T10.
- **Placeholders:** none intended; two "pin after the first run" `.expect`
  values are deliberate (DOM innerText spacing is the page's fact).
- **Type consistency:** `tui_event`, `tui_keyev`, `tui_bindings`,
  `key_resolver`, `focus_state`, `web_model`, `ui_frontend`, `ui_host_ops`
  spelled identically across tasks; `ui::post_event(void *ctx, const char
  *json)` matches the fragment's call; `ui_host_run_fn` returns `long`
  (nonzero = ended) in T5, T6 and the fake.
- **Rule #7 check:** targets and hosts are registries (data); node roles are
  the `roles` registry; module behaviour is row flags; no name ladders.
