#!/bin/bash
# Print line counts for every agent-loaded file and the grand total.
# Useful for spotting rule bloat: Claude Code auto-loads every file
# under .claude/rules/ each turn, and follows @AGENTS.md from CLAUDE.md
# — so the total below is how many lines of instruction Claude re-reads
# on every prompt.
#
# Run after adding / removing / editing rules, and update AGENTS.md's
# "Total rule footprint" line if the count has drifted.

set -e

total=0
longest_name=0

# Pass 1 — find the longest filename for alignment
for f in CLAUDE.md AGENTS.md .claude/rules/*.md; do
    [ ${#f} -gt $longest_name ] && longest_name=${#f}
done

printf "\nPer-file line counts:\n"
printf "%-${longest_name}s  %6s\n" "file" "lines"
printf "%-${longest_name}s  %6s\n" "----" "-----"

# Pass 2 — print counts
for f in CLAUDE.md AGENTS.md; do
    n=$(wc -l < "$f")
    total=$((total + n))
    printf "%-${longest_name}s  %6d\n" "$f" "$n"
done
for f in $(ls .claude/rules/*.md | sort); do
    n=$(wc -l < "$f")
    total=$((total + n))
    printf "%-${longest_name}s  %6d\n" "$f" "$n"
done

rule_count=$(ls .claude/rules/*.md | wc -l)
rule_lines=$(wc -l .claude/rules/*.md | tail -1 | awk '{print $1}')
printf "\n  %d rules totaling %d lines in .claude/rules/\n" "$rule_count" "$rule_lines"
printf "  Grand total (loaded by Claude Code per turn): %d lines\n\n" "$total"
