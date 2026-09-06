# Test Status

> **Current (2026-09-06, v0.98.0 — the macOS full-suite release: the darwin
> D4 burndown waves 1–7c + the MIR aarch64 floor wave 8 + the SIMD arc
> S1–S5 + the vector calling convention + Apple stack-argument packing
> + wave 12, the libc++ `<list>` completion site):** FULL merge-wave
> battery on the wave-12 code content 1ae4650f (b49234a6 18953bfd 94de04fe
> d2bf5a3c 733ce20b 1ae4650f on 606907f1; the candidate e7b628e1 adds only the
> genuine-Windows stage fix scripts/win_suite.sh + testiomanip.win_skip, so
> six ledger rows were re-stamped there from the 1ae4650f runs, each row
> saying so; tmp/logs/w12-battery5.log): fulltest rc=0 (every gate in the chain,
> vector_abi_gate OK x3 lanes, forest_bind_gate 29/29, tsubst flag-on
> gate ok, warning ratchet at baseline) with JIT **1308 passed / 0 failed / 0 timed out / 9 skipped**, native
> EXE **1249/0**, OBJ **1249/0**, packed **1308/0/0/9**,
> headerless **1274/0/0/43** (linux pack 68 = baseline); the libc++
> flavor lane — its first full run since 2026-08-16 — JIT **1303/0/0TO/14skip**,
> EXE **1244/0**, OBJ **1244/0** (5 `.libcxx_skip` fixtures,
> testphpdumpiter among them for the nested-map construct relowering gap);
> wine64 domain **1251/0/0TO/66skip** on the freshly rebuilt PE (verify_pe_release
> OK, win64 pack 68 = baseline); the same PE on genuine Windows 11 through
> the W0.2 channel (`scripts/win_suite.sh`, now staging tools/ + examples/)
> **1253/0/0TO/64skip** (tmp/logs/w12-win-suite2.log; the first run 1247/6/1TO was
> six stage gaps, classified standalone); c-testsuite
> **220/220, baseline empty** (`--std=gnu11`); macOS cross release both arches 835 units,
> darwin pack 48 = baseline, verify_macho OK; the darwin FULL suite on
> GitHub's mac runners (dispatch #14, run 33993568362, suite_gate=true):
> arm64 **1293/0/0TO/24skip**, Intel **1294/0/0TO/23skip**; the owner's arm64
> Mac full suite **1293/0/0TO/24skip**. New reducers in wave 12:
> testlistiter_libcxx, testqualbasemember, testvaluecout_libcxx,
> teststrargcoerce_libcxx; the darwin pack gains a consumer PARSE leg
> (scripts/forest_pack_consumers_darwin.txt) on the container. Every lane is recorded in docs/lane-status.tsv
> at this content (`lane_ledger.sh check --release` green).
>
> **Previous (2026-09-01, v0.97.0 — the s148–s149c merge wave: madcide
> gateway seam + vi modal personality + carrier literal ergonomics):**
> FULL push-gate battery on merged content @d8bf72f4 (+docs-only
> follow-ups): fulltest rc=0 (all gates) with JIT **1247 passed / 0
> failed / 0 timed out / 9 skipped**, native EXE **1193/0**, OBJ
> **1193/0**, packed **1247/0/0/9**, headerless **1220/0/0/36**;
> wine64 domain **1197/0/0/59** on the freshly rebuilt PE; macOS
> arm64 hardware battery **8/3** leg-for-leg parity (standing
> known-opens only; degradation leg zero losses); c-testsuite
> **220/220 baseline EMPTY** (`--std=gnu11`). New in this wave:
> testvalueappend/+mix/+bad, testvalueinitkeyed(+mix),
> testcolonheadexpr, testexprjuxtapose; the var-over-value sweep
> converted 57 test files to the `var` spelling. darwin pack baseline
> 58 → 64 with a stated reason (the juxtaposition wall made a
> pre-existing instantiation-body misparse loud; fix banked as KG Gap
> instantiation_body_templateid_ctor_call).
>
> **Previous (2026-08-28, SMAUG restored + gated —
> feature/smaug-fntypedef merge wave):** the real MadSMAUG 51-TU
> `--project` (upstream/smaug1.8 via compile_commands.json) compiles,
> links, JIT-boots to "Realms of Despair ready", and serves the full
> login greeting with ZERO diagnostics — restored from a SIGSEGV
> shipped in every archived release v0.69.0–v0.96.0 (a function
> declared THROUGH a function typedef — SMAUG's `DECLARE_DO_FUN` —
> registered as an 8-byte storage variable whose ->data the July-31
> ctor_hidden_vbase_owner probe read as a Method*). Fixes, each with
> a reducer: parser — a zero-star fn-typedef declarator declares a
> FUNCTION, one star stays a pointer variable (testc89fntypedef);
> CIR — C-mode functions never enter the ctor/vbase probe; c2mir —
> default warnings match gcc's default (sign-only pointee args,
> ptr-vs-null-zero ordered compares; testcwarnparity, .expect_quiet).
> `scripts/smaug_gate.sh` now boots the REAL SMAUG inside fulltest,
> two-sided (~10 s; skips loudly without the sibling checkout). FULL
> push-gate battery on final content @6ea621f2: fulltest rc=0 (all
> gates incl. smaug_gate) + JIT **1201 passed / 0 failed / 0 timed
> out / 9 skipped** + EXE **1152 / 0** + OBJ **1152 / 0** + release
> rc=0 + packed **1201 / 0** + headerless **1174 / 0 / 36 skipped**.
>
> **Previous (2026-08-28, the variadic-class arc —
> feature/variadic-class-tpl merge wave):** `bin/madc
> examples/embed_hello.cpp` compiles AND RUNS at g++ parity; the
> examples gate carries both legs. Twelve arc fixes (ns-map clobber
> [temp.inst], enum-pointee conversion domain, FuncDef::is_explicit,
> const&-param prvalue misclass, tid-pack inst_key memo, engine
> iostream-capture scoping, eval-child host callbacks, ...) plus the
> battery gate's three catches, each own-committed with a reducer:
> the [tsubst-alias] probe takes the drift gate's allowed-exception
> marker; a REBOUND ranked ctor stamps its `__oN` symbol on
> local_emit_name (pre-existing ≥v0.95.2 — the packed/headerless
> memberwise-copy "too many arguments" wall; tests/testvecmembercopy);
> a missing-content HUSK live-tokenizes by CANONICAL PATH, not
> basename (`<stat.h>` grove-order ambiguity dropped `__S_IFMT` for
> masked `<sys/stat.h>` consumers — teststat red headerless since the
> s140 corpus growth). Sixteen new suite tests. FULL push-gate battery
> on final content @e41acd91: fulltest rc=0 (all gates) + JIT **1199
> passed / 0 failed / 0 timed out / 9 skipped** (suite = 1225) + EXE
> **1150 passed / 0 failed** + OBJ **1150 passed / 0 failed** +
> release rc=0 + packed **1199 / 0** + headerless **1172 / 0 / 36
> skipped**.
>
> **Previous (2026-08-27, the embed_hello template chain —
> feature/embed-hello-tsubst2 merge wave):** five coupled template-core
> fixes on the `bin/madc examples/embed_hello.cpp` diagnosis chain, each
> reduced + g++-oracled: (1) `find_initializer_list_ctor` accepts a
> CLASS element type when every element is ALREADY that class —
> `vector<W>{W(1), W(2)}` (the pgm.call({value,value}) shape) takes the
> initializer-list ctor like g++ instead of falling to plain overloading
> (which deduced the range ctor with _InputIterator = W, a candidate
> g++'s _RequireInputIter SFINAEs away, and died in tsubst on
> iterator_traits<W>::iterator_category — the banked bail); the
> conversion-into-slot half (vector<string>{"a","b"}) stays the task-#56
> residue. (2) [dcl.init.list]/3-4 has ONE owner: the filter moved into
> instantiate_member_ctor_template_for_construction (new
> list_initialization param) — the two TokenObjTemp sites were UNGUARDED
> and minted the bogus range-ctor husk at parse time. (3)
> fn_template_deduce_fnptr_param deduces a TID-classified pack inside a
> fn-ptr parameter (`Ret (*cb)(Args...)`, engine::register_function's
> shape), any element count; a multi-element tid pack aliases into the
> pack_param/pack_elems channel and rides THE one N-copy expansion. (4)
> concrete-param viability is two-pass: strict over every candidate,
> then relaxed (named CLASS params stop vetoing — [over.ics.user]) only
> when none matched; arithmetic/pointer arms stay strict in both passes.
> (5) attach_outofclass_member_template_def: an out-of-class
> member-template definition on a PLAIN class attaches its body to the
> declared member's pattern (libmadc/engine.h) — before it, the call
> SILENTLY froze on the placeholder (undefined MIR import). NEW
> env-gated probes per the repo idiom (MADC_SUBST_PROBE deriv/rebuild,
> MADC_FNPTR_PROBE, FNTPL exit tags, the MTB inj dump, MADC_OOL_PROBE).
> NEW tests: testinitlistclass, testmembertplfnptr (both --std=c++17,
> g++-pinned). STILL OPEN toward embed_hello (reducers in container
> tmp/eh_red*.cpp): detail::callback_adapter's variadic CLASS-template
> instantiation (mixed fixed+pack head) is a SILENT-WRONG frontier —
> sizeof... in a class-pack member folds 0 (eh_red6), a qualified static
> call through Adapter<R,A...> evaluates 0 (eh_red7) — the class-side
> twin of the fn-side N-copy machinery; plus the implicit-copy
> deferred-construction pack arm (eh_red1: vector<V> with NO user copy
> ctor bails LOUD at elem-formal-mismatch) and const-ref operator[]
> member typing (eh_red8's first form). Battery on final content:
> fulltest rc=0 (all gates) + JIT **1183 passed / 0 failed / 0 timed
> out / 9 skipped** (suite = 1209) + EXE **1134 passed / 0 failed** +
> OBJ **1134 passed / 0 failed**.
>
> **Previous (2026-08-27, the running madc IS the compiler —
> feature/parse-build-run merge wave):** the OWNER RULING lands: ^B
> never re-parses and never execs a madc binary — the buffer's LIVE
> parse handle IS the compilation. NEW engine pair on the parse-handle
> family: `madc::parse_build(diags, h, "exe"|"obj", out)` emits a
> native artifact from the handle's EXISTING cir-ready tree
> (madc_cir_emit_native — no fresh parse, no child Program from a
> path; a red parse never reaches the emitter; build rows ride the
> out-param only, the handle's recorded diagnostics snapshot/restore so
> parse_check stays parse-pure) and `madc::parse_run(h)` = fork() at
> the post-parse point (the child inherits the tree COW, resets the
> cooperative scheduler — NEW `__madc_task_atfork_child` + io-layer
> hook clearing waiters/host — hands the tree to madc_cir_execute, and
> leaves via exit() so the GUEST's atexit semantics run; parent
> ignores SIGINT/SIGQUIT until the reap, child restores defaults — the
> system(3) discipline; returns the guest's status via
> map_child_status, or -1 bad handle / -2 red parse / -3 no fork).
> The tui atexit recovery is pid-guarded; the pre-existing
> fork-isolation eval children adopt the same scheduler reset (latent:
> a parked parent queue could schedule in the child). madcide: Build
> refreshes the handle from the SCREEN text and emits from it (the
> disk does not compile — the message inverts to "live buffer: unsaved
> edits compile"), Run = run_buffer (red-parse refusal LOUD into the
> diags pane → suspend → parse_run → THE return pause → resume);
> the `Run {madc} {path}` row is DEAD ({madc} substitution survives
> for manifest commands only); the choose dispatcher routes bld- verbs
> by PREFIX. Dupaudit: 4 families consolidated+gated at birth —
> native_kind_of, parse_tree_backend_ready, fork_child_runtime_reset
> (fork-site count == atfork-reset count; NEW
> check-live-build-owners.sh), terminal_return_pause (the pause-owner
> marker joins check-madcide-single-owners.sh) — and the BATTERY's
> child_status_exit_mapping gate caught parse_run's hand-rolled
> 128+WTERMSIG (adopted map_child_status, now shared via
> madcdis/process.h). NEW testparserun (fork-run rc passthrough +
> inherited stdout, build exe+obj twice from ONE handle, artifact
> runs, parse-pure handle, -1/-2 refusals; win64/wine64 skips);
> testmadcide gates flip to the ruling (run-internal, run-headless,
> live-buffer-build/native-build-fail inverse pair, run-refused-red).
> win64 cross build green (g_starting reset under the !_WIN32 guard).
> Battery on final content: fulltest green (52 unit binaries, all
> gates incl. the new one) + JIT **1181 passed / 0 failed / 0 timed
> out / 9 skipped** (suite = 1207) + EXE **1132 passed / 0 failed** +
> OBJ **1132 passed / 0 failed**.
>
> **Previous (2026-08-27, MT-4c stdin unification —
> feature/mt4c-stdin-unification merge wave):** the tui's input wait
> joins the ONE scheduler poll; read_keys' 50ms live-but-parked cadence
> RETIRES. NEW `taskio::host_wait_readable(fd)`: the tui flow (the main
> task) parks on stdin as an io waiter, so task_next_or_wait's hook
> makes one blocking decision over {stdin, io waiters} bounded by the
> earliest timer. true = the fd fired (read keys); false = a SYNTHETIC
> wake — rt counts task switches (`__madc_task_switch_count`), the host
> records the count at park, and the hook's BLOCKING quiescent point
> (never a zero-timeout probe: a yield-head fire_due probing the hook
> must not steal the CPU from still-running tasks — the unit leg's
> failing trace caught exactly that; gate = timeout_ms != 0) wakes the
> host UNFIRED when switches advanced: the read_keys ran→wake seam
> moved into the one poll. Order: zero-timeout probe pass (real
> readiness wins) → synthetic wake (a pending repaint beats blocking) →
> the blocking poll; EINTR on a WAIT wakes the host (SIGWINCH reaches
> the resize check). The runnable-yield and zero-live-blocking arms are
> untouched. Own prior commit: `input_ready` adopts the
> POLLIN|POLLHUP|POLLERR readable-progress triple — DupFamily
> fd_readable_progress_probe goes open→GATED (NEW
> check-fd-readable-progress.sh in fulltest, negative-controlled; the
> smoke gate grows a terminal-death leg, tui_eof_pty.py — SIGHUP
> ignored in the child so the WAIT is what gets tested; NOTE: a dead
> Linux pty polls POLLIN|POLLHUP so the behavioral leg cannot
> discriminate the POLLIN-only bug here — macOS ptys are the plausible
> HUP-only live shape; the mechanism gate is the enforcement). NEW unit
> legs: test_task_io +3 (readable-now; the fd firing beats the
> synthetic wake — probe-pass-first; activity with no fd = synthetic,
> unfired). Validation: test_task_io 7/7, test_coop_parse 50/50
> interleave, pty smoke(+EOF leg)/scroll gates, testmadcide, MT io
> tests — green. Battery on final content: fulltest rc=0 (all gates,
> the two new ones included) + JIT **1180 passed / 0 failed / 0 timed
> out / 9 skipped** (suite = 1206) + EXE **1131 passed / 0 failed** +
> OBJ **1131 passed / 0 failed**.
>
> **Previous (2026-08-27, IDE-9e windows —
> feature/ide9e-windows merge wave):** madcide grows JOE's second axis:
> N STACKED windows over the AST-5 buffer ring, each with its OWN
> status line and caret (^K O split). The engine was ALREADY per-slot
> everywhere that mattered (paint_edit keeps scroll/hshift per edit
> slot; the cursor paints only on the focused slot; multiple edit
> nodes compose) — the ONE gap was height partitioning, closed as DATA
> per the lineage north star: an edit node's `hints["rows"]` fixes its
> height, edit nodes honor the `focus:1` autofocus hint (the choice
> arm's seat), and the unhinted prompt shape stays byte-identical
> (unit negative control). madcide: `windows` bag rows
> {bufidx, caret, grow} + winat (fewer than two rows = the single
> compose, byte-identical); the ACTIVE window's live state IS the es
> state, rows hold snapshots (the buffer-row save/restore shape one
> level up); window rows own the caret while split — two windows on
> ONE buffer keep separate carets, stale ones clamp through THE one
> extracted owner `clamp_caret_to` (dupaudit family caret_clamp_rule:
> 3 sites folded); per-window status = compose_joe_statusline (the
> formatted half split out; prompt/search/msg overlays stay on the
> active line). Verbs: splitw/nextw/prevw/killwin/onlywin/groww/
> shrinkw; sum-zero grow deltas, min 3 rows; the active window
> composes UNHINTED (flexible) so panes keep taking rows from the
> bottom. KEYS — JOE-exact seats reclaimed: ^K O/N/P/G/T + ^K 0/^K 1
> (Esc-prefixed bindings CANNOT exist — the chord machinery hard-codes
> Esc as cancel); displaced: profile/theme leave the key table (the ^T
> Options pane already carries Keymap/Scheme rows), outline → ^K I,
> AST views → ^K A (⚠️ owner review requested beside the pending ^K ;
> item). Esc does NOT close windows (layout, not a pane). NEW tests:
> test_tui_model rows-hint partition case (+negative control);
> testmadcide +6 gates (rebind probe, split tree shape, per-window
> caret round-trip, stale-caret clamp both consumers, grow, onlywin),
> byte-exact first run; pty smoke/scroll gates green (joe.keys blast
> radius). Residues: inactive windows uncoloured/no views/no
> selection; explode; horizontal splits (the GUI twin's seat). Battery
> on final content: fulltest rc=0 (all gates) + JIT **1180 passed /
> 0 failed / 0 timed out / 9 skipped** (suite = 1206) + EXE **1131
> passed / 0 failed** + OBJ **1131 passed / 0 failed**.
>
> **Previous (2026-08-27, MT-5 scope/await keywords —
> feature/mt5-keywords merge wave):** the structure spellings land as
> CONTEXTUAL keywords under `--std=madc` (the MT-1 error-shape rule —
> never reserved; declared names win, strict modes byte-identical).
> **`scope { ... }`**: the structured-concurrency block — `go` inside
> attaches, the block's end joins every member and rethrows the first
> member error (madc::scope_end's contract); lowered to the NEW rt pair
> `__madc_scope_block_enter/exit` riding the MT-3 seams. A throw
> ESCAPING the block quietly abandons the scope mid-unwind (members
> cancelled + JOINED — parking mid-unwind is safe, the in-flight
> exception is per-context state; the error wins) via a cleanup-stack
> registration; NEW `__madc_cleanup_remove` (rt_except.c, the FOURTH
> conscious host-consumer widening — top-pop is wrong when an enclosing
> try's body locals registered above the entry). The try MARK
> discipline gives nesting for free: a throw caught INSIDE the block
> never touches the scope. return/goto in the block, and
> break/continue that would CROSS it (no loop/switch opened inside —
> RAII parse_loop_depth guards in the four loop parsers; records tagged
> with cur_func_name so lambda bodies are never misjudged), are parse
> errors; early-exit support = named residue. **`await ch`**: Go's
> `<-ch` — blocks, closed-and-drained yields the ZERO value; sugar over
> THE one recv via the extern-C machinery seat `__madc_chan_await`.
> Two shapes ship: `v = await ch;` (claimed at STATEMENT level — the
> value carrier's operator= machinery resolves assignment inside the
> ladder first) and bare `await ch;` (statement head — the identifier
> dispatch otherwise swallowed the two-identifier shape SILENTLY);
> decl-init + deeper positions refuse loud (L3 unlocks them); scalar
> targets refuse at parse time. ONE construction owner
> (Program::make_await_token — the draft's 3 inline copies folded by
> the pre-merge dupaudit; NEW gate check-await-one-builder.sh,
> negative-controlled; shared eligibility test =
> contextual_name_unclaimed). `select` keyword DEFERRED by ruling
> (Go's case grammar doesn't transplant; chan_select + await cover
> fan-in). NEW tests: testscopekw (six deterministic-schedule legs +
> expect_quiet), testawait (assign/bare/closed-zero/parked recv),
> testscopereturn + testawaitexpr + testawaittarget (.expect_err
> refusal reducers); testgoident/testgogate grow scope/await arms.
> Battery on final content: fulltest rc=0 (all gates) + JIT **1180
> passed / 0 failed / 0 timed out / 9 skipped** (suite = 1206) + EXE
> **1131 passed / 0 failed** + OBJ **1131 passed / 0 failed**.
>
> **Previous (2026-08-27, MT-3 structured scopes + cancellation —
> feature/mt3-scopes-cancel merge wave):** tasks get Kotlin-shaped
> structure over Go's spelling. NEW publics: `madc::scope_begin()` /
> `scope_end(h)` / `scope_cancel(h)` / `cancelled()`. `go` attaches the
> child to the spawner's innermost OPEN scope (flat attachment; born
> cancelled into a cancelled scope); `scope_end` is owner-only +
> innermost-first (validated via `__madc_scope_end_check` BEFORE the
> handle is consumed), JOINS all members unconditionally, then rethrows
> the first member error (else the cancelled literal, else returns).
> `scope_cancel` = transitive flag+wake (members + child scopes; the
> opener woken FLAGLESSLY — its cancellation lives in the chain and
> dies at scope_end). Cancellation is cooperative and lands at the
> blocking verbs (chan send/recv/select, sleep_ms, taskio readable)
> through THE one owner `__madc_task_throw_if_cancelled` with eager
> waiter removal; a delivered value always wins over a pending cancel;
> `yield()` is NOT a cancellation point. Scoped tasks run under an SJLJ
> catch-all trampoline: an uncaught error cancels the scope and
> rethrows at scope_end; root tasks keep Go's abort-on-uncaught.
> Consumers (MT-3b): tokenize/parse abort cancelled work (lexer pump
> C++ throw → recorded diagnostic; parse loop set_error after
> parse_yield_point), madcide's internal builds get Stop back (the
> build task's own scope = the job handle; "buildstopreq" covers the
> spawn window; a stopped build reports "Build stopped", diags
> withheld), `Process::wait_or_kill(grace_ms)` = grace-then-SIGKILL
> (the win arm reports terminate()'s 128+SIGTERM shape — Windows has
> no SIGKILL; the battery's win gate caught the first spelling),
> ExecDataChannel close() escalates when cancelled. rt_except: tags
> moved to rt_except.h, `__madc_exception_text` = THE renderer (four
> Unhandled printers folded), try-frame API exposed as the third
> conscious host-consumer widening. Pre-merge dupaudit: families
> current_task_cancel_throw + child_status_exit_mapping +
> scope_join_unlink consolidated; NEW gates check-cancel-throw-owner.sh
> + check-child-status-map-owner.sh (negative-controlled). NEW tests:
> testgoscope (deterministic 30-token schedule), testbuildcancel
> (mid-parse cancel), testchancancel (SIGTERM-ignoring child, SIGKILL
> escalation by wall clock); testmadcide native-build-stop gate.
> Battery on final content: fulltest green (the win gate re-validated
> after the one-line win-arm fix) + JIT **1175 passed / 0 failed /
> 0 timed out / 9 skipped** (suite = 1201) + EXE **1129 passed /
> 0 failed** + OBJ **1129 passed / 0 failed**. Named residues: the
> emit phase has no yield points (cancel lands at parse);
> declaration/1024-token grain; member-error rethrow is text-only; no
> NonCancellable regions; main's own unended scopes leak at exit.
>
> **Previous (2026-08-27, IDE-10c internal builds + esc-any-pane —
> feature/ide10c-internal-builds merge wave):** the ^B rows go INTERNAL
> (owner ruling: the IDE lives inside the compiler — never shell out to
> a PATH madc). NEW engine publics: `madc::build_native(diags, path,
> "exe"|"obj", outpath)` = the CLI's AOT lane in-process (child-Program
> parse + `madc_cir_emit_native`; diagnostics rows either way, a silent
> failure synthesizes one error row) and `madc::compiler_path()` (Run
> rows spawn a child OF SELF via {madc} in build_subst). Default rows:
> Check / Build / Build object internal, Run `{madc} {path}`, Run
> native `./{base}`; no default Stop (MT-3 owns in-process cancel; the
> exec:// pump stays for manifest commands). `madc_object_mode` now has
> ONE scoped entry (`ObjectModeScope`, both emit lanes — pre-merge
> dupaudit family object_mode_emit_scoping; NEW gate
> check-object-mode-scope.sh, negative-controlled, whose first run
> caught the marker matching a comment). Esc backs out of ANY pane.
> Ring-lifetime trap hit again: const char* params held across
> build_subst's ring calls rotted — var& params per the banked
> prescription. NEW test `tests/testbuildnative.mad` (+2 helper TUs):
> exe/obj clean builds, the artifact RUNS, positioned error rows,
> unknown-kind/unreadable-path rows, post-emit eval proves the
> object-mode restore; testmadcide +6 gates (default-row shape, esc,
> {madc} subst, in-process build clean + failure rows in the diags
> pane). Battery on final content: fulltest rc=0 (all gates) + JIT
> **1172 passed / 0 failed / 0 timed out / 9 skipped** (suite = 1198)
> + EXE **1126 passed / 0 failed** + OBJ **1126 passed / 0 failed**.
>
> **Previous (2026-08-27, IDE-10a+10b palettes + build controls —
> feature/ide10a-palette merge wave):** madcide becomes an IDE: ^P file
> palette + ^B build palette on ONE popup-list widget (the model's
> choice focusable gains LIST + AUTOFOCUS presentations, data-driven
> hints), joe.keys reclaims the WordStar diamond (^P/^F/^N/^B) + ^R
> retype (`ui::tui_refresh` — the delta-paint baseline drops so the
> next render rewrites the viewport). Builds stream into the [build]
> buffer through an exec:// pump whose loop is the MT-4b mixed select
> ({stop chan, byte readiness}; NEW `channel::cancel()` SIGTERMs so
> Stop and quit are prompt — a stopped `sleep 5` build returns in
> milliseconds); Run rows get the REAL terminal (suspend/resume).
> Engine: `__madc_task_fire_due` (yield fires due io beside timers —
> the timer-starvation reasoning applied to fds) + read_keys' unified
> cooperative wait (build output repaints without a keystroke). NEW
> gates: check-select-fire-owner + check-madcide-single-owners (whose
> count caught a third buffer-row site the by-hand audit missed);
> test_tui_model +2 (list/autofocus), test_task_io +1 (fire_due),
> testmadcide +palette/build gates (whole test 1.7s incl. the stop
> path). Battery on final content: fulltest rc=0 (all gates) + JIT
> **1171 passed / 0 failed / 0 timed out / 9 skipped** (suite = 1195)
> + EXE **1125 passed / 0 failed**.
>
> **Previous (2026-08-27, MT-4b io/fd select —
> feature/mt4b-io-select merge wave):** byte endpoints select beside
> value channels: `madc::chan_readable(channel)` registers an
> `exec://` endpoint as a `chan_select` case (fires with out = null on
> readable progress; drained-EOF/failed endpoints DISABLE, so `-1`
> still terminates mixed fan-ins), and reads under live tasks PARK on
> the fd through the scheduler's new io-wait seat
> (`__madc_task_io_wait_hook` in task_next_or_wait — the scheduler
> stays fd-blind; task_enqueue now idempotent). `select_fire` is THE
> one claim+wake owner (pre-merge dupaudit consolidation), gated by
> NEW `check-select-fire-owner.sh` (negative-controlled, in fulltest).
> NEW tests: `tests/unit/test_task_io.cpp` (park-until-readable, EOF
> is progress, double-unpark belt) + `tests/testgoselectio.mad`
> (phased deterministic mixed select — matched the hand-computed
> schedule first run; JIT/exe/obj byte-identical). Battery on final
> content: fulltest rc=0 (all gates) + JIT **1171 passed / 0 failed /
> 0 timed out / 9 skipped** (suite = 1195) + EXE **1125 passed / 0
> failed**.
>
> **Previous (2026-08-26, MT-4a select + time —
> feature/mt4-select-time merge wave):** `madc::chan_select`
> (deterministic lowest-index fan-in; husk/wake-once discipline),
> `madc::chan_try_recv` (1/0/-1), and `madc::sleep_ms` on the
> pluggable time source (virtual under `MADC_TASK_VTIME=1` — the
> clock jumps deadlines; `tests/unit/test_rt_vtime.cpp` pins 10
> madc-seconds in <1s wall, deadline order). One scheduling decision
> (task_next_or_wait) owns timers at every pick-next site. NEW tests
> `testgosleep` + `testgoselect` — both pin complete hand-computed
> schedules that matched byte-for-byte on first run, JIT and native
> lanes. Battery on final content: fulltest rc=0 (all gates) + JIT
> **1170 passed / 0 failed / 0 timed out / 9 skipped** (suite = 1194)
> + EXE **1124 passed / 0 failed**.
>
> **Previous (2026-08-26, stage-2 cooperative parse —
> feature/stage2-coop-parse merge wave):** the front end cooperates
> with the task scheduler: `parse_yield_point()` at every top-level
> decl + every ~1k lexed tokens (ambients re-bound on resume; batch
> compiles pay one queue check), the tui input wait yields to runnable
> tasks and delivers `{event:"wake"}` on drain, and madcide spawns its
> parse with the language's own `go` — typing stays live while a C++
> TU compiles. NEW gates: `tests/unit/test_coop_parse.cpp` (the
> deterministic interleave gate — two parses interleaved at every
> yield are byte-identical to serial, yields proven real) + wake
> transparency cases in test_tui_model (319/319). tui_scroll_gate
> (madcide's real pty loop with the spawned parse) PASS; testmadcide
> byte-stable; quit-mid-parse drain probe rc 0. Battery on final
> content: fulltest rc=0 (all gates) + JIT **1168 passed / 0 failed /
> 0 timed out / 9 skipped** (suite = 1192) + EXE **1122 passed / 0
> failed**.
>
> **Previous (2026-08-26, MT-2b joining main wrapper + static-fn internal
> linkage — feature/mt2b-main-join merge wave):** a spawning
> `--std=madc` TU's main now emits as `__madc_main` behind a
> synthesized joining wrapper — the task root scope drains at MAIN'S
> END (before atexit/TLS teardown) identically in the JIT / `-o` /
> `-r` / `-static-libmadc` lanes; the join callee is the NEW
> ledger-safe dispatcher `rt_task_join.c`. Pure programs keep their
> unwrapped runtime-free main (the battery's `test_native_shared`
> purity probe caught the first cut and forced the parse-time
> `_uses_go_spawn` gate). FOUND + FIXED in its own commit: madc dropped
> C internal linkage from every file-scope `static` function (ld
> "multiple definition" on two-object links; the ledger's
> rt_format+rt_dump static-inline pair fataled any `println` program
> under `-static-libmadc` — pre-existing, baseline reproduces). NEW
> tests `testgojoin` (argv forwarding + the drain schedule, all lanes)
> + `testprojectstaticfn` (gcc oracle a=2 b=300; nm STB_LOCAL); NEW
> gate: `forest_ledger_gate.sh` leg 5b names the exact pre-fix fatal.
> Battery on final content: fulltest rc=0 (all gates) + JIT **1168
> passed / 0 failed / 0 timed out / 9 skipped** (suite = 1192) + EXE
> **1122 passed / 0 failed**.
>
> **Previous (2026-08-26, MT-2 channels + task-local exceptions —
> feature/mt2-task-except merge wave):** value channels land
> (`madc::chan_*`, Go's contract; scheduler park/unpark; loud deadlock
> aborts) and the SJLJ exception state switches per-task (a `try`
> across a `yield` catches its own throw — the pre-fix build SEGFAULTS
> on the reducer, negative-controlled). The text ring is now immortal:
> glibc runs TLS dtors before atexit, where the native lane drains its
> root scope — testgochan's EXE lane caught the double free
> (gdb-backtraced to `__madc_fmt_take_cstr`). NEW tests `testgotry` +
> `testgochan` (both pin complete hand-computed schedules, byte-exact
> on first success; suite = 1190); all 19 exception-family neighbors
> green. One battery red was the mtime-poison trap (stale win64
> `rt_task.o` — clock skew; purged, banked). Battery on final content:
> fulltest rc=0 (all gates) + JIT **1166 passed / 0 failed / 0 timed
> out / 9 skipped**; EXE lane spot-green on the task family.
>
> **Previous (2026-08-26, cooperative tasks MT-1 —
> feature/mt1-substrate merge wave):** `go f(args);` + `yield()` land
> on the stackful cooperative substrate (`src/rt/rt_task.c`; FIFO
> run-to-yield, deterministic; root-scope join in
> `CirJitSession::run_main` + atexit for native artifacts). NEW tests:
> `testgo` (the complete deterministic schedule pinned byte-for-byte,
> spawn through post-main join), `testgoident` (declared names win —
> the error-shape negative control), `testgogate` (strict gnu17 mode
> byte-identical), `tests/unit/test_rt_task.cpp` (4 cases, exact FIFO
> interleavings). Suite = 1188. TWO GATES CAUGHT REAL DEFECTS in the
> new code before merge: i64-spec-spelling (slot spec bypassed
> `append_i64`) and the win64 archive gate (mingw `GetCurrentFiber`
> TIB intrinsic vs `-Werror=array-bounds`; fallback removed,
> Wine-verified). Battery on final content: fulltest rc=0 (all gates)
> + JIT **1164 passed / 0 failed / 0 timed out / 9 skipped**; EXE lane
> spot-green on all three go tests (the atexit join path).
>
> **Previous (2026-08-26, char[]-text boundary —
> feature/charbuf-text-boundary merge wave):** a fixed char array at
> the `{}` format boundary now DECAYS ([conv.array]) and formats as a
> C string (g++ std::format oracle matched: variable, struct member,
> and full-subscript element shapes); a non-char array is refused loud
> exactly as std::format's ill-formed treatment (previously the
> pointer printed as an integer, silently). The one decay owner
> (`array_decay_pointer`) now feeds the classifier. Reducers
> `testcharbuftext` (+ EXE lane green) and `testcharbuftextintarr`
> (expect_err) joined (suite = 1185). Battery on final content:
> fulltest rc=0 (all gates) + JIT **1161 passed / 0 failed / 0 timed
> out / 9 skipped**. KG Gap `madc_char_array_text_boundary` CLOSED.
>
> **Previous (2026-08-26, AST-5 multi-buffer —
> feature/ast5-multibuffer merge wave):** madcide gains JOE's `^K E`
> multi-buffer (buffer table on the editor-state bag; per-doc facts
> stay doc-scoped; switch restores caret/block and recolours from
> lexical spans with the parse deferred behind the paint), and the
> subject-document rule collapses from ten .madv copies to ONE owner
> (`verbs/_subject.madv`) gated by NEW
> `scripts/check-one-subject-doc.sh` in fulltest (negative control
> verified). `testmadcide` grew the editfile flow (new / back with
> caret restore / same-path switch); no new test files (suite = 1183,
> unchanged). Battery on final content: fulltest rc=0 (all gates, the
> new gate GREEN) + JIT **1159 passed / 0 failed / 0 timed out / 9
> skipped**; release rebuilt + NAS binaries pulled.
>
> **Previous (2026-08-26, lex_spans — feature/lex-spans merge wave):**
> `madc::lex_spans` classifies from lexing the buffer alone
> (skip_includes; the classifier factored out of `parse_spans`), and
> madcide's first paint now carries colour (pty probe: colour SGR in
> the first drain window on a header-heavy C++ file). Reducer
> `testlexspans.mad` joined (suite = 1183). Battery: fulltest rc=0
> (all gates) + JIT **1159 passed / 0 failed / 0 timed out / 9
> skipped**.
>
> **Previous (2026-08-26, span-coordinate truth —
> feature/span-coordinate-truth merge wave):** macro-expansion tokens
> no longer emit highlight spans (`tfSYNTHPOS`), and synthesized
> pushback text advances no source column — the SMAUG fight.c
> "everything turns blue at strip_grapple" smear (spans overrunning
> their physical lines by hundreds of columns) is fixed at the Source/
> lexer layer. Reducer `testspansmacro.mad` joined (suite = 1182).
> Battery on final content (lexer blast radius): fulltest rc=0 (all
> gates) + JIT **1158 passed / 0 failed / 0 timed out / 9 skipped** +
> EXE lane **1113/0**.
>
> **Previous (2026-08-26, IDE-9c perf half — feature/ide9c-scroll-perf
> merge wave):** the renderer's frame diff is cell-granular
> (`tui_diff_spans`; `tui_dirty_rows` delegates) and shift-aware
> (`tui_diff_plan`: shift verified by SIMULATION, cost-gated, emitted
> as DECSTBM + DL/IL) with EL finishing normal-space span tails.
> Measured (new meter `scripts/tui_scroll_bytes.py`, 24x80, 70 steps):
> scroll step 2,194 B/24 rows → 126 B/3 rows; caret step 113 → 35 B;
> session total 61,869 → 3,793 B (JOE 4.6 oracle 2,252). The win64
> archive gate caught an LLP64 truncation in the new FNV hash
> (`unsigned long` is 32-bit on Win64 — `uint64_t` fix). Wave battery
> on final content: fulltest rc=0 (all gates) + JIT **1157 passed / 0
> failed / 0 timed out / 9 skipped** + 48/48 unit binaries
> (test_tui_model 21 cases / 303 asserts; suite = 1181, unchanged).
>
> **Previous (2026-08-26, Track 7.2 IDE-9 session — three merges):**
> (1) the IDE-9c scroll corruption is FIXED (raw `\t` bytes in grid
> cells desynchronized grid/screen columns — tabs now expand at the
> document→grid projection through THE byte→display-column map; the
> `put()` cell-invariant belt renders control bytes `?`); the NEW
> `scripts/tui_scroll_gate.sh` joined fulltest (madcide on a real pty,
> VT100 screen reconstruction with true tab-stop semantics + negative
> control; pre-fix 828 corrupted rows / post-fix 0 / joe 4.6 same-input
> oracle 0). (2) madcide loud startup failures: `profile_dir` derives
> from `__FILE__` (any cwd), theme/parse-handle failures post status
> messages. (3) IDE-9a/9b JOE screen model: ONE top status row from
> profile-owned joerc `lmsg`/`rmsg` formats (`profiles/joe.status`),
> `%k` chord echo LIVE (`ui::tui_pending` + chord repaint events),
> banner+menu dropped, prompts overlay, transient bottom hint. Under
> it: `perl::substr` past-end offset ABORTED the process — clamped to
> Perl's undef→empty (real-perl oracle,
> `tests/testperlsubstrrange.mad`). Wave-2 battery: fulltest green
> (units + all gates) + JIT **1157 passed / 0 failed / 0 timed out / 9
> skipped** + EXE lane **1112/0** (suite = 1181; testperlsubstrrange
> joined). test_tui_model: 16 cases / 280+ asserts (tab expansion +
> chord repaint cases joined).
>
> **Previous (2026-08-26, braced-init-list call arguments /
> [over.ics.list] slice 1 — feature/braced-list-call-args-claude merge
> wave):** the braced-list call-argument SIGSEGV
> (`parser_segv_braced_list_call_arg`, the madcide in-process crash) is
> FIXED: a `{`-headed argument re-spells against the callee's parameter
> type through THE re-spell owner (`respell_braced_list_for_target`,
> extracted from `TokenRETURN::parse`; both call readers adopt it,
> hidden-`__this` aware); `TokenObjTemp` carries braced-ness so the
> existing [dcl.init.list]/4+/5 CIR arms serve functional-form
> temporaries; `initializer_list_literal` emits SIZED backing arrays
> (also fixes the latent decl-path garbage-elements silent-wrong);
> `parseExpression` refuses a `{` HEAD loudly (the belt). TWO deeper
> finds fixed in their own commits: the c2mir check guard treated
> `N_ASSIGN`'s NULL context barrier like an owning declaration (nested
> unsized array literal = garbage past `[0]` from plain C, uncast form
> crashed gen — stock-upstream, PR candidate; c2mir interp + bootstrap
> suites green) and `install-libmadc` never shipped `madc_typeid.h`
> (installed `madc_api.h` includes it). New reducers
> `tests/testinitlistarg.mad` (six shapes, g++/clang++ oracle) and
> `tests/testnestedcomplit.mad` (`--std=c17`, gcc/clang oracle).
> `examples/embed_hello.cpp` parses CLEAN through a parse handle (0
> problems, 123 spans). Loud residues banked (KG):
> `braced_list_decl_ctor_argument`, `embed_hello_full_compile_residues`.
> Merge-wave battery: authoritative Linux fulltest green (units +
> gates) + JIT **1156 passed / 0 failed / 0 timed out / 9 skipped** +
> EXE lane **1111/0** (suite = 1180; testinitlistarg +
> testnestedcomplit joined; testquotedincfallback's first wave).
>
> **Previous (2026-08-25, error-tolerant parse slice A / arc doc §3.5 —
> feature/error-tolerant-parse-claude merge wave):** the parser CONTAINS
> each top-level error (record → restore entry depths → DelimDepth
> resync seeded with STREAM-TRUTH brace debt, so a mid-body failure
> syncs at the region's close → `SkippedTokens` node → continue) and
> reports EVERY top-level error before refusing — gcc canon. One
> `TokenError` class + all eight `ErrorNodeKind` kinds (owner ruling);
> `Program::error_nodes` gates translate at `cir_translate_guarded`
> (run/eval/emit-c11/freeze/native all refuse); `--emit=c++` stays a
> source view (renders the retained echo, exit nonzero). Parse handles
> serve broken trees: `parse_check` reports every contained error;
> outline/enclosing/spans answer before AND after a broken region
> (testparserecoverh, `.expect_quiet` gating the capture mute;
> testparserecover pins the 3-error CLI cascade via `.expect_err`).
> Three dupaudit families consolidated + gated
> (`check-one-parse-error-recorder.sh`, 3 markers + negative controls):
> `record_frontend_error` (10 inlined copies, incl. the lexer's
> tokenize/tokenize_buffer clusters), `record_throw_diagnostic` (5 —
> throwbuf::sync renders but never records), and
> `restore_parse_scope_depths` (3, incl. derive-lazy-catch which the
> gate itself caught). Unit predicates updated: "accepted"/"rejected"
> now means a CLEAN parse (`error_nodes == 0` — the gate's predicate).
> Merge-wave battery: authoritative Linux fulltest green (units +
> gates) + JIT **1153 passed / 0 failed / 0 timed out / 9 skipped** +
> EXE lane **1108/0** (suite = 1177; testparserecover +
> testparserecoverh joined).
>
> **Previous (2026-08-25, spans/styles/schemes / AST-2 —
> feature/madcide-ast2-claude merge wave):** highlight spans as
> edit-node hints painted through the ONE range-overlap rule (selection
> wins); `tui_attr` = the style struct speaking JOE's vocabulary with
> bold-as-bright (owner: VT-102 ANSI); `emit_sgr` = the one
> reset-then-set SGR table with the historical 7m/0m spellings — the
> no-colour stream is byte-identical (testvised/testlineed pinned).
> `madc::parse_spans` classifies the handle's retained tokens (one
> classifier; comments from trivia — handles arm keep_trivia, cost in
> the noise); colour SCHEMES = profiles/*.theme swapped via ^K T.
> Two lexer defects the pins exposed were fixed at their owners
> (pushback_reread column rewind; the `char *s` lookahead space —
> a live --emit=c++ fidelity leak; emitcxx_rt3 joined the gate).
> Merge-wave battery: authoritative Linux fulltest **1151 passed / 0
> failed / 0 timed out / 9 skipped** (testparsespans joined) + EXE lane
> **1107/0**; pty colour smoke green (33/32/1;34/7m/0m).
>
> **Previous (2026-08-25, persistent parse handles / AST-1 —
> feature/madcide-ast1-claude merge wave):** the compiler-data child
> machinery given a LIFETIME. `madc::parse_open/open_file/refresh/close`
> + `parse_outline/parse_check/parse_enclosing` (outline-at-offset from
> TokenFunc + `end_line`; outline rows gained `end_line`) answer from
> RETAINED state; `project_open/tus/close` parse a cc.json manifest's
> TUs each with its own options (`apply_project_tu_options` = THE one
> -I/-D/--std rule, extracted; its absence in the handle path was a
> phantom-diagnostic divergence the measurement itself caught). madcide:
> one handle per buffer, one refresh entry, enclosing-function status
> line. MEASURED: parse-on-load ~0.25 s (adventure) / ~0.5 s
> (open-adventure C 18k LOC); largest-TU refresh 55–120 ms → disk-cache
> NO-GO at current scale (§3.4); error-tolerant parse banked as §3.5
> (discussion pending). Dupaudit: `handle_table<T>` (4 copies → 1,
> GATED by `check-one-handle-table.sh`, in-battery OK); tu_own_function.
> Merge-wave battery: authoritative Linux fulltest **1150 passed / 0
> failed / 0 timed out / 9 skipped** (testparsehandle joined) + EXE lane
> **1106/0**.
>
> **Previous (2026-08-25, --emit=c++ / AST-4 slice 1 —
> feature/madcide-ast4-claude merge wave):** the C++ reverse-render
> (owner-required). `celCxx` through the one converter; the render =
> the TU's RETAINED SOURCE (recorded `#include` directives + whole-TU
> token echo via the exposed one spelling owner + trailing trivia;
> `CirEmitSource` passes it as data). String literals re-escape through
> the NEW one owner `madc_c_escape_string` (`cir_emit_c` N_STR adopted
> it — dupaudit family consolidated same-session; also fixed a latent
> macro-arg re-lex bug). `madc::emit` accepts `"c++"`; madcide's `^K N`
> adds the C++ view on C/C++-extension buffers (the app's document-kind
> rule). Two probe-driven deviations recorded in the design doc §3.3
> (whole-TU echo, no compiler-side dialect refusal). Merge-wave battery:
> authoritative Linux fulltest **1149 passed / 0 failed / 0 timed out /
> 9 skipped** + EXE lane **1105/0**; NEW fulltest gate
> `emitcxx_roundtrip_gate` (2 reducers × g++ AND clang++, behavioral
> byte-compare, two negative controls — in-battery OK); testmadcide
> extended with the .cpp-buffer view world (all pins matched first run).
>
> **Previous (2026-08-25, the view seam / AST-3 —
> feature/madcide-ast3-claude merge wave):** the madcide AST arc's
> first slice, owner-pulled ("build the view seam"). Engine:
> `madcdis/doc_lens.h` `doc_map` — the ONE display↔stored coordinate
> owner (copy segments as data, strict codec; gap-adjacent boundaries
> belong to the copy that ENDS there; empty map = 0) with
> `ui::lens_to_display/lens_to_stored` as its dialect face;
> `madc::emit(out, source, filename, target)` — the render query
> (diagnostics/outline child + `madc_cir_emit` through the existing
> StringCapture owner; byte-identical to CLI `--emit=`), target names
> through the ONE converter `cir_emit_lang_of()` the CLI now shares.
> App: madcide `^K N` cycles read-only code views original→MC11→C11
> over the one document (lens applied at composition; view-buffer
> entity so the one navigation implementation works in display space;
> `nav_doc()` routing; renderer untouched — identity-lens
> byte-identity structural). Scoped dupaudit: view_active_predicate
> (5 sites born on-branch) consolidated into `view_name()`;
> emit_target_name_conversion recorded. Merge-wave battery:
> authoritative Linux fulltest **1149 passed / 0 failed / 0 timed out
> / 9 skipped** (suite unchanged — testmadcide extended in place with
> the view battery: enter/exit pins, lens hints, display-space find,
> refusals, byte-identical exit; the tail byte-count re-proves the
> stored document never moved), EXE lane **1105/0**; NEW unit battery
> test_doc_lens **6 cases / 106 asserts** (identity, concealed,
> synthetic, codec + add negative controls); real-pty view smoke 5/5;
> testvised/testlineed byte-identity green.
>
> **Previous (2026-08-25, madcide v2 — feature/madcide-v2-claude merge
> wave):** the owner's FULL JOE binding set on the relocated tool
> (`tools/madcide`; work order = the plan doc's "Owner review" section,
> items 1+2 — item 3, the AST arc, awaits the owner brainstorm).
> Engine feeders: tui_keyparse 0x1c–0x1f (`^\ ^] ^^ ^_`); text_buffer
> REDO (two stacks, `now_meta`-carrying pair, checkpoint clears redo) +
> word motion; `tui_target::suspend/resume` + `ui::tui_suspend/resume`
> (one enter/leave_grid_mode implementation for all four callers); the
> ctrl-insensitive CONTINUATION convention (`^K ^Z` == `^K Z`,
> `tui_bindings::cont_spelling` — forced live by the pty probe). App:
> motion delegation by key-name to the one shared edit_key; block model
> mark+bend; one prompt mode (find/goto-line/insert-file) + `^L`
> find-next; help pane = the loaded profile projected; `^K Z` shell
> verified on a real pty (suspend → shell on the tty → resume, full
> repaint). Scoped dupaudit: find_wrap_rule + block_refusal_message
> consolidated same-session. Merge-wave battery: authoritative Linux
> fulltest **1149 passed / 0 failed / 0 timed out / 9 skipped** (suite
> unchanged — testmadcide extended in place with the v2 pins, every
> edit undone so the tail byte-count proves the round trips), EXE lane
> **1105/0**; unit batteries test_tui_model 206 asserts (punctuation
> controls, ctrl continuations), test_text_buffer 192 (redo, word
> motion); trailers 558/0; purity/dialect-lean/write-path/adventure
> parity green in-battery.
>
> **Previous (2026-08-25, madcide / hub Phase 2 — feature/madcide-claude
> merge wave):** the madc IDE and its feeders
> (docs/plans/2026-08-25-madcide-phase2.md). Bindings-as-data chords
> (`ui::tui_bind_keys` — key sequences → action names, per-profile,
> JOE/WordStar default per the owner; the `action` event; one
> key-spelling owner both directions in the model); buffer history/undo
> on the text component (`ui::text_checkpoint`/`text_undo`, pieces
> snapshots + opaque payload); compiler data as data
> (`madc::diagnostics`/`madc::outline` — compile-never-execute child,
> capture replaces rendering via `DiagnosticRenderMute`); the R3
> sibling enforced (re-entrancy latch, `ui::bind_require_key`
> code-entity gating; compile-once re-scoped on the baked-ctx blocker);
> `tools/madcide` (relocated from examples/ 2026-08-25 — a tool, not an
> example) over the shared `editor_events.inc` split
> (testvised/testlineed byte-identical). FIXED en route (own commit):
> stale ambient token position — eval-child and `--project` TU2+
> diagnostics now carry their OWN file:line (byte-matching the
> file-based oracle). Merge-wave battery: authoritative Linux fulltest
> **1149 passed / 0 failed / 0 timed out / 9 skipped** (suite +5:
> testmadcide — the Phase-2 gate headless, all three clauses;
> testcompilerdata — structured diagnostics/outline, `.expect_quiet` =
> the capture proof; testprojecterrline — the position-fix reducer,
> `.expect_err`; testreenter; testbindgate), EXE lane **1105/0** (of
> 1149 JIT-passing); tui_smoke_gate green in-battery, now pinning a
> bound chord across read batches on the real pty; unit batteries:
> test_tui_model 174 asserts (chords), test_text_buffer 145 (history),
> test_verbs 69 (re-entrancy latch); adventure parity + purity +
> write-path + dialect-lean gates green in-battery; madcide AND vised
> verified end-to-end on real ptys.
>
> **Previous (2026-08-25, Track 7.2 R5 — feature/tui-provider-claude
> merge wave):** the level-1 TUI: `madcdis/tui_model.h` (the
> dependency-free grid/focus/key model) + `madcdis/tui_provider.h` (the
> target seam) + `src/ui_term.cpp` (the hand-rolled VT100/xterm target —
> owner-decided over vendoring ncurses/termbox2/notcurses) +
> `ui::tui_open/close/rows/cols/render/event`; availability CHECK
> bindings (`ui::bind_check`, both kinds, ONE evaluator behind
> affordances and dispatch — design invariant 5; DupFamily
> lineed_readonly_gate consolidated into `checks/editable.madv`); and
> `tools/texteditor/vised.mad`, the visual editor over the SAME
> document actions and w/q/q! script verbs as the line editor (design
> success criteria 3 + 4). Merge-wave battery: authoritative Linux
> fulltest **1144 passed / 0 failed / 0 timed out / 9 skipped** (suite
> +2: testeditcheck — one state rule flips enumeration and dispatch
> together; testvised — the headless semantic core over the exact
> tui_event value objects; both `.expect_quiet`), EXE lane **1101/0**
> (of 1144 JIT-passing); NEW fulltest gate `tui_smoke_gate.sh` — the
> VT100 target on a REAL pty (alt-screen discipline, drawing,
> attributes, cursor, the exact semantic event stream incl. coalescing
> and resize) plus a negative-control program that must fail the
> harness; unit batteries incl. test_tui_model (NEW, 100 asserts),
> test_verbs (65 — the check cases), test_interaction (31); adventure
> parity + purity + write-path + dialect-lean gates green in-battery;
> testlineed byte-identical across the check consolidation.
>
> **Previous (2026-08-25, Track 7.2 R4 line-mode scope —
> feature/texteditor-claude merge wave):** the piece-table text
> component (`madcdis/text_buffer.h`, the hub's second component kind)
> + `ui::world_new` + the `ui::text_*` family +
> `php::file_get_contents`/`file_put_contents` + the line-mode editor
> (`tools/texteditor/`, nine script verbs through the one registry).
> Merge-wave battery: authoritative Linux fulltest
> **1142 passed / 0 failed / 0 timed out / 9 skipped** (suite +2:
> testuitext — the component surface, line-composed range edits;
> testlineed — the §7.7 two-phase transcript gate, writable then
> read-only, `.expect_quiet`), EXE lane **1099/0** (of 1142
> JIT-passing); unit batteries incl. test_text_buffer (mirrored
> std::string oracle, 128 asserts) and test_verbs (42 asserts, the
> mutation-context text case); adventure parity + purity +
> write-path + dialect-lean gates green in-battery.
>
> **Previous (2026-08-24, eval gaps closed + Track 7.2 R2):** computed
> carrier text survives function returns — `madarray_cstr` string kind
> rings a COPY (the pre-L3 c_str contract, uniformly) and
> `translate_return` coerces a carrier operand for char*-returning
> functions through `object_cstr_arg` — ctx `const char *` bindings
> bake to literals in EVERY read shape (`baked_cstr_constant`, one fold
> owner: plain read + subscript + deref) — and R2 projection-as-data is
> EXECUTED (`choice` role, uinode↔value bridges,
> ui::inspect_tree/render_tree; test_projection at 67 asserts).
> Authoritative Linux fulltest
> **1140 passed / 0 failed / 0 timed out / 9 skipped** (suite grew +4:
> testevalreturn — all six eval-body return shapes incl. `+=`;
> testcstrreturn — the plain-function twin; testevalctxderef — ctx
> bindings under `[]`/`*`; testuitree — the R2 gate: script-composed
> choice tree → numbered menu, inspect_tree as walkable data,
> render_tree∘inspect_tree ≡ render_inspect; all `.expect_quiet`);
> Colossal Cave Adventure parity **3 fragments + 94 whole logs
> byte-identical** in-battery; check-engine-app-purity OK; all gates +
> ratchets green. Eval bodies have NO idiom restrictions left.
>
> **Previous (2026-08-24, the Rule #7 eviction — interaction core +
> script-entity verbs, @67a901c7 on
> feature/ui-interaction-rework-claude):** the merge-wave battery:
> authoritative Linux fulltest **1136 passed / 0 failed / 0 timed out /
> 9 skipped** (suite grew +1: testaffordances, the pinned affordance
> enumeration probe); ALL unit batteries green (test_verbs reworked to
> the seam-law contract + the script binding kind, 37 asserts;
> test_interaction NEW, 31 asserts); Colossal Cave Adventure parity
> **3 fragments + 94 whole logs byte-identical**; NEW gate
> `check-engine-app-purity.sh` OK (negative-controlled) beside
> `check-hub-write-path.sh`; warning/tsubst/trailer ratchets green.
> The pilot transcripts (testadventure, testadventurebuilder) run
> entirely on madc-SOURCE verbs (`tests/adventure_verbs/*.madv`)
> through the generic registry with EMPTY stderr, and an EXE-lane spot
> check (`bin/madc -o` of testadventure) reproduces the full transcript
> natively — runtime eval works under AOT. `include/madcdis/adventure.h`
> is deleted; the engine ships zero verbs.
>
> **Previous (2026-08-24, win64 lazy wrapper fixed + addressof-reference
> typing):** the merge-wave battery at @f626e7b3 content: authoritative
> Linux fulltest **1135 passed / 0 failed / 0 timed out / 9 skipped**;
> EXE **1092/0**, OBJ **1092/0** (of 1135 JIT-passing); release, packed
> (**1135/0/0/9**) and headerless (**1108/0/0/36**) green; linux pack
> gate at the **93/93** baseline with hard-zero load counters. Wine
> packed suite on the LAZY-interface exe (both v0.95.1 eager guards
> deleted): **1092 passed / 0 failed / 0 timed out / 52 skipped** —
> green run 5; runs 1–4 each had 1–2 wine-console flake-class
> singletons (seven distinct names, every one individually
> rc=0-correct; distribution matches the eager era — evidence in KG gap
> `wine_suite_flake_class`). MIR c2mir-gen-test AND c2mir-gen-test3
> both **1143/2286/0 (exact baseline)** after the mir-x86_64.c edits.
> Darwin packs: arm64 AND x86-64 both at **58** pack errors (six
> `<filesystem>` overload refusals fixed at the `&reference` typing
> origin; baseline lowered 64 → 58). Suite grew +1
> (testaddrofrefoverload — the addressof-reference reducer).
>
> **Previous (2026-08-23, v0.95.1 — the v0.95 binary-shipping patch):**
> the three-platform promotion gate's wine suite caught the v0.95.0
> win64 `--project` regression (all six project tests:
> EXCEPTION_ACCESS_VIOLATION at the first lazily generated call; MIR's
> win64 lazy first-call redirect — one-flag proof: the identical run
> under `-g` is byte-correct). Fixed @41c0a663 (win64 keeps the eager
> gen interface; Linux/darwin keep the lazy startup lever; KG gap
> `mir_win64_lazy_gen_wrapper` is the follow-up). Wine packed suite
> after the fix: **1091 passed / 0 failed / 0 timed out / 52 skipped**
> — two consecutive clean full runs (one earlier single-run flake of
> `teststaticmemberaddr` did not reproduce in either). Linux compiles
> are textually identical (`_WIN32` arm drops out), so the v0.95.0
> battery below remains the Linux evidence.
>
> **Previous (2026-08-23, v0.95.0 — ui:: + Adventure + cold startup):**
> the merge-wave battery for feature/track7-hub-projections-claude, run
> ONCE from clean obj/ at the final content (rb-20260823-051447):
> authoritative Linux fulltest **1134 passed / 0 failed / 0 timed out /
> 9 skipped**; EXE **1091/0**, OBJ **1091/0** (of 1134 JIT-passing);
> release rc=0 and packed suite **1134/0/0/9** against the packed
> `bin/madc-release`; headerless **1107/0/0/36**; Colossal Cave
> Adventure parity **3 fragments + 94 whole logs byte-identical** (+
> negative controls) and the roundtrip gate green; forest_bind_gate all
> cells; rule trailers **537/0**; packed forest gate at the 93-error
> baseline with hard-zero load-side counters. Suite grew +1
> (testuiprompt — the ui::prompt scripted-arm pin). Windows/macOS lanes
> re-validate at the v0.95.0 master promotion per promote.md step 5.
>
> **Previous incremental (2026-08-23, c2mir registry page arena):**
> `mir-tests/c2mir-custom-alloc.c` supplies a shifted-pointer `MIR_alloc`
> (so any raw-libc deallocation aborts), compiles a C reducer through
> c2mir, and requires balanced allocation/free counts. The gate passes at
> **828/828** and Valgrind reports zero errors / zero bytes live. c2mir's
> simple sieve test, the repository build, and `make -C src test` are
> green; packed Adventure output is byte-identical and the packed forest
> gate holds the **93/93** parse-error baseline. The last merge-wave
> fulltest remains **1133/0**; fulltest/EXE/OBJ were not repeated for this
> incremental slice.
>
> **Current (2026-08-20, v0.94.0 — upstream-community MIR hardening):**
> authoritative Linux fulltest **1104 passed / 0 failed / 0 timed out /
> 9 skipped**; EXE **1063/0**; MIR c2mir-gen-test AND c2mir-gen-test3
> both **1143/2286/0 (exact baseline)**. Reducers: upstream issue #467's
> min_repro/min_repro_loop 0/0/0/0 across gen levels (were SIGSEGV/hang
> at level 2); PR #468's `t*t` reducer rc=0 at `-O2 -eg`; upstream issue
> #429's two reducers PASS on real Apple Silicon at levels 1 and 2
> (fork not affected — darwin ABI arc covers them). New in-fork test:
> `make aarch64-mem-disp-test` (aarch64 hosts, from PR #466).
>
> **Previous (2026-08-20, v0.93.0 — the MIR convert false-dependency
> fix):** authoritative Linux fulltest **1104 passed / 0 failed / 0
> timed out / 9 skipped**; EXE **1063/0**; MIR c2mir-gen-test
> **1143/2286/0 (exact baseline)** — the merge-wave battery for the
> one-commit fork change (`mir-gen-x86_64.c` pattern table: `pxor`
> dep-break on scalar SSE converts). Perf evidence: donut.c 300 frames
> 1.374s → **0.493s** (gcc -O0 0.514s), outputs md5-identical;
> sin-only/sweep microbenchmarks unregressed. No fixture changes.
>
> **Previous (2026-08-20, v0.92.1 — the v0.92 binary-shipping patch):**
> authoritative Linux fulltest **1104 passed / 0 failed / 0 timed out /
> 9 skipped**; EXE **1063/0**, OBJ **1063/0**, release rc=0 — one
> merge-wave battery at the fixed content. Windows: wine packed suite
> **1061 passed / 0 failed / 52 skipped** (both v0.92.0-era regressions
> fixed: `testlibcnoheaderargs` — the four float-math names ucrtbase
> hides are pinned; `testphpvardump` — the LLP64 platform `long` words
> as its source name). macOS: both arches cross-built,
> verify_macho_release green, and the hardware battery on the arm64
> tarball is **8 passed / 3 failed — exact leg-for-leg parity with the
> shipped v0.82.0 binary** (re-run on the same host as a negative
> control): the three fails are the standing known-opens (groves
> `os.str()` husk, value intrinsic — now a loud compile error where
> v0.82.0 SIGSEGV'd — and the exec:// channel), so zero darwin
> regressions. First three-platform asset set since v0.82.0 (deb/rpm,
> windows zip, two macOS tarballs, SHA256SUMS).
>
> **Previous (2026-08-20, v0.92.0 — bare `cout << value` + std::format/
> print/println intrinsics):** authoritative Linux fulltest **1104 passed /
> 0 failed / 0 timed out / 9 skipped**; **1113 tests compiled with ZERO
> warnings**; EXE **1063/0**, OBJ **1063/0**, packed **1104/0/0/9**,
> headerless **1077/0/36skip**, release rc=0 — one merge-wave battery.
> New: `testvaluecout`/`testvaluecoutorder`/`testvaluecoutsstream` (the
> value inserter as always-included surface, guard-keyed injection),
> `teststdprint`/`teststdformat`/`teststdprintvalue`/`teststdformaterr`
> (the formatting intrinsics; plus `test_rt_format`, 4334 unit assertions
> against 1430 generated libstdc++ oracle rows), `testcastsizeof` (cast of
> paren-less sizeof — front-end fix found by the AOT ledger parse of the
> new engine).
>
> **Previous (2026-08-19, v0.91.0 — `<iomanip>` manipulator objects):**
> authoritative Linux fulltest **1096 passed / 0 failed / 0 timed out /
> 9 skipped**; **1105 tests compiled with ZERO warnings**; EXE **1056/0**,
> OBJ **1056/0**, packed **1096/0/0/9**, headerless **1069/0/36skip**
> (testiomanip carries a headerless_skip — <iomanip> is a documented
> pack-corpus blocker, forest_pack_headers.txt v3), release rc=0 — full
> battery. New: `testiomanip`
> (setprecision/setw/setfill/fixed, g++ AND clang++ oracle byte-identical,
> plus a manipulator applying to a streamed `value`). Both v0.88.0 priority
> residues are now closed (value(N) construction in v0.90.0, manipulator
> objects here).
>
> **Previous (2026-08-19, v0.90.0 — `value(N)` constructs):** authoritative
> Linux fulltest **1095 passed / 0 failed / 0 timed out / 9 skipped**;
> **1104 tests compiled with ZERO warnings**; EXE **1055/0**, OBJ
> **1055/0**, packed **1095/0/0/9**, headerless **1069/0/35skip**, release
> rc=0, MIR c-tests **1143/2286/0** (exact baseline) — full battery. New:
> `testvaluector` (ctor overloads on the carrier: temporaries, direct-init,
> carrier copy, temps as call args with kind AND payload preserved) and
> `testforinitctor` (for-init parens direct-init + per-iteration while-header
> temporary; g++ AND clang++ oracle, byte-identical). Recorded with
> reducers: madc's front end ignores user __attribute__((cleanup)) on
> locals; declaration-flow ';' conventions remain asymmetric.
>
> **Previous (2026-08-19, v0.89.0 — `php::array_push` overload set):**
> authoritative Linux fulltest **1093 passed / 0 failed / 0 timed out /
> 9 skipped**; **1102 tests compiled with ZERO warnings**; EXE **1053/0**,
> OBJ **1053/0**, packed **1093/0/0/9**, headerless **1067/0/35skip**,
> release rc=0 — full battery. New: `testarraypush` (one overloaded name,
> PHP-parity count returns, every kind's var_dump word — float arg lands on
> the double overload, literal 0 stays an integer push) and
> `testoverloadnumrank` (numeric overload GRADING: float selects double —
> promotion — never the truncating int64 by registration order; g++ and
> clang++ oracle both agree, unambiguous). Retired spellings
> `array_push_int` / `array_push_array` migrated across 7 tests + 4 docs
> pages.
>
> **Previous (2026-08-19, v0.88.0 — `cout << value`):** authoritative Linux
> fulltest **1091 passed / 0 failed / 0 timed out / 9 skipped**; **1100 tests
> compiled with ZERO warnings**; EXE **1051/0**, OBJ **1051/0**, packed
> **1091/0/0/9**, headerless **1065/0/35skip**, release rc=0 — full battery.
> New: `testvaluestream` (each kind streams via the REAL inserter — hex,
> boolalpha, chaining — byte-identical to the plain-type twin program, which
> g++ and clang++ agree on; null empty; the array line = stderr notice +
> nothing streamed + the stream survives). Found and recorded with reducers:
> `value(N)` functional-cast temporaries build a garbage-kind temp;
> `std::setprecision` (manipulator OBJECTS) fails with plain doubles too.
>
> **Previous (2026-08-19, v0.87.0 — `for (value v : a)`):** authoritative Linux
> fulltest **1090 passed / 0 failed / 0 timed out / 9 skipped**; **1099 tests
> compiled with ZERO warnings**; EXE **1050/0**, OBJ **1050/0**, packed
> **1090/0/0/9**, headerless **1064/0/35skip**, release rc=0 — full battery.
> New: `testforeachvalue` (the loop element is the carrier itself: kind
> preserved through the copy — string lengths + the caught integer throw in one
> loop; mutation leaves the source intact; empty array = zero iterations;
> `value &v` refusal pinned). The other half of the original probe —
> `cout << value` — fails identically with NO loop (a streaming/operator gap,
> recorded in docs/plans/2026-08-19-range-for-auto-deduction.md).
>
> **Previous (2026-08-19, v0.86.0 — the compiler knows the C library's
> signatures):** authoritative Linux fulltest **1089 passed / 0 failed / 0 timed
> out / 9 skipped**; **1098 tests compiled with ZERO warnings** (full all-zero
> baseline); EXE **1049/0**, OBJ **1049/0**, packed **1089/0/0/9**, headerless
> **1063/0/35skip**, release rc=0 — the full battery run per code commit (three
> times: once per arc).
>
> Five new tests, and what each is the gate for:
> `testlibcnoheader` (an UNDECLARED libc call's RETURN type, headerless, gcc -O0
> oracle — before the fix `strcmp(a,b) < 0` was FALSE and `floor(2.7)` was 1.0;
> two deliberate, documented divergences where gcc's own output is UB garbage:
> madc's atof/atoll answers are the correct ones), `testlibcnoheaderargs` (the
> ARGUMENT convention — all nine C99-math argument shapes × three suffixes,
> byte-identical to gcc; `floorf(3.9f)` was 2.000 through float promotion),
> `testvaluecount` (the owner .count()/.size() semantics: elements / length /
> catchable error — `value s = "hello"; s.count()` was a silent 0; the range-for
> bound pinned unchanged by the visited=0/count=2 line), `testforeachauto` (the
> `auto` range-for element, g++ AND clang++ -O0 oracle, including the
> [stmt.ranged] shadowing case `for (auto x : x)`), `testforeachautoarray` (the
> madc array deduces `string`, the #91 subscript ruling). New fulltest gate:
> `scripts/check-libc-alias-signatures.sh` — every `__builtin_` alias target
> must carry a signature entry, with a negative control that must fail.
>
> **Previous (2026-08-18, v0.85.0 — php::print_r / php::var_dump over ANY madc
> type, THE ARC COMPLETE):** authoritative Linux fulltest **1083 passed / 0
> failed / 0 timed out / 9 skipped**; **1092 tests compiled with ZERO warnings**
> under the c2mir ratchet (baseline 0, and it is a full all-zero baseline);
> trailer gate 456 code commits / 0 missing. The dump tests are oracled against
> **php-cli 8.3.6** — every `print_r` block is byte-identical to PHP's for the
> corresponding PHP value, captured with `cat -A`; for the iterator containers all
> TEN blocks PHP produces for that data match byte for byte
> (`tmp/or/map.php`, `tmp/or/iter_pr.php`). `php-cli` is in
> `scripts/provision_container.sh`: it is an oracle like g++ and clang++, and its
> absence would have read as green.
>
> New in session #104: `testphpdumpiter` (std::map / set / list — int and string
> keys, nested sequence and map values, struct and string values, empty, inside a
> struct, through a pointer, captured, plus a hand-rolled container for EACH
> iterator shape: a class iterator and a raw-pointer one — 196 expect lines),
> `testforeachiter` (the range-based `for` over the same containers, oracled
> against `g++ -O0` and `clang++ -O0`, including a `T&` loop variable mutating an
> element and a `continue` that must still advance), and `testptrcmpupcast` (a
> derived->base pointer COMPARISON owes the base adjustment — `B2 *p2 = &d;
> p2 == &d` answered 0 where both compilers answer 1). `testforeachkeyed` was
> retargeted: madc no longer refuses the container, so the rejection moved to the
> element assignment where c2mir reports it, and `testphpdumprefuse` was rebuilt
> on the one PERMANENT refusal (a container with begin()/end() and no size()).
> `testphpdumpselfref` gates the type-path guard — a container whose element type
> is the container used to consume 4 GB and die in `std::bad_alloc`, and its
> `Twice` case is the negative control that a visited set (rather than a PATH set)
> would fail.
>
> `--emit=c11` parity spot-checked for the new walk: the generated C compiles
> under `gcc -std=c11` and its output is byte-identical to the JIT's
> (`tmp/probe/emitc_iter.mad`). Native lanes: **EXE 1044/0**, **OBJ 1044/0**,
> packed **1084/0/0/9**, headerless **1058/0/35skip**, release rc=0 — the whole
> battery re-run over the five code commits.
>
> ⚠️ **A 1084/0 run is a SLICE, and this session proved it.** `d237d83b`'s
> type-path guard refused `php::print_r(k)` on
> `struct Link { int v; Link *next; }` — a struct dumped BY VALUE holding a pointer
> to itself — and fulltest was **1084 / 0 with that bug in**, because every pointer
> test dumps a POINTER at top level (`php::print_r(&a)`) so the by-value shape had
> no coverage in 1084 tests. It was found by re-measuring the probe set AFTER the
> release commit (`54da4e38` is the fix). `tests/testphpdumpselfref.mad` now carries
> the three by-value shapes. Coverage on the container's v0.85.0 binary: **21 OK,
> 0 not-OK**.
>
> **Previous (2026-08-17, the dump arc merged to `develop` and still
> UNRELEASED at that point):** authoritative Linux fulltest **1071 passed / 0 failed / 0
> timed out / 9 skipped** (eight new tests: four for the dump intrinsics, two for
> the range-for protocol fix, one for print_r's $return, one for madc::value
> initializers); zero warnings under `-Werror`. The dump tests are
> oracled against **php-cli 8.3.6** — every `print_r` block is byte-identical to
> PHP's for the corresponding PHP value, captured with `cat -A` (the captures
> live in `docs/plans/2026-08-17-php-print-r-var-dump-plan.md` §2). `php-cli` is
> now in `scripts/provision_container.sh`: it is an oracle like g++ and clang++,
> and its absence would have read as green.
>
> New tests: `testphpprintr` (scalars), `testphpprintrstruct` (structs, classes
> with access, inheritance, unions, anonymous unions, bit-fields, fixed arrays),
> `testphpvardump`, `testphpseq` (`std::string` as text, `std::vector` as an
> array, a string member inside a struct, a vector of strings),
> `testforeachkeyed` (a keyed container's range-for is a compile ERROR, as g++
> and clang say — it used to SIGSEGV), `testforeachrefindex` (a positional
> `operator[](const long &)` container iterates instead of dereferencing an
> integer). The dump + foreach + string + vector + subscript families — the
> blast radius of the two `operator[]`/nullary call owners — are green in JIT,
> `--exe` and `--obj` (36 tests per lane).
>
> **Previous (2026-08-17, v0.84.0 on `develop` — the pack degradation gate,
> task #63):** authoritative Linux fulltest **1064 passed / 0 failed / 0 timed
> out / 9 skipped**; native **EXE 1026/0** and **OBJ 1026/0**; zero warnings
> under `-Werror`. Two new gates inside fulltest: `forest_pack_gate --selftest`
> **17 legs** (hermetic, both directions of every boundary) and
> `forest_bind_gate` **26/26** — up from 25/25 with the new `[ldouble]` case,
> which pins the `long double`-member-lost-at-bind fix against g++/clang.
> Suite counts are unchanged because the release adds gates and a bind case, not
> `.mad` tests.
>
> All three packs re-measured with the gate wired in, and every number is now a
> baseline in `docs/parity/pack-degradation-baseline.txt`:
> **linux** 93 pack parse errors, 0 mir-blob-skips, dk-none 55, closure-drops 0,
> 41 listed headers checked / 0 missing; **win64** 93, 0, 55, 0, 34/0;
> **darwin** 58 + 1 mir-blob-skip and 55/0 headers on EACH arch. The load-side
> hard-zero halves (`materialize fill: DROPPED`, `forest_restore_decls: SKIPPED`)
> are 0 on both profiles that can run a consumer. Trailer gate 438/0.
> Logs: `tmp/logs/rb-20260817-142741.log` (fulltest/exe/obj) and
> `tmp/logs/rb-20260817-142423.log` (release/release-win/release-macos).
>
> Known and now GATED rather than silent: both macOS packs ship with **no MIR
> cache blob** (linux packs 467 KB, win64 497 KB), baselined at
> `darwin mir-blob-skips 1`. Pre-existing — byte-identical before and after this
> release's code changes.
>
> **Previous (2026-08-17, v0.83.0 on `develop` — UFCS):** authoritative Linux
> fulltest **1064 passed / 0 failed / 0 timed out / 9 skipped**; native
> **EXE 1026/0** and **OBJ 1026/0**; zero warnings under `-Werror` on every
> lane. `ufcs_gate` green inside fulltest (12 C + 9 C++ `--std=` modes x 3
> probes, plus the one-owner check on the entry condition), and negative-
> controlled against the shipped pre-UFCS `madc-release-v0.82.0` binary.
> The ten new `tests/testufcs*.mad` reducers all carry g++ 13 and clang++ 18
> oracles. Windows, macOS and the packed/headerless lanes were last measured at
> v0.82.0 and are unchanged by UFCS, which is a front-end-only feature.
> Log: `tmp/logs/rb-20260817-0*.log`.
>
> **Previous (2026-08-16, `feature/win3-pe-coff-codex` W5-close content):**
> authoritative Linux fulltest **1050 passed / 0 failed / 0 timed out /
> 9 skipped**; emitted-code warning census **1059 compiles / 0 warnings**;
> libc++ JIT **1046/0/0TO/13skip**, EXE **1013/0**, OBJ **1013/0**.
> The exact rebuilt packed Win64 release passed `verify_pe_release.sh`, the
> persistent-Wine packed-product domain at **1008/0/0TO/51skip**, and the
> genuine-Windows stage-once domain at **1010/0/0TO/49skip**. Fulltest's
> post-suite forest bind gate is **24/24**, and every remaining structural,
> carrier, ABI, and packaging gate returned success. Logs:
> `tmp/logs/rb-20260816-005957.log` (fulltest) and
> `tmp/logs/rb-20260816-011436.log` (full libc++ battery).
>
> **Previous (2026-08-14, validated code head `63f008ad` — v0.80.0 POSIX
> target surface + the zero-warnings law):** authoritative Linux fulltest
> **1040 passed / 0 failed / 0 timed out / 9 skipped**, libc++ JIT
> **1036/0/0TO/13skip**, EXE **1009/0**, OBJ **1009/0**, and the
> persistent-Wine hosted Win64 domain **998/0/0TO/51skip**.
>
> **Zero compiler warnings on every build lane, now enforced.** Clean
> rebuilds of host `-O0`, release `-O2`, debug, `hosted-x86-64-windows`,
> `hosted-arm64-macos` and `hosted-x86-64-macos` each returned rc=0 with
> **0 warnings**, including 39 unit-test binaries, with `-Werror` active
> (escape hatch `WERROR=0`). The emitted-code ratchet
> (`scripts/warn_census.sh`) is back to an **all-zero baseline** — its last
> entry was stale, and because a ratchet only forbids increases it had been
> reporting GREEN over a goal already met, printing `tests improved : 1`
> every run.
>
> Two gates were added and negative-controlled in both directions:
> `check-cross-mode-compiles.sh` (the `cross-*` modes were in NO validation
> lane and had been uncompilable for two days across a release — the mode
> the macOS artifacts build through), and `-Werror` itself, whose presence
> on both the C++ and `rt/*.c` compile lines was verified with `make -Bn`
> because a plain `make -n` prints nothing for an up-to-date target.
>
> **Previous (2026-08-14, validated code head `3d5bd90c` — v0.79.0
> Win64 JIT milestone):** authoritative Linux fulltest **1033 passed /
> 0 failed / 0 timed out / 9 skipped**, libc++ JIT
> **1029/0/0TO/13skip**, EXE **1004/0**, and OBJ **1004/0**; every
> `remote_build.sh` stage returned 0. Log:
> `tmp/logs/rb-20260814-051011.log`. The hosted MinGW+UCRT executable
> built successfully and the complete persistent-Wine domain passed
> **987/0/0TO/55skip** (46 domain fixtures + 9 MIR fixtures), from the
> 46b handoff's **947/30/0TO/59skip**. A first run without the documented
> persistent wineserver produced one rotating
> `testderefpostincstore` failure; that specimen immediately passed its
> capped 1/1 rerun, and the full run with `wineserver -p` passed 987/0.
> The three scoped duplication families are gated at zero divergent
> implementations. The aggregate gate measures one definition owner / 7
> references, one owner-to-contract edge, one aggregate and one member
> contract builder, one c2mir aggregate and member consumer, and one
> emit-C pack reader; its negative control is rejected. Focused units:
> CIR freeze **36/36 cases, 761 assertions**; DataDef **85/85 cases, 555
> assertions, 9 skipped**. Rule trailers: **390 code commits, 0 missing**.
>
> **Current (2026-08-13,
> `feature/win64-46b-burndown-codex` @`0bc84193` — Win64 46b JIT
> burndown close):** Wine domain suite **981/0/0TO/57skip**, down from
> the handoff baseline **947/30/0TO/59skip**. The 57 audited skips are
> 25 libc++-flavor tests outside the Win64 libstdc++ lane, 20
> structural Win64/POSIX exclusions, 3 Wine-only environment
> exclusions, and 9 known MIR gaps. Wine 9.0's rotating failures were
> traced to `wine client error: recvmsg: Connection reset by peer`;
> keeping one `wineserver -p` instance alive produced the zero-failure
> full runs. Final-content deferred gates, each run once: Linux
> fulltest **1029/0/0TO/9skip**, libc++ JIT
> **1025/0/0TO/13skip**, both rc=0. Full log for those two gates:
> `tmp/logs/rb-20260813-211415.log`.
>
> **Current (2026-08-07, `feature/data-channel-flow-codex` @cd1f19c6 -
> session #70 close):** clean enabled remote fulltest
> **999/0/0TO/9skip**, total rc=0; all warning, forest, rule-trailer,
> SIGPIPE-owner, and close-on-exec-owner gates passed. Focused unit suites:
> data channels **23/23 cases, 143/143 assertions**; process **7/7,
> 95/95**; storage contract **25/25, 223/223**. A separate clean
> `--enable-madcdat=no` remote build passed the same focused suites and
> proved the dependency-free DSV/FLR/VLR and memory/file/FIFO/TCP/UDP/UDS
> core. GCC 13.3 and Clang 18 reducers both reported
> `tcp=ok uds=ok udp_trunc=ok` and `cloexec=ok`. Battery log:
> `tmp/logs/rb-20260807-205332.log`. Release follow-up at documentation HEAD
> @ff68c1d5: `make -C src release` rebuilt the stripped `-O2`
> `bin/madc-release`, appended a compressed frozen forest containing
> **242 units, 872066 records, and 276510 tokens**, and passed bind-cache
> parity. The full suite against that exact packed binary passed
> **999/0/0TO/9skip**; release rc=0, packed rc=0, total rc=0. Log:
> `tmp/logs/rb-20260807-225834.log`. The pulled local release binary is
> byte-identical to the tested remote artifact (SHA-256
> `8e2b29990effcc06c38afe394d58a248a43491079838a1f96ef74c29ce176c0f`).
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @429842b4 —
> session #66 close):** fulltest **997/0/9skip**; lane
> **993/0/13skip**; session-end native legs EXE **976/0**, OBJ
> **976/0** (of 997 JIT-passing tests). Battery log:
> `tmp/logs/rb-20260806-194726.log` (fulltest, libcxx jit, exe, obj
> all rc=0). +13 fulltest tests over session #65: the multi-return
> struct-transport gates (testmultiret double/ptr/string/hetero +
> reject/exprpos/bare — class values, Go-style heterogeneous
> `(int, string) f()` signatures, loud rejects; @a369cb17), the
> zero-ceremony gates (testautoincludecpp/ns, testpreferdefault,
> testautoceremonystd, testnsheaderfirst; @1fafe265 @0bc6ce07
> @715fbadb), and testsizeofvaluepack (@b411715c). Also @429842b4:
> the enum-constant parse-time slot heap overflow fixed
> (Variable::slot_size owns the 64-bit ddINT slot contract;
> valgrind-verified). Doc-example harness: **53/53** fenced examples
> green at this HEAD. Ships as **v0.68.0**.
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @520e77d6 —
> session #65 close: 🏁 LANE ZERO):** fulltest **984/0**; lane
> **980/0/13skip** — the `-stdlib=libc++` flavored lane's failing set
> is **EMPTY** for the first time: full behavior-parity with the
> default libstdc++ lane. testtuple (the #110 pack wall's last
> standing test) FIXED; the other +4 passes are the four new session
> gates (testctortemplatetrait, testusingfnoverload,
> testexternblockbody, testctorttpdefault). Four fixes: (30)
> `trait_class_constructible` no longer refuses same-class
> constructibility when the ctor set contains a TEMPLATE (the -1
> refusal laundered through a failed static-const capture into a
> silent 0 — `is_move_constructible<allocator<T>>` folded false);
> (31) a using-declared function JOINS the target namespace's
> overload set ([namespace.udecl] — `std::swap(int,int)` with
> `<memory>` bound the exception_ptr overload); (32) extern
> linkage-block context no longer leaks into function bodies
> ([dcl.link]p7 — "__tmp in block scope with external linkage");
> (33) THE WALL: a template-template parameter defaulted to a
> DIFFERENT named template binds as a template NAME ([temp.param]p11
> — libc++ tuple()'s `_IsDefault = is_default_constructible` idiom
> captured as a never-foldable non-type default, so the ctor never
> instantiated and construction called the never-defined
> placeholder). The 13 lane skips = the 9 baseline `.mir_skip` + the
> 4 documented `.libcxx_skip`. `docs/parity/libcxx-failset.txt`
> records the ZERO and becomes the P2.7 gate per its charter.
> Battery log: `tmp/logs/rb-20260806-145634.log` (fulltest rc=0,
> libcxx jit rc=0, total rc=0). Session-end native legs GREEN: EXE
> **967/0**, OBJ **967/0** (of 984 JIT-passing tests;
> `tmp/logs/rb-20260806-151939.log`, total rc=0).
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @01e0e7d7 —
> session #64 close):** fulltest **980/0**; lane **975/1/13skip**
> (+5 gates: testaggrdecl, teststructbraceexpr, testint128global,
> testemptystructret, testcomplexretconv, testcastcallpostfix — the
> last five landed after the 970/1 checkpoint). The only remaining lane
> failure is **testtuple** (#110 pack wall). Six fixes: (24) DECL-lane
> braced aggregate init of object-member aggregates; (25) `P{7, 3.5}`
> braced functional construction of plain structs parses
> (parse_compound_struct_lit — one brace reader for `(T){...}` and
> `T{...}`); (26) const `__int128` file-scope initializers (fork:
> gen_initializer int128 data arm — was the pack-freeze SEGV);
> (27) empty-struct call results reserve a real call-arg slot (fork:
> "undeclared func reg fp" at pack-thaw); (28) `_Complex` return-value
> conversion (fork: `return 3.0;` loaded components from absolute
> address 0); (29) cast operands continue the postfix chain
> (`(int)getb().n`). Follow-ons recorded: swap<allocator> return-type
> mistyping (tsubst), duration<double>::operator%= drain
> instantiation (pack gate is check-only), dependent-decltype
> pattern-freeze. Session-end native legs GREEN: EXE **963/0** and OBJ
> **963/0** (of 980 JIT-passing tests).
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @8f8f4009 —
> DECL-lane braced aggregate init):** fulltest **975/0**; lane
> **970/1/13skip** (+1 gate: testaggrdecl). The only remaining lane
> failure is **testtuple** (#110 pack wall). Residual (a) resolved:
> braced aggregate init of OBJECT-member aggregates in the DECLARATION
> lanes — var_decl's C INIT list bit-copied the class member and ordered
> the materialized temp's decl after the SPEC_DECL ("undeclared
> identifier __madc_objtmp_0"), and the decl copy-elision arm
> `S v = S{a, b}` silently DROPPED the full list (garbage, exit 0).
> Storage stays bare (braced_aggregate_needs_construction); the three
> FULL-list declaration sites claim via decl_aggregate_claim →
> class_aggregate_init; multi-element aggregate-shaped declines fail
> loud.
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @e658a5b8 —
> testfreezerun FLIPPED):** fulltest **974/0**; lane **969/1/13skip**
> (+1 gate: testaggrinit). The only remaining lane failure is
> **testtuple** (#110 pack wall). Four fixes: the flavor-runtime dlopen
> moved into `cir_translate_guarded` (the freeze lane's CIR-time dlsym
> probes shaped a different tree — facet-id externs unrecorded); frozen
> containers carry the flavor `link_libs` and the thaw reopens them (16
> trap-bound imports → 0); nested-class ctor/dtor no longer false-match
> the owner's out-of-line defs (sentry's Itanium bind restored); and
> aggregate list-init of ctor-less classes stops DROPPING initializers
> (class_aggregate_init, [dcl.init.aggr] — was silent garbage in the
> PLAIN lane too, S{string,42} printed junk with exit 0).
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @588d9e73 —
> one-key fix for typedef'd anon-aggregate template args):** fulltest
> **973/0**; lane **967/2/13skip** (+1 gate: testanontypedefspec). The
> fix removed a SILENT wrong value in plain JIT (explicit spec invisible
> behind a typedef'd anon-struct key — 0 for 7, exit 0) and pushed the
> testfreezerun libc++ frontier from the ClassPattern-base error to
> thaw-time static-member facet imports (num_put/ctype `id`). Remaining
> 2: testfreezerun, testtuple.
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @4e1a4004 —
> testsysobject FLIPPED):** fulltest **970/0**; lane **966/2/13skip**
> (+2 gates: testfriendnonmember, testfreeoptemplate). Two fixes: a
> class-body FRIEND template never registers as a MEMBER ([class.friend] —
> libc++ string:1762's `bool friend operator==` poisoned
> `method_map["operator=="]` with a basic_string return, so
> `string == "lit"` typed as basic_string and `cout <<` bound the string
> inserter over a bool rvalue), and GLOBAL-scope free operator templates
> bind (retained-body key walk dropped the exact `"::operatorX"` key,
> `<=` vs the sibling walk's `<`; plain C struct operands now engage the
> lowering via operand_value_datadef + DataDefSTRUCT). Remaining 2:
> testfreezerun, testtuple.
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @022cbb3b —
> testmathheader FLIPPED):** fulltest **968/0**; lane **963/3/13skip**
> (+2 gates: testcastmembertype, teststaticoverload). Two fixes:
> qualified member-TYPE casts (`(typename __promote<T>::type)x` — the
> __math::isinf undefined-import root) and non-template static overload
> ranking by argument types + [expr]/5 argument reference-collapse (the
> silent `__promote<double>::type == long double` wrong value). Residual
> filed: dependent-decltype pattern-freeze (tmp/r58b.mad). Remaining 3:
> testfreezerun, testsysobject, testtuple.
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @bd6fed08 —
> testifconstexpr FLIPPED; `<format>` chain THROUGH):** fulltest **968/0**;
> lane **960/4/13skip** (+5 gates: testnsdmineg, testenumqualcase,
> testinlinenstype, teststaticbraceinit, testvartemplatefold). Five fixes:
> NSDMI isolated-parse context reset, ns-qualified scoped-enum case labels,
> inline-namespace type descent, static brace-or-equal-init brace form
> (the flip), ns-qualified variable-template constant fold (transactional
> `fold_constant_qualified_member` + `inline_namespace_descendants`
> consolidation). testinvocable reclassified `.libcxx_skip` (clang++
> -stdlib=libc++ rejects its libstdc++-internal `__is_invocable` source).
> Remaining 4: testfreezerun, testmathheader, testsysobject, testtuple.
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @1de7b430 —
> fixes 8-11; parser_std_format_spec.h open to :339):** fulltest **963/0**;
> lane **954/6** byte-identical failset (+4 gates: testparenctor,
> testanonbitfield, testenumsize, testtraitcopyable); enum fixed bases now
> drive layout ([dcl.enum]p8, freeze-carried); EXE **938/0**, OBJ **938/0**
> (session-end legs, of 954 JIT-passing).
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @aaee9009 —
> seven front-end fixes; unicode.h through):** fulltest **959/0** (rc=0,
> forest oracles green). Seven oracle-verified fixes advanced
> testifconstexpr's chain five links (ranges_construct_at.h:94 →
> buffer.h:62 → unicode.h:51/:70/:302 → parser_std_format_spec.h:58):
> nested-name-specifier head vs the auto fn-ptr shortcut, concept-headed
> template params, braced NSDMI, enum trailing declarator, bit-field
> brace-init skip, u/U/u8 literal prefixes + UCNs, and the scoped-enum
> pseudo-namespace bridge for using-declarations. The flavored
> measurement is **950 passed / 6 failed / 0 timed out / 12 skipped**
> (byte-identical failing set at every batch checkpoint; +6 = the new
> gates testnsfncollide, testconceptparam, testbracensdmi, testenumdecl,
> testbitfieldinit, testcharlit, testusingenum), eligible EXE **934/0**,
> OBJ **934/0**. NEW TEST PROTOCOL (owner): per fix targeted globs + one
> frontier test; per batch fulltest + `libcxxjit`; EXE/OBJ legs at
> session end.
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @179d1ab0 —
> C++20 abbreviated function templates, member form):** fulltest green
> (rc=0) and default EXE leg green. [dcl.fct]/18 lands as a token-level
> desugar: `auto` parameter placeholders become invented identifiers
> under a synthesized `template<...>` head, so the member-template
> capture + tsubst own the rest. libc++'s `dangling(auto&&...)`
> (testifconstexpr's first blocker) is THROUGH; the chain moved to
> ranges_construct_at.h:94, so zero flips: the whole flavored
> measurement is **944 passed / 6 failed / 0 timed out / 12 skipped**
> (byte-identical failing set; +2 = gates testbarestring +
> testabbrevtpl), eligible EXE **928/0**, OBJ **928/0**. New gate
> `testabbrevtpl` (pack ctor + bodied auto ctor, both oracles, both
> flavors).
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @bb435bfd —
> namespace-scope using-aliases flat-register when free):** fulltest
> **950 passed, 0 failed, 0 timed out, 9 skipped** (rc=0) and default EXE
> leg green (freeze/forest gates included). The dialect's unqualified
> visibility for namespace-scope type names is a flat `datatype_map`
> write that only the TYPEDEF lane performed; the USING-ALIAS lane was
> cut from the flat map after `std::pmr::string`'s alias clobbered the
> real `string`. libc++ spells `std::string` as a using-alias
> (`__fwd/string.h`) where libstdc++ uses a typedef, so bare `string`
> was unresolvable in every declaration context only under
> `-stdlib=libc++`. The alias arm now flat-registers only when the name
> is FREE (primary wins; pmr stays namespace-only). testexterncstringptr
> and testforeachheaderbody flip: the whole flavored measurement is
> **942 passed / 6 failed / 0 timed out / 12 skipped**, eligible EXE
> **926/0**, OBJ **926/0**, zero newly broken (two-way name diff). New
> gate `testbarestring` (file-scope var + fn decl/def + block local,
> both flavors).
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @075c7f81 —
> system-header global C++ overloads register distinctly):** fulltest
> **950 passed, 0 failed, 0 timed out, 9 skipped** (rc=0) and default EXE
> leg green. libc++'s `stdlib.h` declares five inline C++ `abs` overloads
> at GLOBAL scope after glibc's extern-C `int abs(int)`; plain globals
> were excluded from the tracked-overload arm, so all five spliced into
> one shared-id FuncDef and the last body (long double, `fabsl`) emitted
> as a plain-named linkonce `abs` clobbering the libc import — `abs(-7)`
> silently returned 0 under `-stdlib=libc++` (testincludenext "42 0" vs
> oracle "42 7"). A system-header plain global C++ function whose name is
> already taken now joins the per-overload model; first/solo declarations
> keep the source name (dlsym imports intact). testincludenext flips: the
> whole flavored measurement is **939 passed / 8 failed / 0 timed out /
> 12 skipped**, eligible EXE **923/0**, OBJ **923/0**, zero newly broken
> (two-way name diff). New gate `testglobaloverload` (abs/labs values
> against both oracles, validated in default JIT + libc++ JIT + EXE).
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @01d774fe —
> transitive secondary vtable groups):** fulltest **950 passed, 0 failed,
> 0 timed out, 9 skipped** (rc=0, all forest gates green). Itanium gives
> every polymorphic base subobject off the primary chain its own vtable
> group + ctor vptr stamp — TRANSITIVELY; madc collected only direct
> non-primary bases, so `E : D` (D : A, B) left the B-subobject on B's
> standalone vtable with wrong vbase-offset slots. Under libc++ that was
> stringstream (`basic_ostream` = `basic_iostream`'s second base): the
> virtual `basic_ios` resolved at +24 vs clang's +128 through any
> `basic_ostream` view — real libc++ code and emitted bodies read an
> uninitialized `basic_ios`, every insert silently lost, `<< 42` SIGSEGV
> in the locale copy ctor. testsstream and testopinherit flip: the whole
> flavored measurement is **938 passed / 9 failed / 0 timed out /
> 12 skipped**, eligible EXE **922/0**, zero newly broken (two-way name
> diff). New gate `testtranssecondary` (plain depth-2 + template stream
> shapes, both oracles, both flavors).
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @41cbb2c5 — the
> bucket-A chain):** fulltest **949 passed, 0 failed, 0 timed out, 9 skipped**
> (rc=0, all forest gates green). Session #58 bucketed the 15 remaining
> flavored failures by first error and cleared the largest bucket in five
> commits: class-typed `return {...}` selects a constructor (the bare `{`
> unbalanced the scope stack — libc++ `proximate()` lost its parameters);
> conversion-type-ids take cv-qualifiers and reference conversions route
> through `returnDecl` (six copy-pasted cv-skip loops consolidated into
> `Program::skip_cv_qualifier_tokens`); `friend` may follow other
> declaration-specifiers (one friend-decl owner, both entry arms); using-alias
> targets take east-cv suffixes via `consume_declarator_stars`; and the
> SILENT-WRONG headline — the free-operator body deduction lacked the
> derived-to-base receiver walk, so `ofstream << "text"` bound the member
> `operator<<(const void*)` and wrote pointer values into files. A sixth
> commit restored the identity-return pattern recording @7b63f8c6 had
> accidentally severed.
>
> The whole flavored measurement is **935 passed / 11 failed / 0 timed out /
> 12 skipped**: testdefer, testfstream, testloop, testmanipview FIXED with
> zero newly broken (two-way comm-diff against the 15-name set); eligible EXE
> and OBJ each **919/0**. New gate `testofstreamwrite`; extended gates
> `testbracedreturn`, `testconvopclass`, `testfriendkeyword`,
> `testaliasptrtarget` — all match g++ AND clang++ in both stdlib flavors.
> Next: `libcxx_stringstream_construction_state` (testsstream + testopinherit
> share the locale-copy-ctor SIGSEGV; minimal reducer tmp/r19.cpp).
>
> **Previous (2026-08-04, `feature/libcxx-parity7-claude` @7b63f8c6 — the
> noexcept operator):** fulltest **948 passed, 0 failed, 0 timed out,
> 9 skipped** (rc=0, all forest gates green). The `[expr.unary.noexcept]`
> operator is implemented: `noexcept` is a reserved C++11 keyword (the lexer
> erasure destroyed the operator — an expression-context `noexcept(e)`
> SIGSEGV'd and a template-argument `BC<noexcept(e)>` lost the argument);
> `evaluate_noexcept_operator` folds the noexcept-spec conjunction over the
> unevaluated operand's parsed tree, instantiating conditional-spec callees on
> demand ([temp.inst]/14 — caught by `forest_selfexe_gate` when the refusal
> dropped `_S_nothrow_relocate`'s body). Registration placeholders capture
> declaration exception specs; a qualified template-id pack expansion
> (`std::declval<_Args>()...`) is one unit including its qualifier chain.
> New gates: `testnoexceptop`, `testqualpackelide`.
>
> The whole flavored measurement is **930 passed / 15 failed / 0 timed out /
> 12 skipped**: `testconstructible` FIXED with zero additions (two-way
> comm-diff), eligible EXE and OBJ each **914/0**. madc-as-GCC compiles
> libc++'s non-builtin nothrow-trait arm (`_LIBCPP_COMPILER_GCC` at
> `__config:38`), whose `integral_constant` base is exactly the noexcept
> operator over a ctor call — the whole family escaped as silent 0s before.
> The recorded DataDefREF `T&`/`T&&` gap was NOT this test's cause and stays
> open only for the ungated `is_nothrow_move_constructible<std::string>`.
>
> **Previous (2026-08-04, `feature/libcxx-parity6-codex` @672a0966 —
> forwarding-reference deduction owners):** fulltest **946 passed, 0 failed,
> 0 timed out, 9 skipped**, warning census **0**, tsubst fallback **0**;
> `forest_index_oracle` is **5227 indexed names / 3521 registered lookups**.
> `testfwdpackvaluecategory` proves a named lvalue and a value-returning call
> with the same value type deduce `T=int&` and `T=int`: GCC, Clang, and madc
> print `1 0`. Its focused ten-test blast radius passes **10/0** in JIT, EXE,
> and OBJ. `testmembertmplctor` remains `10 400`, proving dependent `sizeof(U)`
> measures the referent when forwarding deduction binds `U=tag&`. The function-
> template and dependent-type-query ownership gates are green alongside the
> existing reference-argument gate.
>
> Real libc++ `testcontainerdtor` now completes in production: vector size 4,
> integer vector size 3, set size 2, map size 2, then `done`; the experimental
> `MADC_FWDREF_ARM` is deleted. The whole flavored measurement is **927 passed /
> 16 failed / 0 timed out / 12 skipped**. Eight prior failures cleared with zero
> additions: `testcastarrow`, `testcontainerdtor`, `testforinitscope`,
> `testmadc_ns`, `testmap`, `testmapiter`, `teststdmapint`, and `testsubscript`.
> Eligible EXE and OBJ are each **911/0**. Logs:
> `tmp/logs/rb-20260804-193248.log` (fulltest),
> `tmp/logs/rb-20260804-194241.log` (whole libc++ battery), and
> `tmp/logs/rb-20260804-193158.log` (focused JIT/EXE/OBJ).
>
> **Previous (2026-08-03, `feature/libcxx-parity6-claude` @ba7517b4 —
> pack/variadic correctness, unreleased):** fulltest **922 passed, 0
> failed, 0 timed out, 9 skipped**, unittest rc=0, `--exe` **875/0** and
> `--obj` **875/0** (of the 891 JIT-passing). All gates green (delimiter
> ratchet 0, rule-trailer gate 207/0 since epoch, tsubst fallback 0,
> warning ratchet 0). Three new gates: `testvariadicmember`,
> `testbasepacktwo`, `testsizeofpack` — each carries at least TWO pack
> elements with DIFFERENT values, because at arity 1 splice and
> replicate are indistinguishable and three real defects shipped green
> behind arity-1 gates.
>
> The flavored parity lane was unchanged at 891 passed / 28 failed / 0
> timed out / 12 skipped** (`run_tests.sh --stdlib=libc++`, measured at
> @ba7517b4; the failing set is comm-diffed BOTH WAYS against
> `docs/parity/libcxx-failset.txt` — 28 vs 28, no new, no fixed). All
> three commits this session are default-lane correctness; none of them
> moved the lane. The 28 are now bucketed into named roots (see
> `claude_status.json`): `__tree` tsubst (5), the retbuf-ABI predicate
> disagreement pinned to `cir_builder.cpp:5074` (best next target),
> free functions not overloading, C++20 abbreviated templates (2),
> `basic_string_view(__long**)` (3, untriaged), the
> `filesystem/operations.h:240` group (4, mechanism unconfirmed), and
> ~9 singles including a SIGSEGV and a silent wrong answer.

> **Previous (2026-08-01, v0.67.0 — the flavor-ABI release, pre-merge
> battery on `feature/libcxx-parity5-claude` @190ff9d2 + release
> files):** fulltest **911 passed, 0 failed, 0 timed out, 9 skipped**,
> `--exe` **894/0**, `--obj` **894/0**, and the packed release arbiter
> **911/0/0/9**. All gates green (delimiter ratchet 0, rule-trailer
> gate 180/0 since epoch, tsubst ratchet 0, retire-std-hardcoding,
> forest self-exe). The flavored parity lane stands at
> **880 passed / 26 failed / 2 timed out** (`run_tests.sh
> --stdlib=libc++`, measured @e09c5381; the failing set is banked and
> comm-diffed in `docs/parity/libcxx-failset.txt` — 21 net flips over
> v0.66.0, zero regressions at every measured step). Nine new gates:
> `testarrayparam`, `testcinstr_libcxx`, `testclassproto`,
> `testconstaccess`, `testconstovl`, `testdeductionguide`,
> `testpacktypedef`, `testprivmethod`, `testtypedefarg`. Fork
> unchanged (**1.0-madc.0.63.0** @8f3934ac). Battery logs:
> `tmp/logs/rb-20260801-231615.log` + `rb-20260801-234210.log`.

> **Previous (2026-07-30, v0.63.0 — the libc++ parity-lane burn-down,
> pre-merge battery on `feature/libcxx-string2-claude` @c4a98c9c):**
> fulltest **856 passed, 0 failed, 0 timed out, 9 skipped**, `--exe`
> **840/0**, `--obj` **840/0**, and the packed release arbiter
> **856/0/0/9**. All gates green (libcxx_gate incl. the operator+ leg,
> delimiter ratchet 0, rule-trailer gate clean, tsubst ratchet 0).
> The flavored parity lane stands at **747 passed / 108 failed**
> (`run_tests.sh --stdlib=libc++`; the failing set is banked and
> set-diffed in `docs/parity/libcxx-failset.txt` — zero regressions at
> every measured step from the 534/282 baseline). Fork at
> **1.0-madc.0.63.0** @8f3934ac (zero-length-array diagnostic parity).
> Battery log: `tmp/logs/rb-20260730-192345.log`.

> **Previous (2026-07-28, `develop` — v0.57.0, the libc++ burn-down: eight
> core-C++ defects in one chain):** fulltest **784 passed, 0 failed,
> 0 timed out, 9 skipped**, `--exe` **768/0**, `--obj` **768/0**, and the
> packed release arbiter **784/0/0/9**. All gates green: `libcxx_gate` OK
> (two new legs: `<cwchar>` compiles AND runs; the CRTP base-arg shape is
> bounded on the forced-legacy lane and `<string>` terminates loudly, never
> with a signal), delimiter ratchet at 0, rule-trailer gate clean, tsubst
> ratchet 0. Fork unchanged (**1.0-madc.0.52.0** @ba216dea).
> Eight new gates this release, each byte-identical across g++ 13 and
> clang++-18: `testinlinensopen`, `testusingaliasfnptr`,
> `testnestedinlinens`, `testcrtpbasearg`, `testnestedtagctor`,
> `testbracedctor`, `testunderlyingtype`, `testdeclonlyspec`.
> The `-stdlib=libc++` parse frontier now stops at ONE recorded defect for
> both `<string_view>` and `<string>`:
> `Gap{common_type_dependent_member_key_explosion}`.
> Battery log: `tmp/logs/rb-20260728-162624.log`.

> **Previous (2026-07-28, `feature/class-static-alias-claude` — eval scope
> capture + the instantiation-product lookup surface):** fulltest **770 passed,
> 0 failed, 0 timed out, 9 skipped**, `--exe` **754/0**, `--obj` **754/0**, and
> the packed release arbiter **770/0/0/9**. All gates green: `libcxx_gate` OK,
> `forest_index_oracle` OK (5180 indexed names cover 3487 registered lookups,
> 40 allowlisted), delimiter ratchet at 0, rule-trailer gate clean.
> Fork unchanged (**1.0-madc.0.52.0** @ba216dea).
> New gate: `testevalexterncapture` (+1 baseline), plus `.expect_quiet` added
> to the four `testmadceval*` tests — they had none, so a flood of
> "undeclared identifier" diagnostics on stderr passed on stdout alone.
> Worth keeping from the run: **`fulltest` went rc=2 with every one of the 770
> tests passing in all four lanes.** The failure was `forest_index_oracle`, and
> the only reason it was actionable is that the driver now prints a per-stage rc
> summary — the previous battery reported a bare `total rc=1` through a `tail`
> pipe that had discarded the stage, and the whole ~30-minute run was wasted.
> The tooling fix (self-logging, stage summary, `TESTS=` subset runs) shipped
> with the same batch; targeted runs are the inner loop now, full suite is the
> pre-merge gate.

> **Previous (2026-07-27, `develop` — v0.54.0, six C++ correctness fixes,
> four of them silent wrong answers):** fulltest **769 passed, 0 failed,
> 0 timed out, 9 skipped**, `--exe` **753/0**, `--obj` **753/0**, and the
> packed release arbiter **769/0/0/9**. All gates green: the delimiter
> ratchet at 0, the rule-trailer gate clean, `libcxx_gate` OK.
> Fork unchanged (**1.0-madc.0.52.0** @ba216dea).
> Four new gates this release: `testqualifiedpostfix`,
> `testclassqualifiedcall`, `teststaticmemberstorage`,
> `testnestedtypescope` — each byte-identical across g++ 13, clang++-18
> and madc, each with empty stderr.
> Worth keeping from the run: **two of the three batteries on the
> static-member fix went RED**, each naming a different class of
> static-member storage symbol that nothing in the translation unit
> defines, and **45 green reducers said nothing about either** — a user
> class's static always has its definition in the same file, so that
> shape is unreachable from a reducer. The suite found what the reducers
> structurally could not, which is the argument for running it rather
> than trusting a reducer sweep.

> **Previous (2026-07-26, `develop` — v0.52.0, Mach-O axis B step 4:
> the darwin `.o` lane is real — axis B is DONE):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped**, `--exe`
> **740/0**, and `--obj` **740/0** — the ELF `.o` lane run explicitly,
> because `MIR_object_read` was refactored onto a format-neutral input
> view so the fork keeps ONE merge implementation behind two container
> fronts. Unit tests unchanged (`test_object_load` among them, exercising
> the refactored reader). Packed release arbiter **756/0/0/9** (`make
> release`: 240 units + the ledger, 2 modules / 22 symbols / 3175 bytes) —
> the fork's ELF merge path rides libmir into the release binary too.
> NEW gate `scripts/macho_obj_gate.sh` / `make -C src machogate`:
> **30 assertions, 15 per arch (arm64 + x86_64)**, over TWO INDEPENDENT
> AUTHORITIES. (a) `ld64.lld-18` + the macOS SDK: Apple's own linker
> links our `MH_OBJECT`, including a MIXED link where a clang-18 TU calls
> into the madc-compiled one, and the relocations it applies land where
> they must — pool slots inside `__text` and `__bss`, the import slot as a
> real dyld bind, a global constructor's entry kept in
> `__mod_init_func`. (b) madc's own read-back: `-c` then link
> disassembles IDENTICALLY to the direct `-o` emit, pool contents
> included (a full-file `cmp` differs only in the code-signature
> identifier, which is the output basename). The gate is NOT in fulltest —
> its cross-madc / llvm-18 / SDK prerequisites would make it silently
> skip there — so the make target rebuilds both cross madcs first and it
> can never validate a stale binary.
> That equivalence leg is the one that earned its keep: it caught a real
> read-back bug every structural check passed over. Mach-O has ONE
> `ARM64_RELOC_PAGEOFF12` where ELF has `LDST64_LO12` and `ADD_LO12`, so
> the reader recovers the kind from the instruction's opcode — and the
> first mask dropped bit 31 (`sf`), reading every `add Xd, Xn, #imm12`
> back as a scaled load: immediate `#0x1` where the direct emit had
> `#0x8`. Structurally valid, silently wrong arithmetic. Lesson kept in
> the plan doc: for a format round trip, assert EQUIVALENCE against the
> path that does not round-trip, not just "the linker accepted it".
> Gate-craft trap found the same way: `llvm-otool -s` dumps section bytes
> as 4-byte WORDS on arm64 and single BYTES on x86-64, so a byte-only
> parser silently finds zero slots and fails on one arch only.
> Still the owner's Mac: RUNNING any emitted Mach-O binary (every darwin
> slice's RUN leg).
> Fork release **1.0-madc.0.52.0** (@ba216dea).

> **Previous (2026-07-26, `develop` — v0.51.0, forest-carriers S6
> complete — the carriers track is DONE):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** with every
> forest gate green, including the NEW `forest_config_gate` — the
> `madc.ini` promise: **39 checks over 18 legs**, and every settings leg is
> PAIRED with a baseline that would fail without the config file. The
> dialect baseline is the ABSENCE of `__STDC_VERSION__` (so `std = c99` →
> `199901L` cannot pass by accident), the include fixture is unreachable
> without the ini, and `mem-limit = 24` trips the address-space guard with
> the message NAMING the ini value — then `MADC_MEM_LIMIT=4096` overrides
> it, which is the precedence rule proved rather than asserted. The
> `forest` key is discovery **arm 5** and gets the S3 ordering treatment:
> a valid `$MADC_FOREST` binds with NO not-a-container notice (proving arm
> 5 was never probed), while the same junk ini path with an empty
> environment IS reached and IS loud. Strictness has its own legs: unknown
> key (naming file:line + the accepted set), foreign section, missing
> `=`, non-numeric limit, and a named `--config=` that does not exist all
> refuse nonzero.
> Unit tests **+4 cases** (`test_config_file`: 19 cases / 86 assertions),
> including a **schema-blind reader reuse** suite that drives the reader as
> `"madcdat"` with madcdat's own keys — the only test that proves the
> reader is reusable rather than merely generic-shaped, and the guard that
> fails if anyone re-welds madc's schema into it.
> Suite hermeticity: `run_tests.sh` now passes `--no-config` on EVERY madc
> invocation (including the AOT compile legs, which take no
> `$BACKEND_FLAG`), and both pack scripts do too — an ambient `madc.ini`
> would otherwise change the frozen corpus's producer config and send every
> ordinary compile through the dialect gate.
> `--exe` lane **740/0** and packed arbiter **756/0/0/9** (measured at the
> feature commit `3edccef2`; the layering re-cut after it touches no
> codegen, emit path, forest format or pack script, and was covered by a
> grouped fulltest — test scoping by blast radius, owner directive
> 2026-07-26).
> Configure-axis evidence, which no gate can produce because a gate cannot
> reconfigure the tree: `--disable-config-file` → `ENABLE_CONFIG_FILE=0` in
> config.mk → the define drops and the axis stamp flips; an ambient
> `./madc.ini` is not read; `--config=` refuses naming
> `enable-config-file`; `--no-config` stays a no-op; bare `./configure`
> restores. That exercise is what caught the missing `config.mk.in`
> substitution — without it `--disable-config-file` would have been a
> SILENT no-op.
> Also fixed: the installed `madcdis/snapshot.h` did not compile downstream
> (it names `PchCompression` in public signatures but `madc_pch.h` was
> never installed) — proven both ways by staging an install and compiling a
> TU that includes only `<madcdis/snapshot.h>`, then reproducing the
> original `fatal error` with the header removed.
> Fork unchanged (`1.0-madc.0.47.0` @74e705e4).

> **Earlier (2026-07-26, `develop` — v0.50.0, forest-carriers S5
> complete):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** with every
> forest gate green, including the NEW `forest_ledger_gate` — the
> `-static-libmadc` promise: 14 checks over a container the gate freezes
> itself with `--freeze-ledger=` (the same call the release pack makes).
> A **baseline leg per program** proves the program is genuinely
> runtime-needing (it keeps `libmadc.so.0` WITHOUT the flag), so the
> `-static-libmadc` legs cannot pass vacuously; then try/catch and VLA
> each emit with **no madc library and no `__madc_*` imports**, output
> byte-identical to the JIT run, and the try/catch binary runs under an
> **empty library path**. Failure surfaces are separated: a Tier-B (C++
> script-lane) program refuses NAMING its symbols, while a carrier with
> no ledger gets the BUILD-side message and never blames Tier B; `-c`
> and the `.o` link lane refuse at their own layers.
> **PRODUCT path** (the real one, no `--forest-bind`): `make release`
> packs `bin/madc-release` with 240 units **plus the ledger** (2 modules,
> 22 symbols), that packed binary reports it via `--dump-forest`, emits a
> try/catch program with **0 `libmadc` DT_NEEDED entries**, and the
> program runs correctly under `env -i LD_LIBRARY_PATH=/nonexistent`.
> Packed arbiter **756/0/0/9**; `--exe` lane **740/0**. Unit tests +1
> (`test_forest_policy`: the ledger carrier probe is silent and
> policy-free on an empty chain). Fork unchanged (`1.0-madc.0.47.0`
> @74e705e4).

> **Previous (2026-07-26, `develop` — v0.49.0, forest-carriers S4
> complete):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** with every
> forest gate green, including the NEW `forest_library_gate` — the
> shared shape's carrier: nine legs over a staged `bin/` + `lib/`
> install (thin-CLI live parity; **library-image** bind with `-v` arm
> naming + byte parity vs `--no-forest-bind`; arm order — the library
> image beats a present `<exe>.forest` AND a junk `MADC_FOREST`;
> `<lib>.forest` bind; and the embedding-host legs with no CLI knob:
> strict+sandboxed binding THROUGH the library image, the
> `enable_external_forest=false` refusal the S3 slice owed (same env,
> knob flipped, opposite outcome), strict-on-empty, silent library
> default). Thin-CLI parity suite (`MADC_BIN=bin/madc-thin`)
> **756/0/0/9** — the shared-linked CLI behaves exactly like the
> monolithic one. PRODUCT `--enable-shared` shape: `make release`
> packs `lib/libmadc.so` (**240 units**), packed arbiter
> **756/0/0/9** through the library carrier, and the installed tree
> (133 KB `usr/bin/madc` + 11.5 MB `usr/lib/libmadc.so.0`) binds 240
> units via `[library-image]`. The DEFAULT monolithic product shape was
> re-run too (`forest_pack.sh` was refactored this slice): release packs
> `bin/madc-release` (240 units), packed arbiter **756/0/0/9**.
> `--exe` lane **740/0**. Unit tests +1
> (`test_forest_policy`: monolithic image identity). Fork unchanged
> (`1.0-madc.0.47.0` @74e705e4).

> **Earlier (2026-07-25, `develop` — v0.48.0, forest-carriers S3
> complete):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** with every
> forest gate green, including the NEW `forest_sidecar_gate` — the
> carrier discovery chain: sidecar (`<exe>.forest`) and `$MADC_FOREST`
> arms bind with `-v` engagement evidence + byte parity vs
> `--no-forest-bind` live parse, arm order pinned (sidecar before
> env), junk-sidecar and explicit-miss failure surfaces LOUD. The
> `forest_emitpack_gate` Mach-O legs are now rev-skew-immune (each leg
> freezes with the same cross madc that emits/dumps). Packed arbiter
> through **BOTH carriers**: embedded **756/0/0/9** and sidecar
> (`forest_pack.sh --sidecar`, 240 units in `bin/madc-release.forest`)
> **756/0/0/9** + smokes (sidecar bind, loud-on-missing,
> quiet-on-config-mismatch). `--exe` lane **740/0**. Unit tests +6
> (`test_forest_policy`: policy triad, one-shot notice,
> config-mismatch matrix). Mac hardware (`~/s3side`): **7/7 legs per
> arch** (A64 native + X64-Rosetta) — embedded self-dump/bind/run
> regression, sidecar bind + parity, loud-on-missing,
> quiet-on-mismatch; AMFI accepted all four binaries. Fork unchanged
> (`1.0-madc.0.47.0` @74e705e4).

> **Previous (2026-07-25, `develop` — v0.47.0, forest-carriers S2
> complete):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** with every
> forest gate green, including the NEW `forest_emitpack_gate` — the
> `--pack-forest` carrier: ELF leg RUNS the packed emitted executable
> (rc + output) and pins `--dump-forest` byte-parity container vs
> packed image plus both refusal arms; Mach-O legs (both arches, cross
> madcs) pin the same parity through the writer-laid `__MADC,__forest`
> section. Packed arbiter **756/0/0/9** through `bin/madc-release` +
> `forest_pack: OK (240 units; bind cache == no-cache)`. `--exe` lane
> **740/0**. Full battery run twice (pre- and post- macro-collision
> fix), green both times. Mac hardware (`~/s2pack`): packed emitted
> binaries carrying the real 30-unit darwin groves ran `emitpack ok`
> rc=42 under AMFI (A64 native + X64-Rosetta); hosted `--dump-forest`
> over packed files byte-identical to the containers; full native loop
> (freeze → pack-emit → AMFI → run → read-back) green on both arches.
> Fork release `1.0-madc.0.47.0` (@74e705e4 — the extra-section
> carrier seam).

> **Previous (2026-07-25, `develop` — v0.46.0, forest-carriers S1
> complete):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** with every
> forest gate green, including the NEW `forest_bind_gate` case
> `[fnptrbody]` (typedef-of-fnptr-member-struct — the darwin `FILE`
> shape that the class-parser typedef branches under-registered).
> Packed arbiter re-proven at the parser fix: `forest_pack: OK (240
> units; bind cache == no-cache)` + **756/0/0/9** through
> `bin/madc-release`. `--exe` lane **740/0**. Hosted darwin binaries
> ship PACKED (`__MADC,__forest` section, 30 units per arch): Mac
> hardware matrix GREEN on both architectures — section read-back,
> all lanes (JIT/AOT × `.mad`/`.c`), grove bind engaged and
> byte-identical to live parse. Fork unchanged
> (`1.0-madc.0.45.0` @a3cf84ae).

> **Previous (2026-07-25, `develop` — v0.45.0, madc-on-macOS Route 1
> Phase 1 complete):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** (+2 this
> release: `testpragmapack` — GCC pack semantics with parse-time
> application, gcc-oracle-matched — and `testfnptrdecl` — fn-ptr
> declarator breadth incl. the C spiral and deref-postinc binding).
> `--exe` lane **740/0**. G2 on Apple hardware GREEN in every lane on
> both architectures (hosted arm64 native + x86-64 under Rosetta):
> JIT, native Mach-O AOT emit+run, labeled POSIX symbols, ctype
> inlines, stdio macros. Fork release `1.0-madc.0.45.0`
> (@a3cf84ae).

> **Previous (2026-07-20, `develop` @71a36e9d — component-correct GNU
> integer `_Complex`, task #69):**
> fulltest **729 passed, 0 failed, 0 timed out, 13 skipped** (+2:
> `testcomplexint` — the integer-complex lock, JIT and
> gcc-on-emitted-C both green — and `testvarargsstructcomplex`, its
> `mir_skip` lifted). Packed suite (`MADC_BIN=bin/madc-release bash
> scripts/run_tests.sh`) also **729/0/0/13**; `forest_pack: OK (240
> units; bind cache == no-cache)`. gcc-torture **1614 passed**
> steady-state, failset **11 names** in
> `docs/parity/torture-failset-current.txt` (3 integer-complex tests
> UNSKIPPED — skip manifest 33 → 30 — and `20020227-1` FIXED by the
> fork's complex-compare conversion fix; `memclr`/`memcpy-a*` run
> 3.4–3.7 s against the 5 s cap and can flap under neighbor load —
> they pass solo). MIR fork test battery green at fork develop
> @a4a7aa32 (integer-complex specifier rejection + stmt-expr
> init-slot layout fix + mixed-width complex compare conversions, each
> with a c-tests/new regression test). SMAUG `--project` soak green on
> both binaries ("ready at address"). The `--exe` lane is structurally
> unavailable on the CIR backend (`-o` says so explicitly) until AOT
> R4 lands `--emit-object`.

> **Previous (2026-07-19, `develop` @daed32ce — AOT R1: madc `-g`
> source-level gdb on the JIT lane, task #82):**
> fulltest **727 passed, 0 failed, 0 timed out, 14 skipped** (+1:
> `testdebuginfo`, the `-g` pipeline lock). Packed suite
> (`MADC_BIN=bin/madc-release bash scripts/run_tests.sh`) also
> **727/0/0/14**; `forest_pack: OK (240 units; bind cache == no-cache)`.
> gcc-torture **1610 passed**, failset **12 names byte-identical** to
> `docs/parity/torture-failset-current.txt`. MIR fork test battery green
> at fork develop @b6a411fa (upstream sync with vnmakarov master
> a8ab7c31 + the 13-commit debug-support arc + `.debug_frame` CFI +
> the force_val pr34099-2 restoration). SMAUG `--project` soak green on
> both binaries ("ready at address"). Interactive gdb gate:
> `break file.mad:line`, named typed frames across JIT + host, and
> `info locals` in any frame — verified on the final binaries.

> **Previous (2026-07-15, class-KIND parse-once B2 on
> `feature/class-parse-once-codex`):**
> The fulltest component matrix is green with **696 passed, 0 failed, 0 timed
> out, 16 skipped**. Every static and forest gate is green, including
> `forest_bind_gate [subbind]`, the full bind matrix, and both forest oracles.
> The initial fulltest invocation used the census cap (`MADC_CPU_LIMIT=30`), so
> its final `testsubscript --freeze` process hit that cap; only the interrupted
> forest bind gate and the two unreached tail oracles were resumed at the
> documented `MADC_CPU_LIMIT=120`, and all passed.
> `make -C src release` exits 0 and appends 240 packed units; the full packed run
> (`MADC_BIN=bin/madc-release bash scripts/run_tests.sh`) is also
> **696/0/0/16**. `bin/test_class_pattern` is **2/2** with **159 assertions**,
> covering structural-versus-legacy metadata equivalence, GCC/Clang Itanium
> symbols, and loud rollback without parser retry. `bin/test_cir_freeze` is
> **36/36** with **740 assertions**,
> including class-pattern semantic/token fingerprint and forest round-trip
> coverage; `bin/test_stringpool` is **7/7** with **10,032 assertions**,
> including scoped keyed-map transactions. The tsubst matrix is **13/13** and
> the suite ratchet remains **10 hit / 0 fallback**. The full class census is
> pattern **3**, parse **48604**, cache **99334**, opaque **24431**. B2 admits
> the narrow aliases/member/layout/simple-method/forward-completion subset;
> every other shape stays on the single parser lane under a typed reason. Both
> the debug/PIC build and the `-Wall -O2` release build emit **0 host compiler
> warnings**; the source warning census compiles **712** tests with **0
> warnings**. The packed artifact carries a readable 240-unit forest, is
> **10,219,496 bytes**, and has `MADCSNAP` footer magic.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`,
> task #68 — real SysV `__builtin_va_list`, 20041214-1 flips):**
> gcc-torture **1610 passed, 2 compile-failed, 10 runtime-failed, 0
> timed out, 63 skipped** — name-set diff vs the post-#78 baseline is
> EXACTLY {20041214-1.c}, zero regressions; failset refreshed to **12**
> names = 11 class-(b) GNU-ext + **1 class-(a) single** (pr22061-1 VLA
> param bound). The lexer's `__builtin_va_list` → `long` macro is gone:
> the compiler owns the type (`Program::builtin_va_list_type()`, the
> SysV `struct __madc_va_list_tag[1]` singleton), embedded <stdarg.h>
> aliases it, va_end/va_copy macro bodies are array-correct, and the
> singleton is PINNED as type-id slot 34 so frozen typedefs restore in
> any process (the pre-pin packed run failed 10 varargs tests with
> "undeclared identifier va_list"; packed is now **726/0/0/14 == dev**
> with forest_pack OK). The synthesized tag is a Class-5
> forest_index_allowlist entry. testbuiltinvalisttypedef reworked to
> the gcc-parity `.expect_err` (`ap = 0` on an array-typed va_list must
> reject; stale "ok" .expect + .mir_skip removed → +1 pass, −1 skip).
> Fulltest **726/0/0/14**, tsubst ratchet green, SMAUG soak GREEN
> dev+packed. Pre-existing and unchanged: testvarargsstructruntime
> (c2mir lacks VLA-in-struct layout — fork work, pr41935/pr82210
> family) and testvarargsstructcomplex (integer `_Complex` MIR-gen
> fatal = task #69, fork-or-clean-reject) — both verified independent
> of the va_list model.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`,
> task #78 — array-typedef dims order + &array-lvalue typing, strlen-4
> flips):** gcc-torture **1609 passed, 2 compile-failed, 11
> runtime-failed, 0 timed out, 63 skipped** — name-set diff vs the
> post-singles baseline is EXACTLY {strlen-4.c} removed, zero
> regressions; `docs/parity/torture-failset-current.txt` refreshed to
> **13** names = 11 class-(b) GNU-ext + **2 class-(a) singles**
> (20041214-1 va_list delegation, pr22061-1 VLA param bound). Two
> stacked fixes: (1) parser dims order for `A28 row[3]` with
> `typedef char A28[28]` — the declarator's dims are the OUTER
> dimensions; the peeled typedef dims now rotate behind them
> (`sizeof(row[0])` was 3, initializers truncated); (2) c2mir fork
> @8a6a6c57 — `&a[i]` on a decayed array lvalue now constructs the true
> pointer-to-array type instead of copying the decayed element pointer
> (`*(&a[i] + k)` yielded a char scalar; strlen crashed at 0x31).
> Fulltest **725/0/0/15** (+`testarraytypedef`, gcc-oracle byte-equal),
> packed arbiter **725/0/0/15** with forest_pack OK (240 units, bind
> cache == no-cache), SMAUG soak GREEN dev+packed. Adjacent gap filed
> as #79: the CAST form `(char (*)[28])expr` is rejected by the fn-ptr
> cast arm.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`,
> task #77 — liberal default resource guards, owner directive):** the CLI's
> self-limits no longer throttle legitimate work: `MADC_CPU_LIMIT` default
> 60 s → **0 (disabled, opt-in)** — madc RUNS the program, so any finite
> CPU default eventually kills a legitimate long-running server with
> SIGXCPU; an armed CPU guard now trips LOUDLY (new SIGXCPU handler names
> the knob via the crash-write plumbing, then re-raises so the shell sees
> the real signal status). `MADC_MEM_LIMIT` base **2048 → 4096 MB**
> (+128 MB/TU `--project` scaling and the knob-naming `bad_alloc`
> attribution unchanged); both knobs are now documented in `--help`
> (Environment section). Probes: `MADC_CPU_LIMIT=2` + spin → knob-named
> trip, exit 152; **65-CPU-second spin survives the default env** (died
> at 60 s before); malloc-loop NULLs at exactly the 4096 MB ceiling
> (4032 MB allocated over a ~64 MB baseline) and honors a 256 MB override
> (192 MB). Guards install only in `main()` — libmadc embedding hosts set
> their own. Fulltest and the packed arbiter re-verified green (counts
> unchanged: 724/0/0/15 dev + packed).
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`,
> promote-gate singles — 🏁 THE ≥1608 THRESHOLD IS MET):** gcc-torture
> **1608 passed, 2 compile-failed, 12 runtime-failed, 0 timed out, 63
> skipped** — name-set diff vs the post-#74 baseline is EXACTLY
> {20030714-1.c, struct-ret-1.c} removed, zero regressions;
> `docs/parity/torture-failset-current.txt` refreshed to the **14** remaining
> names = 11 class-(b) GNU-ext + **3 class-(a) singles** (strlen-4,
> 20041214-1 va_list delegation, pr22061-1 VLA param bound). Two fixes:
> (1) fn-ptr declarations whose RETURN type is a typedef (`X (*fp)(void)`)
> emitted `X fp` — the alias swallowed the declarator; new
> `fnptr_alias_is_fn()` gates the alias-spec form to typedefs that name the
> function type itself, applied to both the variable and MEMBER arms
> (members emitted `bool *m` via the unknown-alias star fallback).
> (2) `_Bool` bit-fields: the signedness reconciliation emitted
> `unsigned _Bool` (rejected by c2mir, C11 6.7.2p2) — `N_BOOL` now counts
> as sign-complete. Fulltest **724/0/0/15** (+2: `testfnptrtypedefret`,
> `testboolbitfield` — the latter locks VALUE semantics only; the
> pre-existing `_Bool:1`-then-wider-type allocation-unit divergence
> (sizeof 8 vs gcc 4) is filed as task #76). The branching.md gate reads
> "ALL class-(a) fixed (≥1608)": the NUMBER is met; 3 class-(a) singles
> remain — the promote call is the owner's.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`, task #74
> dead-branch fold keeps function-scope labels):** gcc-torture **1606 passed,
> 2 compile-failed, 14 runtime-failed, 0 timed out, 63 skipped** — name-set
> diff vs the post-#73 baseline is EXACTLY {pr17078-1.c, vla-dealloc-1.c}
> removed, zero regressions; `docs/parity/torture-failset-current.txt`
> refreshed to the **16** remaining names (11 class-(b) GNU-ext + 5 class-(a)
> singles). vla-dealloc-1's VLA-dealloc half already worked — the label drop
> was its whole story. Fix: `stmt_contains_label()` walk guards BOTH constant
> fold arms in `translate_if_core` (a label makes a dead arm a live goto
> target, C11 6.2.1p3 — the fold falls through to the full N_IF, gcc -O0's
> shape). Fulltest **722/0/0/15** (+1: new `tests/testgotodeadarm.mad`,
> gcc-oracle byte-equal). Gate math: 1606 + 5 class-(a) singles (va_list
> delegation 20041214-1, VLA param bound pr22061-1, _Bool bitfield
> 20030714-1, strlen-4, struct-ret-1) = 1611 ≥ 1608 — TWO more singles cross
> the promote gate.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`, task #73
> wide string literals):** gcc-torture **1604 passed, 2 compile-failed, 16
> runtime-failed, 0 timed out, 63 skipped** — name-set diff vs the post-#72
> baseline is EXACTLY {20010325-1.c, widechar-3.c} removed, zero regressions;
> `docs/parity/torture-failset-current.txt` refreshed to the **18** remaining
> names (memcpy-a8 passed this sweep — the documented load-margin flake, never
> in the failset). Fulltest **721/0/0/15** (+1: `tests/testwideconcat.mad`
> lifted, its `.mir_skip` removed). The fix is the Tier-1 wide-literal
> lowering in the CIR builder: content-hash-named
> `static int __wlit_<fnv1a64>[]` definitions emitted from the parser's baked
> UTF-32 data, uses routed through `var_emit_name`, the constant-scalar READ
> fold now excludes fixed arrays, and each definition rides the rung-3
> referenced-surface filter (`cond_mark_sym`) so dead literals from
> live-parsed-but-unused template bodies can't break the `forest_bind_gate`
> byte-identity oracle (caught by [strbind] during development, fixed before
> landing). Gate math: 1604 + 7 remaining class-(a) (#74 ×2, va_list
> delegation, VLA param bound, _Bool bitfield, strlen-4, struct-ret-1) =
> 1611 ≥ 1608.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`, task #75
> SMAUG --project soak restored):** the soak is GREEN again on the dev binary —
> `Realms of Despair ready at address madc-dev on port 4000` under DEFAULT
> guards. Root cause was madc's own `RLIMIT_AS` resource guard
> (`install_resource_guards()`, fixed 2048 MB `MADC_MEM_LIMIT` default, commit
> @1713e2ba 2026-04-30 — present while the soak was green): the `--project`
> driver holds all 51 parsed Programs at once and legitimately peaks at
> **~2.9 GB VA** (measured `VmPeak` 3,039,872 kB with the guard off; maxrss
> only 985 MB — RLIMIT_AS counts address space, not residency), so natural
> footprint growth crossed the 2 GB line at ~TU #44 (stances.c) and the
> guard's ENOMEM surfaced as an UNPRINTED `std::bad_alloc`. NOT cross-TU state
> poisoning (gdb catchpoint: healthy token arena, 452 × 1 MB chunks,
> `malloc(1 MB)` → NULL; peak maps 105 of 65530). Fixes: workload-scaled
> guard default (+128 MB per manifest TU; guards now install after argument
> parsing since RLIMIT hard limits can never be raised), a `set_new_handler`
> that names `MADC_MEM_LIMIT` when the guard trips (verified: `MADC_MEM_LIMIT=512`
> prints the guard line + `comments.c:...: error: std::bad_alloc` + rc=1),
> and `Program::print_unrendered_diagnostic()` in all four
> `catch(std::exception&)` phase arms — a tokenize/parse failure can never
> again exit silent.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`, task #72
> implicit-int/K&R function definitions):** gcc-torture **1601 passed, 2
> compile-failed, 18 runtime-failed, 0 timed out (memcpy-a8 timed out under
> box load during the sweep; verified passing 4× standalone at 3.6–4.1s vs the
> 5s cap — load-margin flake, not counted), 63 skipped** — all **30** cluster-1
> names flipped, zero regressions (byte-identical name-set diff);
> `docs/parity/torture-failset-current.txt` refreshed to the **20** remaining
> names. Fulltest **720/0/0/16** (+1 = `tests/testknrdef.mad`, gcc-oracle
> byte-equal under `--std=c17`). The three arms all sit behind the existing
> `knr_supported()` gate. Adjacent std-gating fix: C89 implicit function
> declarations in expression context were gated on the `.c` filename
> extension only — `--std=c17` on a `.mad` file now behaves as C17 (the
> extension predicate stays for default-dialect C sources). ⚠️ The mandatory
> SMAUG soak FAILED — and fails BYTE-IDENTICALLY at the pre-#72 baseline
> (stash-rebuild A/B proof): "stances.c: tokenize failed" with no diagnostic,
> only under --project after ~40 green TUs; standalone-with-flags compiles
> clean. Pre-existing madc-side breakage (MadSMAUG tree untouched) — filed as
> **P0 task #75**; #72 introduces no soak delta.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`, task #64
> gcc-torture re-baseline):** full sweep at HEAD @1aa53a4e via
> `scripts/run_gcc_testsuite.py` (defaults: dev binary, `--std=c17`, formal
> skip manifest): **1572 passed, 32 compile-failed, 18 runtime-failed, 0 timed
> out, 63 skipped** — the 50-name failset is **byte-identical to
> `docs/parity/torture-failset-current.txt`** (name-set diff empty both ways;
> ZERO regressions across the #35–#63 span; the previously recorded
> "1571/33, 51-name" banner was one stale against the file). Cluster refresh
> of the 50: **39 class-(a)** + 11 class-(b) GNU-ext (aligned>16 ×3,
> packed/misalign ×2, SIMD vector_size ×3, __sync_* ×1, empty-union ABI ×1,
> one-void-arm conditional ×1). The class-(a) map COLLAPSED on evidence: the
> old "implicit-decl forward call" cluster (5) is a SYMPTOM of implicit-int
> definitions failing to parse (`mpn_print (){}` defines nothing → "import of
> undefined item"), so ONE parser work item — implicit-int function
> definitions (bare K&R identifier lists `f(x){}`, empty-parens `dummy(){}`,
> and typed-param defs after first use misparsed as calls) — covers **30 of
> 39**; the declaration-list K&R form `f(x) int x; {...}` ALREADY parses at
> HEAD. Remainder: wide literals ×2 (undeclared `__wliteral__*`, same cause
> as testwideconcat), labels-in-if-arm ×2 (pr17078-1 attributed to the CIR
> builder — stock c2m passes it), va_list delegation ×1, VLA param bound ×1,
> _Bool sign-qualifier bitfield ×1, strlen-4, struct-ret-1. Gate math:
> 1572 + 39 = 1611 ≥ 1608 — the promote gate is reachable on class-(a) alone.
> Follow-on tasks filed: #72 (the 30-test parser lever, SMAUG-soak-gated),
> #73 (wide literals + testwideconcat lift), #74 (if-arm label drop).
> Reducers banked: tmp/s64_knr.c, tmp/s64_implicitint.c, tmp/s64_wlit.c,
> tmp/s64_labelscope.c (passing control), tmp/s64_failset_new.txt.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`, task #61
> mir_skip audit):** all **16** `tests/*.mir_skip` fixtures re-run at live HEAD
> with runner-equivalent fixture handling — **all 16 still fail; zero lifted**;
> the suite surface is unchanged (arbiter remains 717/0/0/16). Five recorded
> reasons verified still-true (testbitfieldwidearith, testbuiltinllabsoverride,
> teststructleadingattrmember, testunionscalarcast, testvarargsstructruntime);
> **eleven fixtures reworded** because the recorded reason had drifted or was
> wrong: `testvarargsstructcomplex` ("no _Complex" — stale; true cause is GNU
> INTEGER complex `_Complex int` hitting a MIR gen fatal even as a scalar),
> `testvlastructmember` (c2mir now accepts VLA struct members but miscompiles
> the stmt-expr copy — runtime abort, not a reject),
> `testfloattointclamp` (GCC itself saturates via front-end constant folding —
> verified `gcc -O0` prints 2147483647, the .expect IS canon; c2mir converts at
> runtime → INT_MIN), `testfinstrumentfunctions` (no inline asm in the test;
> instrumentation IS implemented — `no_instrument_function` on a prototype
> doesn't merge into the later definition), `testbuiltinframeaddress` /
> `testbuiltinsetjmp` (both lower to runtime helpers in va_helpers.cpp that
> execute in the helper's frame — structurally unable to satisfy
> frame-address/returns-twice semantics), `testdlcall` (madc's `dlcall()`
> builtin has no MIR-lane runtime; the test has no `#load`), `testdlopen`
> (#load parses and lowers; the MIR import resolver just doesn't consult the
> #load'd handles), `testbuiltinvalisttypedef` (the test is INVALID on x86-64 —
> gcc and clang both reject `ap = 0` on the array-typed va_list; madc's
> `__builtin_va_list` divergently accepts it), `testnestedpackedmember` (c2mir
> packed nested-struct member offset not packed while sizeof is), and
> `testwideconcat` (madc-side mixed-width concat lowering emits undeclared
> `__wliteral__a`). Follow-on near-miss tasks filed: fold-spine float→int
> overflow folding (lifts testfloattointclamp), prototype-attr merge for
> `no_instrument_function`, #load/dlcall MIR import resolution, mapping
> `__builtin_va_list` to the target's real type (then convert the test to
> `.expect_err` and lift), and the fork-side `_Complex int` gen fatal.
> Reducers banked: `tmp/s61_cx{A..E}.mad`, `tmp/s61_ftoi.c`,
> `tmp/s61_valist.c`, `tmp/s61_packed.c`.
>
> **Local branch update (2026-06-28, `feature/front-end-performance-claude` @
> Kind 3 dependent-member body tsubst slice):** fulltest
> **670 passed, 0 failed, 0 timed out, 18 skipped** (exit 0, both check gates
> GREEN). The env-gated path is also green:
> `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` reports **670/0/0/18**.
> `bin/test_cir` reports **92 test cases, 1137 assertions, 4 skipped** after
> adding nested dependent member-template body coverage and dependent explicit
> destructor rematerialization coverage. `MADC_XTEST_DEP_PARSE=1 bin/madc
> --show-stats tests/testvector.mad` now reports **14 hit / 1 fallback**;
> `std::allocator_traits::destroy<_Up>` is gone from the fallback profile and
> the remaining vector fallback is the out-of-scope
> `std::__cxx11::basic_string::_M_construct<_InIterator>` template-id body
> shape. The C11 spot-check confirms allocator-traits destroy passes the
> allocator reference receiver as `__a`, not a rewritten callee symbol, and
> `__new_allocator<basic_string>::destroy` calls the real `basic_string` D1
> destructor. gcc.c-torture remains byte-identical to
> `docs/parity/torture-failset-current.txt`: **1571 passed, 33 compile-failed,
> 18 runtime-failed, 0 timed out, 63 skipped**, 51-name failset.
>
> **Local branch update (2026-06-26, `feature/front-end-performance-claude` @
> converted system-header reference-forwarded placement-new pack slice,
> including the earlier reserved scalar/pointer helper call widening,
> with two-tree direct type-arg binding and direct
> value/ref/expression/forwarding-call/constructor pack fan-out plus covered
> system-header placement-new pack fan-out and simple class `_Up` placement-new
> tsubst plus direct `__destroy(T*)` helper tsubst and local non-pack nested
> namespace-call tsubst plus nested function-template instantiation, plus
> dependent-parse-error scope balancing and pointer-parameter-pack call
> expansion plus guarded direct `_Destroy_aux`/member-template `__destroy`
> tsubst plus direct `std::move<Args>(args)...` forwarding-pack coverage plus
> the `--show-stats` tsubst engagement counter and ranked fallback profile):**
> fulltest
> **670 passed, 0 failed, 0 timed out, 18 skipped** (exit 0, both check gates
> GREEN). The env-gated tsubst path is also green:
> `MADC_XTEST_DEP_PARSE=1 make -C src fulltest` reports **670/0/0/18**.
> `bin/test_cir` reports **86 test cases, 1067 assertions, 4 skipped** after
> adding coverage for direct `tsubst_type_args` binding of a body-only template
> parameter and direct `tsubst_type_arg_packs` capture for a variadic member
> template, plus direct CIR fan-out for value-pack call arguments like
> `sink(args...)` and reference-pack call arguments like
> `Args&... args` / `sink(args...)`, pointer-pack call arguments like
> `Args*... ps` / `sink(ps...)`, and expression-pattern packs like
> `sink((args + 1)...)`, plus the first forwarding-call pack pattern
> `sink(std::forward<Args>(args)...)` and the same direct structural path for
> `sink(std::move<Args>(args)...)`, plus covered local member-template
> constructors like `Holder(Args... args) { member = sink(args...); }`, plus
> system-header placement-new pack bodies, scalar `_Up` lowering, and simple
> class `_Up` lowering with scalar/pointer constructor pack elements for
> allocator-style `new ((void*)p) _Up(std::forward<Args>(args)...)`, plus
> direct `__destroy(T*)` helpers that defer pointee inspection until after
> substitution and lower class pointees to the concrete destructor, plus guarded
> member-template bodies named `__destroy` whose retained body itself contains a
> direct `__destroy(T*)` marker, plus local
> non-pack nested namespace calls such as `sink(nn::ident(v))` whose copied
> callee ids re-resolve from substituted argument types, plus by-value
> class-object constructor packs such as `Box(Item)` and `PairBox(Item, Item)` inside
> allocator-style placement-new bodies, plus value-returning forwarded class objects
> bound to reference constructor parameters such as `PairRef(const Item&, const Item&)`,
> plus local reference-returning identity-forwarding constructor packs that pass
> `Args&...` through `std::forward<Args>(args)...` and bind to class-reference
> constructor parameters, plus simple system-header dependent calls
> whose substituted args/return are concrete non-class scalar/pointer/void shapes
> and whose resolved callees have a materializable body or external symbol,
> including reserved `__*` helper names and copied-call reachability for lazy
> body emission, plus direct system-header reference-forwarded placement-new
> constructor packs whose per-element nested call resolves through
> `resolve_copied_dependent_call` and returns the same/derived class expected by
> the constructor reference parameter, plus converted system-header
> reference-forwarded placement-new packs where that returned class is accepted
> by the target's single-argument converting constructor and a target temp is
> materialized before the outer constructor call, plus `--show-stats` tsubst
> body engagement counters proving real workload visibility of hit/fallback
> split (`testsubscript` currently reports 6 hit / 29 fallback under
> `MADC_XTEST_DEP_PARSE=1`) and a ranked fallback profile. Clean `-O2`
> `testsubscript` profiling ranks the top real fallback shapes as
> `std::allocator_traits::construct<_Up,_Args...>` (4),
> `std::allocator_traits::destroy<_Up>` (4), and
> `std::__new_allocator::construct<_Up,_Args...>` (3), with instantiate time
> 0.327 s and total in-process time 0.804 s on this host. The latest robustness
> slice also adds
> `tests/testdependentparseerror.mad`, proving an env-gated dependent parse
> error balances the temporary parameter compound scope and exits nonzero
> without SIGSEGV. Phase 4 is now tracked at
> roughly **74% implemented** by coverage weight. Broader system-header
> destructor/object-address pack surfaces, class-valued placement-new
> constructor argument packs beyond those direct/converted apertures, broader
> system-header nested/dependent calls, and template-id body/return surfaces
> remain on the re-parse fallback.
>
> **Current (2026-06-22, `develop`, v0.30.0 — set wall CLEARED, pushed to
> origin/develop):** Fulltest **669 passed, 0 failed, 0 timed out, 18 skipped**
> (exit 0, both check gates GREEN); gcc.c-torture failset byte-identical to the
> 51-name baseline; zero regressions. The `std::set`/`std::map` "set wall" — a
> STACK of eight root-cause container bugs — is **fully cleared** on the default
> C++17 real-header path; `testset`, `testmap`, `testsubscript`,
> `testcontainerdtor`, `testmadc_ns` all green. **bug-7b @ `da96d7a`** (the final
> red, `testsubscript`): the earlier cont. 7 "`std::get` return-type unresolved"
> reading was the SYMPTOM — the get instantiation was already correct. Real cause:
> `resolve_fn_template_return_by_key` bound `std::get<0>`'s non-type arg `0` to the
> TYPE parameter of the by-type `std::get` overload, naming the return type `"0"`,
> which leaked into the call's parse-time `return_override` and broke `std::get`
> ref-binding in the piecewise pair ctor of `map<K,string>::operator[]` (undefined
> `basic_string…__o15`); surfaced only when two `pair<const string,V>`
> instantiations coexist. Fix (deepest layer): a non-type value arg cannot bind to
> a TYPE template parameter (`[temp.arg.nontype]` substitution failure → candidate
> removed), via `datadef_is_nontype_constant`. Drift gate refined with an audited
> `// allowed-exception` opt-out (`24e8125`) so `make fulltest` exits 0. Full
> trail: `docs/plans/2026-06-19-map-instantiation-strategy.md` (**cont. 8**,
> supersedes cont. 5/6/7). **bug-7a @ `b5698d7`** (`set<string>::insert("lit")`
> UDC ctor scan; cleared `testset`). **bug-5c @ `bc7693d`** (`set<int>` SIGSEGV: the
> copy-elision/NRVO init path for a retbuf-returning method delegated explicit
> args to `build_call_args` with no `__this` offset → a reference arg coerced
> against `__this`, passed as a value where an `int*` was expected → int-as-
> address deref; fix adds `param_base` to `build_call_args`; test
> `testretbufrefarg`). **bug-6 @ `9e959fd`** (`set<string>`+`map<string,
> string>` → MIR `Repeated item declaration _Tp2___dtor`: the nested
> `__aligned_membuf<_Tp>::_Tp2` had its store key enclosing-qualified but
> `dds->name` left bare so both emitted the same dtor; fix renames the emitted
> identity; cleared `testcontainerdtor` + `testmadc_ns`; `testset.mad` converted
> off C++20 `contains` to C++17 find/end). **bug-7 remains** (`testset`/
> `testsubscript`): MIR `import of undefined item ..._o<N>` — an implicit
> `const char*`→`std::string` conversion at a header method-call argument emits
> a `__o<N>` wrapper call but neither materializes the string temporary nor
> emits the wrapper body; see
> `docs/plans/2026-06-19-map-instantiation-strategy.md` (2026-06-22 cont. 5).
> Earlier this session: bugs 1–4. **bug-1 @ `cbd693a`** (call `(` after a substituted type-param
> misparsed as a cast; test `testfunctorctorarg`). **bug-2 @ `94d0798`**
> (`return` skipped the implicit converting-ctor on a trivially-copyable
> by-value class return; test `testreturnconvctor`). **bug-3 @ `7d53ed1`**
> (two classes' hidden-friend operators sharing a `_Self` typedef collapsed to
> one overload — identity used raw param text; test `testfriendopself`).
> **bug-4 @ `c781287`** (a functor whose `operator()` is a member TEMPLATE was
> never dispatched — `g(args)` mis-lowered to `g = args`; five parser sub-fixes
> covering operator-id declarator recognition, the param-list paren after an
> operator-id, the instantiation rename across the operator-id span, pointer
> return-type preservation, and `is_static` scanning only the header; tests
> `testfunctortmploperator`, `testmembertmplptrret`). With bugs 1–4, real
> `std::set<int>` now **compiles**. **bug-5c remains** (set's runtime blocker):
> SIGSEGV in `_Rb_tree::_M_construct_node__mti`, a variadic-member-template /
> allocator-`construct` bug; see
> `docs/plans/2026-06-19-map-instantiation-strategy.md` (2026-06-22 cont. 3).
> gcc torture non-timeout failset byte-identical to the 51-name baseline across
> all four fixes (zero regressions).
>
> **Prior WIP (2026-06-22, `wip/map-cxx17-salvage-codex` @ `3534b44`
> plus local recovery fixes):** the previous dirty session is preserved at
> `failed/2026-06-22-map-cxx17-attempt-codex` commit `3534b44`; live work
> continues on the salvage branch with dirty fixes in `include/datatokens.h`,
> `src/parser.cpp`, and `src/cir_builder.cpp`. Focused C++17 map validation is
> green: `teststdmapint` pins `std::map<int,int>` insert/update through real
> libstdc++ headers under `--std=c++17 --no-embedded-headers`, and
> `tests/testmap.mad` uses C++17 `find/end` rather than C++20 `contains` and
> passes for `std::map<std::string,int>`. Recovered regressions from the
> interrupted handoff: `testforeach2`, `testtuple`, `testfstream`, `testloop`,
> `testmadcevalexpr`, `testmadcevalexprctx`, and `testmadcevalexprtyped` are
> green. C++20 canaries `testcompare_realhdr`, `testspaceship_realhdr`,
> `testdefaultedcmp_realhdr`, `testrewritten_realhdr`, and `testinvocable`
> are also green under per-test `--std=c++20 --no-embedded-headers` flags.
> Latest fulltest attempts are noisy under the runner's default 5-second
> per-test integration cap on this host: run 1 reported
> **657 passed, 4 failed, 2 timed out, 18 skipped**; run 2 reported
> **650 passed, 3 failed, 10 timed out, 18 skipped** with shifting unrelated
> timeouts. Isolated timeout candidates pass sequentially under the default
> cap, so the stable functional red list is now `testcontainerdtor`,
> `testmadc_ns`, `testset`, and `testsubscript`; `testmap` is no longer in the
> focused failure set. The former
> `FEATURE_CONST_TYPES` and `FEATURE_DERIVED_TO_BASE_DEDUCTION` paths are now
> default after external-method typed-return/ref-argument lowering fixed the
> historical stream/string regressions. Remaining diagnostics: non-fatal
> libstdc++ `stl_tree`/`stl_map` pointer-type warnings. Latest local handoff
> update: the previous `std::get` scoped-alias blocker is fixed generically
> through same-DataDef typedef-alias preservation plus concrete partial-spec
> completion from the opaque template path; `_Nth_type`/tuple reducers and
> `teststdmapint` pass. The later undefined `basic_string...__o15` wrapper was
> moved forward by generic CIR reference-return/constructor-argument handling.
> The remaining runtime corruption was caused by flattening libstdc++ anonymous
> union members in `std::basic_string` as sequential fields, inflating the
> string layout and overflowing pair/tree storage. `DataDefSTRUCT` now records
> anonymous aggregate groups and CIR emits unnamed anonymous struct/union
> members, preserving the ABI layout. `make -C src` passed; `--emit=c11` for
> `tests/testmap.mad` shows `_M_local_buf`/`_M_allocated_capacity` inside an
> anonymous union; `tests/teststdmapint.mad` and `tests/testmap.mad` both pass.
> Focused regressions `tests/testtuple.mad`, `tests/testforeach2.mad`,
> `tests/testfstream.mad`, `tests/testloop.mad`, and the eval-expression
> regressions also pass after this fix. Revalidated after a clean non-debug
> rebuild; `tests/testmathh.timeout` keeps that passing math-header test off
> the default 5-second wall cap. gcc.c-torture and SMAUG were not rerun in this
> WIP validation.
>
> **Previous (2026-06-11, `feature/strict-equality-claude` @ `1ffbc8c`
> — `===`/`!==` strict equality, STD_MADC dialect):** fulltest
> **577 passed, 0 failed, 0 timed out, 18 skipped** (make exit 0, both
> check gates GREEN, clean rebuild zero warnings). New operators:
> type-domain identity AND value equality (`uint32_t === int32_t` false;
> `long === long long` true; enums/bool own domains; literals keep their
> C type), user `operator===`/`operator!==` overloading (vendor-extended
> manglings `v23eq3`/`v23ne3`), class domain rule via `operator==`
> (`string s === "x"` true), STD_MADC token gating (conformance fix),
> eval-DSL `!==` + comparison-result inference fix (a string `!==`
> previously segfaulted the host). New tests: `test3eq`, `test3eqclass`,
> `test3eqerr` (expect_err), `test3eqgate`/`test3noteqgate` (std-floor).
> gcc.c-torture failset **byte-identical** (1567/26/29/0/63); SMAUG soak
> green (exit 124 + ready line). Deferred (spec): script-side `array`
> strict equality (no whole-value scalar ops on that surface yet).
>
> **Previous (2026-06-11, `feature/template-instantiation-claude` @ `01528ed`
> — `= default` comparison synthesis; `<=>` track COMPLETE):** fulltest
> **572 passed, 0 failed, 0 timed out, 18 skipped** (make exit 0, both
> check gates GREEN). Ordering-vs-ordering `==`/`!=` and
> `auto operator<=>(const V&) const = default` (all six comparisons from
> one declaration) work — new test `testdefaultedcmp_realhdr` (8 shapes,
> g++-verified). gcc.c-torture failset **byte-identical**
> (1567/26/29/0/63); SMAUG green.
>
> **Previous (2026-06-11, `feature/template-instantiation-claude` @ `aff26fa`
> — `<=>` rewritten candidates):** fulltest **571 passed, 0 failed, 0 timed
> out, 18 skipped** (make exit 0, both check gates GREEN). `r != 0`,
> `0 == e`, and the only-`<=>`/`==` user-class idiom (all six comparisons)
> work — new test `testrewritten_realhdr` (9 g++-verified shapes).
> gcc.c-torture failset **byte-identical** (1567/26/29/0/63); SMAUG green.
> NEW known gap (pre-existing): `Q a(1), b(2);` ctor-arg multi-declarator
> hangs the parser — split declarations.
>
> **Previous (2026-06-11, `feature/template-instantiation-claude` @ `7a56d72`
> — `<=>` slice 3a, the token lowering):** fulltest **570 passed, 0 failed,
> 0 timed out, 18 skipped** (make exit 0, both check gates GREEN).
> `a <=> b` works on builtin scalars (byte-select into a category temp;
> partial_ordering unordered=2 for NaN) and class operands (hoisted friend
> operator<=>, forward + reversed). New test: `testspaceship_realhdr`
> (8 g++-verified shapes). gcc.c-torture failset **byte-identical**
> (1567/26/29/0/63); SMAUG soak green.
>
> **Previous (2026-06-11, `feature/template-instantiation-claude` @ `5f63a20`
> — `<=>` slice 2b COMPLETE, hidden-friend operator bodies):** fulltest
> **569 passed, 0 failed, 0 timed out, 18 skipped** (make exit 0, both
> check gates GREEN). `r < 0` on `std::strong_ordering` calls the compiled
> TU-local friend from the real `<compare>`. New tests: `testfreeop`
> (free-operator dispatch by overload resolution + literal-0
> null-pointer-constant overload choice, g++-verified), `testhiddenfriend`
> (user-code hidden-friend operators, default std), `testcompareops_realhdr`
> (`<compare>` strong/weak/partial orderings vs literal 0, reversed
> operands, unordered — 7 shapes, g++-verified). NOT in scope: C++20
> rewritten candidates (`r != 0`) — errors loudly, queued in
> cpp-support.md P2.15. gcc.c-torture failset **byte-identical** per
> ingredient commit (1567/26/29/0/63); SMAUG soak green per commit.
>
> **Previous (2026-06-11, `feature/template-instantiation-claude` @ `e124de5`
> — template-instantiation batch COMPLETE, 2a/2b/2c/2d):** fulltest **564
> passed, 0 failed, 0 timed out, 18 skipped** (make exit 0, both check gates
> GREEN). New tests this branch: `teststod{,_realhdr}` (2a fn-template
> empty-pack elision — std::stof/stod), `teststrlitplus_realhdr` (2b-i
> `"pre" + s` exported mangled-direct), `teststrplusbody_realhdr` (2b-ii
> `a + "bar"` free-operator BODY instantiation, foobar/prefoobar),
> `testctornomatch` (2c: a class declaration whose initializer matches no
> ctor is a LOUD compile error — first `.expect_err` compile-error test),
> `testctorrefarg` (`A local(p)` from `const A&` selects the copy ctor — the
> one real silent drop the 2c gate surfaced), `teststrrefparam_realhdr` (2d:
> reference operands resolve as the referenced class — `cout << s` with a
> `const string&` parameter, `a + b` on reference params). Includes the
> char_traits explicit-spec instantiation-key fix (`1abbee8` —
> `std::char_traits<char>::length` silently folded to 0 before).
> gcc.c-torture failset **byte-identical** to the post-audit baseline
> (1567/26/29/0/63); SMAUG soak green (exit 124 + ready line).
>
> **Previous (2026-06-11, `develop` @ `21bfec9` — failset classification
> audit):** fulltest **557 passed, 0 failed, 0 timed out, 18 skipped** (make
> exit 0, both check gates GREEN; re-verified at the audit commit — no
> compiler code changed). gcc.c-torture moves to the post-audit baseline:
> **1567 passed, 26 compile-failed, 29 runtime-failed, 0 timed out, 63
> skipped** — the 33 class-(c) gcc-internal/torture-only tests are now
> FORMAL skips via `docs/parity/torture-skip-manifest.txt` (user-signed
> audit `docs/parity/failset-classification.md`; in-scope denominator 1652,
> promote gate = all 41 class-(a) standard-C failures fixed = ≥1608).
> Regression baseline `tmp/failset_lsq.txt` regenerated (55 lines);
> `docs/parity/torture-failset-current.txt` synced.
>
> **Previous (2026-06-11, `feature/eval-leftovers-claude`
> against `/workspace/mir` `develop` @ `2ffebff`):** fulltest is
> **557 passed, 0 failed, 0 timed out, 18 skipped** (make exit 0, both check
> gates GREEN). The eval-leftovers branch landed packages B (DSL string
> compares via strcmp lowering), A0 (MadValue/MadArray → one `madc::value`
> end-to-end), and A (declaration-only mangled-direct `<ns_madc>` +
> user-call-site scope capture): `testmadcevalexprctx` extended (string
> value compares) and `testmadcevalscope` un-skipped — 42/42/echo plus the
> typed-out forms — and `test_libmadc_program` is **97 passed / 38 skipped**
> (the four script-side scope-access categories join). `test_mangle` pins
> the madc::value class-param symbols (the substitution back-ref N..E wrap
> fix) and the text-carrier marshalling predicate. torture **1567/31/56/1**,
> failset byte-identical to the baseline; SMAUG boots (soak exit 124 +
> ready line).
>
> **Previous (2026-06-10, `develop` @ `7e84242`
> against `/workspace/mir` `develop` @ `2ffebff`):** fulltest was
> **555 passed, 0 failed, 0 timed out, 20 skipped** (make exit 0, both check
> gates GREEN) — **ALL integration reds are green.** New this cycle:
> `teststringplus_realhdr.mad` pins real-header `std::string a+b` — by-value
> FREE-operator returns bind the exported `_ZStpl…` mangled-direct and decl
> inits copy-elide into the declared variable (the former handoff-§4 wall #1
> SIGSEGV, `23027f7`) — and `testcstdio.mad` pins default-mode
> `#include <cstdio>` over the completed embedded stdio.h shim (`8a897f8`).
> torture 1567/31/56/1, failset byte-identical after both changes.
>
> **Previous (2026-06-10, `feature/cpp-detection-idiom-claude` @ `883c26e`):**
> fulltest **547/0/0/26** — the first all-greens run.
> `testfstream.mad` (the last red) passes rewritten to standard
> C++ through REAL libstdc++ headers (`.flags` `--std=c++17
> --no-embedded-headers` + `.expect`), executing `ofstream`/`ifstream` file
> I/O, `std::getline`, and — via the NEW namespace function-template BODY
> instantiation — real-header `std::to_string(42)` and `std::stoi(string)`
> (the non-exported `__gnu_cxx::__stoa` / `std::__detail::__to_chars_*`
> template bodies compile on demand). The session's chain
> (plan `docs/plans/2026-06-10-testfstream-alias-reference-plan.md`, all
> tasks done): alias-spelled reference returns (DataDefREF; `s[1]` deref +
> `&s[1]`), fn-template instantiation (explicit args, packs, fn-ptr
> deduction), overload-ranking fixes (`user_argc` default-arg poisoning,
> fn-to-pointer decay), `current_namespace` restore in qualified statements,
> fortify `__builtin___mem*_chk`, and `[namespace.udir]` unqualified-call
> fallback (POSIX `::getline` vs `std::getline`). Standalone gcc.c-torture is
> **1567 passed, 31 compile-failed, 56 runtime-failed, 1 timed out, 30
> skipped** — failset byte-identical across the whole session. SMAUG boots.

> **Previous (2026-06-09 late):** fulltest **546/1/0/26**; the one red was
> `testfstream.mad`. `testlargesizeofquery.mad` went green via the 64-bit
> `carray_dim_t` array-dim widening, `testdefer.mad` via defer execution on
> CIR, and `testloop.mad` earlier via real headers; `991014-1.c` newly passed
> in torture. Real-header C++ canaries: `testcout_realhdr`,
> `test_extern_polymorphic`, `cout << std::string`, `std::getline`, the
> `inf.good()` loop; `std::to_string`/`std::stoi` resolved (overload sets)
> but their bodies still hit the alias-reference wall.

> **Previous SIMD baseline (2026-06-06, `feature/simd-consume-claude` against `/workspace/mir`
> `develop` @ `2ffebff`, `MIR_COMMIT` bumped `8864a73`→`2ffebff`):** integration
> **515 passed, 4 failed, 1 timed out, 26 skipped** on the latest capped
> `make -C src fulltest` — a session arc of 486→515 (+29): +18 from the MIR pin
> bump (un-skipping tests that needed c2mir features the old pin lacked) and +11
> from the madc SIMD frontend lowering (`DataDefSIMD` → c2mir vector type; all
> `testgccvector*`/`testsimd*` now pass, including one-lane `__int128` and the
> 32B/64B wide vectors via c2mir's scalar-lane fallback). Earlier additions:
> `testheaderstringops.mad`, `testclasscopyretbuf.mad`, and
> `teststdcppinclude.mad`, plus `testforeachheaderbody.mad` for range-for
> locals in included/header function bodies, `testexternclinkage.mad` for
> `extern "C"` linkage specs, and `testexterncstringptr.mad` for
> typedef-preserved string-pointer extern C prototypes. Embedded polyglot
> namespace headers and `<algorithm>` helpers now route calls through generated
> wrappers over explicit `extern "C"` ABI declarations, with PHP string helpers
> now importing real `_ZN3php...` C++ namespace symbols and `__php_*` kept as C
> ABI convenience wrappers. `test_mangle` covers the GCC-backed nested namespace
> symbol shape plus namespace variable symbols such as `_ZSt3cin`. `testcin.mad`
> is recovered on CIR: `std::cin` binds to the real libstdc++ global and string
> extraction delegates to real C++ iostreams. The parser/PCH checkpoint also
> rejects stale embedded PCH blobs and keeps real-header parsing on generic
> type/alias/member machinery. The known red tests are
> `testdefer.mad`, `testfortypedcomma.mad` (historically flaky fail/timeout;
> classified as `TIMEOUT` in the aggregate run),
> `testfstream.mad`, `testlargesizeofquery.mad`, and
> `testloop.mad`.
> MIR SIMD side checkpoint `c69f4da` imports the remaining 21 exact GCC vector
> torture fixtures found by the vector-construct scan, so all 37 GCC execute
> tests mentioning vector constructs are checked in and pass under C2MIR `-ei`
> and `-eg`. This follows the `59117d8` checked `__builtin_copysignf` /
> `__builtin_nan` lowering, one-lane unsigned `__int128` vector equality,
> union-array alias, leading GNU vector-attribute, `__builtin_memcmp`, and
> narrow address-taken register rvalue checkpoints. MIR SIMD side checkpoint
> `55c65ee` adds text and binary `MIR_T_V128` data I/O round-trip coverage via
> `mir-tests/scan-test.c` and `mir-tests/io.c`. MIR SIMD side checkpoint
> `e4a8945` adds direct MIR and C frontend coverage for v128 lane-count shift
> opcodes (`vlshvi*`, `vrshvi*`, `vurshvi*`) across i8/i16/i32/i64 lanes.
> MIR SIMD side checkpoint `360fdb5` extends one-lane `__int128` vector
> lowering to non-div/mod arithmetic, bitwise, unary, comparison, shift,
> compound, and GCC inc/dec operators in `c-tests/new/vector-size.c`.
> MIR SIMD side checkpoint `2ffebff` closes the final known <=16-byte SIMD
> gap by lowering one-lane signed and unsigned `__int128` vector div/mod
> through helper-call imports, with saved MIR/BMIR resolver support and
> additional `c-tests/new/vector-size.c` coverage.
> `/workspace/mir` `timeout 900 make test` passed at `2ffebff` with
> interpreter/O0 **Tests 1121, Success tests 2242** and generated-mode
> **Tests 1125, Success tests 2250** plus bootstrap checks.
> The 419/0 figures below are the
> *removed* asmjit/MIR-transpiler backend and are retained only as the C89
> coverage target the CIR path is climbing back to. ★ Milestone: SMAUG 1.8
> boots, runs as a live server, and is playable (character creation, world
> navigation, the Newgate serpent fight) through `cir_node → c2mir → MIR → JIT`.
> Canonical live state: `develop` live git and fulltest output; compiler
> warnings are release-prep blockers and should be cleaned rather than ignored.
> The clean `make -C src` rebuild on 2026-06-05 emitted no compiler warnings.
> The older parity snapshots below are retained as historical context.

Test results as of May 28, 2026 (v0.24.0, GCC parity 1649/1685 = 97.9%, 475 integration tests, 294 unit tests).

MIR default backend: 419 passed, 0 failed, 56 skipped.
All 12 _Complex tests now pass via native c2mir _Complex support (13 commits).

Run with: `bin/madc tests/<name>.mad` or `make -C src fulltest`

Operational default: when work is clearly limited to core `madc` /
  `libmadc` / parser / compiler surfaces, prefer a workspace configured
with `./configure --enable-madcdat=no` so builds and unit validation
stay on the smaller core footprint. Re-enable `madcdat` before final
validation when storage/federation code or shared surfaces may be
affected.

## Current Batch Status — 472 JIT pass / 0 fail

Latest results (2026-05-24):

### JIT mode (`scripts/run_tests.sh`)
- Passing: 452 integration tests
- Failing: none
- Note: test count previously dropped from 542 to 274 because 316 scratch/reducer files were moved to `tmp/` (gitignored). Dedicated regressions for function-pointer arrays, statement-expression member access, and nested flat struct initializers now bring the tracked integration count to 277.
  Additional tracked regressions for GNU designated initializers,
  nested designated initializers, file-scope compound-literal global
  pointers, struct-copy compound literals, union compound literals,
  nested deref post-increment, the `20060420-1.c` global array
  pointer-cast loop, `_Complex` / `iF` compatibility via
  `testcomplexkw.mad`, and VLA-sized local struct members via
  `testvlastructmember.mad`, and indirect function-pointer array calls via
  `testfnptrarraycall.mad`, plus K&R varargs function-pointer calls via
  `testkrfnptrvarargs.mad`, plus `_Complex unsigned short` compatibility via
  `testcomplexushort.mad`, plus GNU computed goto via
  `testcomputedgoto.mad`, plus the `20050502-1.c` deref-postinc read
  shape via `testderefpostincread.mad`, plus GNU `vector_size`
  compound-literal coverage via `testgccvectorlit.mad`, now bring the
  tracked integration count to 297. Additional tracked regressions for
  typedef'd VLA `sizeof(type)` handling via `testtypedefvlasizeof.mad`
  and SIMD integer/float vector cast lane preservation via
  `testgccvectorcasts.mad` now bring the tracked integration count to
  306. Additional tracked regressions for typedef'd array pointer-
  subscript decay via `testtypedefarrayptrsubscript.mad`, struct-by-
  value call copies via `teststructbyvaluecallcopy.mad`, `__real`
  address-taking via `testcomplexrealaddr.mad`, typedef-enum bitfield
  extraction via `testenumbitfieldalias.mad`, and repeated nested-
  function inline-asm barriers via `testnestedasmbarrier.mad` now bring
  the tracked integration count to 315. Additional tracked regressions
  for pure-imaginary complex literals via `testcompleximagadd.mad`,
  builtin/`~` conjugation via `testcomplexconjop.mad` and
  `testbuiltinconjf.mad`, component-wise complex `+=` via
  `testcomplexaddeq.mad`, and old-style forward declarations with
  complex-typed later definitions via `testcomplexfwddeclparams.mad`
  now bring the tracked integration count to 320. Additional tracked
  regressions for complex fixed-array decay / pointer comparison via
  `testcomplexptrcmpdecay.mad` and unsigned complex compound division
  via `testcomplexunsigneddiveq.mad` now bring the tracked integration
  count to 322. An additional tracked regression for split-line complex
  declarations plus complex truthiness in `if (c = f())` via
  `testcomplexsplitdeclcond.mad` now brings the tracked integration
  count to 323. The latest tracked regression covers embedded standard-
  header auto-inclusion for names like `size_t`, `intptr_t`, and
  `DBL_MIN` via `testautoincludestdheaders.mad`. An additional tracked
  regression for function `__alignof__` / `__attribute__((aligned(N)))`
  coverage via `testfunctionalignof.mad` now brings the tracked
  integration count to 353. An additional tracked regression for
  `long long` ternary width preservation under casts via
  `testternaryllcast.mad` now brings the tracked integration count to
  354. An additional tracked regression for C integer-promotion rules
  on signed/unsigned bitfield arithmetic via `testbitfieldpromote.mad`
  now brings the tracked integration count to 355. An additional tracked
  regression for wide unsigned bitfield arithmetic result precision via
  `testbitfieldwidearith.mad` now brings the tracked integration count
  to 356. Additional tracked regressions for GNU `optimize` attributes
  containing `-fno-strict-aliasing`, GCC byte-swap builtins,
  `__builtin_setjmp` / `__builtin_longjmp`, and integer bit-operation
  builtins plus unsigned shift-result typing now bring the tracked
  integration count to 360. An additional tracked regression for fixed-array struct assignment via
  `testfixedarraystructcopy.mad` now brings the tracked integration
  count to 329. An additional tracked regression for
  `-finstrument-functions` plus `no_instrument_function` handling via
  `testfinstrumentfunctions.mad` now brings the tracked integration
  count to 330. Additional tracked regressions for contextual `struct`
  tag parsing via `teststructtrytag.mad` and float-return indirect
  function-pointer comparisons via `testfnptrfloatretcmp.mad` now bring
  the tracked integration count to 369. Additional tracked regressions for typedef'd struct
  array aliases via `testtypedefstructarrayalias.mad`, nested
  multidimensional VLA locals via `testmultidimvla.mad`, preserving
  `defined(...)` operands in `#if` via `testifdefdefinedoperand.mad`,
  integer wrap-before-widen casts via `testuint32wrapbeforecast.mad`,
  nested VLA parameter declarators via `testnestedvlaparam.mad`,
  fixed-array struct assignment via `testfixedarraystructcopy.mad`, and
  `-finstrument-functions` / `no_instrument_function` coverage via
  `testfinstrumentfunctions.mad` now bring the tracked integration
  count to 337. Additional tracked regressions for the builtin
  `strcmp` macro cycle via `testbuiltinstrcmpmacrocycle.mad`, signed
  bitfield assignment-expression extraction via
  `testsignedbitfieldassignexpr.mad`, native string-literal subscript
  global pointer initialization via `teststrlitaddrsubscriptglobal.mad`,
  and multidimensional struct-member array decay via
  `teststructmembermultidimdecay.mad` now bring the tracked integration
  count to 341. An additional tracked regression for odd-sized local
  struct array direct/varargs pass-by-value handling via
  `testsmallstructarraycall.mad` now brings the tracked integration
  count to 342. Additional tracked regressions from the subsequent GCC
  parity slices now bring the tracked integration count to 351,
  including unsigned-char pointer string-literal coercion via
  `testucharptrstringlit.mad`, empty-template inline asm `"+r"`
  operand evaluation via `testasmrwoperand.mad`, size_t-width
  `sizeof` / `alignof` tokens via `testlargesizeofquery.mad`, exact
  decimal-real lexing via `testhexfloatcompare.mad`, global
  alias-backed array storage identity via `testglobalaliasarray.mad`,
  and scalar alias write-through via `testglobalaliasscalar.mad`.

  An additional tracked regression for file-scope `-0.0` sign
  preservation via `testnegzerostatic.mad` now brings the tracked
  integration count to 370.

  Additional tracked regressions for GNU compound-literal field
  designators via `testcompoundlitgnudesignator.mad`,
  `__builtin_types_compatible_p(...)` via
  `testbuiltintypescompatible.mad`, `__builtin_prefetch(...)` side
  effects via `testbuiltinprefetcheffects.mad`, and unsigned 32-bit to
  real coercions via `testuint32realcoerce.mad` now bring the tracked
  integration count to 364. An additional tracked regression for
  pointer-arithmetic member-address expressions like
  `&((array + 1)->field)` via `testconstaddrexprarrow.mad` now brings
  the tracked integration count to 365. An additional tracked
  regression for IEEE NaN comparisons and builtin predicates via
  `testieeefpcompare.mad` now brings the tracked integration count to
  366. An additional tracked regression for IEEE huge-value, infinity,
  finite, and NaN builtins via `testieeehugeval.mad` now brings the
  tracked integration count to 367.
  Subsequent GCC builtin and stdio regression coverage, including
  `testbuiltinunsignedabs.mad`, `testmacrovariadicfixedargs.mad`,
  `testconstptrarrayderef.mad`, and
  `teststdiobuiltinredirects.mad`, now brings the tracked integration
  count to 376.
  Additional tracked regressions for fixed-array pointer arithmetic over
  pointer elements via `teststrpbrklocal.mad` and postfix `++` / `--`
  before bitwise `&` via `testpostincbitand.mad` now bring the tracked
  integration count to 378. An additional tracked regression for GCC
  limit macros `__PTRDIFF_MAX__` and `__SIZE_MAX__` via
  `testgcclimitmacros.mad` now brings the tracked integration count to
  379.
  The std-surface cleanup updated legacy snippets to import std names
  explicitly and added `teststdstringconv.mad` for direct
  `std::string` / `std::to_string` / `std::stoi` / `std::stod`
  coverage, bringing the tracked integration count to 405. Additional
  focused regressions for volatile token-paste preservation, nested
  packed struct members, nested variadic function calls, multi-level
  pointer dereference chains, and output-only inline-asm operands bring
  the tracked integration count to 410.
  Additional focused regressions for unsigned division/modulo natural
  arithmetic type, narrow bitwise assignment, unsigned integer-to-real
  conversion, cast-precedence around unary dereference, small integer
  SIMD relational compares, SIMD scalar arithmetic splats, and explicit
  scalar-to-SIMD bitcast casts now bring the tracked integration count
  to 434.

### GCC torture sweep (`scripts/run_gcc_testsuite.py`)
- Passing: 1569 / 1685 (93.1%)
- Compile-failed: 38
- Runtime-failed: 48
- Timed out: 0
- Skipped: 30
- Current front edge: `gcc_testsuite/gcc.c-torture/execute/pr122000.c`

### Native EXE mode (`scripts/run_tests.sh --exe`)
- Passing: 434 (of 434 JIT-passing tests)
- Failing: none
- Requires: `sudo make -C src install-libmadc` and
  `LD_LIBRARY_PATH=/usr/local/lib` for libmadc.so
- The native EXE parity lane is currently fully green.
- File-scope compound literals that feed global pointer initializers now
  also relocate correctly in the EXE/AOT lane.
- The new `_Complex` arithmetic / conjugation regressions are green in
  native EXE mode too.
- A fresh `smaug.exe` probe also now survives the room 109 serpent fight
  and serpent death on the standalone executable path.

### Unit tests
- 80 datadef + 24 IR + 133 libmadc_program + 5 libmadc_error + 19 libmadc_value (261 total)
- Installed-library smoke: `make -C src libmadc-smoke` passes, staging
  `libmadc.so` plus public headers under `/tmp/madc-libstage/usr/local/`
  and then compiling/running both `tests/libmadc_cpp_smoke.cpp` and
  `tests/libmadc_c_smoke.c` against that staged install.

The latest IR-focused validation batch passes directly, including:

- `testassignexprmem.mad`
- `testcompoundassignmem.mad`
- `testderefarray.mad`
- `testassign.mad`
- `testassigninexpr.mad`
- `testc23_bool.mad`
- `testcin.mad`
- `testfnptrtypedef.mad`
- `testint.mad`
- `testpostfix.mad`
- `test_ptr_fn_deref.mad`
- `test_get_argv_deref.mad`
- `test_errno_deref.mad`
- `testfnptrmemberarrow.mad`
- `testglobalptrarrayarrow.mad`
- `testmapidentifier.mad`
- `testderefparenarrow.mad`
- `testfnptrcast.mad`
- `testcaseconstexpr.mad`
- `testneginit.mad`
- `testdupliteral.mad`
- `testderefmember.mad`
- `testdirtype.mad`
- `testternaryvalue.mad`
- `testternarystring.mad`
- `testsizeofexpr.mad`
- `testarrayc.mad`
- `testcompoundnarrow.mad`
- `teststringcast.mad`
- `teststrcmpret.mad`
- `teststrcharptrarr.mad`
- `testptrarith.mad`
- `testdoublestore.mad`
- `testdoublecompound.mad`
- `teststrarrinit.mad`
- `testsigneddiv.mad`
- `teststructptrsub.mad`
- `testfloat.mad`
- `testintsuffix.mad`
- `testdoubleptr.mad`
- `testderefeq.mad`
- `testderefcmp.mad`
- `teststructdoublecompound.mad`
- `testdoubleptrwrite.mad`
- `testfloatvarargs.mad`
- `testderefpostincstore.mad`
- `teststructcopy.mad`
- `testparenderefmember.mad`
- `testleadingdotfloat.mad`
- `testsubscriptexprmember.mad`
- `teststructarrsub.mad`
- `testrealconstfold.mad`
- `testclassident.mad`
- `testreturnnextident.mad`
- `testcompoundsubexpr.mad`
- `testnegbraceInit.mad`
- `testcharnoterm.mad`
- `testgoto.mad`
- `testmadcevalscope.mad`

## Passing Tests — 185 integration (latest batch)

`scripts/run_tests.sh` drives `testcin.mad` with piped stdin (`Alice 42
hello world`) and `testargv.mad` with argv (`hello world`), asserting
on their output instead of skipping. The runner now also reports
`TIMEOUT: tests/...` explicitly when `timeout 5` kills a spinning test,
instead of collapsing that case into a generic `FAIL`.

### New post-v0.8.0 (SMAUG Phase F regressions — hashstr.mad runs)

| Test | What it tests |
|------|--------------|
| `testincmember.mad` | Prefix/postfix inc/dec on struct members (`++ptr->links`, `obj.f--`), including if-guarded for the size-aware load/store path |
| `testunsignedcmp.mad` | Unsigned comparisons in if-conditions (setb/seta path) for short and int |
| `testglobalptr.mad` | Global pointer variable read/assign (DataDefPTR qword overrides) |
| `testsubtomember.mad` | `p->next = arr[i]` — subscript result into a struct member Mem |
| `testcastargcomma.mad` | Cast+arith as first call arg with a following comma, e.g. `strcpy((char *)h+8, "x")` |
| `testcommaincrement.mad` | `for (...; ptr = ptr->next, c++)` — SMAUG's comma-increment pattern |
| `testpostdeclstr.mad` | `char *p; p = "literal";` and `r->name = "literal";` |
| `testcoutcstr.mad` | Chained `cout << char*` output, including function-returned `char*` and mixed string-prefix chains |
| `testdeclassignexpr.mad` | Assignment as an expression inside declaration initializers (`int y = (x = 42)`) |
| `testprintfmember.mad` | Varargs wrapper calls with `->` member arguments, macro-expanded nested members, and plain `printf` mixes |
| `testprintfdouble.mad` | `%f` / `%e` / `%g` formatting through direct `printf` and `...` wrappers, including mixed args and multiple doubles |
| `testsmaug_requests.mad` | Upstream SMAUG `requests.c` compatibility test with a minimal `mud.h` shim and embedded POSIX/C headers |
| `testc23_bool.mad` | C `_Bool` keyword aliasing to madc's bool type, including scalar and fixed-array initialization |
| `teststaticassert.mad` | `_Static_assert` / `static_assert` with arithmetic, `sizeof`, and `alignof` constant expressions |
| `testalignof.mad` | `alignof` / `_Alignof` on primitive, pointer, struct, array, and member expressions |
| `testtypeof.mad` | `typeof(expr)` / `typeof(type)` driving global and local declarations |
| `testnullptr.mad` | Typed `nullptr` literal in pointer declarations and boolean tests |
| `testdigitsep.mad` | C23 digit separators in decimal, hex, binary, and floating literals |
| `testbinlit.mad` | C23-style binary integer literals (`0b...` / `0B...`) in assignments, expressions, and conditions |
| `testrestrict.mad` | `restrict` as a parsed no-op qualifier in pointer declarations and function parameters |
| `testflock.mad` | Embedded `<sys/file.h>` and `flock()`/`LOCK_*` constants via dlsym fallback |
| `testincludeonce.mad` | `#include` include-once behavior for repeated local includes within a single compile |
| `testassigninexpr.mad` | Assignment expressions used in `while` / `if` conditions and chained assignment value flow |
| `testassignexprmem.mad` | Stack-local Mem destinations on plain arithmetic / `%` expressions |
| `testcompoundassignmem.mad` | Stack-local compound assignment with Mem-backed LHS (`*=`, then `+=`) |
| `testderefarray.mad` | Unary `*` on fixed arrays (`!*buf`, `*word`) via array-to-pointer decay |
| `test_ptr_fn_deref.mad` | Dereference of a user-function `char *` return (`*get_msg()`) |
| `test_get_argv_deref.mad` | Dereference of a method-call `char *` return (`*(version.c_str())`) |
| `test_errno_deref.mad` | Dereference of builtin/external pointer-return path via `errno` / `__errno_location()` |

### New in Phase E / F session

| Test | What it tests |
|------|--------------|
| `testchain.mad` | Chained `->` and `.` member access (a->b->c, a->b.c, a.b.c) |
| `testfixedarr.mad` | C fixed-size arrays (1D + multi-dim), brace init, char* init, string-literal init |
| `teststructinit.mad` | Struct initializer lists and array-of-structs init |
| `teststructinterop.mad` | struct tm, struct timeval, struct fd_set + FD_* macros, select() |
| `testfileline.mad` | `__FILE__` / `__LINE__` predefined macros, including inside function-like macros |

### New tests added in this session

| Test | What it tests |
|------|--------------|
| `testcompoundassign.mad` | All 10 compound assignment operators (+=, -=, *=, etc.) |
| `testfortypedcomma.mad` | Typed `for` initializer with comma-separated declarations (`for (int i = 0, j = 10; ...)`) |
| `testhex.mad` | Hex integer literals (0xFF, 0xDEAD, 0X1A) |
| `testpostfix.mad` | Postfix x++/x-- with old-value-return semantics, including `for` and `while (x--)` |
| `testdefine.mad` | #define, #undef, #ifdef, #ifndef, #if, #elif, #else, #endif |
| `testlibc.mad` | dlsym fallback: getpid(), sleep(), getuid(), getppid() |
| `testmathh.mad` | #include <math.h>: M_PI, sqrt, floor, ceil, fabs, pow, sin, cos |
| `testargv.mad` | int main(int argc, char **argv) — requires cmd args (manual) |
| `teststruct3.mad` | C ABI alignment, __attribute__((packed)), mixed field sizes |
| `testsizeof.mad` | sizeof(type), sizeof(struct), sizeof in expressions |
| `testshadowlocalglobal.mad` | A local variable may shadow a same-named global without rebinding later codegen to the global slot |
| `testparamshadowglobalcharptr.mad` | A `char *` parameter may shadow a same-named global pointer without aliasing the global |
| `testaotdamageextern.mad` | AOT/native executable path keeps function-scope global pointer-table lookups stable across repeated branches, matching SMAUG `damage()`-style access |
| `testaotexternarray.mad` | AOT/native executable path preserves global struct-array layout and function-scope extern access across repeated lookups |
| `testaotsysdataextern.mad` | AOT/native executable path preserves `sysdata`-style extern struct storage and member reads across branchy control flow |
| `testbugbufbranch.mad` | Branch-skipped buffer write followed by later `strcpy`/`strcat` on the same stack buffer |
| `testsetcharcolor_noprint.mad` | Simplified `set_char_color()` `sprintf` formatting path across two color modes |
| `testsprintf4str.mad` | `sprintf` with four `%s` arguments in one formatting call |
| `testtypedefptrmemberchain.mad` | Typedef-backed pointer-member chain through `pcdata->learned[idx]` |
| `testtypedefptrmemberchain_smaugshape.mad` | Deeper SMAUG-shaped typedef pointer-member chain through a larger `CHAR_DATA` layout |
| `testvariadicterstrtwice.mad` | Repeated ternary string arguments inside a variadic `sprintf` call evaluate once per live branch |

### Notes

- `testcin.mad` is driven by `scripts/run_tests.sh` with piped stdin
- `testargv.mad` is driven by `scripts/run_tests.sh` with argv
- `include_helper.mad` is not standalone (included by testinclude.mad)
- `include_once_helper.mah` is not standalone (included by testincludeonce.mad)
- All tests that use `cout`/`cin`/`cerr`/`endl` now require
  `#include <iostream>` plus explicit `std::` qualification or `using`
  import.

## Previously Passing Tests — 54/54 integration + 25/25 unit

| Test | What it tests | Output |
|------|--------------|--------|
| `test.mad` | String variable, puts() | Prints string |
| `test2.mad` | Large loop (100M iterations) | `100000000` |
| `test3.mad` | Basic program structure | Runs silently |
| `test4.mad` | Char literals, putchar(), user-defined string funcs | `Hello, World!`, `hi`, `test`, `Hello, World!`, `HEY`, `hey 123`, `v0.0.1` |
| `test5.mad` | String ops | `Hello, World!`, `hi` |
| `testassign.mad` | Variable assignment | `456` |
| `testbsl.mad` | Bit shift operators (`<<` and `>>`) | `200`, `40`, `16`, `15` |
| `testcout.mad` | cout stream output | `This is a test, x = -1` |
| `testfor.mad` | For loop | `a == 5` |
| `testfunc.mad` | User-defined functions | `10`, `15` |
| `testif.mad` | If/else | `this is a test` |
| `testif2.mad` | If with integer condition | `1` |
| `testinc.mad` | Increment/decrement | `1`, `0` |
| `testint.mad` | Integer types, assignment | `123: 123`, `i: 456`, `j: 456` |
| `testlocal.mad` | Local string variable | `Hello, World!` |
| `testmath.mad` | Integer arithmetic | `0`, `-2` |
| `testmath2.mad` | More arithmetic | `15`, `5` |
| `testnot.mad` | Bitwise NOT | `-2`, `-1` |
| `testprint.mad` | String print | `Hello, World!` |
| `testreturn.mad` | Function return values | `100`, `101` |
| `testsstream.mad` | Stringstream | `456`, `123`, `5`, stream content, `This is a test to cout: 5` |
| `teststruct.mad` | Struct member access | `test.name: Joe Blow`, `test.id: 2`, `test.age: d` (uint8=char in stream) |
| `testversion.mad` | Version string | `v0.0.1` |
| `testns.mad` | Namespace resolution (std::) | `Hello from std::cout!`, `x = 42`, stderr output, `using namespace std` imports unqualified stream names |
| `testphp.mad` | php:: namespace functions | trim/ltrim/rtrim, ucfirst/lcfirst, str_replace, str_repeat, explode/implode, sort, nested-array `array_column` |
| `teststruct2.mad` | User-defined structs | `p.x: 10`, `p.y: 20`, `bob.name: Bob Smith`, `bob.age: 42`, `bob.id: 1001` |
| `testclass.mad` | Class definitions with data members | `p.x: 100`, `p.y: 200`, `bob.name: Bob`, `bob.age: 30` |
| `testinclude.mad` | `#include` directive | `Hello, World!`, `Hello, Mad-C!`, `include works!` |
| `testusing.mad` | `using namespace std` | `using namespace std works!` |
| `testwhile.mad` | While loop | `100000000` |
| `testcapture.mad` | Lambda capture of outer variables | Captured values printed |
| `testcin.mad` | `cin >>` input from stdin | Reads and echoes user input (needs stdin) |
| `testcolon.mad` | `:=` short variable declaration (Go-style type inference) | Inferred-type variables |
| `testdefer.mad` | `defer` statement (Go-style deferred execution) | Deferred output at scope exit |
| `testdlcall.mad` | `dlcall()` through function pointer | Calls C library function via pointer |
| `testdlopen.mad` | `dlopen`/`dlsym`/`dlclose` | Loads and calls shared library symbols |
| `testescape.mad` | Escape sequences in string literals (`\n`, `\t`, etc.) | Formatted output with escapes |
| `testforeach.mad` | Range-based `for (type var : array)` | Iterates over MadArray elements |
| `testforeach2.mad` | Range-based for with STL containers | Iterates over vector/map/set |
| `testfstream.mad` | File I/O with ifstream/ofstream/fstream | Read/write file operations |
| `testfuncptr.mad` | Function pointers via `auto fn = func` | Calls through stored function pointer |
| `testlambda.mad` | Lambda expressions with `auto` and `[]` | Defines and calls inline lambdas |
| `testlang.mad` | Multi-language namespace usage in one program | php/perl/python/ruby/js functions together, including `ruby::chars` |
| `testloop.mad` | Loop constructs (for, while, do-while) | Various loop patterns |
| `testmadc_ns.mad` | `madc::` namespace (regex, array) | madc::regex_match, regex_search, regex_replace |
| `testmap.mad` | `map<K,V>` typed STL container | Insert, find, erase, iterate |
| `testmethod.mad` | Class methods with `this` pointer | Method call compiles and dispatches |
| `testmultiret.mad` | Multiple return values (Go-style) | Function returns multiple values via `__retbuf`; runtime output asserted via `.expect` |
| `testprefer.mad` | Namespace precedence directives | `prefer rust, c;` and `#pragma prefer rust, c` change bare identifier lookup order |
| `testrust.mad` | rust:: namespace helpers | trim/contains/replace, split/join, first/last/get, push/pop |
| `testrubycharsshadow.mad` | Namespace-call argument shadowing | `ruby::chars(chars, s)` resolves local arg, not namespace function |
| `testperl.mad` | perl:: namespace functions | chop, chomp, split, join, grep, glob |
| `testregex.mad` | Regex functions (match, search, replace) | Pattern matching and substitution |
| `testset.mad` | `set<T>` typed STL container | Insert, find, erase, iterate |
| `testsubscript.mad` | `[]` subscript operator on strings and containers | Indexed access |
| `testswitch.mad` | `switch`/`case`/`default` statement | Branch selection by value |
| `testternary.mad` | Ternary operator (`cond ? a : b`) | Conditional expression |
| `testvector.mad` | `vector<T>` typed STL container | push_back, size, at, iterate |

## Phase 1 Fixes Applied

| Fix | Status | Details |
|-----|--------|---------|
| `-v/--verbose` flag | ✓ Done | `DBG()` macro gated on `madc_verbose`; parse `-v`/`--verbose` in `main()` |
| Char literal compile | ✓ Done | Added `TokenChar::compile()` and `operand()`; `case ttChar:` in `TokenBase::compile()` |
| Error reporting | ✓ Done | `throwbuf::sync()` prints before throwing; catch block was correct |
| Struct member access | ✓ Done | Fixed `addOffset` vs `setOffset`; load numeric members into Gp; LEA for string members; construct/destruct string members in struct |
| `register` keyword | ✓ Done | Added `vfREGISTER` flag, `TokenREGISTER` token, parsed in `TokenREGISTER::parse()` |
| doctest framework | ✓ Done | `include/doctest.h`, `tests/unit/test_datadef.cpp` (25 tests), `make test` |

## Unit Tests

Run with: `make -C src test`

| Test File | Tests | Status |
|-----------|-------|--------|
| `tests/unit/test_datadef.cpp` | 25 | All pass |
| `tests/unit/test_ir.cpp`      | 23 | All pass — IR Stage 0 scaffolding + Stage 1/2 coerce coverage |

## Phase 2 Fixes Applied

| Fix | Status | Details |
|-----|--------|---------|
| String parameter pass-by-ref | ✓ Done | `voperand()` creates bare Gp for `vfPARAM` non-numeric vars; `cleanup()` skips param destruction |
| `dtSTRING → dtCHARptr` coercion | ✓ Done | `string_cstr()` helper auto-converts string args to `const char*` when calling `puts()` etc. |
| User-defined structs (2.1) | ✓ Done | `TokenSTRUCT::parse()` parses `struct Name { type member; ... };`, builds `DataDefSTRUCT` dynamically, registers in `struct_map` |
| Namespace resolution (2.3+2.4) | ✓ Done | `namespace_map` registry, `::` resolution in `parseExpression()`, `std::` namespace with cout/cerr/endl |
| `#include` + `using` (2.5) | ✓ Done | Lexer handles `#include "file.mad"` with relative paths; parser handles `using namespace X;` and `using X::member;` |
| Class definitions (2.2) | ✓ Done | Data members, `class Name { ... };` syntax, type registered in `datatype_map` for prefix-free use |

## Known Issues

- String pass-by-value is implemented as pass-by-reference (caller's string is shared, not copied)
- No true string copy semantics yet for function parameters
