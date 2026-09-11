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
socket_delegate=$(grep -c 'detail::write_fd_without_sigpipe(fd_' src/madc_socket_channel.cpp)
datagram_send=$(grep -c 'result = ::send(fd_' src/madc_socket_channel.cpp)
echo "file/FIFO delegate: $file_delegate (target 1)"
echo "process-pipe delegate: $process_delegate (target 1)"
echo "stream-socket delegate: $socket_delegate (target 1)"
echo "datagram send owner: $datagram_send (target 1)"
if [ "$file_delegate" -ne 1 ] || [ "$process_delegate" -ne 1 ] \
	|| [ "$socket_delegate" -ne 1 ] || [ "$datagram_send" -ne 1 ]; then
	echo "  -> every stream FD-backed channel must delegate; keep one datagram send owner."
	exit 1
fi

# A raw ::write(2) is sanctioned only with an in-line marker naming WHY it is
# outside the stream-write owner:
#   SIGPIPE-OWNER     — the one write_fd_without_sigpipe body itself.
#   ASYNC-CHILD-WRITE — a post-fork/pre-exec child write (async-signal-safe).
#   EVENTFD-WAKE      — an eventfd counter write (the async I/O reactor's I/O-
#                       thread wake + doorbell): a control-plane 8-byte write
#                       that cannot SIGPIPE and must not pay the owner's
#                       pthread_sigmask cost on its hot path.
raw=$(grep -rnE --include='*.cpp' '^[[:space:]]+.*::write\(' src/ \
	| grep -v 'SIGPIPE-OWNER' \
	| grep -v 'ASYNC-CHILD-WRITE' \
	| grep -v 'EVENTFD-WAKE' || true)
if [ -n "$raw" ]; then
	echo "unowned raw write(2) call(s):"
	printf '%s\n' "$raw" | sed 's/^/  /'
	echo "  -> use write_fd_without_sigpipe, or mark an ASYNC-CHILD-WRITE /"
	echo "     EVENTFD-WAKE exception."
	exit 1
fi

echo "GREEN -- stream writes share one SIGPIPE-safe owner; datagram send is singular."
