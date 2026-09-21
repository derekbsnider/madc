# Nexus L4a — the git substrate (libgit2 vendored, `madc::GitRepo`, the `git` source adapter, `madc::git_*`) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give madc a read-only, in-process view of a git repository — HEAD,
rev-parse, a path-filtered log, a blob at a revision, blame over a line range,
a dirty check — through ONE owner class over a vendored libgit2, exposed to
madcdis as a source adapter and to the dialect as `madc::git_*` publics, so the
L4b PAST verbs (`graph.history / commits / revision / diff / status`) have a
substrate to stand on.

**Architecture:** Four layers, each doing one thing. (1) **`third_party/libgit2`**
— upstream v1.9.7 as an UNMODIFIED subtree; (2) **`third_party/libgit2-madc/`** —
madc's build of it: a hand-written `Makefile.madc` (the same sources and defines
CMake would use; CMake is never run) plus the committed per-platform feature
headers, producing `obj/libgit2/<variant>/libgit2.a` exactly like `$(MIRLIB)`;
(3) **`madc::GitRepo`** (`include/madcdis/git_repo.h`, `src/madcdis_git_repo.cpp`)
— the ONE place a `git_*` libgit2 call is made, read-only by construction, plus
the `git_source_adapter` beside it and a `git` scheme row in `DataSource`;
(4) **the engine face** — a `handle_table` of open repositories and
value-shaped wrappers (`src/madc_git.cpp`), bridged through the five layers
the parse/graph publics use (`parser.cpp` → `ns_common.h` → `ns_madc.cpp` →
`include/madc/ns_madc`).

**Tech Stack:** libgit2 1.9.7 (C90, bundled pcre2 / xdiff / llhttp / sha1dc /
rfc6234, system zlib), GNU make, C++11 (madcdis + engine), doctest unit tests,
value-first `.mad` integration tests, bash gates with negative controls.

**Spec:** `docs/plans/2026-09-13-nexus-L4-design.md` §4.1 (vendoring), §4.2
(`GitRepo` + adapter + publics), §7 (thread-safety), §8 L4a (tests), §9 (slice
table), §10 (7: bundled pcre), §11 (laws). Owner decision 2026-09-13: "vendor it
the MIR way, network off, behind a madcdis source adapter". Owner approval of
the design: 2026-09-13 ("I think this sounds good, let's do it").

## Decisions this plan settles (spec §4.1 details the design left to the plan)

1. **Version = v1.9.7** (tag `49e408b3208bc3093757a1c2db938d3590f3f412`, the
   newest 1.9.x release on 2026-09-13; listed by `git ls-remote --tags`).
2. **Squashed subtree, not full history** (differs from the MIR import, §4.2 of
   `mir-into-madc-repo-2026-08-11.md`, on purpose): MIR is maintained as madc
   source WITH fixes, so its ancestry earns its place in `git log`; libgit2
   carries ZERO madc edits, upstream comparison happens against the tag, and
   ~15 000 foreign commits would swamp the history every rehydration reads.
   `git subtree add --squash` records the split sha in the squash commit
   (`git-subtree-split`), so provenance is exact. The trailer gate skips both
   the squash commit (no root `VERSION` in its snapshot) and the merge. **Open
   to the owner's veto before the import is PUSHED** — a full-history import is
   the same command without `--squash`.
3. **Config dir OUTSIDE the subtree: `third_party/libgit2-madc/`.** A
   `git subtree pull` can then never touch a madc-owned file (the design said
   "inside"; beside is strictly safer and is what `scripts/gen_webview_header.py`
   does for the webview subtree). Files: `Makefile.madc`, `README.md`,
   `linux/git2_features.h`, `linux/pcre2/config.h`, `darwin/…`, `win32/…`.
4. **SHA1 = the bundled collision-detecting backend** (`GIT_SHA1_COLLISIONDETECT`,
   `hash/collisiondetect.c` + `hash/sha1dc/`): libgit2 1.9 has NO "builtin" SHA1
   (the CMake `USE_SHA1` values are CollisionDetection | OpenSSL* | CommonCrypto
   | mbedTLS | Win32 — `cmake/SelectHashes.cmake`); the design's "builtin SHA1"
   is corrected to this. SHA256 = `GIT_SHA256_BUILTIN` (`hash/builtin.c` +
   `hash/rfc6234/`).
5. **Measured, not estimated (the spike, 2026-09-13, this container):** the
   static archive is 3.2 MB; a program linking exactly the read API `GitRepo`
   uses (open, head, revparse, revwalk + tree lookup per commit, blob, blame,
   status) is **1.40 MB stripped with clang -O2** (1.65 MB with gcc -O3 through
   CMake) — under the design's 3 MB stop-and-ask line, so the slice proceeds.
   Of that, ~150 KB is plain-HTTP / smart-protocol code pulled through
   `branch.c → remote.c → transport.c`'s table; unreachable from madc (no
   `git_remote_fetch`/`clone`/`push` is ever called — the one-git-owner gate),
   compiled without TLS/SSH/auth, and left in rather than patching upstream
   (zero divergence beats 150 KB).
