# ASTRA HANDOFF — the webview spike (web-target arc, slice 1)

**For:** Codex GPT-6 Astra (first trial on this repo). **From:** Claude (s158,
2026-09-06). **Owner:** Derek Snider.

Read first: `AGENTS.md` (the rules — every `src/`/`include/` commit needs the
four trailers), `docs/agent-handoff.md`, then the design doc
[`2026-09-06-ui-web-target-and-madcide-gui.md`](2026-09-06-ui-web-target-and-madcide-gui.md)
§2.3 (platform facts), §3.2 (what the provider will be), §4 (standing
defaults), §5 slice 1, §3.8 (licences), §3.9 (what we do NOT build).

## What this is

A **spike**: a one-day, throwaway feasibility probe whose output is an
ANSWER per platform lane, not code we keep. The question:

> Can a `.mad` program, through `import madcwebview;`, open the platform's
> own webview and show a page — on the Linux build container under
> `xvfb-run`, on the owner's Mac, and on the Windows box through the WSL
> interop channel?

Slice 2 (the real web provider) starts only from a go on the lanes we need.
Do not build the provider; do not vendor for keeps; do not touch `tui_model`.

## What already exists (do not re-derive)

- **`import` (slice 0, landed 2026-09-06 on develop):** `import name;` binds
  a module's interface + library through the module map,
  `src/madc_modules.cpp` — the ONE place a library is spelled (`lib<name>.so`
  / `lib<name>.dylib` / `<name>.dll`; `scripts/check-one-library-spelling.sh`
  fails the build on a second spelling). `import name as ns;` binds a library
  under a namespace whose members resolve by name at first call, in every
  lane (JIT, exe, obj). Docs: `docs/language/import.md`. For the spike a row
  `{ "madcwebview", NULL or "madcwebview.h", "libmadcwebview.so",
  "libmadcwebview.dylib", "madcwebview.dll" }` in the map, or simply the bare
  name (the platform rule spells it), and `madc_module_open` looks beside the
  running binary's `../lib` first, then the loader's search path.
- **The dl seam:** `include/madc_dl.h` (open/sym/close, POSIX + Win32).
- **`#load "<file>" as ns;`** is the low-level verbatim-file directive (kept,
  owner ruling) — fine for a throwaway probe if the module map is not worth
  touching for a spike.
- **The build/test host is the container** (`scripts/remote_build.sh sync
  build`, `ssh -p 2299 dev@localhost`); the NAS never builds or tests.
  `scripts/provision_container.sh` owns the apt layer (add
  `libwebkitgtk-6.0-dev` + `xvfb` there — the container has neither today,
  no DISPLAY; `sudo -n` works).
- **Windows channel:** `scripts/win_suite.sh` / `scripts/win_run.sh` reach
  the owner's Windows 11 box (`derek@host.docker.internal` from the
  container). The channel lands in **WSL**; Windows executables run through
  interop and land in the logged-in desktop session (explorer in Console,
  session 1). WebView2 is UNSUPPORTED in Session 0 / non-interactive logins —
  the spike must PROVE a window appears through this path, never assume it.
  Evergreen WebView2 runtime 152.0.4191.66 is installed.
- **Mac:** owner's Mac (ssh alias `madc-mac`; macOS 15.3.2, console user
  logged in; bash 3.2, no `timeout`, use `LC_ALL=C`). The darwin cross madc
  is built on the container (`scripts/remote_build.sh release-macos`);
  binaries do not execute on Linux.

## The library

Standing default (owner veto welcome): **webview/webview** (MIT, tag 0.12.0,
C API of ~15 functions — `webview_create/destroy/run/terminate/dispatch/
set_title/set_size/navigate/set_html/init/eval/bind/unbind/return/
get_window/get_native_handle/version`). Backends: WebKitGTK 6.0 (GTK4) on
Linux — use 6.0, not 4.1; Cocoa + WKWebView on macOS; Win32 + WebView2 with
its built-in loader (mingw has no WebView2 header; the library carries its
own loader). Build it as a shared library named `libmadcwebview.so` /
`libmadcwebview.dylib` / `madcwebview.dll` per platform — for the spike a
plain `cmake` or a one-line compiler invocation in `tmp/` is fine. Do NOT
add it under `third_party/` yet; that is slice 2's vendoring (subtree, with
its notice, like MIR).

## The probe program (all three lanes, same source)

```c
import madcwebview as wv;        // or: #load "<abs path>/libmadcwebview.so" as wv;

int main()
{
    var w = wv::webview_create(0, 0);
    wv::webview_set_title(w, "madc webview spike");
    wv::webview_set_size(w, 480, 320, 0);
    wv::webview_set_html(w, "<h1 style='font-family:system-ui'>hello from madc</h1>");
    wv::webview_run(w);           // for the automated leg: webview_dispatch a terminate after N ms,
    wv::webview_destroy(w);       //   or eval JS that calls a bound function which terminates
    return 0;
}
```

Alias-form members are called with the actual argument types and return a
64-bit integer (handles are pointers; that is fine). Strings coerce to
`const char *`. If a signature needs a typed prototype (a `void *` out
parameter, a callback), declare it in a small header and `#include` it —
report that as a finding, it tells slice 2 what the interface header must
declare.

## Lanes and the verdict format

| Lane | How | Pass |
|------|-----|------|
| Linux (container) | `xvfb-run -a bin/madc tmp/spike/hello.mad` after provisioning webkitgtk-6.0 + xvfb; a `webview_dispatch`'d terminate (or a bound JS callback) ends the run | exit 0 within the timeout; a screenshot via `xwd`/`import` (ImageMagick) or a DOM read-back through a bound callback proves the page rendered |
| macOS (owner's Mac) | build the library with clang against WebKit.framework; run the same `.mad` with the darwin madc (or a native `-o` build) | a window appears on the console session; exit 0 |
| Windows (WSL channel) | build the DLL with the mingw toolchain on the container (WebView2 loader bundled), stage it beside the packed PE the way `win_run.sh` stages, run through the channel | a window appears in the desktop session (screenshot via PowerShell `System.Drawing` or the owner's eyes); or a documented NO with the Session-0 evidence |

Report ONE line per lane: **GO** / **NO-GO** / **BLOCKED (reason)**, the
exact command, the wall time, and anything the interface header must
declare. Also report the library build recipe per platform (flags, the
WebKitGTK pkg-config name, the WebView2 loader linkage) — slice 2 turns it
into `make -C src` rules.

## Rules that bite here

- Nothing under `src/` or `include/` needs to change for the spike. If it
  does (a coercion gap, a missing dl feature), that is a FINDING for the
  report — fix it only if it is small, in its own commit, with the four
  trailers (`Hypothesis:` / `Layer:` / `Searched:` / `Oracle:`), and a
  reducer in `tests/`.
- No `&&` chains in single shell invocations (script files are fine).
- Scratch files go in `tmp/` (gitignored), never in `tests/` or the root.
- Do not push anything but your own branch (`feature/webview-spike-astra`
  off `develop`); do not touch `develop`/`master`.
- Hand back: a short note in `claude_status.json`'s `live_handoff` (append,
  do not replace) + this file's "Findings" section filled in.

## Findings

(to be filled in by the spike)
