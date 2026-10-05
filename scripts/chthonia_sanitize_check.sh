#!/bin/bash
# chthonia_sanitize_check.sh — Chthonia carries no identification of AI
# assistance (docs/plans/2026-10-04-chthonia-own-repository.md §5, owner
# 2026-10-04). It lives in madc, so the Chthonia tree holds no list of what
# it avoids.
#
#   scripts/chthonia_sanitize_check.sh
#       madc's moving set (tools/chthonia/, tools/madcide/plugins/chthonia/):
#       the files that become Chthonia's repository (run by `make gates`)
#   scripts/chthonia_sanitize_check.sh --checkout DIR
#       a Chthonia checkout, before every push: its tracked files, the agent
#       files it must not track, and every commit message
#   scripts/chthonia_sanitize_check.sh --message FILE
#       one commit message (a Chthonia checkout's local commit-msg hook)
#
# Fails on: an assistant or tool name, an attribution or session line,
# madc's rule trailers in a message, a reference to madc's plans or their
# sections, a dated owner ruling, and (a checkout) a tracked agent
# instruction file.
#
# Negative control: a synthetic file with an attribution line, and a message
# with a rule trailer, must FAIL; plain project text must PASS — else the
# gate itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

# Text that identifies an assistant, its sessions, or madc's process.
CONTENT='\b(Claude|Anthropic|Codex|OpenAI|ChatGPT|GPT-[0-9]|Gemini|Copilot|Aider|Windsurf|LLM|AI)\b|Co-Authored-By|[Gg]enerated with|claude\.ai|docs/plans/|plan §|§[0-9]|\([Oo]wner,? 20[0-9][0-9]|OWNER LAW'
# A commit message also carries none of madc's rule trailers.
TRAILERS='^(Hypothesis|Layer|Searched|Oracle):'
# Agent instruction files a checkout must not track.
AGENT_FILES='^(AGENTS\.md|CLAUDE\.md|GEMINI\.md|\.claude/|\.cursor/|\.windsurfrules|\.aider\.conf\.yml|\.github/copilot-instructions\.md)'

# scan_files FILE... — prints each identifying line; 0 = clean.
scan_files() {
	local out
	out=$(grep -n -H -I -E "$CONTENT" "$@" 2>/dev/null)
	[ -z "$out" ] && return 0
	echo "$out"
	return 1
}

# scan_message FILE — prints each identifying line of one message; 0 = clean.
scan_message() {
	local out
	out=$(grep -n -E "$CONTENT|$TRAILERS" "$1" 2>/dev/null)
	[ -z "$out" ] && return 0
	echo "$out"
	return 1
}

# --- negative controls -------------------------------------------------------
tmpd=$(mktemp -d)
printf 'int main(void) { return 0; }\n// Co-Authored-By: someone\n' > "$tmpd/bad.c"
printf 'Fix the toolbar.\n\nLayer: the menu reader\n' > "$tmpd/bad.msg"
printf '// The toolbar: its buttons, each with its picture.\nint main(void) { return 0; }\n' > "$tmpd/good.c"
printf 'Fix the toolbar.\n\nThe Run button drops its menu down.\n' > "$tmpd/good.msg"
control() {
	rm -rf "$tmpd"
	echo "chthonia_sanitize_check: CONTROL FAILED — $1" >&2
	exit 2
}
scan_files "$tmpd/bad.c" > /dev/null && control "an attribution line passed"
scan_message "$tmpd/bad.msg" > /dev/null && control "a rule trailer in a message passed"
scan_files "$tmpd/good.c" > /dev/null || control "plain project text failed"
scan_message "$tmpd/good.msg" > /dev/null || control "a plain message failed"
rm -rf "$tmpd"

status=0
case "${1:-}" in
--message)
	[ -f "${2:-}" ] || { echo "usage: $0 --message FILE" >&2; exit 2; }
	if ! out=$(scan_message "$2"); then
		echo "chthonia_sanitize_check: the commit message identifies AI assistance or madc's process:" >&2
		echo "$out" >&2
		exit 1
	fi
	exit 0
	;;
--checkout)
	dir=${2:-}
	git -C "$dir" rev-parse --git-dir > /dev/null 2>&1 || { echo "usage: $0 --checkout DIR (a git checkout)" >&2; exit 2; }
	agents=$(git -C "$dir" ls-files | grep -E "$AGENT_FILES")
	if [ -n "$agents" ]; then
		echo "chthonia_sanitize_check: the checkout tracks agent instruction files:" >&2
		echo "$agents" >&2
		status=1
	fi
	files=$(git -C "$dir" ls-files | grep -v -E "$AGENT_FILES" | sed "s|^|$dir/|")
	tmpm=$(mktemp)
	for c in $(git -C "$dir" rev-list --all); do
		git -C "$dir" log -1 --format=%B "$c" > "$tmpm"
		if ! out=$(scan_message "$tmpm"); then
			echo "chthonia_sanitize_check: commit $c's message:" >&2
			echo "$out" >&2
			status=1
		fi
	done
	rm -f "$tmpm"
	what="the checkout $dir"
	;;
"")
	files=$(git ls-files tools/chthonia tools/madcide/plugins/chthonia)
	what="madc's moving set (tools/chthonia, tools/madcide/plugins/chthonia)"
	;;
*)
	echo "usage: $0 [--checkout DIR | --message FILE]" >&2
	exit 2
	;;
esac
# shellcheck disable=SC2086
if [ -n "$files" ] && ! out=$(scan_files $files); then
	echo "chthonia_sanitize_check: a Chthonia file identifies AI assistance or madc's process:" >&2
	echo "$out" >&2
	status=1
fi
[ $status -eq 0 ] && echo "chthonia_sanitize_check: OK ($what)"
exit $status
