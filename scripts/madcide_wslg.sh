#!/bin/bash
# madcide in a window on the Windows desktop, run from the build container:
# the container's bin/madc-release runs madcide from the checkout's source
# (tools/madcide), drawing on WSLg through WSLg's X socket, forwarded over
# ssh to the owner's WSL distro (the win_run.sh link). Files are the
# container's.
#
#   docker exec -it -u dev -w /workspace/madc madc-dev bash scripts/madcide_wslg.sh [FILE] [madcide options]
#   (no arguments: tmp/madcide-gui/hello.c)
#
# Knobs: MADC_WIN_SSH (default derek@host.docker.internal), MADC_WSLG_PORT
# (the forwarded X port, default 6077 = display :77).
export HOME="${HOME:-/home/dev}"
cd "$(dirname "$0")/.." || exit 1
WSL="${MADC_WIN_SSH:-derek@host.docker.internal}"
PORT="${MADC_WSLG_PORT:-6077}"
if ! pgrep -f "127.0.0.1:$PORT:/mnt/wslg/.X11-unix/X0" > /dev/null; then
	ssh -o BatchMode=yes -o ExitOnForwardFailure=yes -f -N \
		-L "127.0.0.1:$PORT:/mnt/wslg/.X11-unix/X0" "$WSL" \
		|| { echo "madcide_wslg: cannot reach WSL ($WSL) for its display" >&2; exit 1; }
fi
if [ $# -eq 0 ]; then
	mkdir -p tmp/madcide-gui
	[ -f tmp/madcide-gui/hello.c ] || printf '#include <stdio.h>\n\nint main(void)\n{\n\tprintf("Hello, world!\\n");\n\treturn 0;\n}\n' > tmp/madcide-gui/hello.c
	set -- tmp/madcide-gui/hello.c
fi
DISPLAY="127.0.0.1:$((PORT - 6000))" GDK_BACKEND=x11 \
	exec bin/madc-release tools/madcide/madcide.mad "$@" --gui
