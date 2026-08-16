#!/usr/bin/env bash
# win_battery.sh — in-vivo battery for the Windows release ZIP on the REAL
# Windows host (windows-release-lane plan W5; mac_battery.sh is the
# template: stage once, per-leg rc discipline, classify by OUTPUT MARKER —
# WER swallows crash banners and interop exit codes are unreliable).
#
#   bash scripts/win_battery.sh [dist/madc-<ver>-windows-x86_64.zip]
#
# Channel (W0.2): ssh to the owner's Ubuntu WSL distro at
# host.docker.internal; WSL interop runs .exe files as GENUINE Windows
# processes (real PE loader / ntdll / ucrtbase — not wine). The zip is
# staged to a Windows-visible /mnt/c path and extracted by Windows' OWN
# tar.exe (ships since Windows 10 1803; bsdtar reads zip natively), so the
# battery exercises the artifact exactly as a user would receive it.
#
# Legs (each = one product promise):
#   extract   the zip unpacks and bin/madc.exe exists
#   jit       madc.exe JIT-compiles a C++ vector/iostream program (forest-
#             served headers — the box has no compiler installation)
#   aot       madc.exe -o emits hello.exe; it runs beside the shipped DLLs
#   ledger    -static-libmadc covers a VLA program from the embedded AOT
#             ledger; the emitted exe runs with no madc DLL
#   errno     a direct <errno.h> include works on the compiler-less host;
#             this is the standalone missing-content-husk fallback
#   refuse    a program with an error must FAIL (negative control: a
#             battery that cannot see failures proves nothing)
#
# Knobs: MADC_WIN_SSH (default derek@host.docker.internal),
#        MADC_WIN_DIR (default /mnt/c/Users/Public/madcwin),
#        MADC_WIN_TIMEOUT (per remote command, default 300),
#        MADC_WIN_KEEP=1 to keep the staged tree for debugging.
set -u
cd "$(dirname "$0")/.."

VER=$(cat VERSION)
ZIP="${1:-dist/madc-${VER}-windows-x86_64.zip}"
WIN_SSH="${MADC_WIN_SSH:-derek@host.docker.internal}"
WIN_BASE="${MADC_WIN_DIR:-/mnt/c/Users/Public/madcwin}"
TIMEOUT="${MADC_WIN_TIMEOUT:-300}"
ROOT="madc-${VER}-windows-x86_64"

if [ ! -f "$ZIP" ]; then
    echo "win_battery: $ZIP missing — run scripts/package_release_windows.sh first" >&2
    exit 1
fi

fails=0
leg() { # leg <name> <ok|FAIL> [detail]
    local name="$1" verdict="$2" detail="${3:-}"
    printf 'win_battery: LEG %-8s %s%s\n' "$name" "$verdict" "${detail:+ ($detail)}"
    [ "$verdict" = ok ] || fails=$((fails + 1))
}
remote() { # remote <cmd...> — run in the staged dir, capped
    timeout "$TIMEOUT" ssh -o BatchMode=yes "$WIN_SSH" "cd '$DIR/$ROOT' && $*"
}

DIR="$WIN_BASE/battery.$$"
if ! ssh -o BatchMode=yes "$WIN_SSH" "mkdir -p '$DIR'"; then
    echo "win_battery: channel down ($WIN_SSH)" >&2
    exit 3
fi
if ! scp -q -o BatchMode=yes "$ZIP" "$WIN_SSH:$DIR/"; then
    echo "win_battery: zip copy failed" >&2
    exit 3
fi

# --- extract: Windows' own tar.exe reads the zip (the user's door). -------
out=$(timeout "$TIMEOUT" ssh -o BatchMode=yes "$WIN_SSH" \
      "cd '$DIR' && /mnt/c/Windows/System32/tar.exe -xf $(basename "$ZIP") && ls '$ROOT/bin/madc.exe'" 2>&1)
case "$out" in
*madc.exe*) leg extract ok ;;
*)          leg extract FAIL "$out" ;;
esac

# Test sources, written on the far side (heredocs would fight ssh quoting;
# printf survives it).
remote "printf '#include <iostream>\n#include <vector>\nint main(){ std::vector<int> v; v.push_back(1); v.push_back(2); v.push_back(3); int s=0; for(size_t i=0;i<v.size();++i) s+=v[i]; std::cout << \"sum=\" << s << std::endl; return 0; }\n' > hello.cpp" \
    || leg stage FAIL "could not write test sources"
remote "printf '#include <iostream>\nint main(){ int n=3; int a[n]; a[2]=7; if (a[2]==7) std::cout << \"vla-ok\" << std::endl; return 0; }\n' > vla.cpp" || true
remote "printf '#include <errno.h>\n#include <stdio.h>\nint main(){ FILE *fp=fopen(\"C:/this/path/cannot/exist/madc-errno\",\"r\"); if(fp) return 2; printf(\"errno=%%d\\n\",errno); return errno==ENOENT?0:3; }\n' > errno.cpp" || true
remote "printf 'int main(void){ return undeclared_symbol; }\n' > bad.c" || true

# --- jit: forest-served headers, genuine PE process. -----------------------
out=$(remote "bin/madc.exe hello.cpp" 2>&1)
case "$out" in
*sum=6*) leg jit ok ;;
*)       leg jit FAIL "$out" ;;
esac

# --- aot: emit beside the DLLs, then run the emitted exe. ------------------
out=$(remote "bin/madc.exe -o bin/hello.exe hello.cpp && bin/hello.exe" 2>&1)
case "$out" in
*sum=6*) leg aot ok ;;
*)       leg aot FAIL "$out" ;;
esac

# --- ledger: -static-libmadc from the embedded AOT ledger (the VLA free
# --- rides rt_vla's ledger module). The emitted exe runs from a directory
# --- holding ONLY the C++ runtime DLLs — deliberately NOT libmadc_rt.dll:
# --- the madc runtime dependency is what the flag removes, and cout's
# --- mangled-direct libstdc++ imports are outside that promise. (The
# --- C-lane spelling of this leg is blocked on task #58 — the forest is
# --- single-config, so --std=gnu17 cannot compile on a headerless box.)
out=$(remote "bin/madc.exe -o vla.exe -static-libmadc vla.cpp && mkdir -p alone && cp vla.exe bin/libstdc++-6.dll bin/libwinpthread-1.dll alone/ && cd alone && ./vla.exe" 2>&1)
case "$out" in
*vla-ok*) leg ledger ok ;;
*)        leg ledger FAIL "$out" ;;
esac

# --- errno: task #57's unit-granular decline must have a native provider.
# --- Wine can see the build container's mingw headers through Z:, so only
# --- this real-Windows, compiler-less leg proves the embedded fallback.
out=$(remote "bin/madc.exe errno.cpp" 2>&1)
case "$out" in
*errno=2*) leg errno ok ;;
*)         leg errno FAIL "$out" ;;
esac

# --- refuse: the negative control. -----------------------------------------
out=$(remote "bin/madc.exe bad.c" 2>&1)
case "$out" in
*error*|*Error*|*undefined*) leg refuse ok ;;
*)                           leg refuse FAIL "compile error not reported: $out" ;;
esac

if [ "${MADC_WIN_KEEP:-0}" != 1 ]; then
    ssh -o BatchMode=yes "$WIN_SSH" "rm -rf '$DIR'" >/dev/null 2>&1
fi

if [ "$fails" -eq 0 ]; then
    echo "win_battery: ALL LEGS OK ($ZIP on real Windows)"
else
    echo "win_battery: $fails LEG(S) FAILED" >&2
    exit 1
fi
