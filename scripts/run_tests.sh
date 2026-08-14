#!/bin/bash
# Run all integration tests.
#
# Convention: each .mad test carries its own fixtures, so this runner
# stays generic. For a test tests/foo.mad, the runner will use:
#   tests/foo.flags  — whitespace-split compiler flags prepended before
#                      the source path
#   tests/foo.input  — redirected to stdin if present
#   tests/foo.argv   — each whitespace-separated word appended as argv
#   tests/foo.expect — if present, the test must exit 0 AND produce
#                      output that contains every line listed here as
#                      a substring. Otherwise exit-0 is enough.
#   tests/foo.expect_err — marks a compile-error test: madc must exit
#                      NONZERO (not a timeout) and its diagnostics
#                      (stderr) must contain every line listed here as
#                      a substring. The EXE pass skips these — the
#                      source does not compile by design.
#   tests/foo.expect_quiet — if present (content ignored), the JIT run
#                      must produce EMPTY stderr — locks diagnostic
#                      hygiene (no leaked warnings/errors) for tests
#                      that compile real system headers.
#   tests/foo.<domain>_expect — replaces foo.expect when MADC_SKIP_EXT
#                      includes <domain> (first listed domain wins): for
#                      tests whose CORRECT output differs on the domain
#                      (sizeof(long)-derived values on win64); content
#                      comes from the domain's oracle compiler.
#
# No test names are hard-coded here.
#
# Options:
#   --exe   Also compile each test to a native executable and run it.
#           Failures are reported as "FAIL(exe): ..." separately.
#   --obj   Also compile each test to a relocatable .o and execute it via
#           the in-process loader (`madc foo.o` — the precompiled-cache
#           lane). Failures are reported as "FAIL(obj): ..." separately.
#
#   --stdlib=NAME
#           Run the whole suite against a NON-DEFAULT C++ standard library
#           flavor (`--stdlib=libc++`). This is the parity lane: the measure of
#           whether a flavor behaves like the default one is the same suite
#           passing under it, not a handful of flavor-specific fixtures. A test
#           that is structurally out of scope for the flavor carries
#           tests/<base>.<flavor>_skip (`+` written as `x`, so libc++ ->
#           .libcxx_skip) with one line saying why. The summary is labelled so a
#           flavored run can never be quoted as the default-lane baseline.
#
# MADC_BIN (env): the madc binary to test (default bin/madc). Generic
# runner capability — lets the suite run against e.g. a forest-packed
# copy (tmp/madc_packed) without touching the tree's binary.
#
# MADC_WRAPPER (env): command prefix that runs the binary on its execution
# domain — `wine` for the PE madc.exe on the build container. Word-split
# deliberately (a wrapper may carry flags). Same generic-capability rule
# as MADC_BIN: never a per-test hook. CRLF note: win64 stdout is CRLF in
# text mode (gcc-parity-correct platform behavior); the .expect model is
# per-line SUBSTRING match (grep -F), which tolerates the trailing \r —
# fixtures stay LF, no normalization layer needed.
RUN_EXE=0
RUN_OBJ=0
BACKEND_FLAG=""
# Flags passed to EVERY madc invocation below, including the AOT compile legs
# (which deliberately do not take $BACKEND_FLAG).
#
# --no-config is hermeticity (forest-carriers S6): the suite must not be
# perturbed by an ambient madc.ini — one in the repo root, in the developer's
# config dir, or installed in the system config dir would silently change every
# test's dialect or include path.
#
# --stdlib=NAME appends the FLAVORED-LANE flag here, which is why every leg of
# every pass picks it up without touching a single invocation line.
HERMETIC_FLAGS="--no-config"
STDLIB_NAME=""
STDLIB_SKIP_EXT=""
MADC="${MADC_BIN:-bin/madc}"
# Export the resolved default too: exec-channel fixtures that spawn madc as
# their child must exercise this lane's artifact, including the default lane
# where the caller did not need to set MADC_BIN explicitly.
export MADC_BIN="$MADC"
MADC_WRAPPER="${MADC_WRAPPER:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) RUN_EXE=1; shift ;;
        --obj) RUN_OBJ=1; shift ;;
        --backend=*) BACKEND_FLAG="$1"; shift ;;
        --stdlib=*) STDLIB_NAME="${1#--stdlib=}"; shift ;;
        *) break ;;
    esac
