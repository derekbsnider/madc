#!/bin/bash
# DRIFT-PREVENTION GATE -- runtime-phase error composition.
#
# One rule, one owner: the channel/process subsystem composes
# error(severity::error, phase::runtime, ...) in exactly ONE place — the
# one-argument detail::set_channel_error in src/madc_datachannel.cpp.
# Every sibling helper (set_process_error, set_state_error, the two-arg
# operation+detail overload, the errno formatters) DELEGATES there.
# dupaudit family: runtime_error_composition (2026-08-08).
set -u
cd "$(dirname "$0")/.."

composers=$(grep -rn 'error(error::severity::error, error::phase::runtime' \
	src/*.cpp | grep -v 'ERROR-COMPOSER-OWNER' || true)
if [ -n "$composers" ]; then
	echo "unowned runtime-error composition site(s):"
	printf '%s\n' "$composers" | sed 's/^/  /'
	echo "  -> delegate to detail::set_channel_error (madc_datachannel.cpp)."
	exit 1
fi

owner=$(grep -c 'ERROR-COMPOSER-OWNER' src/madc_datachannel.cpp)
echo "runtime-error composer owner: $owner (target 1)"
if [ "$owner" -ne 1 ]; then
	echo "  -> keep exactly one ERROR-COMPOSER-OWNER site in madc_datachannel.cpp."
	exit 1
fi

process_delegate=$(grep -c 'detail::set_channel_error(' src/madc_process.cpp)
object_delegate=$(grep -c 'detail::set_channel_error(' src/madc_channel_object.cpp)
echo "process delegate: $process_delegate (target >=1)"
echo "channel-object delegate: $object_delegate (target >=1)"
if [ "$process_delegate" -lt 1 ] || [ "$object_delegate" -lt 1 ]; then
	echo "  -> the sibling helpers must delegate to the one owner."
	exit 1
fi

echo "GREEN -- runtime-phase errors have one composition owner."
