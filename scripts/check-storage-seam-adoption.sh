#!/bin/bash
# DRIFT-PREVENTION GATE -- storage drivers adopt the owned channel/cursor seams.
#
# Two DupFamily rules (KG: seekable_channel_probe, cursor_drain_to_vector):
#  1. "Is this channel truthfully seekable" has ONE owner:
#     madc::seekable_surface (interface present AND capabilities().seek).
#     Nobody else dynamic_casts the SeekableDataChannel mixin.
#  2. "Drain a status-aware cursor into a container" has ONE owner:
#     madc::copy + to_container (flow.h / sink.h). No hand-rolled
#     cursor_next drain loops in implementation files — per-item pipeline
#     stages live in the flow/dataset headers, which stay out of scope here.
set -u
cd "$(dirname "$0")/.."

probe_owner=$(grep -c 'dynamic_cast<SeekableDataChannel' src/madc_datachannel.cpp)
probe_outside=$(grep -rn --include='*.cpp' 'dynamic_cast<SeekableDataChannel' src/ \
	| grep -v 'src/madc_datachannel.cpp' || true)
drain_loops=$(grep -rn --include='*.cpp' 'cursor_next(\*' src/ || true)

echo "seekable probe owner definitions: $probe_owner (target 1)"
if [ -n "$probe_outside" ]; then
	echo "seekable probes outside the owner:"
	printf '%s\n' "$probe_outside" | sed 's/^/  /'
fi
if [ -n "$drain_loops" ]; then
	echo "hand-rolled cursor drain loop(s):"
	printf '%s\n' "$drain_loops" | sed 's/^/  /'
fi

if [ "$probe_owner" -ne 1 ] || [ -n "$probe_outside" ] || [ -n "$drain_loops" ]; then
	echo "  -> probe seekability via madc::seekable_surface; drain cursors"
	echo "     via madc::copy(cursor, to_container(v)). One owner each."
	exit 1
fi

echo "GREEN -- seekable probe and cursor drain each have one owner."
