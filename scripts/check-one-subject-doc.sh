#!/bin/bash
# THE subject-document rule has ONE owner: verbs/_subject.madv (prepended
# to every document verb/check at bind). A body that looks the document up
# by singleton name bypasses the active-buffer resolution — under multi-
# buffer (madcide AST-5) that is the WRONG buffer, and for the save verb
# it is data loss. Fails on any singleton lookup outside the owner, and
# (the negative control) on the owner losing its own fallback.
cd "$(dirname "$0")/.."
hits=$(grep -rln 'entity_by_name(w, "document")' \
       tools/texteditor/verbs tools/texteditor/checks \
       | grep -v '_subject\.madv')
if [ -n "$hits" ]; then
    echo "check-one-subject-doc: singleton document lookup outside the owner:"
    echo "$hits"
    exit 1
fi
if ! grep -q 'entity_by_name(w, "document")' \
     tools/texteditor/verbs/_subject.madv; then
    echo "check-one-subject-doc: the owner lost its fallback (marker rot)"
    exit 1
fi
echo "check-one-subject-doc: PASS"
