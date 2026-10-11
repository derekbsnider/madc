#!/bin/bash
# THE subject-document rule has ONE owner: access_verdict in
# tools/texteditor/editor_events.inc — the actor's active document ("adoc"),
# else the world's one "document". Code that looks the document up by
# singleton name bypasses the active-buffer resolution — under multi-buffer
# (madcide AST-5) that is the WRONG buffer, and for a save it is data loss.
# (Until split arc 1D the owner was the line editor's verbs/_subject.madv,
# prepended to every verb; the verbs are retired.) Fails on any singleton
# lookup outside the owner, and (the negative control) on the owner losing
# its own fallback.
cd "$(dirname "$0")/.."
OWNER=tools/texteditor/editor_events.inc
hits=$(grep -rn 'entity_by_name(w, "document")' tools/madcide tools/texteditor \
       | grep -v "^$OWNER:")
if [ -n "$hits" ]; then
    echo "check-one-subject-doc: singleton document lookup outside the owner:"
    echo "$hits"
    exit 1
fi
if [ "$(grep -c 'entity_by_name(w, "document")' "$OWNER")" != 1 ]; then
    echo "check-one-subject-doc: the owner ($OWNER) must hold the ONE fallback (marker rot or a second copy)"
    exit 1
fi
echo "check-one-subject-doc: PASS"
