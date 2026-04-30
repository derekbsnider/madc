#!/bin/bash
# Thin wrapper around redis-cli for the madc-knowledge FalkorDB graph.
# Use this when the MCP `mcp__falkordb__*` tools aren't available.
#
# Usage:
#   scripts/kg_query.sh '<cypher query>'
#   scripts/kg_query.sh -ro '<cypher query>'   # read-only (GRAPH.RO_QUERY)
#   scripts/kg_query.sh -g <other-graph> '<cypher query>'
#
# Defaults:
#   Host:  $FALKORDB_HOST  (default: falkordb)
#   Port:  $FALKORDB_PORT  (default: 6379)
#   Graph: $FALKORDB_GRAPH (default: madc-knowledge)
set -euo pipefail

HOST="${FALKORDB_HOST:-falkordb}"
PORT="${FALKORDB_PORT:-6379}"
GRAPH="${FALKORDB_GRAPH:-madc-knowledge}"
CMD="GRAPH.QUERY"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -ro|--read-only) CMD="GRAPH.RO_QUERY"; shift ;;
        -g|--graph)      GRAPH="$2"; shift 2 ;;
        -h|--host)       HOST="$2"; shift 2 ;;
        -p|--port)       PORT="$2"; shift 2 ;;
        -l|--list)       redis-cli -h "$HOST" -p "$PORT" GRAPH.LIST; exit 0 ;;
        --help)
            sed -n '2,18p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) break ;;
    esac
done

if [[ $# -eq 0 ]]; then
    echo "Usage: $0 [-ro] [-g graph] '<cypher>'" >&2
    exit 1
fi

redis-cli -h "$HOST" -p "$PORT" --no-raw "$CMD" "$GRAPH" "$1"
