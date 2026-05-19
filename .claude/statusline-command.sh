#!/bin/sh
# Status line: \u@\h:\w [ctx% total_tokens] Δ: delta1 delta2 ...
# Reads JSON from stdin (Claude Code statusLine input)
# Persists token history in /workspace/madc/.claude/statusline-state.json

STATE_FILE="/workspace/madc/.claude/statusline-state.json"

input=$(cat)
cwd=$(echo "$input" | jq -r '.cwd // .workspace.current_dir // empty')
[ -z "$cwd" ] && cwd=$(pwd)
used=$(echo "$input" | jq -r '.context_window.used_percentage // empty')

# Extract current total used tokens from context_window
# input_tokens in current_usage reflects the current context size
current_tokens=$(echo "$input" | jq -r '.context_window.current_usage.input_tokens // empty')
# Fall back to total_input_tokens if current_usage not available
[ -z "$current_tokens" ] && current_tokens=$(echo "$input" | jq -r '.context_window.total_input_tokens // empty')

# Format a raw token count as compact k-suffix string
tok_fmt() {
    val="$1"
    if [ -z "$val" ] || [ "$val" = "null" ] || [ "$val" -eq 0 ] 2>/dev/null; then
        echo "0"
    elif [ "$val" -ge 1000 ]; then
        echo "$val" | awk '{printf "%.0fk", $1/1000}'
    else
        echo "$val"
    fi
}

# Compute deltas and update state file
delta_str=""
if [ -n "$current_tokens" ] && [ "$current_tokens" != "null" ]; then
    # Read previous history array (list of token counts)
    if [ -f "$STATE_FILE" ]; then
        history=$(jq -r '.history // [] | .[]' "$STATE_FILE" 2>/dev/null)
    else
        history=""
    fi

    # Get last value from history for delta computation
    last_val=$(echo "$history" | tail -1)

    # Compute delta
    if [ -n "$last_val" ] && [ "$last_val" -gt 0 ] 2>/dev/null; then
        delta=$((current_tokens - last_val))
    else
        delta=0
    fi

    # Append current_tokens to history, keep last 20 entries
    new_history=$(echo "$history" | tail -19)
    new_history=$(printf "%s\n%s" "$new_history" "$current_tokens" | sed '/^$/d')

    # Save updated history as JSON array
    new_json=$(echo "$new_history" | jq -R 'tonumber' | jq -s '.' 2>/dev/null)
    if [ -n "$new_json" ]; then
        printf '{"history":%s}' "$new_json" > "$STATE_FILE"
    fi

    # Build delta display string from full history (differences between consecutive entries)
    delta_str=$(echo "$new_history" | awk '
        NR==1 { prev=$1; next }
        {
            d = $1 - prev
            if (d < 0) d = 0
            if (d >= 1000) printf "%.0fk ", d/1000
            else printf "%d ", d
            prev = $1
        }
    ' | sed 's/ $//')
fi

# Format total tokens
total_fmt=$(tok_fmt "$current_tokens")

# Build status line
if [ -n "$used" ]; then
    base=$(printf "%s@%s:%s [%.0f%% %s]" "$(whoami)" "$(hostname -s)" "$cwd" "$used" "$total_fmt")
else
    base=$(printf "%s@%s:%s [%s]" "$(whoami)" "$(hostname -s)" "$cwd" "$total_fmt")
fi

if [ -n "$delta_str" ]; then
    printf "%s Δ: %s" "$base" "$delta_str"
else
    printf "%s" "$base"
fi
