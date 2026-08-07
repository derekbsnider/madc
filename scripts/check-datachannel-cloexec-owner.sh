#!/bin/bash
# DRIFT-PREVENTION GATE -- descriptors owned by data/process channels.
set -u
cd "$(dirname "$0")/.."

owner=$(grep -c '^bool set_fd_close_on_exec(' src/madc_posix_io.cpp)
file_delegate=$(grep -c 'detail::set_fd_close_on_exec(fd)' src/madc_datachannel.cpp)
process_delegate=$(grep -c 'detail::set_fd_close_on_exec(fds\[i\])' src/madc_process.cpp)
socket_delegate=$(grep -c 'detail::set_fd_close_on_exec(fd)' src/madc_socket_channel.cpp)
echo "FD close-on-exec owner: $owner definition(s) (target 1)"
echo "file/FIFO delegate: $file_delegate (target 1)"
echo "process-pipe delegate: $process_delegate (target 1)"
echo "socket delegate: $socket_delegate (target 1)"
if [ "$owner" -ne 1 ] || [ "$file_delegate" -ne 1 ] \
	|| [ "$process_delegate" -ne 1 ] || [ "$socket_delegate" -ne 1 ]; then
	echo "  -> all owned descriptors must use the shared close-on-exec policy."
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