done
if [ -n "$STDLIB_NAME" ]; then
    HERMETIC_FLAGS="$HERMETIC_FLAGS -stdlib=$STDLIB_NAME"
    # Per-flavor skip fixture, discovered by CONVENTION like every other
    # fixture: the flavor's own spelling with `+` written as `x`, matching how
    # this tree already names its flavored tests (tests/testcommontype_libcxx.mad
    # for -stdlib=libc++). So `libc++` -> tests/<base>.libcxx_skip. A mechanical
    # transform, not a per-flavor table — a third flavor needs no runner change.
    STDLIB_SKIP_EXT=$(printf '%s' "$STDLIB_NAME" | tr '+' 'x')
fi

# MADC_SKIP_EXT (env): the execution-DOMAIN twin of the stdlib skip lane —
# tests/<base>.${ext}_skip marks a test structurally out of scope for the
# domain the binary under test targets (win64 lanes run with
# MADC_SKIP_EXT=win64; a fixture's one line says why, e.g. "POSIX sockets —
# mingw-gcc rejects sys/socket.h too"). A whitespace-separated LIST layers
# domains: a wine run of the win64 binary is two domains at once
# (MADC_SKIP_EXT="win64 wine64" — wine64 fixtures mark wine-environment
# limits like unimplemented ucrtbase stubs that PASS on real Windows).
# Same convention rules as every fixture: never a per-test branch in the
# runner, and the summary is labelled so a domain run can't be quoted as
# the default-lane baseline.
MADC_SKIP_EXT="${MADC_SKIP_EXT:-}"

# Remaining positional arguments are basename GLOBS selecting a SUBSET of the
# suite: `run_tests.sh 'testmadceval*' testevalexterncapture`. No test name is
# hard-coded here — the caller supplies the pattern, so this stays a generic
# runner capability (.claude/rules/test-fixtures.md). With no globs the whole
# suite runs, which is what every pre-merge invocation does.
TEST_GLOBS="$*"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")"; pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.."; pwd -P)
EXE_LD_LIBRARY_PATH="$REPO_ROOT/lib:/usr/local/lib"

