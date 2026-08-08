#!/bin/bash
# DRIFT-PREVENTION GATE -- descriptors owned by data/process channels.
set -u
cd "$(dirname "$0")/.."

owner=$(grep -c '^bool set_fd_close_on_exec(' src/madc_posix_io.cpp)
file_atomic=$(grep -c 'flags |= O_CLOEXEC' src/madc_datachannel.cpp)
process_atomic=$(grep -c 'pipe2(fds, O_CLOEXEC)' src/madc_process.cpp)
process_delegate=$(grep -c 'detail::set_fd_close_on_exec(fds\[i\])' src/madc_process.cpp)
socket_atomic=$(grep -c 'socket_type | SOCK_CLOEXEC' src/madc_socket_channel.cpp)
socket_delegate=$(grep -c 'detail::set_fd_close_on_exec(fd)' src/madc_socket_channel.cpp)
echo "FD close-on-exec owner: $owner definition(s) (target 1)"
echo "file/FIFO atomic O_CLOEXEC: $file_atomic (target 1)"
echo "process-pipe atomic pipe2 + fallback delegate: $process_atomic/$process_delegate (target 1/1)"
echo "socket atomic SOCK_CLOEXEC + fallback delegate: $socket_atomic/$socket_delegate (target 1/1)"
if [ "$owner" -ne 1 ] || [ "$file_atomic" -ne 1 ] \
	|| [ "$process_atomic" -ne 1 ] || [ "$process_delegate" -ne 1 ] \
	|| [ "$socket_atomic" -ne 1 ] || [ "$socket_delegate" -ne 1 ]; then
	echo "  -> creation sites must set close-on-exec ATOMICALLY where the"
	echo "     platform provides it (O_CLOEXEC / pipe2 / SOCK_CLOEXEC) and"
	echo "     keep the shared post-hoc owner as the portable fallback."
	exit 1
fi

raw=$(grep -rnE --include='*.cpp' 'F_SETFD.*FD_CLOEXEC' src/ \
	| grep -v 'src/madc_posix_io.cpp' || true)
if [ -n "$raw" ]; then
	echo "ad hoc close-on-exec implementation(s):"
	printf '%s\n' "$raw" | sed 's/^/  /'
	exit 1
fi

echo "GREEN -- data and process descriptors share one close-on-exec owner."
