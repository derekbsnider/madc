#!/bin/sh
# context-gauge.sh — relays the context-window percentage the status line
# already computes into the agent's own context, once per user prompt.
#
# .claude/statusline-command.sh writes Claude Code's own used_percentage
# for this session to tmp/context-gauge/<session>.json on every render.
# This hook only reads that file and hands a short note back through
# UserPromptSubmit's additionalContext channel. It writes nothing, and it
# does not affect tool permissions or approvals in any way — it only adds
# a line of text to the agent's context, the same number the user's
# status line already shows.
#
# Registered in .claude/settings.json under hooks.UserPromptSubmit.

input=$(cat)
session=$(printf '%s' "$input" | jq -r '.session_id // empty' 2>/dev/null)
[ -n "$session" ] || exit 0

gauge_dir="/workspace/madc/tmp/context-gauge"
gauge="$gauge_dir/$session.json"
[ -f "$gauge" ] || exit 0

used=$(jq -r '.used_percentage // empty' "$gauge" 2>/dev/null)
tokens=$(jq -r '.tokens // empty' "$gauge" 2>/dev/null)
window=$(jq -r '.window // empty' "$gauge" 2>/dev/null)
[ -n "$used" ] || exit 0

note="context gauge: ${used}% used (${tokens} of ${window} tokens)."

jq -n --arg note "$note" \
    '{hookSpecificOutput: {hookEventName: "UserPromptSubmit", additionalContext: $note}}'
