#!/bin/bash
# DRIFT-PREVENTION GATE -- a speculative scope (a SFINAE probe, a constraint,
# a constant fold with a fallback, a tsubst attempt) has ONE owner:
# Program::SilentReplay (include/madc.h). It sets DiagnosticRenderMute, so a
# diagnostic raised inside is never RENDERED; it points std::cerr at a null
# buffer for any direct write; it snapshots the diagnostic state for rewind().
#
# The scope was hand-rolled at sixteen sites as a swap of std::cerr to a
# discarding streambuf (three such streambuf classes existed). A swap without
# the mute still renders every discarded diagnostic in full -- the header
# line, the screen column and the source echo, each echo re-reading an
# #included header from disk up to the cited line -- and throws the text
# away: 1.6% of a libstdc++-header compile (callgrind, 2026-10-08).
#
# Rule, over src/*.cpp and include/*.h: no discarding streambuf (an
# `overflow(int c)` that returns c), no std::cerr.rdbuf(&object) swap and no
# std::cerr.setstate() outside the owner's lines (marked
# `// allowed-exception: the owner`). Redirecting cerr to a REAL buffer (the
# engine's capture and tee buffers, by pointer) is output routing, not
# silencing, and is not matched. A comment line is skipped.
# Two-sided: the negative control proves the rule still bites.
set -u
cd "$(dirname "$0")/.."

scan() {
	awk '
	function code(s) { return s !~ /^[[:space:]]*\/\// }
	{
		if (!code($0) || $0 ~ /allowed-exception: the owner/)
			next
		if ($0 ~ /overflow[[:space:]]*\(int[[:space:]]+[A-Za-z_]+\)[^{]*\{[[:space:]]*return[[:space:]]+[A-Za-z_]+;[[:space:]]*\}/)
			print FILENAME ":" FNR ": a discarding streambuf"
		else if ($0 ~ /cerr[.]rdbuf\(&/)
			print FILENAME ":" FNR ": std::cerr silenced by hand"
		else if ($0 ~ /cerr[.]setstate\(/)
			print FILENAME ":" FNR ": std::cerr silenced by hand"
	}' "$@"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
    int overflow(int c) override { return c; }
	{ int overflow(int c) { return c; } };
    std::cerr.rdbuf(&g_madc_null_streambuf);
	std::cerr.setstate(std::ios::badbit);
    int overflow(int c) override { return c; }   // allowed-exception: the owner
    // a comment line is skipped: std::cerr.rdbuf(&null_buf);
	std::cerr.rdbuf(eb);
    std::cerr.rdbuf(error_tee_buf.get());
    SilentReplay quiet(*this);
CTL
c=$(scan "$ctl" | grep -c .)
if [ "$c" -ne 4 ]; then
	echo "check-one-silent-replay: NEGATIVE CONTROL FAILED -- matched $c of 4"
	scan "$ctl"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'include/*.h')
hits=$(scan $files)
n=$(printf '%s' "$hits" | grep -c . || true)
echo "speculative scopes outside Program::SilentReplay: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$hits"
	echo "  -> open the scope with Program::SilentReplay (include/madc.h): it"
	echo "     mutes rendering, silences cerr, snapshots the diagnostics;"
	echo "     rewind() drops them, end() closes the scope early."
	exit 1
fi
echo "GREEN -- a speculative scope has one owner."
