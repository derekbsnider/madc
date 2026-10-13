# The pack serves every system header

Owner law, 2026-10-13: the forest pack's header list "was always supposed to
contain all system headers, not only ones to satisfy a collection of tests".
KG Gap `pack_list_not_all_system_headers`.

## Where it stands

The three lists (`scripts/forest_pack_headers.txt` 41 roots,
`_windows.txt` 35, `_darwin.txt` 22 plus the darwin prelude's C set) were grown
one root at a time, each time a test first included a header and failed the
headerless lane (v3 … v7). A user's program that includes any other system
header fails on a machine without headers. On the 24.04 build container the
public set is 242 glibc headers (no `bits/`, no `gnu/`) and 113 C++ standard
library headers; the list names about 40.

## Design

Two arms, as now, with the source arm made complete:

1. **The source arm holds every system header.** A new freeze input,
   `--freeze-sources=<list>`, names files whose exact text joins the container
   as source-only units (records and tokens empty — already a legal directory
   entry, `cir_freeze.cpp` unit writer). They are not parsed at pack time, so
   headers that cannot share one translation unit (glibc vs kernel
   definitions) never meet. A packed binary already serves such a unit:
   `forest_source_path` / `forest_source_text` find it by path, then by path
   tail, and the lexer re-tokenizes it under the live policy.
2. **The frozen grove stays the common set**, parsed once at pack time — a
   speed-up for the headers most programs include, never the coverage
   boundary. A header that does not freeze quietly is a parser defect, not a
   reason to leave it out of the source arm.

The source list is GENERATED from each platform's include roots by the pack
step, never hand-kept:

| Platform | Source arm | Raw size |
|---|---|---|
| Linux | every header under the C library's include roots (glibc, public and internal, since public headers include the internal ones) and the C++ standard library's | glibc 2.3 MB, libstdc++ 12.4 MB |
| Windows | the mingw/UCRT equivalents of the Linux libc set plus the core Windows headers the list already carries — not the whole Windows SDK (owner 2026-10-13) | measured at implementation |
| macOS | libc + libc++ from the SDK, the same scope as Linux | measured at implementation |

The kernel UAPI tree (988 headers, 6.6 MB) is reached through the closure of
the glibc headers that include it; the rest is a question for the owner if a
program needs `<linux/*.h>` directly.

## Gates

- The pack step: every generated list entry has a unit with source
  (`forest_pack_gate`'s dump check, extended from the listed roots to the
  generated list).
- A headerless test that includes a sample from each family outside the
  frozen set (`<sys/wait.h>`, `<poll.h>`, `<sys/mman.h>`, `<regex.h>`, ...),
  on Linux, win64 and darwin.
- The parse-cost gate's forest rows: the source arm must not slow a compile
  that binds the grove (it is read only on a miss).
- Pack size before / after, per platform, recorded in the commit.

## Thread safety

The freeze runs once, single-threaded, at build time. The source arm is part
of the immutable mapped container; readers share it without locks, as the
grove already is.
