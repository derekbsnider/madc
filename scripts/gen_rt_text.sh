#!/bin/bash
# gen_rt_text.sh — a dual-build runtime source's TEXT for a startup member
# (src/madc_crt.cpp): the source with its quoted includes inlined (c2mir
# compiles a member with no include path) and MADC_RT_IMAGE defined (the
# image build, as opposed to the host build that compiles the same file into
# libmadc or libmir), written as one C++11 raw string literal. One
# implementation, two builds: the member never carries a hand-kept copy of the
# source. Inlining is recursive, each file once (an included header's own
# quoted includes are inlined where it names them; a file named again is
# already in the text, as its include guard would have it).
#
# Usage: gen_rt_text.sh <src/rt/file.c> <out.inc>
set -eu
src=$1
out=$2
{
	echo 'R"madc_rt('
	echo '#define MADC_RT_IMAGE 1'
	awk '
		function inline_file(f,    line, inc, dir, r) {
			if (f in seen)
				return
			seen[f] = 1
			dir = f
			if (sub(/\/[^\/]*$/, "", dir) == 0)
				dir = "."
			while ((r = (getline line < f)) > 0) {
				if (line ~ /^#include "[^"]*"/) {
					inc = line
					sub(/^#include "/, "", inc)
					sub(/".*$/, "", inc)
					inline_file(dir "/" inc)
					continue
				}
				print line
			}
			if (r < 0) {
				printf "gen_rt_text.sh: cannot read %s\n", f > "/dev/stderr"
				failed = 1
			}
			close(f)
		}
		BEGIN { inline_file(ARGV[1]); exit failed }
	' "$src"
	echo ')madc_rt"'
} > "$out.tmp"
mv "$out.tmp" "$out"