6. **One host archive serves every host-running mode.** libgit2 has no target
   knobs (unlike MIR's codegen variants): `develop`, `debug`, `release` and the
   emit-only `cross-*` modes all link `obj/libgit2/host/libgit2.a`; only the
   hosted cross builds (`hosted-arm64-macos`, `hosted-x86-64-macos`,
   `hosted-x86-64-windows`) build their own variant with their toolchain.
7. **Fixtures:** the unit test builds its repository IN the test through
   libgit2's own write API (the archive carries it regardless — `GitRepo` just
   never calls it); the dialect test reads THE MADC REPOSITORY ITSELF (the
   runner's cwd is the checkout root on every lane) and asserts SHAPE, never a
   sha — no `git` binary, no tarball, no lane skip.
8. **Handle publics return 0 / `{error}`**, never throw: `git_open` answers 0
   for a non-repository; every value-returning public answers `{error: <libgit2
   message>}` on failure — the L3 `graph_error_result` shape the seat already
   turns into `isError`.

## Global Constraints

Every task's requirements implicitly include this section.

- **Zero source edits under `third_party/libgit2/`.** A needed fix goes
  upstream first. madc-owned build files live in `third_party/libgit2-madc/`.
- **Network OFF in every committed feature header:** `GIT_HTTPS`, `GIT_SSH`,
  `GIT_SSH_EXEC`, `GIT_SSH_LIBSSH2`, `GIT_NTLM`, `GIT_GSSAPI`, `GIT_GSSFRAMEWORK`,
  `GIT_WINHTTP`, `GIT_OPENSSL*`, `GIT_MBEDTLS`, `GIT_SCHANNEL`,
  `GIT_SECURE_TRANSPORT` are ALL undefined; `deps/ntlmclient`, `deps/winhttp`,
  `deps/zlib`, `deps/chromium-zlib` are never compiled. Gated by
  `scripts/check-libgit2-features.sh`.
- **ONE git owner:** every `git_*` libgit2 call lives in
  `src/madcdis_git_repo.cpp`; no `exec://git` anywhere in `src/`, `include/`,
  `tools/`. Gated by `scripts/check-one-git-owner.sh`. `GitRepo` is READ-ONLY
  (no write API is wrapped).
- **Build products under `obj/libgit2/<variant>/`**, never inside the subtree;
  `git2clean` removes them, `clean` does not (the MIR model).
- **Rule trailers** (`Hypothesis / Layer / Searched / Oracle`) on every commit
  touching `src/` or `include/`. Subtree, `third_party/libgit2-madc/`,
  `src/Makefile`, scripts, tests and docs ride without.
- **Zero warnings** on every surface: libgit2 compiles under its own upstream
  warning set (0 warnings measured with clang 18); madc code under madc's.
- **Thread contract (design §7):** `GitRepo`, its handle table and every public
  are CONFINED to the thread that opened the handle (the runtime-eval
  confinement the parse handles state); `git_libgit2_init` runs once per
  process behind a static guard. Stated in the header comment.
- **Value-first dialect code:** `var`, out-param carriers, bare `println`, no
  includes, no `std::`; `.expect` lines never carry a sha or a count.
- **madcdis stays DataDef-agnostic:** the adapter emits `value` records with a
  `SourceLocator`; nothing in madcdis includes engine headers.
- **Targeted tests only** (`make -C src` + the new unit binary + `testgit` +
  the gates); the battery rides the V6 arc seam.

---

## File structure

| Path | Action | Responsibility |
|---|---|---|
| `third_party/libgit2/` | subtree add (squash) | upstream libgit2 v1.9.7, untouched |
| `third_party/libgit2-madc/Makefile.madc` | create | the build recipe (sources + defines CMake would use) |
| `third_party/libgit2-madc/README.md` | create | what this is, the version, how to rebuild / bump |
| `third_party/libgit2-madc/{linux,darwin,win32}/git2_features.h` | create | the feature header CMake would generate, per platform |
| `third_party/libgit2-madc/{linux,darwin,win32}/pcre2/config.h` | create | the bundled pcre2 config CMake would generate |
| `scripts/check-libgit2-features.sh` | create | network-off gate (+ negative control) |
| `scripts/check-one-git-owner.sh` | create | one-git-owner gate (+ negative control) |
| `src/Makefile` | modify | `GIT2*` variables, per-variant archive rules, `git2clean`, `$(GIT2LIB)` on every `$(MIRLIB)` link line, `-I$(GIT2DIR)/include`, `-lsecur32` on win64, fulltest gate lines, two new objects |
| `include/madcdis/git_repo.h` | create | `GitRepo` (read-only wrapper), record structs, `git_source_adapter` |
| `src/madcdis_git_repo.cpp` | create | the ONE libgit2 caller; the adapter |
| `include/libmadc/datasource.h` | modify | the `git` scheme row |
| `src/madc_git.cpp` | create | handle table + value-shaped `internal_program_git_*` |
| `src/parser.cpp` | modify | forward decls + `madc_git_*` bridges (beside the graph ones) |
| `include/ns_common.h` | modify | `madc_git_*` C-shaped decls |
| `src/ns_madc.cpp` | modify | `madc::git_*` wrappers |
| `include/madc/ns_madc` | modify | `madc::git_*` declarations (the dialect face) |
| `tests/unit/test_gitrepo.cpp` | create | fixture repo via libgit2; `GitRepo` + adapter cases |
| `tests/testgit.mad` (+ `.expect`, `.expect_quiet`) | create | the dialect publics over the madc repo itself |
| `docs/plans/2026-09-13-nexus-L4-design.md` | modify | §4.1/§4.2 corrections (SHA1, config dir, adapter file, measured size) |

---

### Task 0: The size spike — DONE 2026-09-13 (recorded here as evidence)

Shallow-cloned v1.9.7 to `tmp/libgit2-probe`; configured with CMake
(`-DBUILD_SHARED_LIBS=OFF -DUSE_HTTPS=OFF -DUSE_SSH=OFF -DUSE_NTLMCLIENT=OFF
-DUSE_GSSAPI=OFF -DUSE_ICONV=OFF -DUSE_BUNDLED_ZLIB=OFF -DREGEX_BACKEND=builtin
-DUSE_SHA1=CollisionDetection -DUSE_SHA256=builtin -DUSE_HTTP_PARSER=builtin
-DUSE_THREADS=ON -DUSE_NSEC=OFF`): 0 warnings, archive 3.55 MB, a read-API
probe 1.65 MB stripped. Then built the SAME sources with the hand-written
`Makefile.madc` below (no CMake, clang 18): 0 warnings, archive 3.2 MB, the
probe 1.40 MB stripped, and it ran against this repository (head, HEAD~1, a
path-filtered log of `VERSION`, the blob, a blame, the status list). Verdict:
proceed (< 3 MB).

---

### Task 1: Vendor libgit2 v1.9.7 + the madc build recipe + the network-off gate

**Files:**
- Subtree: `third_party/libgit2/`
- Create: `third_party/libgit2-madc/Makefile.madc`, `README.md`,
  `linux/git2_features.h`, `linux/pcre2/config.h`, `darwin/git2_features.h`,
  `darwin/pcre2/config.h`, `win32/git2_features.h`, `win32/pcre2/config.h`
- Create: `scripts/check-libgit2-features.sh`
- Modify: `src/Makefile` (fulltest gate line, beside `check-one-spawn-owner.sh`
  at `src/Makefile:1088`)

**Interfaces:**
- Produces: `make -f third_party/libgit2-madc/Makefile.madc SRC=<subtree>
  BUILD_DIR=<dir> PLATFORM=linux|darwin|win32 CC=<cc> AR=<ar> CFLAGS_EXTRA=<…>`
  → `<dir>/libgit2.a`; public headers at `third_party/libgit2/include`.

- [ ] **Step 1: Import the subtree (squash — decision 2; drop `--squash` only if the owner vetoes)**

Working tree must be clean (`git status --short` shows only the owner's
untracked `donut.c test.mad testsort.mad`).

```bash
git subtree add --prefix=third_party/libgit2 https://github.com/libgit2/libgit2.git v1.9.7 --squash
```

Expected: two new commits (`Squashed 'third_party/libgit2/' content from commit
49e408b3` and the merge). Verify the snapshot:

```bash
test -f third_party/libgit2/src/libgit2/repository.c
test -f third_party/libgit2/src/util/git2_features.h.in
grep -c 'LIBGIT2_VERSION "1.9.7"' third_party/libgit2/include/git2/version.h
```

Expected: both files exist; the version grep prints `1`.

- [ ] **Step 2: Write `third_party/libgit2-madc/Makefile.madc`** (verified content —
  it built v1.9.7 with clang 18 at 0 warnings and the probe ran):

```make
# Makefile.madc — madc's build of the vendored libgit2 (third_party/libgit2, an
# UNMODIFIED upstream subtree; design docs/plans/2026-09-13-nexus-L4-design.md
# §4.1). libgit2 ships CMake; madc does not run it. This file lists the same
# sources CMake globs for the configuration madc uses and compiles them with
# the same defines, against the COMMITTED feature headers beside this file
# (<platform>/git2_features.h, <platform>/pcre2/config.h — the two headers
# CMake would generate). Products land in BUILD_DIR (obj/libgit2/<variant>),
# never inside the subtree. Invoked by src/Makefile like $(MIRLIB).
#
# Configuration (the owner's "network OFF — the Nexus only READS history"):
#   threads ON · HTTPS/SSH/NTLM/GSSAPI OFF (no TLS, no libssh2, no auth libs)
#   regex = bundled pcre2 · SHA1 = bundled collision-detecting · SHA256 = bundled
#   zlib = the system -lz madc already links · xdiff + llhttp bundled
#   deps/zlib, deps/chromium-zlib, deps/ntlmclient, deps/winhttp: NOT built
# The unreachable plain-HTTP/smart-protocol objects still link (branch.c ->
# remote.c -> the transport table; ~150 KB of 1.4 MB, measured 2026-09-13);
# the one-git-owner gate keeps madc from ever calling into them.
#
# Parameters: SRC (subtree root) · BUILD_DIR · PLATFORM = linux|darwin|win32 ·
# CC · AR · CFLAGS_EXTRA. Gate: scripts/check-libgit2-features.sh.

SRC          ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../libgit2)
CFG          := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_DIR    ?= build
PLATFORM     ?= linux
CC           ?= cc
AR           ?= ar
CFLAGS_EXTRA ?=

ifeq ($(PLATFORM),win32)
OS_SRC    := $(wildcard $(SRC)/src/util/win32/*.c)
PLAT_DEFS := -DWIN32 -D_WIN32_WINNT=0x0600
PIC       :=
else
OS_SRC    := $(wildcard $(SRC)/src/util/unix/*.c)
PLAT_DEFS := -D_GNU_SOURCE
PIC       := -fPIC
endif

# The same globs src/libgit2/CMakeLists.txt and src/util/CMakeLists.txt use.
GIT2_SRC   := $(wildcard $(SRC)/src/libgit2/*.c $(SRC)/src/libgit2/streams/*.c $(SRC)/src/libgit2/transports/*.c)
UTIL_SRC   := $(wildcard $(SRC)/src/util/*.c $(SRC)/src/util/allocators/*.c) $(OS_SRC) \
              $(SRC)/src/util/hash/collisiondetect.c $(wildcard $(SRC)/src/util/hash/sha1dc/*.c) \
              $(SRC)/src/util/hash/builtin.c $(wildcard $(SRC)/src/util/hash/rfc6234/*.c)
XDIFF_SRC  := $(wildcard $(SRC)/deps/xdiff/*.c)
LLHTTP_SRC := $(wildcard $(SRC)/deps/llhttp/*.c)
# deps/pcre2/CMakeLists.txt's PCRE2_SOURCES = every .c but the fuzz harness.
PCRE2_SRC  := $(filter-out %/pcre2_fuzzsupport.c,$(wildcard $(SRC)/deps/pcre2/*.c))

ALL_SRC := $(GIT2_SRC) $(UTIL_SRC) $(XDIFF_SRC) $(LLHTTP_SRC) $(PCRE2_SRC)
OBJS    := $(patsubst $(SRC)/%.c,$(BUILD_DIR)/%.o,$(ALL_SRC))

INCLUDES := -I$(SRC)/src/libgit2 -I$(SRC)/src/util -I$(SRC)/include -I$(CFG)/$(PLATFORM) \
            -I$(SRC)/deps/llhttp -I$(SRC)/deps/pcre2 -I$(SRC)/deps/xdiff
# libgit2's own warning set (cmake/DefaultCFlags.cmake) minus the clang-only
# documentation flag; C90 + extensions off, as CMake sets C_STANDARD 90.
WARN     := -Wall -Wextra -Wno-missing-field-initializers -Wmissing-declarations \
            -Wstrict-aliasing -Wstrict-prototypes -Wdeclaration-after-statement \
            -Wshift-count-overflow -Wunused-const-variable -Wunused-function \
            -Wint-conversion -Wformat -Wformat-security
CFLAGS   := -std=c90 -O2 -DNDEBUG -fvisibility=hidden $(PIC) $(PLAT_DEFS) \
            -DPCRE2_STATIC -DPCRE2_EXPORT= -DPCRE2_EXP_DECL= -DPCRE2_EXP_DEFN= \
            $(INCLUDES) $(WARN) $(CFLAGS_EXTRA)
UTIL_DEFS  := -DSHA1DC_NO_STANDARD_INCLUDES=1 \
              -DSHA1DC_CUSTOM_INCLUDE_SHA1_C='"git2_util.h"' \
              -DSHA1DC_CUSTOM_INCLUDE_UBC_CHECK_C='"git2_util.h"'
# pcre2 sees ONLY its own directories (as CMake compiles it): its
# `#include "config.h"` must resolve to the committed pcre2/config.h, and
# src/libgit2/config.h (the git-config subsystem) would shadow it.
PCRE2_CFLAGS := -std=c90 -O2 -DNDEBUG -fvisibility=hidden $(PIC) $(PLAT_DEFS) \
                -DPCRE2_STATIC -DPCRE2_EXPORT= -DPCRE2_EXP_DECL= -DPCRE2_EXP_DEFN= \
                -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 \
                -I$(CFG)/$(PLATFORM)/pcre2 -I$(SRC)/deps/pcre2 $(WARN) $(CFLAGS_EXTRA)
# The bundled deps carry upstream's own per-file suppressions
# (deps/llhttp/CMakeLists.txt, deps/xdiff/CMakeLists.txt) — applied here per
# directory, nothing beyond what upstream disables.
DEPS_WARN_OFF := -Wno-unused-parameter -Wno-missing-declarations -Wno-sign-compare

.PHONY: all clean
all: $(BUILD_DIR)/libgit2.a

$(BUILD_DIR)/libgit2.a: $(OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# GNU make picks the pattern rule with the shortest stem: the util and deps
# rules win for their trees, everything else takes the generic rule.
$(BUILD_DIR)/src/util/%.o: $(SRC)/src/util/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(UTIL_DEFS) -c $< -o $@

$(BUILD_DIR)/deps/pcre2/%.o: $(SRC)/deps/pcre2/%.c
	@mkdir -p $(@D)
	$(CC) $(PCRE2_CFLAGS) -c $< -o $@

$(BUILD_DIR)/deps/llhttp/%.o: $(SRC)/deps/llhttp/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPS_WARN_OFF) -c $< -o $@

$(BUILD_DIR)/deps/xdiff/%.o: $(SRC)/deps/xdiff/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPS_WARN_OFF) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
```

- [ ] **Step 3: Write the feature headers.** `linux/git2_features.h` is EXACTLY
  what the CMake configure of Task 0 generated (copy
  `tmp/libgit2-probe/build/gen_headers/git2_features.h` if the probe tree is
  present; otherwise write the file below — it is the same text). Its defined
  macros are:

```c
#define GIT_THREADS 1
#define GIT_ARCH_64 1
#define GIT_USE_STAT_MTIM 1
#define GIT_USE_FUTIMENS 1
#define GIT_REGEX_BUILTIN 1
#define GIT_QSORT_GNU
#define GIT_HTTPPARSER_BUILTIN 1
#define GIT_SHA1_COLLISIONDETECT 1
#define GIT_SHA256_BUILTIN 1
#define GIT_COMPRESSION_ZLIB 1
#define GIT_RAND_GETENTROPY 1
#define GIT_RAND_GETLOADAVG 1
#define GIT_IO_POLL 1
#define GIT_IO_SELECT 1
```

every other `#cmakedefine` of `src/util/git2_features.h.in` rendered as
`/* #undef NAME */`, inside `#ifndef INCLUDE_features_h__ / #define
INCLUDE_features_h__ … #endif`.

