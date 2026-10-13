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

Owner scope (2026-10-13): "under Linux and MacOS, we need the full POSIX libc
header collection... under Windows, we need some additional headers than the
other platforms, but not the full 10mb (uncompressed) worth".

| Platform | Source arm | Raw size (uncompressed) |
|---|---|---|
| Linux | every file the C library's, the kernel headers' and the C++ standard library's packages install (`libc6-dev`, `linux-libc-dev`, `libstdc++-N-dev`; the package manager's own lists) | 2279 files, 21.4 MB (24.04 container) |
| Windows | every glibc header name mingw-w64/UCRT also ships, plus the Microsoft C runtime's own headers (`scripts/forest_pack_crt_windows.txt`: `io.h`, `direct.h`, `process.h`, `conio.h`, `tchar.h`, `mbstring.h`, `sec_api/*`, ...), plus what they include; never `windows.h` and the Windows API (261 files, 7 MB) or all of mingw (69 MB); the core Windows headers the frozen list names stay grove units; the whole libstdc++ tree | C 99 files 0.73 MB; libstdc++ 814 files 12.5 MB |
| macOS | every file of the staged open-licensed Apple libc tree, raw, on top of the embedded prelude's core names; every file of the pinned libc++ tree | libc 553 files 6.6 MB; libc++ 1017 files 7.4 MB |

The generator is `scripts/forest_pack_sources.sh` (`linux` / `windows` /
`darwin`); each pack script writes its list to `tmp/` (darwin: beside the
container) and hands it to the freeze and the gate. Every path is spelled as
the target's search list spells it (search dir + include name), the string a
packed madc forms when it looks a header up.

### macOS C

The darwin prelude is ONE flattened umbrella (`clang -E -dD` over the open
tree) that every core C header name includes. Growing its name list to the
full set would make it 4.7 MB (522 headers, measured) against 0.42 MB, read by
every darwin C compile. Instead the rest of the library is served RAW: the
umbrella keeps every `#define` of Apple's cdefs and availability headers and
their include guards, so a raw header reads on top of it — exactly how a Mac
with an SDK already serves `<netdb.h>` today. Each open-tree file is listed as
`name<TAB>file`: named as the hosted binary's search list spells the SDK
(`DARWIN_RUNTIME_SDK/usr/include/...`, so a Mac WITH the SDK reads its real
file first) and read from the staged tree.

## Consumer rules the source arm needs

- **Binding.** An include binds only a unit with a frozen surface (tokens, PP
  exports or include edges: `CirFrozenForest::unit_has_frozen_surface`); a
  source-only unit takes the live path, which reads its text from the pack
  where no disk has it. Binding one installed nothing (`std::deque`
  undeclared on the packed binary even with headers on disk).
