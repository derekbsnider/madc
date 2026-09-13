# libgit2 in madc

`../libgit2` is upstream libgit2 **v1.9.7** (`49e408b3208bc3093757a1c2db938d3590f3f412`),
imported as a SQUASHED `git subtree` on 2026-09-13 (design
`docs/plans/2026-09-13-nexus-L4-design.md` §4.1; plan
`docs/plans/2026-09-13-nexus-L4a-git-substrate-plan.md`). It carries ZERO madc
edits — a needed fix goes upstream first. This directory is madc's build of it:

- `Makefile.madc` — the sources and defines CMake would use, without CMake;
  `src/Makefile` runs it into `obj/libgit2/<variant>/libgit2.a` like `$(MIRLIB)`
  (`git2clean` removes the products; `clean` does not — the MIR model).
- `<platform>/git2_features.h`, `<platform>/pcre2/config.h` — the two headers
  CMake would generate, committed per platform (`linux`, `darwin`, `win32`).
  `linux/` is exactly what a CMake configure of this version generated on the
  build container; `darwin/` and `win32/` are derived from it by the platform
  probes in upstream's `src/CMakeLists.txt` (stat spelling, `qsort_r` flavour,
  `poll` vs `WSAPoll`, entropy sources) and are validated by their release lanes.

Configuration: threads on; HTTPS / SSH / NTLM / GSSAPI OFF — madc only READS
local history (push/fetch stay git's); `scripts/check-libgit2-features.sh`
fails the build if a network transport is ever enabled or a network dependency
compiled. Regex = bundled pcre2; SHA1 = bundled collision-detecting (libgit2
1.9 has no plain builtin SHA1); SHA256 = bundled; zlib = the system one madc
already links; `deps/xdiff` + `deps/llhttp` bundled; `deps/zlib`,
`deps/chromium-zlib`, `deps/ntlmclient`, `deps/winhttp` never built.

Measured 2026-09-13 (clang 18, -O2): archive 3.2 MB; a program linking exactly
the read API `GitRepo` uses is 1.40 MB stripped; zero warnings. About 150 KB of
that is plain-HTTP / smart-protocol code reached through `branch.c → remote.c →
transport.c`'s table — unreachable from madc (no fetch / clone / push is ever
called) and left in rather than patching upstream.

The ONE consumer is `madc::GitRepo` (`include/madcdis/git_repo.h`,
`src/madcdis_git_repo.cpp`, read-only; `scripts/check-one-git-owner.sh`).

Bumping: `git subtree pull --prefix=third_party/libgit2
https://github.com/libgit2/libgit2.git <tag> --squash`, then re-diff
`src/util/git2_features.h.in` and `deps/pcre2/config.h.in` against the
committed headers, re-check `src/libgit2/CMakeLists.txt`, `src/util/CMakeLists.txt`
and `deps/*/CMakeLists.txt` against `Makefile.madc`'s globs and defines, and
re-run the size spike (Task 0 of the L4a plan).
