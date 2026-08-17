#!/bin/bash
# DRIFT-PREVENTION GATE -- function-like macro replacement expansion.
#
# Normal token expansion and #if expansion used to walk replacement bodies
# independently. They disagreed on argument prescan, literals, stringizing,
# and token pasting. Keep one token owner and one expansion owner; both paths
# must delegate to it. dupaudit family: preprocessor_macro_expansion_engines
# (2026-08-14).
set -u
cd "$(dirname "$0")/.."

source_file=${MADC_MACRO_GATE_SOURCE:-src/lexer.cpp}

tokenizers=$(grep -c '^tokenize_macro_spelling(const std::string &text)' "$source_file")
owners=$(grep -c '^static std::string expand_function_macro_body(' "$source_file")
references=$(grep -c 'expand_function_macro_body(' "$source_file")

echo "macro replacement tokenizer: $tokenizers definition(s) (target 1)"
echo "macro expansion owner: $owners definition(s) (target 1)"
echo "macro expansion owner references: $references (target 3: definition + 2 delegates)"

if [ "$tokenizers" -ne 1 ] || [ "$owners" -ne 1 ] || [ "$references" -ne 3 ]; then
	echo "REGRESSION -- normal and #if expansion must share one replacement-list owner."
	exit 1
fi

legacy=$(grep -nE '\.(body|replacement)[[:space:]]*\[[^]]+\]|const std::string &[[:space:]]*body[[:space:]]*=[[:space:]]*[A-Za-z_][A-Za-z0-9_]*\.body|\.find\("##"\)|auto stringify_macro_arg' "$source_file")
legacy_count=$(printf '%s' "$legacy" | grep -c .)
echo "direct replacement-body walkers: $legacy_count (target 0)"
if [ "$legacy_count" -ne 0 ]; then
	echo "REGRESSION -- replacement spelling must be interpreted by tokenize_macro_spelling."
	printf '%s\n' "$legacy" | sed 's/^/  /'
	exit 1
fi

echo "GREEN -- function-like macro replacement has one expansion owner."
