#!/bin/bash
# gen_rt_text.sh — a dual-build runtime source's TEXT for a startup member
# (src/madc_crt.cpp): the source with its quoted includes inlined (c2mir
# compiles a member with no include path) and MADC_RT_IMAGE defined (the
# image build, as opposed to the host build that compiles the same file into
# libmadc), written as one C++11 raw string literal. One implementation, two
# builds: the member never carries a hand-kept copy of the source.
#
# Usage: gen_rt_text.sh <src/rt/file.c> <out.inc>
set -eu
src=$1
out=$2
dir=$(dirname "$src")
{
	echo 'R"madc_rt('
	echo '#define MADC_RT_IMAGE 1'
	awk -v dir="$dir" '
		/^#include "[^"]*"/ {
			f = $2
			gsub(/"/, "", f)
			f = dir "/" f
			while ((getline line < f) > 0)
				print line
			close(f)
			next
		}
		{ print }
	' "$src"
	echo ')madc_rt"'
} > "$out.tmp"
mv "$out.tmp" "$out"