`darwin/git2_features.h` = linux with `GIT_USE_STAT_MTIM` → `GIT_USE_STAT_MTIMESPEC 1`
(Apple's `struct stat` spells `st_mtimespec`) and `GIT_QSORT_GNU` → `GIT_QSORT_BSD`
(Apple's `qsort_r` takes the context FIRST — `src/CMakeLists.txt`'s BSD probe).
`getentropy`, `getloadavg`, `futimens`, `poll`, `select` all exist on the
supported macOS floors.

`win32/git2_features.h` = linux minus `GIT_USE_STAT_MTIM`, `GIT_USE_FUTIMENS`,
`GIT_QSORT_GNU`, `GIT_RAND_GETENTROPY`, `GIT_RAND_GETLOADAVG`, `GIT_IO_POLL`,
`GIT_IO_SELECT`, plus `GIT_IO_WSAPOLL 1` (what `src/CMakeLists.txt` sets under
`if(WIN32)`). No `GIT_QSORT_*` on win32: `util.c` then uses its own
`insertsort` fallback (correct; mingw's `qsort_s` prototype is not the MSC one
libgit2 probes for). `rand.c` has its own `GIT_WIN32` arm.

`linux/pcre2/config.h` = the generated `tmp/libgit2-probe/build/deps/pcre2/config.h`
(defines `HAVE_ASSERT_H HAVE_BUILTIN_MUL_OVERFLOW HAVE_BUILTIN_UNREACHABLE
HAVE_ATTRIBUTE_UNINITIALIZED HAVE_DIRENT_H HAVE_SYS_STAT_H HAVE_SYS_TYPES_H
HAVE_UNISTD_H HAVE_MEMFD_CREATE HAVE_SECURE_GETENV SUPPORT_PCRE2_8
SUPPORT_UNICODE`, `LINK_SIZE 2`, `HEAP_LIMIT 20000000`, `MATCH_LIMIT 10000000`,
`MATCH_LIMIT_DEPTH MATCH_LIMIT`, `MAX_VARLOOKBEHIND 255`, `NEWLINE_DEFAULT 2`,
`PARENS_NEST_LIMIT 250`, `MAX_NAME_SIZE 128`, `MAX_NAME_COUNT 10000`).
`darwin/pcre2/config.h` = linux minus `HAVE_MEMFD_CREATE`, `HAVE_SECURE_GETENV`
(glibc-only). `win32/pcre2/config.h` = darwin plus `HAVE_WINDOWS_H 1`.

The staged copies of all six headers from the spike are in
`tmp/libgit2-madc/{linux,darwin,win32}/` (session-local; regenerate from the
rules above if absent).

- [ ] **Step 4: Write `third_party/libgit2-madc/README.md`:**

```markdown
# libgit2 in madc

`../libgit2` is upstream libgit2 **v1.9.7** (`49e408b3`), imported as a
SQUASHED `git subtree` on 2026-09-13 (design
`docs/plans/2026-09-13-nexus-L4-design.md` §4.1). It carries ZERO madc edits —
a needed fix goes upstream first. This directory is madc's build of it:

- `Makefile.madc` — the sources and defines CMake would use, without CMake;
  `src/Makefile` runs it into `obj/libgit2/<variant>/libgit2.a` like `$(MIRLIB)`.
- `<platform>/git2_features.h`, `<platform>/pcre2/config.h` — the two headers
  CMake would generate, committed per platform (`linux`, `darwin`, `win32`).

Configuration: threads on; HTTPS / SSH / NTLM / GSSAPI OFF (madc only READS
local history — `scripts/check-libgit2-features.sh` fails the build if a
network transport is ever enabled); regex = bundled pcre2; SHA1 = bundled
collision-detecting; SHA256 = bundled; zlib = the system one madc links.

The ONE consumer is `madc::GitRepo` (`src/madcdis_git_repo.cpp`, read-only;
`scripts/check-one-git-owner.sh`).

Bumping: `git subtree pull --prefix=third_party/libgit2
https://github.com/libgit2/libgit2.git <tag> --squash`, then re-diff
`src/util/git2_features.h.in` and `deps/pcre2/config.h.in` against the
committed headers and re-run the size spike (`Task 0` of the L4a plan).
```

- [ ] **Step 5: Write `scripts/check-libgit2-features.sh`:**

```bash
#!/bin/bash
# GATE — libgit2 is vendored with every NETWORK transport OFF (owner decision
# 2026-09-13: the Nexus only READS local history; push/fetch stay git's).
# The committed feature headers under third_party/libgit2-madc/<platform>/ are
# the one place a transport could be switched on, and Makefile.madc is the one
# place a network dependency could be compiled in. Fail on either.
set -u
cd "$(dirname "$0")/.."

fail=0
net='GIT_(HTTPS|SSH|SSH_EXEC|SSH_LIBSSH2|SSH_LIBSSH2_MEMORY_CREDENTIALS|NTLM|GSSAPI|GSSFRAMEWORK|WINHTTP|OPENSSL|OPENSSL_DYNAMIC|SECURE_TRANSPORT|MBEDTLS|SCHANNEL)'

check_header() {
	# $1 = a git2_features.h; a network macro DEFINED (not "/* #undef */") fails
	local hits
	hits=$(grep -nE "^[[:space:]]*#[[:space:]]*define[[:space:]]+$net\b" "$1")
	if [ -n "$hits" ]; then
		echo "check-libgit2-features: network transport enabled in $1:"
		echo "$hits" | sed 's/^/  /'
		fail=1
	fi
}

for h in third_party/libgit2-madc/*/git2_features.h; do
	[ -f "$h" ] || { echo "check-libgit2-features: no feature headers found"; exit 1; }
	check_header "$h"
done

if grep -nE 'deps/(ntlmclient|winhttp|zlib|chromium-zlib)' third_party/libgit2-madc/Makefile.madc \
	| grep -vE '^[0-9]+:#' >/dev/null; then
	echo "check-libgit2-features: Makefile.madc compiles a network/zlib dependency:"
	grep -nE 'deps/(ntlmclient|winhttp|zlib|chromium-zlib)' third_party/libgit2-madc/Makefile.madc | grep -vE '^[0-9]+:#' | sed 's/^/  /'
	fail=1
fi

# Negative control: a header with HTTPS on must be caught, or the gate is dead.
ctrl=$(mktemp)
printf '#define GIT_THREADS 1\n#define GIT_HTTPS 1\n' > "$ctrl"
before=$fail
fail=0
check_header "$ctrl" >/dev/null
if [ "$fail" -ne 1 ]; then
	echo "check-libgit2-features: NEGATIVE CONTROL FAILED (GIT_HTTPS 1 not caught)"
	rm -f "$ctrl"
	exit 1
fi
fail=$before
rm -f "$ctrl"

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "check-libgit2-features: GREEN — every transport off in every platform header; no network dependency compiled."
exit 0
```

- [ ] **Step 6: Run the gate and the recipe by hand**

```bash
bash scripts/check-libgit2-features.sh
make -f third_party/libgit2-madc/Makefile.madc BUILD_DIR=tmp/git2check PLATFORM=linux CC=clang -j8 2>&1 | grep -c 'warning:'
ls -la tmp/git2check/libgit2.a
```

Expected: `GREEN …`; `0`; the archive (~3.2 MB). Then `rm -rf tmp/git2check`.

- [ ] **Step 7: Wire the gate into fulltest** — `src/Makefile`, after the
  `check-one-spawn-owner.sh` line (1088):

```make
	@cd .. && bash scripts/check-libgit2-features.sh
```

- [ ] **Step 8: Commit** (the subtree commits already exist; this adds the recipe):

```bash
git add third_party/libgit2-madc scripts/check-libgit2-features.sh src/Makefile
git commit -F tmp/l4a_commit1.msg
```

Message: `build(third_party): vendor libgit2 v1.9.7 (squashed subtree) + madc's
build recipe and committed feature headers (network OFF) + check-libgit2-features gate`
— body: decisions 1–5 above in prose, the measured sizes.

---

### Task 2: Build wiring — `$(GIT2LIB)` beside `$(MIRLIB)` on every link line

**Files:**
- Modify: `src/Makefile` — after the MIR block (`:157-164`), the hosted darwin
  block (`:350`), the hosted windows block (`:419`, `:451`), the rules block
  (after `:849 mirclean`), every `$(MIRLIB)` link site (`:1190-1191`, `:1229`,
  `:1232`, `:1254-1255`, `:1282-1284`, `:1413-1415`, `:1429`), `DEFINES` (`:168`).

**Interfaces:**
- Produces: `GIT2DIR`, `GIT2CFG`, `GIT2BUILD`, `GIT2LIB`, `GIT2PLATFORM`;
  `obj/libgit2/host/libgit2.a` on a host build; `-I$(GIT2DIR)/include` on every
  madc TU; `git2clean`.

- [ ] **Step 1: Variables** — after `MIRBUILD = $(abspath ../obj/mir)` (`:164`):

```make
# libgit2 lives IN this repository too (third_party/libgit2, an unmodified
# upstream subtree; madc's build recipe + committed feature headers are in
# third_party/libgit2-madc — design docs/plans/2026-09-13-nexus-L4-design.md
# §4.1). Products land under ../obj/libgit2/<variant>; `git2clean` removes
# them (outside `clean`, the MIR model). libgit2 has no target knobs, so ONE
# host archive serves every host-running mode (develop / debug / release /
# the emit-only cross-* modes); the hosted cross builds make their own below.
GIT2DIR   = $(abspath ../third_party/libgit2)
GIT2CFG   = $(abspath ../third_party/libgit2-madc)
GIT2BUILD = $(abspath ../obj/libgit2)
GIT2LIB   = $(GIT2BUILD)/host/libgit2.a
GIT2PLATFORM = linux
```

and extend `DEFINES` (`:168`) with `-I$(GIT2DIR)/include` (so `GitRepo.cpp` and
the unit test see `<git2.h>`; nothing else includes it).

- [ ] **Step 2: Hosted darwin** — beside `MIRLIB = $(MIRBUILD)/$(MODE)/libmir.a`
  (`:350`):

```make
GIT2LIB = $(GIT2BUILD)/$(MODE)/libgit2.a
GIT2PLATFORM = darwin
```

- [ ] **Step 3: Hosted windows** — beside `MIRLIB` (`:419`):

```make
GIT2LIB = $(GIT2BUILD)/$(MODE)/libgit2.a
GIT2PLATFORM = win32
```

and in that block's `LIBS` (`:451`) add `-lsecur32` right after `-lws2_32`
(libgit2's win32 util uses SSPI symbols even with auth OFF; `src/CMakeLists.txt`
lists `ws2_32 secur32`).

- [ ] **Step 4: Rules** — after `mirclean:` (`:849`):

```make
# libgit2 archives: madc's own recipe over the unmodified subtree (no CMake).
# The host archive is built with the host compiler; the hosted variants with
# that mode's CC/AR (the darwin/mingw toolchains), each into its own dir.
$(GIT2BUILD)/host/libgit2.a: FORCE
	mkdir -p $(GIT2BUILD)/host
	$(MAKE) -f $(GIT2CFG)/Makefile.madc SRC=$(GIT2DIR) BUILD_DIR=$(GIT2BUILD)/host PLATFORM=linux CC='$(CC)' AR='$(AR)'

$(GIT2BUILD)/hosted-arm64-macos/libgit2.a $(GIT2BUILD)/hosted-x86-64-macos/libgit2.a $(GIT2BUILD)/hosted-x86-64-windows/libgit2.a: FORCE
	mkdir -p $(GIT2BUILD)/$(MODE)
	$(MAKE) -f $(GIT2CFG)/Makefile.madc SRC=$(GIT2DIR) BUILD_DIR=$(GIT2BUILD)/$(MODE) PLATFORM=$(GIT2PLATFORM) CC='$(CC)' AR='$(AR)'

git2clean:
	rm -rf $(GIT2BUILD)
```

(`FORCE` delegates up-to-dateness to the recipe's own dependencies, as the MIR
rules do; a no-op rebuild costs one `make` walk.)

- [ ] **Step 5: Link sites** — add ` $(GIT2LIB)` immediately after every
  `$(MIRLIB)` on the lines `:1190`, `:1191`, `:1229`, `:1232`, `:1254`, `:1255`,
  `:1282`, `:1284`, `:1413`, `:1415`, `:1429` (prerequisite lists AND command
  lines: the archive is a dependency and a link input everywhere libmir is).

- [ ] **Step 6: Build and verify**

```bash
make -C src 2>&1 | grep -cE 'warning:|error:'
ls -la obj/libgit2/host/libgit2.a
bin/madc tests/testint.mad
```

Expected: `0`; the archive; the smoke test prints its expected output (the
binary is unchanged in behaviour — nothing references libgit2 yet, so the
linker pulls no object from it).

- [ ] **Step 7: Commit**

```bash
git add src/Makefile
git commit -m "build: libgit2 archive beside libmir on every link line (host + hosted variants), -I for <git2.h>, git2clean"
```

---

### Task 3: `madc::GitRepo` — the ONE read-only libgit2 owner + its unit test

**Files:**
- Create: `include/madcdis/git_repo.h`, `src/madcdis_git_repo.cpp`
- Create: `tests/unit/test_gitrepo.cpp`
- Modify: `src/Makefile:135` (`CORE_OFILES` gains `madcdis_git_repo.o`)

**Interfaces:**
- Produces (consumed by Tasks 4–5):

```cpp
namespace madc {
struct GitRef      { std::string sha; std::string branch; bool detached; };
struct GitCommit   { std::string sha; std::string author; std::string email;
                     int64_t when; std::string summary; };
struct GitBlameRow { int64_t line; int64_t count; std::string sha;
                     std::string author; int64_t when; std::string summary; };
class GitRepo {
public:
    GitRepo(); ~GitRepo();
    bool open(const std::string &path, error *err = nullptr);   // discovers upward
    bool is_open() const;
    const std::string &workdir() const;                        // "" for bare
    bool head(GitRef &out, error *err = nullptr) const;
    bool revparse(const std::string &spec, std::string &sha, error *err = nullptr) const;
    bool log(std::vector<GitCommit> &out, const std::string &path, size_t limit, error *err = nullptr) const;
    bool show(const std::string &sha, const std::string &path, std::string &text, error *err = nullptr) const;
    bool blame(std::vector<GitBlameRow> &out, const std::string &path, size_t line0, size_t count, error *err = nullptr) const;
    bool dirty(const std::string &path, bool &out, error *err = nullptr) const;
private:
    GitRepo(const GitRepo &); GitRepo &operator=(const GitRepo &);
    struct impl; impl *_;                                       // pimpl: <git2.h> stays in the .cpp
};
}
```

- [ ] **Step 1: Write the failing unit test** `tests/unit/test_gitrepo.cpp`. The
  fixture: `git_repository_init` in a fresh temp dir (`mkdtemp("/tmp/madc_gitrepo_XXXXXX")`
  — the `mkstemp` precedent of `test_libmadc_program.cpp:94`; on `_WIN32` use
  `_mktemp_s` + `mkdir`), write `a.txt` = `"one\n"`, add + commit (`git_signature_new`
  with a fixed time `1700000000`, `git_commit_create_v`), then `a.txt` =
  `"one\ntwo\n"` + `b.txt` = `"bee\n"`, commit again. Cases:

```cpp
TEST_CASE("GitRepo opens a repository and answers head/revparse/log/show/blame/dirty")
{
    Fixture fx;                       // builds the two-commit repo; removes it in ~Fixture
    madc::GitRepo repo;
    madc::error err;
    REQUIRE(repo.open(fx.dir, &err));
    madc::GitRef h;
    REQUIRE(repo.head(h, &err));
    CHECK(h.branch == "master");      // git_repository_init's default; libgit2 does not read init.defaultBranch from the user's config in a fresh temp HOME? — assert non-empty instead:
    CHECK(!h.branch.empty());
    CHECK(h.sha.size() == 40);
    std::string sha;
    REQUIRE(repo.revparse("HEAD~1", sha, &err));
    CHECK(sha == fx.first_sha);
    std::vector<madc::GitCommit> rows;
    REQUIRE(repo.log(rows, "b.txt", 10, &err));
    CHECK(rows.size() == 1);          // b.txt appeared in the second commit only
    CHECK(rows[0].sha == fx.second_sha);
    REQUIRE(repo.log(rows, "a.txt", 10, &err));
    CHECK(rows.size() == 2);
    REQUIRE(repo.log(rows, "", 10, &err));
    CHECK(rows.size() == 2);          // "" = no path filter
    std::string text;
    REQUIRE(repo.show(fx.first_sha, "a.txt", text, &err));
    CHECK(text == "one\n");
    REQUIRE(repo.show(fx.second_sha, "a.txt", text, &err));
    CHECK(text == "one\ntwo\n");
    std::vector<madc::GitBlameRow> b;
    REQUIRE(repo.blame(b, "a.txt", 1, 2, &err));
    REQUIRE(b.size() == 2);
    CHECK(b[0].sha == fx.first_sha);
    CHECK(b[0].line == 1);
    CHECK(b[1].sha == fx.second_sha);
    CHECK(b[1].line == 2);
    bool d = true;
    REQUIRE(repo.dirty("a.txt", d, &err));
    CHECK(d == false);
    fx.write("a.txt", "one\ntwo\nthree\n");
    REQUIRE(repo.dirty("a.txt", d, &err));
    CHECK(d == true);
}

TEST_CASE("GitRepo refuses a non-repository and a missing path with prose")
{
    madc::GitRepo repo;
    madc::error err;
    CHECK(!repo.open("/", &err));
    CHECK(!err.message().empty());
    Fixture fx;
    REQUIRE(repo.open(fx.dir));
    std::string text;
    CHECK(!repo.show(fx.first_sha, "nope.txt", text, &err));
    CHECK(!err.message().empty());
    std::string sha;
    CHECK(!repo.revparse("no-such-ref", sha, &err));
}
```

(`Fixture` records `first_sha`/`second_sha` from `git_oid_tostr`; `write()` is
`std::ofstream`. Every libgit2 write call sits in the TEST, not in `GitRepo`.
Check `madc::error`'s accessor name in `include/libmadc/error.h` before
writing — it is `message()` per the constructor `error(severity, phase, msg)`.)

- [ ] **Step 2: Run it to see it fail to compile**

```bash
make -C src ../bin/test_gitrepo 2>&1 | tail -3
```

Expected: `git_repo.h: No such file` (or the target is unknown until the header
exists — either way not green).

- [ ] **Step 3: Write `include/madcdis/git_repo.h`** — the interface above with
  this header comment:

```cpp
// madcdis/git_repo.h — the ONE owner of every libgit2 call in madc (design
// docs/plans/2026-09-13-nexus-L4-design.md §4.2; gate
// scripts/check-one-git-owner.sh). READ-ONLY by construction: no write API is
// wrapped (a commit, a checkout, a fetch stay git's). Records are plain
// structs; the source-adapter face below turns them into value rows.
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): a GitRepo is
// CONFINED to the thread that opened it — libgit2's own objects are not
// shareable, and GIT_THREADS is on only for libgit2's internal correctness.
// git_libgit2_init/shutdown run once per process behind a static guard
// (refcounted by libgit2; the guard makes the first open pay for it).
```

plus the `git_source_adapter`:

```cpp
// The madcdis face: `git://<repo path>` (a `git` scheme row in DataSource —
// storage / file / path_like / local) with record families "commit" (the
// path-filtered log: {sha, author, email, when, summary}), "ref" ({name, sha}
// for every branch/tag) and "blame" ({line, count, sha, author, when, summary}
// over the whole file). The DataSource's path is the repository; the record
// family's file, when it needs one, rides `?path=<file>` in the URI's query
// (parsed here — DataSource keeps only the location).
class git_source_adapter : public SourceAdapter {
public:
    const char *name() const { return "git"; }
    bool can_read(const DataSource &source) const;      // scheme == "git"
    bool discover_types(const DataSource &, std::vector<ExtractedRecordType> &out, error *err = nullptr) const;
    bool extract(const DataSource &source, const std::string &type_name, std::vector<ExtractedRecord> &out, error *err = nullptr) const;
};
```

- [ ] **Step 4: Write `src/madcdis_git_repo.cpp`** — the probe of Task 0 is the
  reference implementation, wrapped:

```cpp
#include "madcdis/git_repo.h"
#include <git2.h>
#include <cstring>

namespace madc {

namespace {
// Once per process (libgit2 refcounts init; a matching shutdown at exit).
struct git_runtime {
    git_runtime()  { git_libgit2_init(); }
    ~git_runtime() { git_libgit2_shutdown(); }
};
void ensure_runtime() { static git_runtime once; (void)once; }

void set_err(error *err, const char *what)
{
    if ( !err ) return;
    const git_error *e = git_error_last();
    *err = error(error::severity::error, error::phase::runtime,
                 std::string(what) + ": " + (e && e->message ? e->message : "unknown libgit2 error"));
}
std::string oid_str(const git_oid *id)
{
    char buf[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(buf, sizeof buf, id);
    return buf;
}
} // namespace

struct GitRepo::impl { git_repository *repo; std::string workdir; impl() : repo(0) {} };

GitRepo::GitRepo() : _(new impl) {}
GitRepo::~GitRepo() { if ( _->repo ) git_repository_free(_->repo); delete _; }
bool GitRepo::is_open() const { return _->repo != 0; }
const std::string &GitRepo::workdir() const { return _->workdir; }

bool GitRepo::open(const std::string &path, error *err)
{
    ensure_runtime();
    if ( _->repo ) { git_repository_free(_->repo); _->repo = 0; }
    if ( git_repository_open_ext(&_->repo, path.c_str(), 0, NULL) < 0 )
        return set_err(err, "git open"), false;
    const char *wd = git_repository_workdir(_->repo);
    _->workdir = wd ? wd : "";
    return true;
}

bool GitRepo::head(GitRef &out, error *err) const
{
    git_reference *ref = 0;
    if ( !_->repo || git_repository_head(&ref, _->repo) < 0 )
        return set_err(err, "git head"), false;
    const git_oid *id = git_reference_target(ref);
    out.sha = id ? oid_str(id) : "";
    out.detached = git_repository_head_detached(_->repo) == 1;
    out.branch = out.detached ? "" : git_reference_shorthand(ref);
    git_reference_free(ref);
    return true;
}

bool GitRepo::revparse(const std::string &spec, std::string &sha, error *err) const
{
    git_object *obj = 0;
    if ( !_->repo || git_revparse_single(&obj, _->repo, spec.c_str()) < 0 )
        return set_err(err, "git rev-parse"), false;
    sha = oid_str(git_object_id(obj));
    git_object_free(obj);
    return true;
}

// `git log [-- path]`: a time-ordered revwalk from HEAD; with a path, keep a
// commit iff the entry's oid at that path differs from EVERY parent's (exact,
// two tree lookups per commit — a root commit that has the path counts).
bool GitRepo::log(std::vector<GitCommit> &out, const std::string &path, size_t limit, error *err) const
{
    out.clear();
    git_revwalk *walk = 0;
    if ( !_->repo || git_revwalk_new(&walk, _->repo) < 0 || git_revwalk_push_head(walk) < 0 )
        return set_err(err, "git log"), false;
    git_revwalk_sorting(walk, GIT_SORT_TIME);
    git_oid oid;
    while ( out.size() < limit && git_revwalk_next(&oid, walk) == 0 )
    {
        git_commit *c = 0;
        if ( git_commit_lookup(&c, _->repo, &oid) < 0 ) break;
        bool keep = path.empty();
        if ( !keep )
        {
            git_tree *tree = 0; git_tree_entry *e = 0;
            const git_oid *mine = 0; git_oid mine_copy;
            if ( git_commit_tree(&tree, c) == 0 && git_tree_entry_bypath(&e, tree, path.c_str()) == 0 )
            { git_oid_cpy(&mine_copy, git_tree_entry_id(e)); mine = &mine_copy; git_tree_entry_free(e); }
            if ( tree ) git_tree_free(tree);
            if ( mine )
            {
                unsigned n = git_commit_parentcount(c);
                keep = (n == 0);
                for ( unsigned i = 0; i < n && !keep; ++i )
                {
                    git_commit *p = 0; git_tree *pt = 0; git_tree_entry *pe = 0;
                    bool same = false;
                    if ( git_commit_parent(&p, c, i) == 0 && git_commit_tree(&pt, p) == 0 )
                    {
                        if ( git_tree_entry_bypath(&pe, pt, path.c_str()) == 0 )
                        { same = git_oid_equal(git_tree_entry_id(pe), mine) != 0; git_tree_entry_free(pe); }
                    }
                    if ( pt ) git_tree_free(pt);
                    if ( p ) git_commit_free(p);
                    if ( !same ) keep = true;
                }
            }
        }
        if ( keep )
        {
            GitCommit row;
            row.sha = oid_str(&oid);
            const git_signature *a = git_commit_author(c);
            row.author = a && a->name ? a->name : "";
            row.email = a && a->email ? a->email : "";
            row.when = (int64_t)git_commit_time(c);
            const char *s = git_commit_summary(c);
            row.summary = s ? s : "";
            out.push_back(row);
        }
        git_commit_free(c);
    }
    git_revwalk_free(walk);
    return true;
}

bool GitRepo::show(const std::string &sha, const std::string &path, std::string &text, error *err) const
{
    git_object *obj = 0; git_tree *tree = 0; git_tree_entry *e = 0; git_blob *blob = 0;
    bool ok = false;
    if ( !_->repo || git_revparse_single(&obj, _->repo, sha.c_str()) < 0 ) { set_err(err, "git show"); return false; }
    if ( git_object_peel((git_object **)&tree, obj, GIT_OBJECT_TREE) == 0
      && git_tree_entry_bypath(&e, tree, path.c_str()) == 0
      && git_blob_lookup(&blob, _->repo, git_tree_entry_id(e)) == 0 )
    {
        text.assign((const char *)git_blob_rawcontent(blob), (size_t)git_blob_rawsize(blob));
        ok = true;
    }
    else
        set_err(err, "git show");
    if ( blob ) git_blob_free(blob);
    if ( e ) git_tree_entry_free(e);
    if ( tree ) git_tree_free(tree);
    git_object_free(obj);
    return ok;
}

bool GitRepo::blame(std::vector<GitBlameRow> &out, const std::string &path, size_t line0, size_t count, error *err) const
{
    out.clear();
    git_blame_options opts = GIT_BLAME_OPTIONS_INIT;
    opts.min_line = line0;
    opts.max_line = count ? line0 + count - 1 : 0;
    git_blame *bl = 0;
    if ( !_->repo || git_blame_file(&bl, _->repo, path.c_str(), &opts) < 0 )
        return set_err(err, "git blame"), false;
    uint32_t n = git_blame_get_hunk_count(bl);
    for ( uint32_t i = 0; i < n; ++i )
    {
        const git_blame_hunk *h = git_blame_get_hunk_byindex(bl, i);
        if ( !h ) continue;
        GitBlameRow row;
        row.line = (int64_t)h->final_start_line_number;
        row.count = (int64_t)h->lines_in_hunk;
        row.sha = oid_str(&h->final_commit_id);
        row.author = h->final_signature && h->final_signature->name ? h->final_signature->name : "";
        row.when = h->final_signature ? (int64_t)h->final_signature->when.time : 0;
        git_commit *c = 0;
        if ( git_commit_lookup(&c, _->repo, &h->final_commit_id) == 0 )
        { const char *s = git_commit_summary(c); row.summary = s ? s : ""; git_commit_free(c); }
        out.push_back(row);
    }
    git_blame_free(bl);
    return true;
}

bool GitRepo::dirty(const std::string &path, bool &out, error *err) const
{
    unsigned int flags = 0;
    if ( !_->repo || git_status_file(&flags, _->repo, path.c_str()) < 0 )
        return set_err(err, "git status"), false;
    out = flags != GIT_STATUS_CURRENT;
    return true;
}

// ---------------------------------------------------------- git_source_adapter
// (the adapter body: can_read = scheme "git"; discover_types pushes commit /
// ref / blame; extract opens a GitRepo on source.path(), splits a `?path=`
// query off the location, and maps the structs to value objects with
// SourceLocator::at_key_path(sha) for commits/refs and at_line_range(line,
// count) for blame rows — see Task 4.)

} // namespace madc
```

(`git_blame_hunk::final_signature->when.time` is a `git_time_t`; the hunk's
summary needs the commit lookup — cheap, and it is what `graph.history` shows.)

- [ ] **Step 5: `CORE_OFILES`** (`src/Makefile:135`): add `madcdis_git_repo.o`
  after `madcdis_source_adapter.o`.

- [ ] **Step 6: Build + run the unit test**

```bash
make -C src 2>&1 | grep -cE 'warning:|error:'
( ulimit -t 120; timeout 180 bin/test_gitrepo )
```

Expected: `0`; doctest `[doctest] Status: SUCCESS!` with 2 test cases. If the
default-branch check fails because the container's global git config sets
`init.defaultBranch`, keep the non-empty assertion only (already written that way).

- [ ] **Step 7: Commit (rule trailers)**

```bash
git add include/madcdis/git_repo.h src/madcdis_git_repo.cpp tests/unit/test_gitrepo.cpp src/Makefile
git commit -F tmp/l4a_commit3.msg
```

Trailers: `Hypothesis: madc has no git reader; one read-only owner over the
vendored libgit2 gives the PAST verbs their substrate.` `Layer: PAST verb
(madcide seat) -> engine public -> GitRepo (madcdis) -> libgit2; GitRepo is the
deepest madc layer — below it is upstream code we do not edit.` `Searched: git
grep -n -i libgit2 / 'git_' / '"git ' in src include tools — nothing; the
SourceAdapter precedent is world_text_adapter (include/madcdis/world_text.h:568);
the handle discipline is include/handle_table.h.` `Oracle: n/a — new capability;
the fixture repository built by libgit2's own write API is the oracle (a blame
of a two-commit file names each commit on its line).`

---

### Task 4: The `git` scheme + the source adapter body + its test cases

**Files:**
- Modify: `include/libmadc/datasource.h:113-152` (one row), `src/madcdis_git_repo.cpp`
  (the adapter body), `tests/unit/test_gitrepo.cpp` (two cases)

- [ ] **Step 1: Add the failing test cases**

```cpp
TEST_CASE("DataSource classifies git:// as local path-like storage")
{
    madc::DataSource s("git:///tmp/somewhere/repo");
    CHECK(s.scheme() == "git");
    CHECK(s.path() == "/tmp/somewhere/repo");
    CHECK(s.is_local());
    CHECK(s.is_file_like());
}

TEST_CASE("git_source_adapter extracts commit, ref and blame records")
{
    Fixture fx;
    madc::git_source_adapter ad;
    madc::error err;
    madc::DataSource src("git://" + fx.dir);
    CHECK(ad.can_read(src));
    std::vector<madc::ExtractedRecordType> types;
    REQUIRE(ad.discover_types(src, types, &err));
    REQUIRE(types.size() == 3);
    CHECK(types[0].name() == "commit");
    std::vector<madc::ExtractedRecord> rows;
    REQUIRE(ad.extract(src, "commit", rows, &err));
    CHECK(rows.size() == 2);
    CHECK(rows[0].record.as_object().at("sha").as_string() == fx.second_sha);
    CHECK(rows[0].locator.locator_kind == madc::SourceLocator::kind::key_path);
    REQUIRE(ad.extract(madc::DataSource("git://" + fx.dir + "?path=a.txt"), "blame", rows, &err));
    CHECK(rows.size() == 2);
    CHECK(rows[1].locator.locator_kind == madc::SourceLocator::kind::line_range);
    CHECK(rows[1].locator.line_start == 2);
    REQUIRE(ad.extract(src, "ref", rows, &err));
    CHECK(rows.size() >= 1);
    CHECK(!ad.extract(src, "blame", rows, &err));   // blame without ?path= refuses with prose
}
```

- [ ] **Step 2: Run** `make -C src ../bin/test_gitrepo` → the cases fail
  (`git` is not a scheme; the adapter has no body).

- [ ] **Step 3: The scheme row** — `include/libmadc/datasource.h`, after the
  `qdbm` row (`:121`):

```cpp
	    // a git repository (working tree or .git) read through
	    // madc::GitRepo — local history only (design 2026-09-13 §4.2)
	    { "git", domain::storage, family::file, true, true },
```

- [ ] **Step 4: The adapter body** in `src/madcdis_git_repo.cpp`:

```cpp
namespace {
// "<repo>?path=<file>" — DataSource keeps the whole string as the path for a
// path_like scheme; the adapter owns the ONE split of its own query.
void split_query(const std::string &location, std::string &repo, std::string &file)
{
    std::size_t q = location.find('?');
    repo = q == std::string::npos ? location : location.substr(0, q);
    file.clear();
    if ( q != std::string::npos && location.compare(q + 1, 5, "path=") == 0 )
        file = location.substr(q + 6);
}
value commit_value(const GitCommit &c)
{
    std::map<std::string, value> f;
    f["sha"] = value(c.sha); f["author"] = value(c.author); f["email"] = value(c.email);
    f["when"] = value(c.when); f["summary"] = value(c.summary);
    return value::make_object(f);
}
value blame_value(const GitBlameRow &b)
{
    std::map<std::string, value> f;
    f["line"] = value(b.line); f["count"] = value(b.count); f["sha"] = value(b.sha);
    f["author"] = value(b.author); f["when"] = value(b.when); f["summary"] = value(b.summary);
    return value::make_object(f);
}
} // namespace

bool git_source_adapter::can_read(const DataSource &source) const
{ return source.scheme() == "git"; }

bool git_source_adapter::discover_types(const DataSource &, std::vector<ExtractedRecordType> &out, error *) const
{
    out.push_back(ExtractedRecordType("commit"));
    out.push_back(ExtractedRecordType("ref"));
    out.push_back(ExtractedRecordType("blame"));
    return true;
}

bool git_source_adapter::extract(const DataSource &source, const std::string &type_name,
                                 std::vector<ExtractedRecord> &out, error *err) const
{
    out.clear();
    std::string repo_path, file;
    split_query(source.path(), repo_path, file);
    GitRepo repo;
    if ( !repo.open(repo_path, err) )
        return false;
    if ( type_name == "commit" )
    {
        std::vector<GitCommit> rows;
        if ( !repo.log(rows, file, (size_t)-1, err) ) return false;
        for ( size_t i = 0; i < rows.size(); ++i )
        {
            ExtractedRecord r; r.type_name = "commit"; r.record = commit_value(rows[i]);
            r.locator = SourceLocator::at_key_path(rows[i].sha); out.push_back(r);
        }
        return true;
    }
    if ( type_name == "blame" )
    {
        if ( file.empty() )
        {
            if ( err ) *err = error(error::severity::error, error::phase::runtime,
                                    "git: the blame family needs ?path=<file> on the source");
            return false;
        }
        std::vector<GitBlameRow> rows;
        if ( !repo.blame(rows, file, 1, 0, err) ) return false;     // 0 = to the end
        for ( size_t i = 0; i < rows.size(); ++i )
        {
            ExtractedRecord r; r.type_name = "blame"; r.record = blame_value(rows[i]);
            r.locator = SourceLocator::at_line_range((size_t)rows[i].line, (size_t)rows[i].count); out.push_back(r);
        }
        return true;
    }
    if ( type_name == "ref" )
        return repo.refs(out, err);   // see below
    if ( err ) *err = error(error::severity::error, error::phase::runtime,
                            "git: unknown record family `" + type_name + "`");
    return false;
}
```

`ref` needs one more `GitRepo` method (add it to the header, read-only):
`bool refs(std::vector<ExtractedRecord> &out, error *err) const` is the wrong
layer — keep `GitRepo` struct-shaped: add `struct GitRefRow { std::string name; std::string sha; };`
and `bool refs(std::vector<GitRefRow> &out, error *err = nullptr) const` over
`git_reference_iterator_new` / `git_reference_next` (branches + tags, resolved
with `git_reference_resolve`), and let the adapter map rows as above with
`SourceLocator::at_key_path(name)`. (The line `return repo.refs(out, err)`
above is then the mapped loop, same shape as `commit`.)

- [ ] **Step 5: Build + run**

```bash
make -C src 2>&1 | grep -cE 'warning:|error:'
( ulimit -t 120; timeout 180 bin/test_gitrepo )
```

Expected: `0`; 4 test cases, SUCCESS.

- [ ] **Step 6: Commit (rule trailers)**

```bash
git add include/libmadc/datasource.h include/madcdis/git_repo.h src/madcdis_git_repo.cpp tests/unit/test_gitrepo.cpp
git commit -F tmp/l4a_commit4.msg
```

Trailers: `Hypothesis: the madcdis query layer reaches git history through the
SourceAdapter contract like every other source; a scheme row makes git:// a
DataSource.` `Layer: DataSet/Query -> SourceAdapter -> GitRepo -> libgit2; the
adapter is a mapping, GitRepo the owner.` `Searched: git grep -n '"git"'
include/libmadc/datasource.h — no row; 'SourceAdapter' implementations —
world_text_adapter only.` `Oracle: n/a — the fixture repository is the oracle
(2 commits, blame rows per line).`

---

### Task 5: The engine face — `madc::git_*` publics through the five layers + the dialect test

**Files:**
- Create: `src/madc_git.cpp`
- Modify: `src/parser.cpp:376-380` (forward decls) and `:1239-1250` (bridges,
  after `madc_graph_at`), `include/ns_common.h:255-256`, `src/ns_madc.cpp:296-299`,
  `include/madc/ns_madc:234-235`, `src/Makefile:135` (`madc_git.o`)
- Create: `tests/testgit.mad`, `tests/testgit.expect`, `tests/testgit.expect_quiet`

**Interfaces:**
- Produces (the dialect face, `include/madc/ns_madc`):

```cpp
    // The git substrate (Nexus L4a; design 2026-09-13 §4.2): a READ-ONLY view of
    // a local repository through madc::GitRepo. git_open discovers the
    // repository upward from `path` (0 = not a repository); git_close frees
    // it. Every other public answers an object, or {error: <libgit2 message>}
    // (the graph_* refusal shape). Thread contract: a handle is used only from
    // the thread that opened it (the runtime-eval confinement).
    //   git_head(out, h)                 -> {sha, branch, detached}
    //   git_revparse(out, h, spec)       -> {sha}
    //   git_log(out, h, path, limit)     -> {rows: [{sha, author, email, when, summary}]}
    //                                       ("" = no path filter; limit <= 0 = 100)
    //   git_show(out, h, rev, path)      -> {text}   (the blob's bytes at rev)
    //   git_blame(out, h, path, line, count) -> {rows: [{line, count, sha, author, when, summary}]}
    //                                       (1-based line; count 0 = to the end)
    //   git_dirty(out, h, path)          -> {dirty: bool}
    int64_t git_open(const char *path);
    bool    git_close(int64_t handle);
    value  &git_head(value &out, int64_t handle);
    value  &git_revparse(value &out, int64_t handle, const char *spec);
    value  &git_log(value &out, int64_t handle, const char *path, int64_t limit);
    value  &git_show(value &out, int64_t handle, const char *rev, const char *path);
    value  &git_blame(value &out, int64_t handle, const char *path, int64_t line, int64_t count);
    value  &git_dirty(value &out, int64_t handle, const char *path);
```

- [ ] **Step 1: Write the failing dialect test** `tests/testgit.mad` (value-first;
  reads THIS repository from the runner's cwd; shape only):

```c
// Nexus L4a: the git substrate's dialect face (madc::git_*) over the repository
// the test runs in — every lane runs the suite from a checkout root, so the
// repository is always there; the assertions are SHAPE (never a sha or a
// count that history would move). Compile-NEVER-execute the guest: nothing
// here runs a program (.expect_quiet).
int main()
{
    long h = madc::git_open(".");
    println("opened: {}", h > 0 ? 1 : 0);

    var hd;
    madc::git_head(hd, h);
    println("head-sha-len: {}", strlen(hd["sha"].c_str()));
    println("head-has-branch-or-detached: {}",
	    strlen(hd["branch"].c_str()) > 0 || hd["detached"].as_boolean() ? 1 : 0);

    var rp;
    madc::git_revparse(rp, h, "HEAD");
    var same = rp["sha"];
    println("revparse-head-matches: {}", same == hd["sha"] ? 1 : 0);

    var lg;
    madc::git_log(lg, h, "VERSION", 5);
    println("log-rows-positive: {}", lg["rows"].size() > 0 ? 1 : 0);
    var first = lg["rows"][0];
    println("log-row-shape: {}", strlen(first["sha"].c_str()) == 40
	    && strlen(first["summary"].c_str()) > 0 && first["when"].as_integer() > 0 ? 1 : 0);

    var sh;
    madc::git_show(sh, h, "HEAD", "VERSION");
    var disk;
    php::file_get_contents(disk, "VERSION");
    println("show-matches-file: {}", sh["text"] == disk ? 1 : 0);

    var bl;
    madc::git_blame(bl, h, "VERSION", 1, 1);
    println("blame-rows-positive: {}", bl["rows"].size() > 0 ? 1 : 0);
    var b0 = bl["rows"][0];
    println("blame-row-shape: {}", b0["line"].as_integer() == 1
	    && strlen(b0["sha"].c_str()) == 40 ? 1 : 0);

    var dt;
    madc::git_dirty(dt, h, "VERSION");
    println("dirty-is-bool: {}", dt["dirty"].is_boolean() ? 1 : 0);

    var bad;
    madc::git_show(bad, h, "HEAD", "no/such/file.txt");
    println("missing-path-error: {}", strlen(bad["error"].c_str()) > 0 ? 1 : 0);
    madc::git_revparse(bad, h, "no-such-ref-anywhere");
    println("bad-ref-error: {}", strlen(bad["error"].c_str()) > 0 ? 1 : 0);

    println("closed: {}", madc::git_close(h) ? 1 : 0);
    println("stale-after-close: {}", madc::git_close(h) ? 0 : 1);
    println("nonrepo: {}", madc::git_open("/") == 0 ? 1 : 0);
    return 0;
}
```

`tests/testgit.expect`:

```
opened: 1
head-sha-len: 40
head-has-branch-or-detached: 1
revparse-head-matches: 1
log-rows-positive: 1
log-row-shape: 1
show-matches-file: 1
blame-rows-positive: 1
blame-row-shape: 1
dirty-is-bool: 1
missing-path-error: 1
bad-ref-error: 1
closed: 1
stale-after-close: 1
nonrepo: 1
```

`tests/testgit.expect_quiet`: one line, `stderr must be empty`.

(Check `value::is_boolean` / `as_boolean` exist in `include/libmadc/value.h`
before relying on them — `as_boolean` is used by `testmadcide_serve_tiers.mad`;
`var == var` compares text, banked L3 fact; `.size()` on an array var is the
L1 tests' idiom.)

- [ ] **Step 2: Run to see it fail**

```bash
bash tmp/l3_check.sh testgit 2>&1 | tail -3
```

(or `( ulimit -t 120; timeout 180 bin/madc tests/testgit.mad )`). Expected:
`madc::git_open` unresolved.

- [ ] **Step 3: `src/madc_git.cpp`** — the engine layer (handle table + value shaping):

```cpp
// madc_git.cpp — the engine face of the git substrate (Nexus L4a, design
// docs/plans/2026-09-13-nexus-L4-design.md §4.2): a handle_table of open
// madc::GitRepo objects and the value-shaped answers the dialect publics
// (include/madc/ns_madc git_*) return, bridged through src/parser.cpp exactly
// like the parse_*/graph_* publics. No libgit2 here — GitRepo is the owner.
// Thread contract: confinement to the opening thread (the parse-handle rule).
#include "madcdis/git_repo.h"
#include "handle_table.h"
#include "libmadc/value.h"
#include <map>
#include <string>
#include <vector>

namespace madc {
namespace {

struct git_repo_state { GitRepo repo; };

handle_table<git_repo_state> &git_handles()
{
    static handle_table<git_repo_state> handles;
    return handles;
}

value error_value(const error &e)
{
    std::map<std::string, value> f;
    f["error"] = value(e.message());
    return value::make_object(f);
}
value no_handle_value()
{
    std::map<std::string, value> f;
    f["error"] = value(std::string("git: no such repository handle"));
    return value::make_object(f);
}
value commit_row(const GitCommit &c)   // same shape as the adapter's commit record
{
    std::map<std::string, value> f;
    f["sha"] = value(c.sha); f["author"] = value(c.author); f["email"] = value(c.email);
    f["when"] = value(c.when); f["summary"] = value(c.summary);
    return value::make_object(f);
}
value blame_row(const GitBlameRow &b)
{
    std::map<std::string, value> f;
    f["line"] = value(b.line); f["count"] = value(b.count); f["sha"] = value(b.sha);
    f["author"] = value(b.author); f["when"] = value(b.when); f["summary"] = value(b.summary);
    return value::make_object(f);
}

} // namespace

int64_t internal_program_git_open(const std::string &path)
{
    git_repo_state *st = new git_repo_state();
    if ( !st->repo.open(path) ) { delete st; return 0; }
    return git_handles().open(st);
}
bool internal_program_git_close(int64_t handle) { return git_handles().close(handle); }

bool internal_program_git_head(int64_t handle, value &out)
{
    git_repo_state *st = git_handles().get(handle);
    if ( !st ) { out = no_handle_value(); return false; }
    GitRef r; error err;
    if ( !st->repo.head(r, &err) ) { out = error_value(err); return false; }
    std::map<std::string, value> f;
    f["sha"] = value(r.sha); f["branch"] = value(r.branch); f["detached"] = value(r.detached);
    out = value::make_object(f);
    return true;
}
// … revparse / log / show / blame / dirty follow the same three lines:
//    get the state (no_handle_value), call GitRepo (error_value), shape the
//    answer ({sha} / {rows:[…]} / {text} / {rows:[…]} / {dirty}).
} // namespace madc
```

(Row shaping is duplicated between the adapter and this file ONLY if written
twice — do NOT: move `commit_value`/`blame_value` into `git_repo.h` as
`value git_commit_value(const GitCommit &)` / `value git_blame_value(const GitBlameRow &)`
declared beside the structs and defined once in `madcdis_git_repo.cpp`; both
consumers call them. One implementation.)

- [ ] **Step 4: Bridges** — `src/parser.cpp` forward decls after `:380`
  (`internal_program_graph_at`):

```cpp
int64_t internal_program_git_open(const std::string &path);
bool internal_program_git_close(int64_t handle);
bool internal_program_git_head(int64_t handle, value &out);
bool internal_program_git_revparse(int64_t handle, const std::string &spec, value &out);
bool internal_program_git_log(int64_t handle, const std::string &path, int64_t limit, value &out);
bool internal_program_git_show(int64_t handle, const std::string &rev, const std::string &path, value &out);
bool internal_program_git_blame(int64_t handle, const std::string &path, int64_t line, int64_t count, value &out);
bool internal_program_git_dirty(int64_t handle, const std::string &path, value &out);
```

and the bridges after `madc_graph_at` (`:1250`), `std::string*` for text,
`madc::value*` for results, NO `require_runtime_eval_program` (a git handle
needs no Program):

```cpp
int64_t madc_git_open(void *path) { return madc::internal_program_git_open(*(const std::string *)path); }
bool madc_git_close(int64_t handle) { return madc::internal_program_git_close(handle); }
void *madc_git_head(void *result, int64_t handle)
{ madc::value &out = *(madc::value *)result; madc::internal_program_git_head(handle, out); return result; }
void *madc_git_revparse(void *result, int64_t handle, void *spec)
{ madc::value &out = *(madc::value *)result; madc::internal_program_git_revparse(handle, *(const std::string *)spec, out); return result; }
void *madc_git_log(void *result, int64_t handle, void *path, int64_t limit)
{ madc::value &out = *(madc::value *)result; madc::internal_program_git_log(handle, *(const std::string *)path, limit, out); return result; }
void *madc_git_show(void *result, int64_t handle, void *rev, void *path)
{ madc::value &out = *(madc::value *)result; madc::internal_program_git_show(handle, *(const std::string *)rev, *(const std::string *)path, out); return result; }
void *madc_git_blame(void *result, int64_t handle, void *path, int64_t line, int64_t count)
{ madc::value &out = *(madc::value *)result; madc::internal_program_git_blame(handle, *(const std::string *)path, line, count, out); return result; }
void *madc_git_dirty(void *result, int64_t handle, void *path)
{ madc::value &out = *(madc::value *)result; madc::internal_program_git_dirty(handle, *(const std::string *)path, out); return result; }
```

`include/ns_common.h` after `:256` (`madc_graph_at`): the eight C-shaped decls.
`src/ns_madc.cpp` after `:299`:

```cpp
int64_t git_open(const char *path)
	{ std::string p = path ? path : ""; return madc_git_open(&p); }
bool git_close(int64_t handle) { return madc_git_close(handle); }
value &git_head(value &out, int64_t handle)
	{ madc_git_head(&out, handle); return out; }
value &git_revparse(value &out, int64_t handle, const char *spec)
	{ std::string s = spec ? spec : ""; madc_git_revparse(&out, handle, &s); return out; }
value &git_log(value &out, int64_t handle, const char *path, int64_t limit)
	{ std::string p = path ? path : ""; madc_git_log(&out, handle, &p, limit); return out; }
value &git_show(value &out, int64_t handle, const char *rev, const char *path)
	{ std::string r = rev ? rev : "", p = path ? path : ""; madc_git_show(&out, handle, &r, &p); return out; }
value &git_blame(value &out, int64_t handle, const char *path, int64_t line, int64_t count)
	{ std::string p = path ? path : ""; madc_git_blame(&out, handle, &p, line, count); return out; }
value &git_dirty(value &out, int64_t handle, const char *path)
	{ std::string p = path ? path : ""; madc_git_dirty(&out, handle, &p); return out; }
```

`include/madc/ns_madc` after `:235` (`graph_at`): the declaration block from
**Interfaces** above (with its comment). `src/Makefile:135`: `madc_git.o` after
`madcdis_git_repo.o`.

- [ ] **Step 5: Build + run the test**

```bash
make -C src 2>&1 | grep -cE 'warning:|error:'
bash tmp/l3_check.sh testgit
```

Expected: `0`; every `.expect` line found; stderr empty. Also re-run the
neighbours that share the bridge file: `testparsehandle`, `testgraphedit`,
`testmadcide_serve_graph` (unchanged output).

- [ ] **Step 6: Commit (rule trailers)**

```bash
git add src/madc_git.cpp src/parser.cpp include/ns_common.h src/ns_madc.cpp include/madc/ns_madc src/Makefile tests/testgit.mad tests/testgit.expect tests/testgit.expect_quiet
git commit -F tmp/l4a_commit5.msg
```

Trailers: `Hypothesis: the dialect (the madcide seat) needs git facts as
values; the parse_*/graph_* five-layer plumbing is the proven path.` `Layer:
madcide seat -> madc::git_* (ns_madc) -> C bridge (parser.cpp) -> engine face
(madc_git.cpp: handles + value shaping) -> GitRepo (madcdis) -> libgit2; every
layer does one thing, none below GitRepo is madc's.` `Searched: git grep -n
'handle_table<' src — parse_tu_handles / parse_project_handles: the ONE handle
discipline reused; 'git_open' anywhere — nothing.` `Oracle: n/a — the
repository the test runs in is the oracle (show(HEAD, VERSION) == the file on
disk; blame line 1 names a 40-char sha).`

---

### Task 6: The one-git-owner gate, the design-doc corrections, the hand-off

**Files:**
- Create: `scripts/check-one-git-owner.sh`
- Modify: `src/Makefile` (fulltest line), `docs/plans/2026-09-13-nexus-L4-design.md`
  §4.1/§4.2, `claude_status.json` (live_handoff UPDATE 12), the L4 ledger

- [ ] **Step 1: `scripts/check-one-git-owner.sh`**

```bash
#!/bin/bash
# GATE — ONE git owner (Nexus L4a, design 2026-09-13 §4.2).
#
# The rule: every libgit2 call (git_* from <git2.h>) lives in
# src/madcdis_git_repo.cpp, the READ-ONLY madc::GitRepo; nothing else in
# src/, include/ or tools/ includes <git2.h>, calls a git_* API, or spawns a
# git binary (exec://git). Consumers use GitRepo (C++) or madc::git_* (the
# dialect) — so "network off" and "read-only" are properties of ONE file.
# tests/ are exempt (unit fixtures build repositories through libgit2's own
# write API; that is the oracle, not a second owner).
set -u
cd "$(dirname "$0")/.."

fail=0
hits=$(grep -rnE '#include[[:space:]]*[<"]git2(/|\.h|>)|\bgit_(repository|revwalk|commit|blame|tree|blob|reference|status|revparse|object|remote|clone|libgit2)_[a-z_]+[[:space:]]*\(' \
	src include tools --include='*.cpp' --include='*.h' --include='*.inc' --include='*.mad' 2>/dev/null \
	| grep -v '^src/madcdis_git_repo\.cpp:')
if [ -n "$hits" ]; then
	echo "one-git-owner gate: libgit2 used outside src/madcdis_git_repo.cpp:"
	echo "$hits" | sed 's/^/  /'
	fail=1
fi
spawn=$(grep -rnE 'exec://git\b|"git[[:space:]]+(log|blame|show|rev-parse|status)' \
	src include tools --include='*.cpp' --include='*.h' --include='*.inc' --include='*.mad' 2>/dev/null)
if [ -n "$spawn" ]; then
	echo "one-git-owner gate: a git BINARY is spawned (use madc::GitRepo / madc::git_*):"
	echo "$spawn" | sed 's/^/  /'
	fail=1
fi

# Negative control: a synthetic violation of each marker must be caught.
ctrl=$(mktemp)
printf '#include <git2.h>\nvoid f(void){ git_repository_open(0, "x"); }\nmadc::channel c("exec://git log");\n' > "$ctrl"
if ! grep -qE '#include[[:space:]]*[<"]git2(/|\.h|>)' "$ctrl" \
   || ! grep -qE '\bgit_(repository|revwalk|commit|blame|tree|blob|reference|status|revparse|object|remote|clone|libgit2)_[a-z_]+[[:space:]]*\(' "$ctrl" \
   || ! grep -qE 'exec://git\b' "$ctrl"; then
	echo "one-git-owner gate: NEGATIVE CONTROL FAILED"
	rm -f "$ctrl"
	exit 1
fi
rm -f "$ctrl"

[ "$fail" -ne 0 ] && exit 1
echo "one-git-owner gate: GREEN — src/madcdis_git_repo.cpp is the only libgit2 caller; no git binary is spawned."
exit 0
```

Wire it after the `check-libgit2-features.sh` line in `fulltest`; run both:

```bash
bash scripts/check-one-git-owner.sh
bash scripts/check-libgit2-features.sh
bash scripts/check-one-handle-table.sh
bash scripts/check-rule-trailers.sh
```

Expected: four GREENs.

- [ ] **Step 2: Design-doc corrections** (`docs/plans/2026-09-13-nexus-L4-design.md`):
  §4.1 — "builtin SHA1" → "the bundled collision-detecting SHA1 (libgit2 1.9 has
  no plain builtin)"; the `Makefile.madc` home → `third_party/libgit2-madc/`
  (beside, not inside); the measured numbers (archive 3.2 MB, 1.40 MB stripped
  growth for the read API, ~150 KB unreachable network objects, 0 warnings).
  §4.2 — the adapter lives in `src/madcdis_git_repo.cpp` with `GitRepo` (one
  TU for the substrate); the engine face is `src/madc_git.cpp`; `git_log`'s
  `limit <= 0` default.

- [ ] **Step 3: Hand-off** — `claude_status.json` live_handoff UPDATE 12 (L4a
  shipped: commits, measured size, gates; NEXT = writing-plans L4b), the ledger
  `tmp/sdd-ast-graph-mcp-L4/progress.md`, memory.

- [ ] **Step 4: Commit + push**

```bash
git add scripts/check-one-git-owner.sh src/Makefile docs/plans/2026-09-13-nexus-L4-design.md claude_status.json
git commit -m "gate(git): one-git-owner check; nexus L4 design §4.1/§4.2 corrected to the shipped substrate; live_handoff UPDATE 12"
git push origin feature/client-server-views-claude
```

---

## Self-review

- **Spec coverage (design §4.1, §4.2, §8 L4a):** vendoring + Makefile.madc +
  feature headers + network-off gate → Task 1; Makefile wiring on every variant
  → Task 2; `GitRepo` (head/revparse/log-by-path/show/blame/dirty, read-only,
  static init guard, error prose) → Task 3; `git` scheme + adapter (commit/ref/
  blame with locators) → Task 4; `madc::git_*` five-layer publics + `testgit`
  → Task 5; `check-one-git-owner.sh` + doc corrections → Task 6; the size spike
  → Task 0 (done). Thread contract stated in the header (Task 3) and the
  dialect comment (Task 5).
- **Placeholders:** none — every code step carries its code; the three rows the
  Task 3 excerpt elides ("revparse / log / show / blame / dirty follow the same
  three lines") name the exact shape per verb in the **Interfaces** comment.
- **Type consistency:** `GitRef/GitCommit/GitBlameRow/GitRefRow` (Task 3/4) are
  what `madc_git.cpp` (Task 5) and the adapter (Task 4) consume; the value
  shapers `git_commit_value`/`git_blame_value` are declared once (Task 5 note);
  the C bridges take `std::string *` / `madc::value *` exactly as the graph
  bridges do; `git_blame(line, count)` is 1-based with `count 0 = to the end`
  in both the adapter (`blame(rows, file, 1, 0)`) and the public.
- **Gates carry negative controls** (both scripts). **Rule trailers** on Tasks
  3, 4, 5 (the `src/`/`include/` commits). **Targeted tests only**; the battery
  rides the V6 seam.