- **Outranked embedded names.** Where a search dir ahead of madc's slot
  supplies an embedded name (libc++'s C wrappers on an Apple target), the
  bind looks up the WRAPPER's unit, never the embedded copy's bare name — the
  precedence the live path already applied (`embedded_header_outranked`).
  libc++'s `<cinttypes>` otherwise met the prelude's `<inttypes.h>` and
  `#error`ed.
- **Lookup order.** Exact name, then the spelling under each of the build's
  include roots — the flavor's whole table, past the `--no-sysroot-includes`
  cut (`Program::forest_unit_under_build_roots`) — then a bare path-tail,
  first in unit order. With every header packed a tail alone is ambiguous:
  `search.h` also ends libc++'s `__algorithm/search.h`.
- **Format v56.** A source-only unit writes its source slot alone, not ten
  slots; every container open indexes the segment directory, and nine empty
  slots per header multiplied it.
- **Format v57: the pack carries its own index (owner 2026-10-13: "an index
  without enough information to let you know what is being indexed is not
  much of an index").** The directory held each unit's name as a handle into
  the pack's one string pool, so "is this header packed, and where" decoded
  the whole pool and the arena first (`complete_open`). Now:
  - the directory stores every unit's name as TEXT — unit-order offsets, the
    unit numbers sorted by name, NUL-terminated bytes — written uncompressed
    and read IN PLACE in the mapped image; a lookup is a binary search
    (`CirFrozenForest::find_unit`), after `open_header` alone;
  - `CIR_FOREST_SEG_FUNC_NAMES` does the same for the bare function names the
    units declare, so an undeclared call's "does the pack declare it"
    (`forest_adopt_declared_function`) never decodes a decl index — glibc's
    `__bswap_64` (`__builtin_bswap64` → madc's `__madc_bswap64`) paid a full
    open plus every unit's decl index on every C compile to learn "no";
  - the header-text lookup opens the directory only; the full open runs only
    when frozen content is used (a bind, an adopted prototype);
  - one reader per (image, profile) per compile (`Program::forest_reader`),
    shared by the bind, the header-text lookup and the AOT ledger.
- **The bind looks before it opens.** `Program::bind_forest_directory()` is
  the exact-config carrier opened to its directory (discovery, a sibling
  TU's shared instance, the missing-container policy); an include asks it,
  and `ensure_bind_forest()` completes the open only for a unit the include
  BINDS. A dialect compile — its fragments are not packed — decoded the
  whole container on every fragment include before (0.104.0 too) and binds
  nothing.
- **The container's segment table is sorted by id** (`snapshot_writer`),
  and `snapshot_reader::find` binary-searches the table it holds; the
  per-open seg_id hash map (~5,600 inserts on this pack) remains only for
  an older unsorted blob.

Measured on the release binary (callgrind Ir, Linux container):

| program | 0.104.0 | v57 | + bind looks first + sorted table |
|---|---|---|---|
| `println("hi")` (default, packed) | 84.7 M | 86.9 M | 18.0 M → 15.1 M |
| `dialect.mad` (default, packed) | 147.2 M | 150.5 M | 81.6 M → 78.7 M |
| `c_headers.c` live | — | 244.0 M | 241.2 M |

The parse-cost gate gains `dialect.forest` and `c_headers.forest` rows
(`scripts/parse_cost/*.modes`): the default packed path of a dialect and a
C compile was never measured, which is how a whole-container decode on every
dialect compile went unseen.

Still open (owner 2026-10-13: decide on hard data, not a snap call): a
compile that binds even one small header pays the container-global decode
(11.5 MB raw from 1.63 MB stored: the string pool, the type arena, its
tokens) and a restore pass over the whole arena — `<stddef.h>` costs 174.3 M
Ir packed vs 12.7 M live (0.104.0: the same). Options and their measured
costs/gains are being gathered before any size trade.

## Measured (Linux 24.04 container, packed `libmadc.so`)

| | before | with the source arm |
|---|---|---|
| pack units | 370 | 2283 |
| packed library | 19.35 MB | 24.32 MB |
| parse-cost `c_headers.live` | 324.37 M Ir | 330.80 M Ir (v56); **244.10 M Ir (v57, −24.75%)** |
| parse-cost, the other rows (v57) | — | cxx_stl.live −0.23%, cxx_stl.forest −0.11%, subscript.live −0.06%, subscript.forest +0.12% |

v56 alone cost +0.63% to +1.98% per row: the name index was rebuilt from the
string pool on every open, and a compile opened the pack up to three times.
v57's in-place indexes remove that and the pre-existing full open of a live
compile (KG Gap `pack_opened_whole_for_source_lookups`, closed).

## Gates

- The pack step: every generated list entry has a unit with source
  (`forest_pack_gate`'s dump check, extended from the listed roots to the
  generated list).
- Headerless tests over headers outside the frozen set:
  `tests/testsysheaders_c` (`<poll.h>`, `<sys/mman.h>`, `<regex.h>`,
  `<sys/utsname.h>`, `<fnmatch.h>`, `<sys/resource.h>`, `<inttypes.h>`,
  `<libgen.h>`, `<search.h>`; POSIX-only, so `.win64_skip`) and
  `tests/testsysheaders_cxx` (`<numeric>`, `<stack>`, `<ratio>`, `<cfloat>`,
  `<cassert>`, `<cinttypes>` — libstdc++ and libc++ alike); both are also
  darwin consumer-leg programs (`forest_pack_consumers_darwin.txt`). Headers
  served but hitting parser Gaps recorded 2026-10-13: `<deque>`,
  `<queue>` (priority_queue), `<array>`, `<unordered_map>`, `<optional>`,
  libc++ `<tuple>` and `<bitset>`, `typeid` of a non-class type. On Linux,
  win64 and darwin.
- The parse-cost gate's forest rows: the source arm must not slow a compile
  that binds the grove (it is read only on a miss).
- Pack size before / after, per platform, recorded in the commit.

## Thread safety

The freeze runs once, single-threaded, at build time. The source arm is part
of the immutable mapped container; readers share it without locks, as the
grove already is.