PASS=0
FAIL=0
TIMEOUTS=0
SKIP=0
EXE_PASS=0
EXE_FAIL=0
OBJ_PASS=0
OBJ_FAIL=0
SELECTED=0
STDLIB_SKIPPED=0
DOMAIN_SKIPPED=0
for t in tests/*.mad; do
    base=$(basename "$t" .mad)
    [ "$base" = "include_helper" ] && continue
    if [ -n "$TEST_GLOBS" ]; then
        keep=0
        for g in $TEST_GLOBS; do
            case "$base" in $g) keep=1; break ;; esac
        done
        [ $keep -eq 0 ] && continue
    fi
    SELECTED=$((SELECTED+1))

    input_file="tests/$base.input"
    argv_file="tests/$base.argv"
    expect_file="tests/$base.expect"
    expect_err_file="tests/$base.expect_err"
    expect_quiet_file="tests/$base.expect_quiet"
    flags_file="tests/$base.flags"
    mir_skip_file="tests/$base.mir_skip"
    timeout_file="tests/$base.timeout"

    # Skip tests marked as not transpilable (MIR is the default backend)
    if [ -f "$mir_skip_file" ]; then
        SKIP=$((SKIP+1))
        continue
    fi

    # Flavored lane: a test structurally out of scope for THIS stdlib flavor
    # (content = one line saying why). Only consulted when --stdlib= selected a
    # flavor, so the default lane is untouched.
    if [ -n "$STDLIB_SKIP_EXT" ] && [ -f "tests/$base.$STDLIB_SKIP_EXT""_skip" ]; then
        SKIP=$((SKIP+1))
        STDLIB_SKIPPED=$((STDLIB_SKIPPED+1))
        continue
    fi

    # Execution-domain lane (MADC_SKIP_EXT, e.g. win64, or a list like
    # "win64 wine64"): same contract as the flavored skip; only consulted
    # when the caller selected a domain, so every default-lane run is
    # untouched.
    domain_skip=0
    for skip_ext in $MADC_SKIP_EXT; do
        if [ -f "tests/$base.$skip_ext""_skip" ]; then
            domain_skip=1
            break
        fi
    done
    if [ $domain_skip -eq 1 ]; then
        SKIP=$((SKIP+1))
        DOMAIN_SKIPPED=$((DOMAIN_SKIPPED+1))
        continue
    fi

    # Execution-domain expect VARIANT: a test whose CORRECT output differs on
    # the domain (e.g. sizeof(long)-derived values on win64, oracle =
    # mingw-gcc) carries tests/<base>.<domain>_expect; the first domain in
    # MADC_SKIP_EXT order wins. Only consulted on a domain run, so the
    # default lane never sees it.
    for skip_ext in $MADC_SKIP_EXT; do
        if [ -f "tests/$base.$skip_ext""_expect" ]; then
            expect_file="tests/$base.$skip_ext""_expect"
            break
        fi
    done

    args=()
    flags=()
    [ -f "$flags_file" ] && read -r -a flags < "$flags_file"
    [ -f "$argv_file" ] && read -r -a args < "$argv_file"
    # Per-test wall-clock cap (seconds); default 10. A `.timeout` fixture overrides
    # it for tests that legitimately need longer — e.g. a real-libstdc++-header
    # compile (no PCH yet) parses the full <iostream> closure. The default build
    # is -O0 (optimization is a last-lap switch), so header-heavy compiles need
    # headroom. Generic filename convention, never a per-test branch in the runner.
    tmo=10
    [ -f "$timeout_file" ] && read -r tmo < "$timeout_file"

    if [ -f "$expect_err_file" ]; then
        # Compile-error test: capture stderr — the diagnostics ARE the
        # expected output.
        if [ -f "$input_file" ]; then
            out=$(timeout "$tmo" $MADC_WRAPPER "$MADC" $HERMETIC_FLAGS $BACKEND_FLAG "${flags[@]}" "$t" "${args[@]}" < "$input_file" 2>&1)
        else
            out=$(timeout "$tmo" $MADC_WRAPPER "$MADC" $HERMETIC_FLAGS $BACKEND_FLAG "${flags[@]}" "$t" "${args[@]}" 2>&1)
        fi
        rc=$?
        ok=1
        timed_out=0
        if [ $rc -eq 124 ]; then
            ok=0
            timed_out=1
        elif [ $rc -eq 0 ]; then
            ok=0
        else
            while IFS= read -r line; do
                [ -z "$line" ] && continue
                if ! grep -qF -- "$line" <<< "$out"; then
                    ok=0
                    break
                fi
            done < "$expect_err_file"
        fi
    else
        # stderr goes to a scratch file so an .expect_quiet fixture can
        # assert it is empty; without the fixture it is simply discarded.
        errf="/tmp/madc_test_stderr_${base}_$$"
        if [ -f "$input_file" ]; then
            out=$(timeout "$tmo" $MADC_WRAPPER "$MADC" $HERMETIC_FLAGS $BACKEND_FLAG "${flags[@]}" "$t" "${args[@]}" < "$input_file" 2>"$errf")
        else
            out=$(timeout "$tmo" $MADC_WRAPPER "$MADC" $HERMETIC_FLAGS $BACKEND_FLAG "${flags[@]}" "$t" "${args[@]}" 2>"$errf")
        fi
        rc=$?

        ok=1
        timed_out=0
        if [ $rc -eq 124 ]; then
            ok=0
            timed_out=1
        elif [ $rc -ne 0 ]; then
            ok=0
        else
            if [ -f "$expect_file" ]; then
                while IFS= read -r line; do
                    [ -z "$line" ] && continue
                    # Each expected line must appear somewhere in the output.
                    if ! grep -qF -- "$line" <<< "$out"; then
                        ok=0
                        break
                    fi
                done < "$expect_file"
            fi
            if [ $ok -eq 1 ] && [ -f "$expect_quiet_file" ] && [ -s "$errf" ]; then
                echo "NOISY(stderr): $t"
                ok=0
            fi
        fi
        rm -f "$errf"
    fi

    if [ $ok -eq 1 ]; then
        PASS=$((PASS+1))
    else
        if [ $timed_out -eq 1 ]; then
            echo "TIMEOUT: $t"
            TIMEOUTS=$((TIMEOUTS+1))
        else
            echo "FAIL: $t"
            FAIL=$((FAIL+1))
        fi
    fi

    # OBJ pass: compile to ONE relocatable .o (-r, the gcc/ld -r shape —
    # a multi-TU --project program becomes one whole-program .o), then
    # execute it through the in-process loader (`madc foo.o` — the
    # precompiled-cache lane). The exe_skip fixture covers ALL
    # native-artifact lanes: a structurally JIT-only test is skipped here
    # too. obj_skip marks tests outside the .o domain entirely (the EXE
    # lane still covers them). Fixture flags are passed to both
    # invocations — the compile needs them, and the run honors -l (the
    # dlopen happens before the .o dispatch); --project is compile-only
    # (the run executes the finished artifact) so it is dropped from the
    # replayed run flags.
    if [ $RUN_OBJ -eq 1 ] && [ $ok -eq 1 ] && [ ! -f "$expect_err_file" ] \
       && [ ! -f "tests/$base.exe_skip" ] && [ ! -f "tests/$base.obj_skip" ]; then
        obj_path="/tmp/madc_test_obj_${base}.o"
        run_flags=()
        skip_next=0
        for fl in "${flags[@]}"; do
            if [ $skip_next -eq 1 ]; then skip_next=0; continue; fi
            if [ "$fl" = "--project" ]; then skip_next=1; continue; fi
            run_flags+=("$fl")
        done
        # -o BEFORE fixture flags — same positional rule as the EXE pass.
        if $MADC_WRAPPER "$MADC" $HERMETIC_FLAGS -r -o "$obj_path" "${flags[@]}" "$t" >/dev/null 2>&1; then
            if [ -f "$input_file" ]; then
                obj_out=$(timeout 5 $MADC_WRAPPER "$MADC" $HERMETIC_FLAGS "${run_flags[@]}" "$obj_path" "${args[@]}" < "$input_file" 2>/dev/null)
            else
                obj_out=$(timeout 5 $MADC_WRAPPER "$MADC" $HERMETIC_FLAGS "${run_flags[@]}" "$obj_path" "${args[@]}" 2>/dev/null)
            fi
            obj_rc=$?
            obj_ok=1
            if [ $obj_rc -ne 0 ]; then
                obj_ok=0
            elif [ -f "$expect_file" ]; then
                while IFS= read -r line; do
                    [ -z "$line" ] && continue
                    if ! grep -qF -- "$line" <<< "$obj_out"; then
                        obj_ok=0
                        break
                    fi
                done < "$expect_file"
            fi
            if [ $obj_ok -eq 1 ]; then
                OBJ_PASS=$((OBJ_PASS+1))
            else
                echo "FAIL(obj): $t"
                OBJ_FAIL=$((OBJ_FAIL+1))
            fi
            rm -f "$obj_path"
        else
            echo "FAIL(obj-build): $t"
            OBJ_FAIL=$((OBJ_FAIL+1))
        fi
    fi

    # EXE pass: compile to native and run. tests/foo.exe_skip marks a test
    # as structurally JIT-only (freeze re-exec machinery, in-process host
    # callbacks) — skipped here, not counted as an exe failure.
    if [ $RUN_EXE -eq 1 ] && [ $ok -eq 1 ] && [ ! -f "$expect_err_file" ] \
       && [ ! -f "tests/$base.exe_skip" ]; then
        exe_path="/tmp/madc_test_exe_${base}"
        # -o BEFORE the fixture flags: a positional .json manifest (project
        # auto-detect) ends madc's flag parsing — everything after it is the
        # program's argv, so a trailing -o would never reach madc.
        if $MADC_WRAPPER "$MADC" $HERMETIC_FLAGS -o "$exe_path" "${flags[@]}" "$t" >/dev/null 2>&1; then
            if [ -f "$input_file" ]; then
                exe_out=$(env LD_LIBRARY_PATH="$EXE_LD_LIBRARY_PATH" timeout 5 "$exe_path" "${args[@]}" < "$input_file" 2>/dev/null)
            else
                exe_out=$(env LD_LIBRARY_PATH="$EXE_LD_LIBRARY_PATH" timeout 5 "$exe_path" "${args[@]}" 2>/dev/null)
            fi
            exe_rc=$?
            exe_ok=1
            if [ $exe_rc -ne 0 ]; then
                exe_ok=0
            elif [ -f "$expect_file" ]; then
                while IFS= read -r line; do
                    [ -z "$line" ] && continue
                    if ! grep -qF -- "$line" <<< "$exe_out"; then
                        exe_ok=0
                        break
                    fi
                done < "$expect_file"
            fi
            if [ $exe_ok -eq 1 ]; then
                EXE_PASS=$((EXE_PASS+1))
            else
                echo "FAIL(exe): $t"
                EXE_FAIL=$((EXE_FAIL+1))
            fi
            rm -f "$exe_path"
        else
            echo "FAIL(exe-build): $t"
            EXE_FAIL=$((EXE_FAIL+1))
        fi
    fi
done
if [ -n "$TEST_GLOBS" ]; then
    # Never let a filtered run read as a full one. A subset that says
    # only "N passed" is indistinguishable from the suite at a glance,
    # and that is exactly how a partial run gets quoted as a baseline.
    echo "SUBSET RUN — filter: $TEST_GLOBS ($SELECTED of $(ls tests/*.mad | wc -l) tests; NOT a suite baseline)"
fi
if [ -n "$STDLIB_NAME" ]; then
    # Never let a FLAVORED run read as the default-lane baseline. The two lanes
    # measure different libraries and legitimately have different skip sets.
    echo "FLAVORED RUN — -stdlib=$STDLIB_NAME ($STDLIB_SKIPPED test(s) carry a .${STDLIB_SKIP_EXT}_skip); NOT the default-lane baseline"
fi
if [ -n "$MADC_SKIP_EXT" ]; then
    echo "DOMAIN RUN — $MADC_SKIP_EXT ($DOMAIN_SKIPPED test(s) carry a domain skip fixture); NOT the default-lane baseline"
fi
echo "$PASS passed, $FAIL failed, $TIMEOUTS timed out, $SKIP skipped"
if [ $RUN_EXE -eq 1 ]; then
    echo "EXE: $EXE_PASS passed, $EXE_FAIL failed (of $PASS JIT-passing tests)"
fi
if [ $RUN_OBJ -eq 1 ]; then
    echo "OBJ: $OBJ_PASS passed, $OBJ_FAIL failed (of $PASS JIT-passing tests)"
fi
[ $FAIL -eq 0 ] || exit 1
[ $TIMEOUTS -eq 0 ] || exit 1
# The EXE and OBJ lanes gate the exit status too. They were PRINTED and then
# ignored: `--obj` reported "OBJ: 3 passed, 1 failed" and exited 0, so
# remote_build's stage summary said `tests obj rc=0` and a battery could be red
# in either native lane while reporting `total rc=0`. Same false-green shape as
# piping a suite through `tail` — the verdict came from a line nobody read
# instead of the exit code. A lane that did not run has a 0 counter, so these
# are unconditional.
[ $EXE_FAIL -eq 0 ] || exit 1
[ $OBJ_FAIL -eq 0 ] || exit 1
