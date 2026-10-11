# test_fixtures.sh — THE reader of a test's run fixtures (.claude/rules/
# test-fixtures.md): .flags .argv .env .input .timeout, and the hermetic flag
# every suite run passes. Sourced (never run) by each script that runs or
# compiles tests/*.mad the way the suite does: run_tests.sh, warn_census.sh,
# forest_soak.sh, class_parse_burndown.sh.
#
# Hand-copies of this reading drifted twice — the .helper marker (six copies,
# docs/rules/test-fixtures.md), then .env: warn_census.sh replayed
# testrepl_opencmd without its EDITOR=echo and the REPL's %edit started the
# real vi on the test's input. scripts/check-one-fixture-reader.sh fails any
# script that reads a fixture by hand.
#
#   fixture_load DIR BASE   for the test DIR/BASE.mad, sets
#       fx_flags fx_args fx_envs   arrays: .flags, .argv, .env (whitespace-
#                                  split; .env's NAME=value pairs go to env(1)
#                                  ahead of every invocation of the test)
#       fx_input                   DIR/BASE.input, or "" (no stdin fixture)
#       fx_timeout                 .timeout's seconds, else
#                                  FIXTURE_DEFAULT_TIMEOUT
#   FIXTURE_HERMETIC_FLAGS        --no-config: no ambient madc.ini (repo root,
#                                  the developer's or the system's config dir)
#                                  changes a test's dialect or include path

# Per-test wall-clock cap (seconds). A `.timeout` fixture overrides it for a
# test that legitimately needs longer — a real-libstdc++-header compile (no
# PCH yet) parses the full <iostream> closure, and the default build is -O0.
# ONE cap for every lane of a test: the JIT, EXE and .o runs all read it.
FIXTURE_DEFAULT_TIMEOUT=10
FIXTURE_HERMETIC_FLAGS="--no-config"

fixture_load() {
	local dir=$1 base=$2
	fx_flags=()
	fx_args=()
	fx_envs=()
	fx_input=""
	fx_timeout=$FIXTURE_DEFAULT_TIMEOUT
	if [ -f "$dir/$base.flags" ]; then
		read -r -a fx_flags < "$dir/$base.flags"
	fi
	if [ -f "$dir/$base.argv" ]; then
		read -r -a fx_args < "$dir/$base.argv"
	fi
	if [ -f "$dir/$base.env" ]; then
		read -r -a fx_envs < "$dir/$base.env"
	fi
	if [ -f "$dir/$base.input" ]; then
		fx_input="$dir/$base.input"
	fi
	if [ -f "$dir/$base.timeout" ]; then
		read -r fx_timeout < "$dir/$base.timeout"
	fi
	return 0
}
