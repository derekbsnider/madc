# Chthonia

An easy IDE to learn C and C++ in: the editor, a C shell below it (a REPL)
and the Symbols view beside it, with Run (F5) and Stop on the toolbar. Type a
line of C in the shell and see its value; run the whole file and see its
output; the Symbols view lists what the shell's session holds.

Chthonia is built on madcide, the IDE that comes with
[madc](https://github.com/derekbsnider/madc), and runs on madc's engine:
there is no separate compiler to install.

## What it needs

A madc installation: `madc`, `libmadc`, `libmadcide` and madcide's data, of
the madc release Chthonia was built with or a newer one. On Linux, its window
needs WebKitGTK 6.0 and GTK 4 (Debian and Ubuntu: `libwebkitgtk-6.0-4
libgtk-4-1`; Fedora: `webkitgtk6.0 gtk4`).

## Installing

Chthonia installs into the madc installation it runs on.

- **Debian, Ubuntu**: with madc's `.deb` installed,
  `sudo apt install ./chthonia_<version>-1_amd64.deb`.
- **Fedora**: with madc's `.rpm` installed,
  `sudo dnf install ./chthonia-<version>-1.x86_64.rpm`.
- **madc's tarball** (Linux, macOS): unpack Chthonia's tarball into the madc
  folder, `tar -xzf chthonia-<version>-<os>-<arch>.tar.gz -C <madc folder>`.
- **Windows**: unzip `chthonia-<version>-windows-x86_64.zip` into the madc
  folder; `chthonia.exe` goes beside `madc.exe`.

## Running

    chthonia hello.c

opens a window (`--tui` uses the terminal instead). Its key style, layout
and menus are its bundle, which installs as one of madcide's plugins; Tools ▸
Key bindings… switches among madcide's key styles.

## Building from source

The scripts take the madc to use from `MADC` (default: `madc` on `PATH`) and
madcide's headers from beside it.

    scripts/build.sh                     # build/chthonia
    scripts/run_tests.sh                 # the console tests (tests/)
    xvfb-run scripts/run_tests.sh --gui  # the window tests (tests/gui/)
    scripts/package_linux.sh             # .deb, .rpm and tarball in dist/
    scripts/package_windows.sh           # the zip, with a Windows madc
    scripts/package_macos.sh             # the tarball, on a Mac
    scripts/check_install.sh <prefix>    # an installed Chthonia, checked

## Licence

Mozilla Public License 2.0 (`LICENSE`).
