#!/bin/bash
# DRIFT-PREVENTION GATE -- FD-backed DataChannel write policy.
set -u
cd "$(dirname "$0")/.."

owner=$(grep -c '^ssize_t write_fd_without_sigpipe(' src/madc_posix_io.cpp)
echo "FD write owner: $owner definition(s) (target 1)"
if [ "$owner" -ne 1 ]; then
	echo "  -> keep exactly one write_fd_without_sigpipe implementation."
	exit 1
fi

file_delegate=$(grep -c 'detail::write_fd_without_sigpipe(fd_' src/madc_datachannel.cpp)
process_delegate=$(grep -c 'detail::write_fd_without_sigpipe(fd_' src/madc_process.cpp)
echo "file/FIFO delegate: $file_delegate (target 1)"
echo "process-pipe delegate: $process_delegate (target 1)"
if [ "$file_delegate" -ne 1 ] || [ "$process_delegate" -ne 1 ]; then
	echo "  -> every FD-backed channel write must delegate to the shared owner."
	exit 1
fi

raw=$(grep -rnE --include='*.cpp' '^[[:space:]]+.*::write\(' src/ \
	| grep -v 'SIGPIPE-OWNER' \
	| grep -v 'ASYNC-CHILD-WRITE' || true)
if [ -n "$raw" ]; then
	echo "unowned raw write(2) call(s):"
	printf '%s\n' "$raw" | sed 's/^/  /'
	echo "  -> use write_fd_without_sigpipe, or document an async-child exception."
	exit 1
fi

echo "GREEN -- FD-backed channels share one SIGPIPE-safe write owner."
