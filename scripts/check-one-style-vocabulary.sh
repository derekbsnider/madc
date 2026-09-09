#!/bin/bash
# check-one-style-vocabulary.sh — the ui RENDER STYLE gate: one style, one
# spec parser, no renderer vocabulary of its own.
#
# A highlight span reaches every renderer as the loaded scheme's style SPEC
# in JOE's vocabulary (`keyword bold`, `string cyan` — profiles/*.theme),
# parsed ONCE by include/madcdis/ui_style.h (ui_style, ui_style_of). Each
# renderer owns only its LAST step from that style: the VT100 target
# style -> SGR, the DOM model style -> the page's classes (st-<attr>,
# fg-<colour>, bg-<colour>) over ONE palette of CSS custom properties
# (pal-<colour>, pal-<colour>-bright — bold-as-bright, the VT-102 model).
# Until 2026-09-08 the web renderer styled the span's SEMANTIC class from a
# palette of its own (`.c-keyword { color: var(--syn-keyword, ...) }`), so
# the window never showed the scheme the terminal showed — the owner saw
# the two colourings differ ("they should be the same"). A second styling
# vocabulary is where two renderers drift; this gate keeps there being one.
#
# Rules:
#   1. the page (include/madc/ui_web/page.css, page.js) carries no
#      syntax class of its own — no `.c-<name>` selector, no `--syn-`
#      property, no `' c-'` class prefix;
#   2. the DOM model reads a span's style spec `c`, never a class name
#      (`find("cls")` on a span row) — and the converter emits no `"cls"`;
#   3. the spec parser is defined ONCE, in ui_style.h, and both renderer
#      models include it.
#
# Negative controls: a synthetic violation of each rule must FAIL the scan,
# else the gate itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

STYLE_OWNER=include/madcdis/ui_style.h
PAGE_CSS=include/madc/ui_web/page.css
PAGE_JS=include/madc/ui_web/page.js
WEB_MODEL=include/madcdis/web_model.h
TUI_MODEL=include/madcdis/tui_model.h
CONVERTER=tools/madcide/madcide_core.inc

PAGE_PATTERN='^\.c-[a-z]|--syn-|'"' c-'"
MODEL_PATTERN='find\("cls"\)|"cls": '

scan() {
	# $1 = pattern, $@ = files; prints violations, returns 0 when clean.
	local pat="$1"; shift
	grep -nE "$pat" "$@" /dev/null
	test $? -ne 0
}

# --- negative controls -------------------------------------------------------
tmp=$(mktemp)
printf '.c-keyword  { color: var(--pal-blue, #8fb4ff); }\n' > "$tmp"
if scan "$PAGE_PATTERN" "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-style-vocabulary: NEGATIVE CONTROL FAILED — the scan did not catch a page syntax class" >&2
	exit 2
fi
printf 'html { --syn-keyword: #8fb4ff; }\n' > "$tmp"
if scan "$PAGE_PATTERN" "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-style-vocabulary: NEGATIVE CONTROL FAILED — the scan did not catch a page syntax property" >&2
	exit 2
fi
printf "      var cls = ' c-' + spans[k][2];\n" > "$tmp"
if scan "$PAGE_PATTERN" "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-style-vocabulary: NEGATIVE CONTROL FAILED — the scan did not catch a page class prefix" >&2
	exit 2
fi
printf 'std::map<std::string, madc::value>::const_iterator ci = ro.find("cls");\n' > "$tmp"
if scan "$MODEL_PATTERN" "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-style-vocabulary: NEGATIVE CONTROL FAILED — the scan did not catch a class-name span read" >&2
	exit 2
fi
printf 'hs[] = { "s": s, "e": e, "cls": cls, "c": c };\n' > "$tmp"
if scan "$MODEL_PATTERN" "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-style-vocabulary: NEGATIVE CONTROL FAILED — the scan did not catch a class-name span field" >&2
	exit 2
fi
rm -f "$tmp"

# --- the owner must exist, once, and both models must read it ----------------
if ! grep -q '^inline bool ui_style_of' "$STYLE_OWNER"; then
	echo "check-one-style-vocabulary: owner ui_style_of not found in $STYLE_OWNER" >&2
	exit 1
fi
files=$(git ls-files 'src/*.cpp' 'src/*.h' 'include/*.h' 'include/**/*.h')
# shellcheck disable=SC2086
parsers=$(grep -lE '^(inline )?bool (ui_style_of|tui_attr_of) *\(' $files /dev/null | grep -v "^$STYLE_OWNER\$")
if [ -n "$parsers" ]; then
	echo "check-one-style-vocabulary: a second style-spec parser outside the one owner ($STYLE_OWNER):" >&2
	echo "$parsers" >&2
	exit 1
fi
for m in "$WEB_MODEL" "$TUI_MODEL"; do
	if ! grep -q '#include "madcdis/ui_style.h"' "$m"; then
		echo "check-one-style-vocabulary: $m does not include $STYLE_OWNER — a renderer model reads the one style" >&2
		exit 1
	fi
done

# --- the tree ------------------------------------------------------------------
page_out=$(grep -nE "$PAGE_PATTERN" "$PAGE_CSS" "$PAGE_JS" /dev/null)
model_out=$(grep -nE "$MODEL_PATTERN" "$WEB_MODEL" "$CONVERTER" /dev/null)
if [ -n "$page_out" ]; then
	echo "check-one-style-vocabulary: the page carries a syntax vocabulary of its own:" >&2
	echo "$page_out" >&2
	echo "  -> render the span's style classes (st-*, fg-*, bg-*) over the pal-* palette; the scheme file is the one vocabulary" >&2
fi
if [ -n "$model_out" ]; then
	echo "check-one-style-vocabulary: a span row read or written by class name instead of its style spec:" >&2
	echo "$model_out" >&2
	echo "  -> rows are {s, e, c}; read c through ui_style_of" >&2
fi
if [ -n "$page_out" ] || [ -n "$model_out" ]; then
	exit 1
fi
echo "check-one-style-vocabulary: OK — one style spec parser ($STYLE_OWNER), both models read it, the page styles only through the pal-* palette (negative controls bite)"
