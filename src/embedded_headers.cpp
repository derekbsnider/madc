// src/embedded_headers.cpp — GENERATED-FILE STUB. Do NOT edit, and do NOT
// commit generated content here.
//
// The real embedded-header table (include/madc/* baked into a
// std::map<std::string,std::string>, plus find_embedded_header) is generated
// at Makefile PARSE time into the per-mode object tree —
// obj/<mode>/embedded_headers.cpp — by scripts/gen_embedded_headers.sh, from
// include/madc/*. The build compiles THAT file; this one is never compiled by
// a correct build. Every mode (host and cross) now writes to the obj tree, so
// no generated prelude text is committed under src/.
//
// This stub is the tracked, discoverable path (docs/architecture.md and
// docs/language/embedded-headers.md point here) AND a loud guard: if a build
// ever reaches this translation unit, generation was bypassed (a
// misconfigured rule, a missing obj tree) and the binary would otherwise bake
// an EMPTY header set — silent header degradation. Fail the build instead.
//
// To regenerate and build normally: `make -C src`.
// Kept a committed stub (not gitignored) on purpose: see
// .claude/rules/embedded-headers.md and check-embedded-headers-stub.sh.
#error "src/embedded_headers.cpp is a generated-file stub; the real table is built into obj/<mode>/embedded_headers.cpp at Makefile parse time. Run `make -C src` (see docs/language/embedded-headers.md)."
