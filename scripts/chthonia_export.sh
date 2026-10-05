#!/bin/bash
# chthonia_export.sh — Chthonia's repository, made from madc's moving set
# (docs/plans/2026-10-04-chthonia-own-repository.md §2 "Moves", §5, §6 step
# 3): a new git repository with one commit, ready for its remote.
#
#   scripts/chthonia_export.sh DIR
#
# DIR must not exist. The tree:
#   tools/chthonia/*                  → DIR/ (the product, its scripts, tests,
#                                       packaging, CI, README, licence)
#   tools/madcide/plugins/chthonia/*  → DIR/plugins/chthonia/ (the bundle)
#   chthonia.json                     its plugin unit at plugins/chthonia/, and
#                                     "madc": VERSION, the release it is cut
#                                     against (the oldest that builds it)
# The repository (§5): a fresh history whose one commit is under this
# checkout's git identity (the owner's) with a plain message; madc's agent
# instruction files excluded through .git/info/exclude, never committed; a
# local .claude/settings.json, excluded the same way, turning off the
# assistant's co-author line ("includeCoAuthoredBy": false); a commit-msg
# hook running this checkout's chthonia_sanitize_check.sh --message. Then
# chthonia_sanitize_check.sh --checkout DIR must pass, and git must report
# .claude/settings.json ignored.
#
# Only tracked files move (git ls-files), so build/, dist/ and tmp/ never do.
set -eu
cd "$(dirname "$0")/.." || exit 2
if [ $# -ne 1 ]; then
	echo "usage: $0 DIR" >&2
	exit 2
fi
dir=$1
if [ -e "$dir" ]; then
	echo "chthonia_export: $dir exists — name a new directory" >&2
	exit 1
fi
madc=$PWD
ver=$(cat VERSION)
name=$(git config user.name)
email=$(git config user.email)
if [ -z "$name" ] || [ -z "$email" ]; then
	echo "chthonia_export: this checkout has no git user.name / user.email" >&2
	exit 1
fi

mkdir -p "$dir"
dir=$(cd "$dir" && pwd)
git ls-files tools/chthonia | while IFS= read -r f; do
	mkdir -p "$dir/$(dirname "${f#tools/chthonia/}")"
	cp -p "$f" "$dir/${f#tools/chthonia/}"
done
git ls-files tools/madcide/plugins/chthonia | while IFS= read -r f; do
	t=$dir/plugins/chthonia/${f#tools/madcide/plugins/chthonia/}
	mkdir -p "$(dirname "$t")"
	cp -p "$f" "$t"
done

# The manifest: the bundle's new place, and the madc release it needs.
python3 - "$dir/chthonia.json" "$ver" << 'EOF'
import sys
path, ver = sys.argv[1], sys.argv[2]
text = open(path).read()
old = '"../madcide/plugins/chthonia/chthonia.mad"'
if text.count(old) != 1:
    sys.exit("chthonia_export: chthonia.json does not name %s once" % old)
text = text.replace(old, '"plugins/chthonia/chthonia.mad"')
if '"madc"' in text:
    sys.exit("chthonia_export: chthonia.json already names a madc release")
head = '{\n'
if not text.startswith(head):
    sys.exit("chthonia_export: chthonia.json does not start with '{' on its own line")
text = head + '  "madc": "%s",\n' % ver + text[len(head):]
open(path, 'w').write(text)
EOF

git -C "$dir" init -q -b main
cat >> "$dir/.git/info/exclude" << 'EOF'
# Agent instruction files stay out of this repository.
AGENTS.md
CLAUDE.md
GEMINI.md
.claude/
.cursor/
.windsurfrules
.aider.conf.yml
.github/copilot-instructions.md
EOF
mkdir -p "$dir/.claude"
printf '{\n  "includeCoAuthoredBy": false\n}\n' > "$dir/.claude/settings.json"
cat > "$dir/.git/hooks/commit-msg" << EOF
#!/bin/sh
exec bash "$madc/scripts/chthonia_sanitize_check.sh" --message "\$1"
EOF
chmod 755 "$dir/.git/hooks/commit-msg"
git -C "$dir" add .
git -C "$dir" -c user.name="$name" -c user.email="$email" commit -q -m "Chthonia, an easy IDE to learn C and C++"
bash scripts/chthonia_sanitize_check.sh --checkout "$dir"
if ! git -C "$dir" check-ignore -q .claude/settings.json; then
	echo "chthonia_export: .claude/settings.json is not ignored in $dir" >&2
	exit 1
fi
echo "chthonia_export: $dir ($(git -C "$dir" ls-files | wc -l | tr -d ' ') files, one commit, madc $ver)"
