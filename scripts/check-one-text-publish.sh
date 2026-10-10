#!/bin/bash
# DRIFT-PREVENTION GATE -- a text has ONE publish point.
#
# The edit notification (split arc 1A.1, docs/plans/2026-10-09-document-
# component-design.md §2.2) is correct only if EVERY change of a text goes
# through the world's text_* mutators in include/madcdis/hub.h, which
# publish one record each. A text_buffer mutator called anywhere else --
# `.load(`, `.insert(`, `.erase(`, `.replace(`, `.undo(`, `.redo(`,
# `.checkpoint(` on a text_buffer -- is a change nobody hears: an anchor
# that does not shift, a journal with a hole, a mirror that trusts a stale
# version. Allowed callers: hub.h (the owner), text_buffer.h itself
# (replace = erase + insert), the unit tests, and the line editor's own
# entry buffer (include/madcdis/line_edit.h -- a prompt's entry is not a
# world text; it has no entity and no listeners).
#
# Two-sided: the negative control proves the pattern still bites.
set -u
cd "$(dirname "$0")/.."

# A text_buffer-typed receiver: `_texts[...]`, `text_buffer &b`, `b.`,
# `it->second.`, `t->second.` -- every spelling hub.h uses, so a copy made
# from hub.h is caught wherever it lands.
PAT='(_texts\[[^]]*\]|\b(b|buf|buffer|tb|text)|->second|_buf|_entry)\.(load|insert|erase|replace|undo|redo|checkpoint)\('
ALLOWED='include/madcdis/hub.h|include/madcdis/text_buffer.h|include/madcdis/line_edit.h|tests/unit/'

ctl_dir=$(mktemp -d)
trap 'rm -rf "$ctl_dir"' EXIT
cat > "$ctl_dir/ctl.cpp" <<'CTL'
static void arm(world &w, entity_id id) {
    madc::hub::text_buffer &b = w.texts()[id];
    b.insert(0, "x");
    _texts[id].load(s);
}
CTL
c=$(grep -cE "$PAT" "$ctl_dir/ctl.cpp")
if [ "$c" -ne 2 ]; then
	echo "check-one-text-publish: NEGATIVE CONTROL FAILED -- matched $c of 2"
	exit 1
fi

# The live scan: every C++ source and header that includes the text
# component, less the allowed owners. A bag-string receiver named `text`
# (madc::value text; text.insert(...)) would be a false positive -- there is
# none today; one that appears names its variable otherwise.
files=$(git ls-files --cached --others --exclude-standard 'src/*.cpp' 'src/*.h' 'include/*.h' 'include/madcdis/*.h' 'include/libmadc/*.h' \
	| grep -vE "^($ALLOWED)")
bad=$(grep -nE "$PAT" $files 2>/dev/null | grep -E 'text_buffer|_texts|hub::' )
n=$(printf '%s' "$bad" | grep -c . || true)
echo "text_buffer mutations outside the world's publish point: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bad"
	echo "  -> mutate a text through world::text_* / mutation_context (hub.h): it publishes."
	exit 1
fi
# The owner still exists and still publishes.
if ! grep -q 'void publish(text_change &c)' include/madcdis/hub.h; then
	echo "check-one-text-publish: the publish point is gone from hub.h"
	exit 1
fi
echo "GREEN -- every text mutation publishes through the world's text_* mutators."
