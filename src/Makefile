-include ../config.mk

# The default goal is ALWAYS `all`, stated explicitly. Several stamp /
# prerequisite-only rules are defined ABOVE the `all:` target (they must be,
# to attach to the pattern rules), and GNU make otherwise takes the first
# rule in the file as the default — so a bare `make -C src` silently "built"
# nothing but the forest-shape stamp (regression shipped in v0.48.0; explicit
# goals like `fulltest` and `release` masked it).
.DEFAULT_GOAL := all

CC ?= clang
CXX ?= clang++
# --- Build modes (B4a; docs/plans/2026-07-04-forest-default-mode-design.md §7)
# make            -> develop: -O0 -> bin/madc. Optimize madc's *algorithms*
#                    first, and keep the binary unoptimized so profiling/gdb
#                    attribution is honest (the long-standing dev contract).
# make debug      -> -O0 -ggdb, unstripped -> bin/madc-debug.
# make release    -> -O2 -> bin/madc-release (the "last-lap switch"), then
#                    strip BEFORE the forest pack is appended (strip rewrites
#                    the ELF image and discards trailing bytes; plain strip
#                    keeps .dynsym, which the dlsym import resolver needs),
#                    then pack + append + verify via scripts/forest_pack.sh.
#                    Packed suite: MADC_BIN=bin/madc-release scripts/run_tests.sh
# Modes NEVER share objects OR the output binary: each compiles into its own
# obj/<mode>/ tree and links its own bin/ name, so dev and packed-release
# binaries coexist (-MMD tracks headers, not flags — mixing -g/-O2/-O0
# objects silently skews; a shared bin path silently skips cross-mode
# relinks). lib/libmadc.{a,so} stay one path — the .libmode stamp below
# forces their relink on a mode switch.
MODE ?= develop
ifeq ($(MODE),release)
MODE_OPTFLAGS = -O2
else ifeq ($(MODE),debug)
MODE_OPTFLAGS = -O0 -ggdb
else
MODE_OPTFLAGS = -O0
endif
# Modes never share the OUTPUT BINARY either: each mode links its own name,
# so `make release` (packed -O2) and `make` (develop -O0) coexist and can
# never clobber each other. (Before this, all modes linked ../bin/madc and
# make's timestamp logic silently SKIPPED the develop relink after a release
# build — bin/madc stayed the packed release binary while claiming dev.)
ifeq ($(MODE),release)
BIN = ../bin/madc-release
else ifeq ($(MODE),debug)
BIN = ../bin/madc-debug
else ifeq ($(MODE),cross-aarch64-linux)
# Cross compiler (gcc cross model, Mach-O/ARM64 track axis A): a HOST binary
# that emits native artifacts for the target and refuses every run lane
# (MADC_CROSS_TARGET below). Same -O0 dev contract as develop.
BIN = ../bin/madc-aarch64-linux
else ifeq ($(MODE),cross-x86-64-macos)
# Mach-O twins (axis B): same emit-only cross model, Mach-O container.
BIN = ../bin/madc-x86-64-macos
else ifeq ($(MODE),cross-arm64-macos)
BIN = ../bin/madc-arm64-macos
else ifeq ($(MODE),hosted-arm64-macos)
# madc-on-macOS (Route 1): FULL madc binaries cross-COMPILED for Apple
# hosts (clang-18 + SDK + ld64.lld). Host == target: JIT + native Mach-O
# AOT both live; no MADC_CROSS_TARGET. Ships unstripped -O2 (stripping
# invalidates the linker signature; re-signer is Phase 3 of the plan).
BIN = ../bin/madc-hosted-arm64-macos
else ifeq ($(MODE),hosted-x86-64-macos)
BIN = ../bin/madc-hosted-x86-64-macos
else ifeq ($(MODE),hosted-x86-64-windows)
# madc-on-Windows (Track 6.4): full madc cross-built with mingw-w64 over
# UCRT. Host == target (JIT + native AOT); Windows names executables .exe.
BIN = ../bin/madc-hosted-x86-64-windows.exe
else
BIN = ../bin/madc
endif
# The zero-warnings law (owner directive 2026-08-14): a warning is a defect
# report from a second compiler, so the build refuses one instead of printing
# it. Every lane is at ZERO as of this commit — host -O0, release -O2,
# hosted-x86-64-windows, both hosted/cross macOS pairs, and the unit-test
# compiles — so holding the line costs nothing now; letting warnings
# accumulate again is what costs.
#
# Scope is madc's OWN translation units. MIR builds through its own Makefile
# with its own flags and is unaffected.
#
# Escape hatch for a surprise toolchain (a newer compiler inventing a warning
# mid-release, say): `make WERROR=0`. Use it to get UNBLOCKED, then fix the
# warning — the flag is never the fix, and it is not a way to land code that
# warns. Note this rides CXXFLAGS, so an explicit `make CXXFLAGS=...` also
# drops it; WERROR=0 is the supported spelling.
WERROR ?= 1
WERROR_FLAG = $(if $(filter 0,$(WERROR)),,-Werror)
CXXFLAGS ?= -std=c++11 -Wall $(WERROR_FLAG) $(MODE_OPTFLAGS)
PIC_CXXFLAGS ?= $(CXXFLAGS) -fPIC
AR ?= ar
# ARFLAGS needs `ifeq ($(origin ...),default)`, NOT `?=`. make DEFINES ARFLAGS
# itself (built-in value `rv`), so its origin is `default` — never `undefined`
# — and a `?=` here silently no-ops: every archive was built `rv`, which has
# no `c` (so ar warns "creating <archive>" on a fresh tree) and has `v` (so it
# narrates every member). This guard still lets an explicit command-line or
# environment ARFLAGS win, which is what the `?=` was reaching for.
ifeq ($(origin ARFLAGS),default)
ARFLAGS = rcs
endif
INSTALL ?= install
INSTALL_DATA ?= $(INSTALL) -m 644
INSTALL_PROGRAM ?= $(INSTALL) -m 755
INSTALL_DIR ?= $(INSTALL) -d
#CFLAGS = -O3 -std=c++14 -stdlib=libc++ -Wall -Wno-nested-anon-types -Wno-gnu-anonymous-struct -Wunused
HFILES	= madc.h datadef.h tokens.h datatokens.h
# src/rt/ — the DUAL-BUILD C-lane runtime (forest-carriers S5). Strict C11
# sources with no compiler builtins: the host build compiles them into libmadc
# (rule below, via $(CXX) -x c so every MODE's target/sysroot flags apply), and
# madc compiles the SAME sources through c2mir at pack time into AOT ledger
# modules, so -static-libmadc programs carry this machinery in their own image.
# One tagged list, every consumer: scripts/ledger_sources.txt is its single
# owner. The selector projects that list for the platform this C compiler is
# actually targeting. Cross-madc modes stay Linux here because their C lane
# is host-built; forest_pack_darwin.sh separately selects darwin sources for
# the target MIR ledger it cross-freezes.
RT_LEDGER_TARGET = linux
ifneq ($(filter hosted-arm64-macos hosted-x86-64-macos,$(MODE)),)
RT_LEDGER_TARGET = darwin
endif
ifeq ($(MODE),hosted-x86-64-windows)
RT_LEDGER_TARGET = win64
endif
RT_LEDGER_MANIFEST = ../scripts/ledger_sources.txt
RT_LEDGER_SELECTOR = ../scripts/select_ledger_sources.sh
RT_LEDGER_INPUTS = $(RT_LEDGER_MANIFEST) $(RT_LEDGER_SELECTOR)
RT_LEDGER_SRCS := $(patsubst src/%,%,$(shell bash $(RT_LEDGER_SELECTOR) $(RT_LEDGER_TARGET) $(RT_LEDGER_MANIFEST)))
RT_OFILES := $(notdir $(RT_LEDGER_SRCS:.c=.o))
# An empty list would link a madc whose try/catch and VLA runtime is simply
# ABSENT — it builds and then dies at run time. Fail the build instead.
ifeq ($(strip $(RT_OFILES)),)
$(error scripts/ledger_sources.txt lists no C-lane runtime sources)
endif
CORE_OFILES = lexer.o parser.o madc_value.o madc_error.o madc_program.o madc_api.o madc_c_api.o madc_config.o madc_config_file.o madc_globals.o madc_dl.o madc_crash.o ns_common.o ns_php.o ns_perl.o ns_python.o ns_ruby.o ns_js.o ns_rust.o ns_madc.o va_helpers.o $(RT_OFILES) pch.o madc_posix_io.o madc_datachannel.o madc_socket_channel.o madc_channel_stream.o madc_process.o madc_channel_object.o madcdis_driver.o madcdis_query.o madcdis_source_adapter.o madcdis_snapshot.o madcdis_storage.o embedded_headers.o sys_include_paths.o predefined_macros.o precompiled_headers.o madc_mir_backend.o madc_mangle.o madc_cir.o cir_builder.o cir_freeze.o cir_emit_c.o madc_project.o madc.o
MADDAT_OFILES = $(MADC_STORAGE_DB_OFILES)
MADHDRS = $(wildcard ../include/madc/*)
# embedded_headers.cpp regeneration (EMBED_GEN) happens below, AFTER the
# MODE blocks — hosted-darwin modes embed a generated SDK prelude alongside
# include/madc/ and write a per-mode file instead of src/embedded_headers.cpp.
LIBMADDHDRS = $(wildcard ../include/libmadc/*.h)
MADDATHDRS = $(wildcard ../include/madcdat/*.h)
# madc::dis substrate headers: tokens.h/madc.h include these, so they MUST be in
# DEPENDS or editing one silently leaves stale objects.
MADCDISHDRS = $(wildcard ../include/madcdis/*.h)
INCDIR	= ../include
BINDIR	= ../bin
LIBDIR	= ../lib
OBJDIR	= ../obj/$(MODE)
PICOBJDIR = $(OBJDIR)/pic
FEATURE_DEFINES ?=
OPTIONAL_CPPFLAGS ?=
OPTIONAL_LIBS ?=
MADC_STORAGE_DB_OFILES ?= madcdat_storage.o
HAVE_MADCDAT ?= 1
# MIR lives IN this repository (subtree import of the former fork —
# docs/plans/mir-into-madc-repo-2026-08-11.md); third_party/mir is ordinary
# madc source. libmir build products land OUTSIDE the subtree, under
# ../obj/mir/<variant>, so a build never dirties third_party/mir.
# Deliberately outside `clean`'s scope (madc objects only) — `mirclean`
# removes them.
MIRDIR	= $(abspath ../third_party/mir)
MIRBUILD = $(abspath ../obj/mir)
# MADC_VERSION (computed below from ../VERSION) is threaded into the binary as a
# string literal so the built-in MADC_VERSION macro tracks the release version —
# single source of truth, no hardcoded copy in the lexer.
DEFINES	= -I$(INCDIR) -I$(MIRDIR) -I$(MIRDIR)/c2mir $(FEATURE_DEFINES) $(OPTIONAL_CPPFLAGS) -DMADC_VERSION_STR='"$(MADC_VERSION)"'
LIBS	= -rdynamic -ldl -lz -lm -lpthread $(OPTIONAL_LIBS)
# MADC_VERSION_STR rides in DEFINES, which make does not track as a
# dependency: bumping ../VERSION left the version-consuming objects stale, so
# the host-baked sys.version and the lexer's MADC_VERSION macro disagreed
# until a clean build (testsysobject "version_matches: 0" after the v0.65.0
# bump). Make those objects depend on ../VERSION; the list is derived from
# the sources so a new consumer joins automatically.
VERSION_DEP_SRCS := $(shell grep -l 'MADC_VERSION_STR' *.cpp 2>/dev/null)
$(VERSION_DEP_SRCS:%.cpp=$(OBJDIR)/%.o) $(VERSION_DEP_SRCS:%.cpp=$(PICOBJDIR)/%.o): ../VERSION
MIRLIB	= $(MIRBUILD)/host/libmir.a
ifeq ($(MODE),cross-aarch64-linux)
# One MIR variant lib per target (mir-target.h knobs; zero mir makefile
# changes — the variant builds through the stock CPPFLAGS hook, rule below).
# The same target define compiles madc's objects (per-mode obj tree), and
# MADC_CROSS_TARGET names the target for the driver's emit-only guard.
MIRLIB	= $(MIRBUILD)/aarch64-linux/libmir.a
DEFINES += -DMIR_TARGET_AARCH64_LINUX -DMADC_CROSS_TARGET='"aarch64-linux"'
endif
# Shared Apple-target facts (cross + hosted MODEs). DARWIN_PRELUDE_TARGET is
# the clang triple the embedded darwin C prelude is generated against — set
# for EVERY Apple-target mode so the cross madcs embed the SAME prelude text
# the hosted binaries carry (the darwin grove cross-FREEZER must parse
# identical text to what the hosted consumer would live-parse; forest-carriers
# plan S1). DARWIN_ARCH names the target arch as spelled in binary/lib names.
MACOS_SDK ?= /workspace/sdk/MacOSX.sdk
MACOS_MINOS ?= 12
# The tree the embedded C prelude is FLATTENED from — open-licensed by
# default (macos-release-lane W0.5): Apple's own APSL/BSD libc headers as
# curated and shipped by Zig, staged once per container by
# scripts/fetch_darwin_open_headers.sh (the pin owner). The SDK stays
# build-side for compiling/linking madc itself (.tbd stubs — "developing
# with the SDK", its licensed purpose); pointing THIS variable at the SDK
# is for the never-shipped oracle diff only — gen_darwin_prelude.sh stamps
# non-open input and verify_macho_release.sh refuses to release it.
DARWIN_PRELUDE_SYSROOT ?= /workspace/darwin-open-headers/sysroot-any-darwin-any
# Apple-target C++ header search, made EXPLICIT. `--sysroot`/`-isysroot` alone
# is not enough: when the build host has its own libc++ installed (we install
# libc++-18-dev for the Linux -stdlib=libc++ lane), clang puts its OWN
# <driver>/../include/c++/v1 FIRST and the SDK's libc++ never gets looked at —
# silently compiling Mach-O-targeting code against the glibc-flavored libc++,
# and baking a build-host path into a binary that will run on a Mac. The
# standard cross spelling fixes it: drop the toolchain's C++ dirs, name the
# SDK's. C is unaffected (-nostdinc++ is C++-only), so $(CC) does not carry it.
DARWIN_CXX_ISYS = -nostdinc++ -isystem $(MACOS_SDK)/usr/include/c++/v1
# The C++ world madc PRESENTS to user code on Apple targets (HOSTTAB_CXX
# below) is LLVM's own libc++ — not the SDK's copy. Two reasons: (1)
# provenance — LLVM libc++ is Apache-2.0-with-LLVM-exception, the openly
# licensed upstream the SDK copies, so frozen groves derived from it are
# redistributable (macos-release-lane plan W0); (2) parity — it is the
# EXACT header text the -stdlib=libc++ lane holds at zero failures, where
# the SDK's Apple fork is text madc has never parsed. It also sits outside
# the SDK prefix, so the CLT prefix map below leaves it alone: the cross
# FREEZER can open it on the build container while the hosted table stays
# byte-identical (LOADED == parsed needs freezer and consumer to agree).
# madc's OWN build keeps DARWIN_CXX_ISYS (the SDK headers + .tbd stubs it
# links against) — only the served include world differs.
LLVM_LIBCXX_INC ?= /usr/lib/llvm-18/include/c++/v1
ifeq ($(MODE),cross-x86-64-macos)
# MADC_CROSS_APPLE: Mach-O link environment (implicit libSystem, no ELF
# sonames/RUNPATH, runtime-free programs only — madc_cir.cpp).
MIRLIB	= $(MIRBUILD)/x86-64-macos/libmir.a
DEFINES += -DMIR_TARGET_X86_64_MACOS -DMADC_CROSS_TARGET='"x86-64-macos"' -DMADC_CROSS_APPLE
DARWIN_ARCH = x86-64
DARWIN_PRELUDE_TARGET = x86_64-apple-macos$(MACOS_MINOS)
MADC_LINKLIBS_STANDIN = 1
endif
ifeq ($(MODE),cross-arm64-macos)
MIRLIB	= $(MIRBUILD)/arm64-macos/libmir.a
DEFINES += -DMIR_TARGET_ARM64_MACOS -DMADC_CROSS_TARGET='"arm64-macos"' -DMADC_CROSS_APPLE
DARWIN_ARCH = arm64
DARWIN_PRELUDE_TARGET = arm64-apple-macos$(MACOS_MINOS)
MADC_LINKLIBS_STANDIN = 1
endif
# --- hosted-on-macOS MODEs (Route 1, docs/plans/2026-07-25-madc-on-macos-plan.md).
# The darwin toolchain: clang-18 against the owner's staged SDK, linking with
# ld64.lld. libmir comes from a darwin-HOST variant build (no MIR_TARGET knobs:
# host detection through the darwin compiler turns MIR_TARGET_APPLE_P on
# natively, making the Mach-O writer the native writer). madcdat is forced off:
# its storage backends' headers do not exist in the SDK.
ifeq ($(MODE),hosted-arm64-macos)
HOSTED_DARWIN_TARGET = arm64-apple-macos$(MACOS_MINOS)
DARWIN_ARCH = arm64
endif
ifeq ($(MODE),hosted-x86-64-macos)
HOSTED_DARWIN_TARGET = x86_64-apple-macos$(MACOS_MINOS)
DARWIN_ARCH = x86-64
endif
ifdef HOSTED_DARWIN_TARGET
DARWIN_PRELUDE_TARGET = $(HOSTED_DARWIN_TARGET)
# -fuse-ld=lld is a LINK flag and rides $(LIBS) below, not the driver: on a
# compile-only invocation clang warns "argument unused during compilation"
# once per TU, which is every object in the mode.
CC  = clang-18 -target $(HOSTED_DARWIN_TARGET) --sysroot $(MACOS_SDK)
CXX_BASE = clang++-18 -target $(HOSTED_DARWIN_TARGET) --sysroot $(MACOS_SDK)
CXX = $(CXX_BASE) $(DARWIN_CXX_ISYS)
AR  = llvm-ar-18
MODE_OPTFLAGS = -O2
MIRLIB	= $(MIRBUILD)/$(MODE)/libmir.a
# The configure-probed host packages (madcdat backends) do not exist in the
# SDK: hosted modes build as if --enable-madcdat=no. zstd — the forest/pch
# container codec the release pack ships — comes from a per-target static
# build in the container (/workspace/zstd, forest-carriers plan S1): the
# hosted binary must READ the zstd containers the cross freezer packs.
HAVE_MADCDAT = 0
FEATURE_DEFINES =
DARWIN_ZSTD_DIR ?= /workspace/zstd
OPTIONAL_CPPFLAGS = -DHAVE_ZSTD -I$(DARWIN_ZSTD_DIR)/lib
OPTIONAL_LIBS = $(DARWIN_ZSTD_DIR)/libzstd-$(DARWIN_ARCH)-macos.a
MADC_STORAGE_DB_OFILES =
# darwin link facts: dl*/pthread live in libSystem (no -ldl), executables
# export their globals to dlsym without -rdynamic, and --whole-archive is
# spelled -force_load <archive> (two ld64 args: the bare archive path that
# follows WHOLE_ARCHIVE_BEGIN on the link line is its operand).
LIBS = -fuse-ld=lld -lz -lm $(OPTIONAL_LIBS)
WHOLE_ARCHIVE_BEGIN = -Wl,-force_load
WHOLE_ARCHIVE_END =
endif
# --- hosted-on-Windows MODE (Track 6.4, docs/plans/2026-08-12-windows-release-lane.md).
# The win64 toolchain: mingw-w64, -posix flavor (winpthreads), over UCRT.
# The UCRT recipe (W0, scripts/win_ucrt_gate.sh is the reference): -D_UCRT
# alone turns mingw's ANSI-stdio interposer OFF and %Lf on x87 80-bit long
# double silently prints 0.00, so __USE_MINGW_ANSI_STDIO=1 always rides with
# it; link-side the specs swap replaces gcc's hard-coded -lmsvcrt with -lucrt,
# and win_ucrt_compat.S supplies the _setjmp/_setjmpex thunks that
# msvcrt-compiled static libs (libstdc++, winpthreads) import.
# __USE_MINGW_SETJMP_NON_SEH is mandatory lane-wide (W2.1): SEH-default
# longjmp runs RtlUnwindEx frame-consistency unwinding, which faults crossing
# a JIT frame with no RUNTIME_FUNCTION entry. All three defines live in
# CC/CXX (not DEFINES) so the parse-time host-table capture (HOSTTAB_GEN)
# sees the same posture the binary is built with.
ifeq ($(MODE),hosted-x86-64-windows)
WIN_MINGW = x86_64-w64-mingw32
# _USE_MATH_DEFINES: mingw math.h hides M_PI (and friends) under
# __STRICT_ANSI__, which the captured -std=c++17 posture defines. On
# glibc targets g++ always predefines _GNU_SOURCE, so the Linux lane
# effectively always has M_PI — this is mingw's own documented knob for
# the same surface. Rides WIN_UCRT_FLAGS so the HOSTTAB capture seeds
# it into every script TU (and the host build, harmlessly).
WIN_UCRT_FLAGS = -D_UCRT -D__USE_MINGW_ANSI_STDIO=1 -D__USE_MINGW_SETJMP_NON_SEH -D_USE_MATH_DEFINES
WIN_SPECS = $(abspath $(OBJDIR))/ucrt.specs
# Parse-time generation (the EMBED_GEN/HOSTTAB precedent): the specs file
# must exist before ANY compile in this make layer, including the recursive
# MIR variant make, whose CC carries the -specs flag too.
WIN_SPECS_GEN := $(shell mkdir -p $(abspath $(OBJDIR)); $(WIN_MINGW)-gcc-posix -dumpspecs | sed 's/-lmsvcrt/-lucrt/g' > $(WIN_SPECS); echo done)
# UCRT-flavor libstdc++ (scripts/build_win_ucrt_libstdcxx.sh, staged per
# container like the zstd builds): the C++ ABI leaks the CRT flavor via
# std::mbstate_t (msvcrt: int; UCRT: _Mbstatet), so the distro's prebuilt
# msvcrt-flavor libstdc++ cannot satisfy -D_UCRT objects (fpos<mbstate_t>
# symbols — seekg/seekpos — mangle differently). Headers AND archive both
# come from the stage: bits/c++config.h is build-generated, so mixing the
# distro headers with the staged archive would be the same mismatch again.
# The -isystem pair rides CXX (the DARWIN_CXX_ISYS pattern) so HOSTTAB_CXX
# inherits it — madc serves user C++ the exact header text it was built
# against.
WIN_UCRT_LIBSTDCXX ?= /workspace/win-ucrt-libstdc++/stage
WIN_CXX_ISYS = -nostdinc++ -isystem $(WIN_UCRT_LIBSTDCXX)/include/c++/13.2.0 -isystem $(WIN_UCRT_LIBSTDCXX)/include/c++/13.2.0/x86_64-w64-mingw32
CC  = $(WIN_MINGW)-gcc-posix $(WIN_UCRT_FLAGS) -specs=$(WIN_SPECS)
CXX_BASE = $(WIN_MINGW)-g++-posix $(WIN_UCRT_FLAGS) -specs=$(WIN_SPECS)
CXX = $(CXX_BASE) $(WIN_CXX_ISYS)
AR  = $(WIN_MINGW)-ar
MODE_OPTFLAGS = -O2
MIRLIB	= $(MIRBUILD)/$(MODE)/libmir.a
# The configure-probed host packages do not exist for the cross target:
# hosted modes build as if --enable-madcdat=no (the darwin posture). zstd
# (the forest/pch container codec) is a W4 rider — zlib alone serves pch
# here until the groves work stages a mingw zstd.
HAVE_MADCDAT = 0
FEATURE_DEFINES =
OPTIONAL_CPPFLAGS =
OPTIONAL_LIBS =
MADC_STORAGE_DB_OFILES =
# win64 link facts: no ELF -rdynamic/-ldl (madcdl rides LoadLibrary out of
# kernel32); -Wl,--export-all-symbols is the -rdynamic analogue — madcdl's
# sym_default walk needs GetProcAddress(GetModuleHandle(NULL)) to see our
# statics. libstdc++ links SHARED (the staged libstdc++-6.dll, copied
# beside the exe by the rule below): PE ld auto-excludes runtime archives
# from --export-all-symbols, so a -static-frozen libstdc++ exposes no _Z*
# symbols and every script-side mangled-direct std:: import goes undefined
# at MIR link. The DLL's export table is the resolution surface instead
# (madcdl_sym_default walks the loaded module — the darwin flat-bind
# analogue), and exe + scripts share ONE libstdc++ instance, the Linux
# posture. winpthread is ALSO shared (the stage's UCRT-flavor
# libwinpthread-1.dll): the shared libstdc++ imports it by name, and a
# static-in-exe copy beside it would be a second winpthread instance —
# pthread objects cross the exe<->DLL boundary (std::thread), and
# winpthread handles are per-instance allocations. zlib stays frozen via
# the -Bstatic window; -static-libgcc keeps the distro's msvcrt-flavor
# libgcc DLL out of the process; the driver's trailing -lstdc++ resolves
# in -Bdynamic mode, where ld prefers the stage's libstdc++.dll.a over
# its .a. System DLLs stay imports either way (ws2_32/kernel32/ucrt are
# import libs).
LIBS = -static-libgcc -L$(WIN_UCRT_LIBSTDCXX)/lib -Wl,--export-all-symbols -lws2_32 -Wl,-Bstatic -lz -Wl,-Bdynamic -lpthread -lm
# The compat thunks join every link via the object list (compile rule beside
# the other per-mode rules below); riding CORE_OFILES also puts them in the
# per-mode libmadc archives, where any downstream consumer link needs them.
CORE_OFILES += win_ucrt_compat.o
# Per-mode include-table source (the darwin precedent): the mingw header
# world is captured from the cross compiler into the obj tree, never src/.
SYS_INCLUDES_CPP = $(OBJDIR)/sys_include_paths.cpp
export MADC_SYS_INCLUDES_OUT = $(SYS_INCLUDES_CPP)
endif
# The compiler whose `#include <...>` world and predefined-macro environment
# this madc presents to the code it COMPILES. Normally $(CXX) — the compiler
# that builds madc is the one whose headers it will read — with the Apple
# modes (cross AND hosted) as the deliberate exception: those binaries face
# Mach-O targets, so they must search Apple-target headers and predefine the
# TARGET's macros. Both tables, not just the include list: libc++'s __config
# branches on __APPLE__/__MACH__, so a madc predefining __linux__ cannot
# configure libc++ however good its header search is.
#
# ONE assignment for all four Apple modes — the cross madc IS the darwin
# grove freezer (forest-carriers S1), so its served include world must be
# byte-identical to the hosted consumer's or LOADED == parsed breaks.
# (The per-cross-block copies this replaces inherited the hosted export of
# MADC_SYS_INCLUDE_PREFIX_MAP through the recursive make, baking CLT paths
# into the cross madc — which then could not open a single C++ header on
# the build container.)
#
# The prefix map applies to BOTH sides for the same reason: the SDK's C
# paths rewrite to the on-Mac CLT location identically in freezer and
# consumer, and neither ever opens them on the container (the embedded
# prelude serves C). $(LLVM_LIBCXX_INC) sits outside the mapped prefix, so
# the freezer opens the C++ headers at their real container paths while
# the hosted binary carries the same spelling (inert on a run-only Mac —
# the packed groves serve C++ there).
ifdef DARWIN_PRELUDE_TARGET
HOSTTAB_CXX = clang++-18 -target $(DARWIN_PRELUDE_TARGET) --sysroot $(MACOS_SDK) -nostdinc++ -isystem $(LLVM_LIBCXX_INC)
MADC_SYS_INCLUDE_PREFIX_MAP = $(MACOS_SDK)=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
export MADC_SYS_INCLUDE_PREFIX_MAP
# Per-mode include-table source (embedded_headers.cpp precedent): the hosted
# and cross tables for one arch DIFFER (only the cross madc carries the
# probe stand-in sonames), and the hosted build recursively makes the cross
# freezer — a shared src/ file would flip between the two make layers, a
# write race under -j. Compile rule beside the embedded_headers.o one below.
SYS_INCLUDES_CPP = $(OBJDIR)/sys_include_paths.cpp
export MADC_SYS_INCLUDES_OUT = $(SYS_INCLUDES_CPP)
# The served predefined-macro table presents as GCC (the posture every host
# lane runs — host tables come from g++, and "madc impersonates gcc" is the
# configuration the libc++ parity campaign proved). The capture compiler here
# is necessarily clang; without the filter libc++ would take its clang-based
# branches and gate <atomic> on __has_feature probes madc refuses to fake.
# 13.3.0 = the host g++, so both postures agree on the version.
MADC_PREDEF_GCC_POSTURE = 13.3.0
export MADC_PREDEF_GCC_POSTURE
endif
HOSTTAB_CXX ?= $(CXX)
# The host-derived tables (system include paths, predefined macros) are
# regenerated at PARSE time like embedded_headers.cpp — placed HERE because
# hosted modes override $(CXX) above, and the tables must match the compiler
# whose binary they end up inside (a mode switch silently left the previous
# toolchain's tables in place before; the generators are idempotent, so an
# unchanged toolchain never churns mtimes).
HOSTTAB_GEN := $(shell mkdir -p $(OBJDIR); CXX="$(HOSTTAB_CXX)" MADC_SYS_INCLUDE_PREFIX_MAP="$(MADC_SYS_INCLUDE_PREFIX_MAP)" MADC_LINKLIBS_STANDIN="$(MADC_LINKLIBS_STANDIN)" MADC_SYS_INCLUDES_OUT="$(MADC_SYS_INCLUDES_OUT)" ../scripts/gen_sys_includes.sh >/dev/null 2>&1; CXX="$(HOSTTAB_CXX)" MADC_PREDEF_GCC_POSTURE="$(MADC_PREDEF_GCC_POSTURE)" ../scripts/gen_predefined_macros.sh >/dev/null 2>&1; echo done)
# Regenerate embedded_headers.cpp NOW, at Makefile parse time, before any target
# is evaluated — so an edited/added/DELETED embedded header is reflected in
# embedded_headers.o within a SINGLE make. The generators are idempotent (rewrite
# only on real content change), so this does not churn mtimes or force rebuilds.
# Apple-target modes (hosted AND cross): the flattened C prelude (stdio.h &
# co for the run-only Macs) is generated from $(DARWIN_PRELUDE_SYSROOT) into
# the per-mode obj tree and embedded ALONGSIDE include/madc/; the per-mode
# embedded_headers.cpp lives in the obj tree so generated prelude text never
# reaches the committed src/ file. stderr stays visible, and a failed
# pipeline REMOVES the .cpp — a stale table from an earlier successful gen
# must not be silently reused (silent header degradation is exactly the G2
# failure class this exists to prevent); with the .cpp gone the build stops
# loudly. The cross madcs embed it too: an Apple-target compile resolves
# Apple headers, and the darwin grove cross-freezer (forest-carriers S1) must
# parse the IDENTICAL embedded text the hosted consumer carries.
ifdef DARWIN_PRELUDE_TARGET
DARWIN_PRELUDE_DIR = $(OBJDIR)/darwin_prelude
EMBEDDED_HEADERS_CPP = $(OBJDIR)/embedded_headers.cpp
EMBED_GEN := $(shell mkdir -p $(OBJDIR) && MADC_PREDEF_GCC_POSTURE="$(MADC_PREDEF_GCC_POSTURE)" ../scripts/gen_darwin_prelude.sh $(DARWIN_PRELUDE_TARGET) $(DARWIN_PRELUDE_SYSROOT) $(DARWIN_PRELUDE_DIR) >/dev/null && ../scripts/gen_embedded_headers.sh $(DARWIN_PRELUDE_DIR) $(EMBEDDED_HEADERS_CPP) >/dev/null || rm -f $(EMBEDDED_HEADERS_CPP); echo done)
else
EMBED_GEN := $(shell ../scripts/gen_embedded_headers.sh >/dev/null 2>&1; echo done)
endif
# Forest carrier shape (--with-forest= configure axis; forest-carriers S3).
# Applies to the PRODUCT modes (release + hosted darwin): what the build
# produces (packed image / <bin>.forest sidecar / nothing) and what the
# binary expects (the baked MADC_FOREST_EXPECT_* define defaults the CLI to
# the loud-fallback notice when no forest is found at runtime). Dev, debug,
# and cross modes always build the `none` dev shape (silent live parse) —
# discovery itself is never restricted by this axis.
WITH_FOREST ?= embedded
ifneq ($(filter $(MODE),release hosted-arm64-macos hosted-x86-64-macos),)
ifeq ($(WITH_FOREST),embedded)
DEFINES += -DMADC_FOREST_EXPECT_EMBEDDED
else ifeq ($(WITH_FOREST),sidecar)
DEFINES += -DMADC_FOREST_EXPECT_SIDECAR
endif
endif
# The expectation is BAKED into madc.o (-D above), and -MMD tracks headers,
# not flags: a WITH_FOREST switch without a clean would silently keep the
# previous shape's madc.o. The stamp records the shape per obj tree
# (the .libmode precedent); switching shapes recreates it and recompiles
# madc.o. Prerequisite-only entry — the recipe stays the pattern rule's.
FOREST_SHAPE_STAMP = $(OBJDIR)/.forest-shape-$(WITH_FOREST)
$(FOREST_SHAPE_STAMP):
	@mkdir -p $(OBJDIR)
	@rm -f $(OBJDIR)/.forest-shape-*
	@touch $@
$(OBJDIR)/madc.o: $(FOREST_SHAPE_STAMP)
$(PICOBJDIR)/madc.o: $(FOREST_SHAPE_STAMP)
# Config file (--enable-config-file configure axis; forest-carriers S6). ON
# bakes madc's madc.ini LOOKUP into madc_config.o (the schema half); OFF removes
# it, so such a build never searches for nor opens a madc.ini and --config=
# refuses loudly. The schema-blind reader (madc_config_file.o) is ordinary
# in-tree code either way — it is shared with madcdat / madcdis-based consumers,
# and a parser nobody calls is not a call path. MADC_SYSCONFDIR names the system
# config directory searched last — the configured $(sysconfdir) when the tree
# was configured, else $(PREFIX)/etc, matching the install target's layout.
ENABLE_CONFIG_FILE ?= 1
ifeq ($(ENABLE_CONFIG_FILE),1)
DEFINES += -DMADC_ENABLE_CONFIG_FILE
endif
SYSCONFDIR ?= $(if $(sysconfdir),$(sysconfdir),$(PREFIX)/etc)
DEFINES += -DMADC_SYSCONFDIR='"$(SYSCONFDIR)"'
# Same baked-define hazard as the forest shape above (-MMD tracks headers, not
# flags): switching the axis without a clean would keep the previous shape's
# madc_config.o. Stamp it per obj tree the same way.
CONFIG_AXIS_STAMP = $(OBJDIR)/.config-file-$(ENABLE_CONFIG_FILE)
$(CONFIG_AXIS_STAMP):
	@mkdir -p $(OBJDIR)
	@rm -f $(OBJDIR)/.config-file-*
	@touch $@
$(OBJDIR)/madc_config.o: $(CONFIG_AXIS_STAMP)
$(PICOBJDIR)/madc_config.o: $(CONFIG_AXIS_STAMP)
# Link shape (--enable-shared configure axis; forest-carriers S4). 0 =
# monolithic: libmadc.a is linked into the CLI (the single-binary shape, the
# default). 1 = thin CLI: the CLI links the SHARED libmadc, so one library
# image serves the CLI and every embedding host — and in the embedded forest
# carrier the container rides that library image (discovery arm 2). Only the
# LINK differs, so no object needs rebuilding; the stamp forces the relink
# when the shape changes (make would otherwise see bin/madc newer than every
# prerequisite and skip it — the .libmode / .forest-shape precedent).
ENABLE_SHARED_CLI ?= 0
LINK_SHAPE_STAMP = $(OBJDIR)/.link-shape-$(ENABLE_SHARED_CLI)
$(LINK_SHAPE_STAMP):
	@mkdir -p $(OBJDIR)
	@rm -f $(OBJDIR)/.link-shape-*
	@touch $@
# Hosted binaries ship PACKED (forest-carriers S1): the darwin groves are
# cross-frozen by the same-arch cross madc (identical embedded prelude, same
# target facts -> same context hash + config word) and embedded at link time
# via -sectcreate — lld signs AFTER section layout, so the build path needs no
# re-signer (appended blobs are AMFI-illegal on signed Mach-O; the __MADC
# section is the darwin carrier). Variables here; the rules live below the
# default targets (a rule this early would hijack the default goal).
# --with-forest=sidecar keeps the cross-freeze but drops the -sectcreate:
# the link rule ships the container as $(BIN).forest instead (FOREST_SIDECAR);
# --with-forest=none skips the forest entirely.
ifdef HOSTED_DARWIN_TARGET
DARWIN_FOREST_BIN = $(OBJDIR)/forest.bin
CROSS_MADC_BIN = ../bin/madc-$(DARWIN_ARCH)-macos
ifeq ($(WITH_FOREST),embedded)
FOREST_EMBED_DEP = $(DARWIN_FOREST_BIN)
FOREST_EMBED_LDFLAGS = -Wl,-sectcreate,__MADC,__forest,$(DARWIN_FOREST_BIN)
else ifeq ($(WITH_FOREST),sidecar)
FOREST_EMBED_DEP = $(DARWIN_FOREST_BIN)
FOREST_EMBED_LDFLAGS =
FOREST_SIDECAR = 1
endif
endif
DEPENDS	= $(patsubst %,$(INCDIR)/%,$(HFILES)) $(LIBMADDHDRS) $(MADDATHDRS) $(MADCDISHDRS)

ifeq ($(HAVE_MADCDAT),1)
OFILES = $(CORE_OFILES) $(MADDAT_OFILES)
else
OFILES = $(CORE_OFILES)
endif

OBJECTS = $(patsubst %,$(OBJDIR)/%,$(OFILES))
PIC_OBJECTS = $(patsubst %,$(PICOBJDIR)/%,$(OFILES))
MADDAT_OBJECTS = $(patsubst %,$(OBJDIR)/%,$(MADDAT_OFILES))
MADDAT_ARCHIVE = $(LIBDIR)/libmadcdat.a
MADC_VERSION ?= $(shell cat ../VERSION 2>/dev/null || echo 0.0.0)
MADC_SOVERSION ?= $(firstword $(subst ., ,$(MADC_VERSION)))
LIBMADC_SHARED = $(LIBDIR)/libmadc.so
LIBMADC_STATIC = $(LIBDIR)/libmadc.a
# Thin-CLI gate vehicle (rule far below, with the other link rules).
THIN_CLI = $(BINDIR)/madc-thin
# The emitted-C freestanding runtime archive (macos-release-lane W3): ONLY
# the dual-build C-lane sources (scripts/ledger_sources.txt stays the one
# list owner — the same membership rule that keeps those strict C11 with no
# C++-runtime dependency is what keeps this archive self-contained). Emitted
# C11 that enters try/catch or frees a VLA references exactly this surface;
# the only other emitter-visible runtime symbols are <ns_madc>'s sys pair,
# which need the FULL libmadc. Completeness is gated: emitc_sret_gate links
# its specimen against this archive ALONE.
LIBMADC_RT = $(LIBDIR)/libmadc_rt.a
# Cross and hosted modes get their own archive/solib names: neither may
# clobber the host-facing ../lib artifacts with foreign objects.
ifneq (,$(filter cross-% hosted-%,$(MODE)))
LIBMADC_STATIC = $(LIBDIR)/libmadc-$(MODE).a
LIBMADC_SHARED = $(LIBDIR)/libmadc-$(MODE).so
LIBMADC_RT = $(LIBDIR)/libmadc_rt-$(MODE).a
endif
# Objects for the library: everything except the main() driver
LIBMADC_STATIC_OBJS = $(filter-out $(OBJDIR)/madc.o, $(OBJECTS))
pkgconfigdir ?= $(libdir)/pkgconfig

TESTDIR  = ../tests/unit
TESTSRCS = $(wildcard $(TESTDIR)/*.cpp)
MADDAT_TESTSRCS = $(TESTDIR)/test_libmadc_bdb.cpp $(TESTDIR)/test_libmadc_gdbm.cpp $(TESTDIR)/test_libmadc_qdbm.cpp $(TESTDIR)/test_libmadc_sqlite.cpp
ifeq ($(HAVE_MADCDAT),1)
ACTIVE_TESTSRCS = $(TESTSRCS)
else
ACTIVE_TESTSRCS = $(filter-out $(MADDAT_TESTSRCS), $(TESTSRCS))
endif
ALL_TESTBINS = $(patsubst $(TESTDIR)/%.cpp,$(BINDIR)/%,$(TESTSRCS))
TESTBINS = $(patsubst $(TESTDIR)/%.cpp,$(BINDIR)/%,$(ACTIVE_TESTSRCS))
PROCESS_FIXTURE = $(BINDIR)/test_process_fixture
# Link tests against all objects except madc.o (which has main())
TESTOBJ  = $(filter-out $(OBJDIR)/madc.o, $(OBJECTS))
LIBMADC_OBJECTS = $(filter-out $(PICOBJDIR)/madc.o, $(PIC_OBJECTS))
LIBMADC_STAGE_ROOT ?= /tmp/madc-libstage
LIBMADC_STAGE_CPP_SMOKE = $(LIBMADC_STAGE_ROOT)/libmadc_cpp_smoke
LIBMADC_STAGE_C_SMOKE = $(LIBMADC_STAGE_ROOT)/libmadc_c_smoke

# Build the unit-test binaries as part of the DEFAULT build so they can never be
# stale relative to the library/objects. Previously `all` built only the lib +
# bin/madc; the test binaries were relinked solely by the `test` target, so
# running `bin/test_*` directly after a bare `make` silently used a STALE binary
# (false pass/fail — this cost several phantom-failure debugging detours). The
# test-binary rule already depends on $(LIBMADC_STATIC), so make relinks each one
# only when the library actually changed. For a fast lib/madc-only iteration
# build when you are NOT about to run unit tests, pass BUILD_TESTS=0.
BUILD_TESTS ?= 1
ifeq ($(BUILD_TESTS),1)
DEFAULT_TARGETS = $(LIBMADC_STATIC) $(LIBMADC_RT) $(BIN) $(LIBMADC_SHARED) $(TESTBINS)
else
DEFAULT_TARGETS = $(LIBMADC_STATIC) $(LIBMADC_RT) $(BIN) $(LIBMADC_SHARED)
endif

all:
	$(MAKE) $(DEFAULT_TARGETS)

libmadc:
	$(MAKE) $(LIBMADC_STATIC) $(LIBMADC_SHARED)

libmadc-smoke:
	rm -rf $(LIBMADC_STAGE_ROOT)
	$(MAKE) install-libmadc DESTDIR=$(LIBMADC_STAGE_ROOT)
	$(CXX) -o $(LIBMADC_STAGE_CPP_SMOKE) $(CXXFLAGS) -I$(LIBMADC_STAGE_ROOT)$(includedir) ../tests/libmadc_cpp_smoke.cpp -L$(LIBMADC_STAGE_ROOT)$(libdir) -Wl,-rpath,$(LIBMADC_STAGE_ROOT)$(libdir) -lmadc
	$(CC) -o $(LIBMADC_STAGE_C_SMOKE) -I$(LIBMADC_STAGE_ROOT)$(includedir) ../tests/libmadc_c_smoke.c -L$(LIBMADC_STAGE_ROOT)$(libdir) -Wl,-rpath,$(LIBMADC_STAGE_ROOT)$(libdir) -lmadc
	$(LIBMADC_STAGE_CPP_SMOKE)
	$(LIBMADC_STAGE_C_SMOKE)

install-libmadc: libmadc
	$(INSTALL_DIR) $(DESTDIR)$(libdir)
	$(INSTALL_DIR) $(DESTDIR)$(includedir)
	$(INSTALL_DIR) $(DESTDIR)$(includedir)/libmadc
	$(INSTALL_DIR) $(DESTDIR)$(includedir)/madcdis
	$(INSTALL_PROGRAM) $(LIBMADC_SHARED) $(DESTDIR)$(libdir)/libmadc.so.$(MADC_VERSION)
	cd $(DESTDIR)$(libdir) && ln -sf libmadc.so.$(MADC_VERSION) libmadc.so.$(MADC_SOVERSION)
	cd $(DESTDIR)$(libdir) && ln -sf libmadc.so.$(MADC_SOVERSION) libmadc.so
	$(INSTALL_DATA) ../include/madc_api.h $(DESTDIR)$(includedir)/madc_api.h
	@for h in $(LIBMADDHDRS); do $(INSTALL_DATA) $$h $(DESTDIR)$(includedir)/libmadc/; done
	@for h in $(MADCDISHDRS); do $(INSTALL_DATA) $$h $(DESTDIR)$(includedir)/madcdis/; done
	# madcdis/snapshot.h names PchCompression in its PUBLIC signatures, so the
	# installed header did not compile downstream without this. madc_pch.h is
	# self-contained (standard includes only), so shipping it closes the hole.
	# The deeper issue is a layering INVERSION — the substrate (madcdis)
	# depends upward on the PCH layer for the compression vocabulary, which
	# should live in the substrate and be consumed by PCH. That move touches
	# ~15 files, so it is a tracked follow-up, not a build-recipe fix.
	$(INSTALL_DATA) ../include/madc_pch.h $(DESTDIR)$(includedir)/madc_pch.h
	@if [ -f ../libmadc.pc ]; then $(INSTALL_DIR) $(DESTDIR)$(pkgconfigdir) && $(INSTALL_DATA) ../libmadc.pc $(DESTDIR)$(pkgconfigdir)/libmadc.pc; fi

ifeq ($(HAVE_MADCDAT),1)
libmadcdat:
	$(MAKE) $(MADDAT_ARCHIVE)
install-madcdat: libmadcdat
	$(INSTALL_DIR) $(DESTDIR)$(libdir)
	$(INSTALL_DIR) $(DESTDIR)$(includedir)/madcdat
	$(INSTALL_DIR) $(DESTDIR)$(includedir)/libmadc
	$(INSTALL_DATA) $(MADDAT_ARCHIVE) $(DESTDIR)$(libdir)/libmadcdat.a
	@for h in $(MADDATHDRS); do $(INSTALL_DATA) $$h $(DESTDIR)$(includedir)/madcdat/; done
	@for h in $(LIBMADDHDRS); do $(INSTALL_DATA) $$h $(DESTDIR)$(includedir)/libmadc/; done
else
libmadcdat:
	@echo "madcdat is disabled; rerun ./configure --enable-madcdat to build libmadcdat."
	@false
install-madcdat: libmadcdat
endif

debug:
	$(MAKE) MODE=debug ../bin/madc-debug

# Recursive-make freshness lever (shared): a FORCE prerequisite makes the
# rule ALWAYS recurse; the inner make's own dependency tracking (the fork's
# -MMD .d files for the variant libs; per-MODE deps for the cross madc)
# decides whether anything rebuilds, and downstream relinks still key off
# the artifact's real mtime. Without it a bare artifact rule goes silently
# stale when fork sources change under an existing build dir.
FORCE:

# Emit-only cross compiler for aarch64-linux ELF (the qemu-gated axis A
# vehicle; the arm64-macos twin arrives with the axis B Mach-O writer).
cross-aarch64-linux:
	$(MAKE) MODE=cross-aarch64-linux ../bin/madc-aarch64-linux

# Host libmir + the c2m compiler the CIR-diff / ABI gates drive: built by
# madc's own build, so a fresh clone needs no separate MIR step (migration
# plan §11). FORCE delegates up-to-dateness to the MIR makefile's own
# dependency tracking; a current build is a cheap no-op.
$(MIRBUILD)/host/libmir.a: FORCE
	mkdir -p $(MIRBUILD)/host
	$(MAKE) -C $(MIRDIR) BUILD_DIR=$(MIRBUILD)/host $(MIRBUILD)/host/libmir.a $(MIRBUILD)/host/c2m

mirclean:
	rm -rf $(MIRBUILD)

# MIR variant lib for the cross target: the MIR makefile with the target
# define via its CPPFLAGS hook (-I. is REQUIRED — a CLI CPPFLAGS clobbers
# the MIR makefile's default -I$$(SRC_DIR)).
$(MIRBUILD)/aarch64-linux/libmir.a: FORCE
	mkdir -p $(MIRBUILD)/aarch64-linux
	$(MAKE) -C $(MIRDIR) BUILD_DIR=$(MIRBUILD)/aarch64-linux CPPFLAGS='-I. -DMIR_TARGET_AARCH64_LINUX' $(MIRBUILD)/aarch64-linux/libmir.a

# Emit-only cross compilers for the Mach-O targets (axis B): the same MIR
# variant-lib pattern; the writer signs ad-hoc, so the produced binaries
# run directly on macOS (Gate B-final = the owner's Macs).
cross-x86-64-macos:
	$(MAKE) MODE=cross-x86-64-macos ../bin/madc-x86-64-macos

cross-arm64-macos:
	$(MAKE) MODE=cross-arm64-macos ../bin/madc-arm64-macos

$(MIRBUILD)/x86-64-macos/libmir.a: FORCE
	mkdir -p $(MIRBUILD)/x86-64-macos
	$(MAKE) -C $(MIRDIR) BUILD_DIR=$(MIRBUILD)/x86-64-macos CPPFLAGS='-I. -DMIR_TARGET_X86_64_MACOS' $(MIRBUILD)/x86-64-macos/libmir.a

$(MIRBUILD)/arm64-macos/libmir.a: FORCE
	mkdir -p $(MIRBUILD)/arm64-macos
	$(MAKE) -C $(MIRDIR) BUILD_DIR=$(MIRBUILD)/arm64-macos CPPFLAGS='-I. -DMIR_TARGET_ARM64_MACOS' $(MIRBUILD)/arm64-macos/libmir.a

# The MH_OBJECT `.o` gate (Mach-O axis B step 4): builds BOTH cross madcs
# first, so the gate can never validate a stale binary, then runs the two
# independent authorities — ld64.lld + the SDK, and madc's own read-back.
# Not part of fulltest: it needs the cross madcs, llvm-18 and a macOS SDK,
# and it skips (rc 0) when any is missing, which would make it silent there.
machogate: cross-arm64-macos cross-x86-64-macos
	@cd .. && bash scripts/macho_obj_gate.sh
	@cd .. && bash scripts/macho_exe_dylib_gate.sh

# macOS release artifacts (macos-release-lane plan W1): the darwin twin of
# `release`. The hosted binaries are ALREADY -O2 and forest-packed at link
# time (the __MADC,__forest section — a signed Mach-O cannot take the ELF
# EOF trailer), so the release step is: build both arches SEQUENTIALLY (the
# per-mode generated tables share src/ file paths — never parallelize
# across arches), strip into the release-named twins, and verify the strip
# kept the forest bytes and the arm64 ad-hoc signature (llvm-strip re-signs
# when it rewrites the file). In-vivo AMFI + behavior evidence is the Mac
# side's job (W2, scripts/mac_battery.sh).
release-macos:
	# verify_macho_release.sh reads the packed forests back through the
	# HOST bin/madc, whose freeze-context hash must match the tree that
	# froze them (the version string rides the predefined-macro table) —
	# so the dev binary is rebuilt first. No-op when already current.
	$(MAKE)
	$(MAKE) hosted-arm64-macos
	$(MAKE) hosted-x86-64-macos
	llvm-strip-18 -o ../bin/madc-release-arm64-macos ../bin/madc-hosted-arm64-macos
	llvm-strip-18 -o ../bin/madc-release-x86-64-macos ../bin/madc-hosted-x86-64-macos
	cd .. && bash scripts/verify_macho_release.sh bin/madc-release-arm64-macos obj/hosted-arm64-macos/forest.bin
	cd .. && bash scripts/verify_macho_release.sh bin/madc-release-x86-64-macos obj/hosted-x86-64-macos/forest.bin
	cd .. && bash scripts/macho_exe_dylib_gate.sh

# madc hosted on macOS (Route 1): full madc binaries for the Macs. The
# emitted-C runtime archive (W3) builds beside each binary from the same
# per-mode objects; package_release_macos.sh ships it as lib/libmadc_rt.a.
hosted-x86-64-macos:
	$(MAKE) MODE=hosted-x86-64-macos ../bin/madc-hosted-x86-64-macos ../lib/libmadc_rt-hosted-x86-64-macos.a

hosted-arm64-macos:
	$(MAKE) MODE=hosted-arm64-macos ../bin/madc-hosted-arm64-macos ../lib/libmadc_rt-hosted-arm64-macos.a

# darwin-HOST MIR variant libs: built BY the darwin compiler, so host
# detection selects the Apple arms natively — no MIR_TARGET knobs.
$(MIRBUILD)/hosted-arm64-macos/libmir.a: FORCE
	mkdir -p $(MIRBUILD)/hosted-arm64-macos
	$(MAKE) -C $(MIRDIR) BUILD_DIR=$(MIRBUILD)/hosted-arm64-macos CC='$(CC)' AR='$(AR)' $(MIRBUILD)/hosted-arm64-macos/libmir.a

$(MIRBUILD)/hosted-x86-64-macos/libmir.a: FORCE
	mkdir -p $(MIRBUILD)/hosted-x86-64-macos
	$(MAKE) -C $(MIRDIR) BUILD_DIR=$(MIRBUILD)/hosted-x86-64-macos CC='$(CC)' AR='$(AR)' $(MIRBUILD)/hosted-x86-64-macos/libmir.a

# madc hosted on Windows (Track 6.4): the win64 twin of the darwin hosted
# targets. Same explicit-goal shape (binary + emitted-C runtime archive);
# forest packing is the W4 workstream, so this mode builds the dev forest
# shape (silent live parse). Run validation: wine + scripts/win_run.sh.
hosted-x86-64-windows:
	$(MAKE) MODE=hosted-x86-64-windows ../bin/madc-hosted-x86-64-windows.exe ../lib/libmadc_rt-hosted-x86-64-windows.a ../bin/libstdc++-6.dll ../bin/libwinpthread-1.dll ../bin/libmadc_rt.dll

# win64-HOST MIR variant lib: built BY the mingw compiler (CC carries the
# UCRT posture + specs swap), so host detection selects the _WIN32 arms
# natively — no MIR_TARGET knobs, same model as the darwin-host variants.
$(MIRBUILD)/hosted-x86-64-windows/libmir.a: FORCE
	mkdir -p $(MIRBUILD)/hosted-x86-64-windows
	$(MAKE) -C $(MIRDIR) BUILD_DIR=$(MIRBUILD)/hosted-x86-64-windows CC='$(CC)' AR='$(AR)' $(MIRBUILD)/hosted-x86-64-windows/libmir.a

# Release pipeline (strip-before-append is MANDATORY ordering: strip rewrites
# the ELF and would destroy an already-appended forest blob). Pack failure =
# build failure; the verify step proves the appended blob reads back (pin OK)
# and survives the stripped binary's dlsym resolver path.
# --with-forest= picks the carrier: embedded appends the container to the
# binary (default), sidecar writes bin/madc-release.forest beside it, none
# skips the pack (live-parse-only product).
# In the thin-CLI shape the carrier is the LIBRARY image (forest-carriers S4):
# one container serves the CLI and every embedding host, so the pack targets
# lib/libmadc.so — and the same strip-before-pack ordering applies to it (the
# install step therefore must NOT re-strip it; see install below).
ifeq ($(ENABLE_SHARED_CLI),1)
PACK_IMAGE_ARG = --image lib/libmadc.so
else
PACK_IMAGE_ARG =
endif
release:
	$(MAKE) MODE=release $(LIBMADC_STATIC) ../bin/madc-release $(LIBMADC_SHARED)
	strip ../bin/madc-release
ifeq ($(ENABLE_SHARED_CLI),1)
	strip --strip-unneeded ../lib/libmadc.so
endif
ifeq ($(WITH_FOREST),embedded)
	cd .. && bash scripts/forest_pack.sh $(PACK_IMAGE_ARG) bin/madc-release
else ifeq ($(WITH_FOREST),sidecar)
	cd .. && bash scripts/forest_pack.sh --sidecar $(PACK_IMAGE_ARG) bin/madc-release
endif

# Install the release artifacts under $(PREFIX) (DESTDIR-aware). Same layout
# as the distribution packages (scripts/package_release.sh): the stripped
# release binary as plain `madc`, the runtime as libmadc.so.0 + dev symlink
# (madc -o output DT_NEEDs the soname), the man page. Depends on `release`
# so lib/libmadc.so is guaranteed to be the -O2 release flavor (.libmode
# stamp) and bin/madc-release is stripped + forest-packed.
PREFIX ?= /usr/local
INSTALL_LIBDIR ?= $(PREFIX)/lib
install: release
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(INSTALL_LIBDIR) \
		$(DESTDIR)$(PREFIX)/share/man/man1
	install -m 755 ../bin/madc-release $(DESTDIR)$(PREFIX)/bin/madc
ifeq ($(WITH_FOREST),sidecar)
ifeq ($(ENABLE_SHARED_CLI),1)
	install -m 644 ../lib/libmadc.so.forest $(DESTDIR)$(INSTALL_LIBDIR)/libmadc.so.0.forest
else
	install -m 644 ../bin/madc-release.forest $(DESTDIR)$(PREFIX)/bin/madc.forest
endif
endif
	install -m 644 ../lib/libmadc.so $(DESTDIR)$(INSTALL_LIBDIR)/libmadc.so.0
# A packed library carries the container as trailing bytes; strip rewrites the
# ELF image and would discard them (the same law as the packed binary), so the
# thin-CLI shape strips at release time, BEFORE the pack, and never here.
ifneq ($(ENABLE_SHARED_CLI),1)
	strip --strip-unneeded $(DESTDIR)$(INSTALL_LIBDIR)/libmadc.so.0
endif
	ln -sf libmadc.so.0 $(DESTDIR)$(INSTALL_LIBDIR)/libmadc.so
	gzip -9n < ../docs/man/madc.1 > $(DESTDIR)$(PREFIX)/share/man/man1/madc.1.gz
	@if [ -z "$(DESTDIR)" ]; then ldconfig 2>/dev/null || true; fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/madc
	rm -f $(DESTDIR)$(INSTALL_LIBDIR)/libmadc.so.0
	rm -f $(DESTDIR)$(INSTALL_LIBDIR)/libmadc.so
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/madc.1.gz
	@if [ -z "$(DESTDIR)" ]; then ldconfig 2>/dev/null || true; fi

clean:
	rm -rf ../obj/develop ../obj/debug ../obj/release ../obj/cross-aarch64-linux ../obj/cross-x86-64-macos ../obj/cross-arm64-macos ../obj/hosted-arm64-macos ../obj/hosted-x86-64-macos ../obj/hosted-x86-64-windows
	rm -f ../obj/*.o ../obj/*.d ../obj/pic/*.o ../obj/pic/*.d
	rm -f $(ALL_TESTBINS)
	rm -f $(MADDAT_ARCHIVE)
	rm -f $(LIBMADC_STATIC)
	rm -f $(LIBMADC_SHARED)
	rm -f $(LIBDIR)/libmadc.so.$(MADC_SOVERSION)
	rm -f $(THIN_CLI)
	rm -f $(PROCESS_FIXTURE)
	rm -f sys_include_paths.cpp
	rm -f predefined_macros.cpp

# Each unit binary runs under a CPU cap (ulimit -t, survives even if SIGTERM is
# ignored) AND a wall-clock timeout. A hung unit test (e.g. an infinite loop in
# generated/interpreted code) is then KILLED and fails the suite, instead of
# spinning forever and pegging the host — see the 2026-06-01 test_cir incident.
TEST_CPU_LIMIT ?= 120
TEST_WALL_LIMIT ?= 180
test: $(TESTBINS)
	@for t in $(TESTBINS); do \
		echo "Running $$t..."; \
		( ulimit -t $(TEST_CPU_LIMIT); LD_LIBRARY_PATH="$(LIBDIR):/usr/local/lib:$$LD_LIBRARY_PATH" timeout $(TEST_WALL_LIMIT) "$$t" ); \
		rc=$$?; \
		if [ $$rc -eq 124 ] || [ $$rc -eq 137 ]; then \
			echo "*** $$t hit the time/CPU cap (rc=$$rc) — a hanging unit test was killed before it could peg the host. Failing the suite."; \
			exit 1; \
		elif [ $$rc -ne 0 ]; then \
			exit $$rc; \
		fi; \
	done

fulltest: ../bin/madc $(LIBMADC_SHARED) $(THIN_CLI) test
	@cd .. && bash scripts/run_tests.sh
	@cd .. && bash scripts/check-no-std-hardcoding.sh
	@cd .. && bash scripts/check-call-emit-symbol.sh
	@cd .. && bash scripts/check-var-emit-name-bypass.sh
	@cd .. && bash scripts/check-one-delim-tracker.sh
	@cd .. && bash scripts/check-one-macro-expander.sh
	@cd .. && bash scripts/check-one-aggregate-layout.sh
	@cd .. && bash scripts/check-one-dl-owner.sh
	@cd .. && bash scripts/check-one-spawn-owner.sh
	@cd .. && bash scripts/check-one-mmap-owner.sh
	@cd .. && bash scripts/check-one-mingw-stdio-map.sh
	@cd .. && bash scripts/check-cross-mode-compiles.sh
	@cd .. && bash scripts/check-i64-spec-spelling.sh
	@cd .. && bash scripts/check-ns-header-widths.sh
	@cd .. && bash scripts/check-template-thaw-choke.sh
	@cd .. && bash scripts/check-ref-arg-lowering-owner.sh
	@cd .. && bash scripts/check-fn-template-deduction-owner.sh
	@cd .. && bash scripts/check-dependent-type-query-owner.sh
	@cd .. && bash scripts/check-datachannel-write-owner.sh
	@cd .. && bash scripts/check-datachannel-cloexec-owner.sh
	@cd .. && bash scripts/check-one-error-composer.sh
	@cd .. && bash scripts/check-storage-seam-adoption.sh
	@cd .. && bash scripts/check-rule-trailers.sh
	@cd .. && bash scripts/warn_census.sh --check
	@cd .. && bash scripts/tag_arith_burndown.sh --check
	@cd .. && bash scripts/tsubst_flagon_gate.sh --check
	@cd .. && bash scripts/class_pattern_equivalence.sh
	@cd .. && bash scripts/despace_cache_gate.sh
	@cd .. && bash scripts/forest_selfexe_gate.sh
	@cd .. && bash scripts/forest_bind_gate.sh
	@cd .. && bash scripts/forest_emitpack_gate.sh
	@cd .. && bash scripts/forest_sidecar_gate.sh
	@cd .. && bash scripts/forest_crosstu_gate.sh
	@cd .. && bash scripts/forest_library_gate.sh
	@cd .. && bash scripts/forest_ledger_gate.sh
	@cd .. && bash scripts/forest_config_gate.sh
	@cd .. && bash scripts/unprototyped_call_abi_gate.sh
	@cd .. && bash scripts/sret_abi_gate.sh
	@cd .. && bash scripts/emitc_sret_gate.sh
	@cd .. && bash scripts/libcxx_gate.sh
	@cd .. && bash scripts/project_gate.sh
	@cd .. && bash scripts/forest_index_oracle.sh
	@cd .. && bash scripts/forest_dm_oracle.sh
	@cd .. && timeout 1500 bash scripts/win_posix_archive_gate.sh

# CIR renderer fidelity gate over the whole corpus (diagnostic; localizes
# missing renderer constructs and gcc-vs-emitted asm divergences).
cirfidelity: ../bin/madc
	@cd .. && bash scripts/cir_fidelity.sh --all

gcctest: ../bin/madc
	@cd .. && scripts/run_gcc_testsuite.py

gcctest-smoke: ../bin/madc
	@cd .. && scripts/run_gcc_testsuite.py --limit 25

# Runtime helpers (__madc_* in va_helpers, the file-stream wrappers, the
# exception runtime, namespace builtins, etc.) are resolved at runtime by
# the CIR/MIR backend via dlsym(RTLD_DEFAULT) and consulted at parse time
# the same way. Nothing references them at link time, so plain archive
# semantics would drop those objects. Force the whole libmadc archive in
# (with -rdynamic) so every runtime symbol is present in the dynamic table.
WHOLE_ARCHIVE_BEGIN ?= -Wl,--whole-archive
WHOLE_ARCHIVE_END   ?= -Wl,--no-whole-archive

$(BINDIR)/%: $(TESTDIR)/%.cpp $(DEPENDS) $(LIBMADC_STATIC) $(MIRLIB)
	$(CXX) -o $@ $(CXXFLAGS) $(DEFINES) $< $(WHOLE_ARCHIVE_BEGIN) $(LIBMADC_STATIC) $(WHOLE_ARCHIVE_END) $(MIRLIB) $(LIBS)

$(BINDIR)/test_process: $(PROCESS_FIXTURE)

$(PROCESS_FIXTURE): ../tests/helpers/process_fixture.cpp
	mkdir -p $(BINDIR)
	$(CXX) -o $@ $(CXXFLAGS) $<

# Where a thin CLI looks for its libmadc: $ORIGIN/../lib resolves in BOTH
# layouts — bin/madc -> lib/libmadc.so in the build tree, $(PREFIX)/bin/madc ->
# $(INSTALL_LIBDIR)/libmadc.so.0 once installed. (Apple's spelling is
# @loader_path; the darwin dylib shape itself rides the later packaging work.)
ifdef HOSTED_DARWIN_TARGET
CLI_RPATH = -Wl,-rpath,@loader_path/../lib
else
CLI_RPATH = -Wl,-rpath,'$$ORIGIN/../lib'
endif
# Link inputs per shape. Monolithic force-includes the WHOLE static archive:
# runtime helpers are resolved by dlsym(RTLD_DEFAULT), so plain archive
# semantics would drop those objects. The thin CLI links the shared library
# instead — its dynamic table already carries every one of them.
ifeq ($(ENABLE_SHARED_CLI),1)
CLI_LIB_DEP = $(LIBMADC_SHARED)
CLI_LINK = -L$(LIBDIR) -lmadc $(CLI_RPATH)
else
CLI_LIB_DEP = $(LIBMADC_STATIC)
CLI_LINK = $(WHOLE_ARCHIVE_BEGIN) $(LIBMADC_STATIC) $(WHOLE_ARCHIVE_END) $(MIRLIB)
endif

$(BIN): $(OBJDIR)/madc.o $(CLI_LIB_DEP) $(MIRLIB) $(FOREST_EMBED_DEP) $(LINK_SHAPE_STAMP)
	$(CXX) -o $@ $(OBJDIR)/madc.o $(CLI_LINK) $(CXXFLAGS) $(DEFINES) $(LIBS) $(FOREST_EMBED_LDFLAGS)
ifdef FOREST_SIDECAR
	cp $(DARWIN_FOREST_BIN) $@.forest
endif

# Thin-CLI vehicle for the library-carrier gate (forest-carriers S4): the SAME
# madc.o linked against the shared libmadc, so the library-image discovery arm
# is exercised in a default (monolithic) tree without reconfiguring it. In an
# --enable-shared tree bin/madc already IS this shape; the gate keeps using its
# own copy so it can pack a PRIVATE library image without touching lib/.
# (THIN_CLI itself is defined with the other artifact paths above — a
# prerequisite list is expanded when the rule is READ, so a definition down
# here would make `fulltest`'s prerequisite silently empty.)
$(THIN_CLI): $(OBJDIR)/madc.o $(LIBMADC_SHARED)
	$(CXX) -o $@ $(OBJDIR)/madc.o -L$(LIBDIR) -lmadc $(CLI_RPATH) $(CXXFLAGS) $(DEFINES) $(LIBS)

# lib/libmadc.{a,so} keep ONE path across modes, so make's timestamps alone
# would silently leave the OTHER mode's libs in place after a mode switch
# (the same skew per-mode obj trees prevent for objects). The stamp file
# records which mode built the libs; switching modes recreates it (newer
# than the libs), forcing a relink from the new mode's objects. The recipes
# name their object lists explicitly so the stamp never enters the archive.
LIBMODE_STAMP = $(LIBDIR)/.libmode-$(MODE)
$(LIBMODE_STAMP):
	@mkdir -p $(LIBDIR)
	@rm -f $(LIBDIR)/.libmode-*
	@touch $@

$(LIBMADC_STATIC): $(LIBMADC_STATIC_OBJS) $(LIBMODE_STAMP) $(RT_LEDGER_INPUTS)
	mkdir -p $(LIBDIR)
	rm -f $@
	$(AR) $(ARFLAGS) $@ $(LIBMADC_STATIC_OBJS)

# The emitted-C runtime subset (W3): the SAME ledger objects libmadc holds,
# re-archived alone so `cc emitted.c -lmadc_rt` links without the full
# runtime. No separate source list — membership is ledger_sources.txt's.
$(LIBMADC_RT): $(addprefix $(OBJDIR)/,$(RT_OFILES)) $(LIBMODE_STAMP) $(RT_LEDGER_INPUTS)
	mkdir -p $(LIBDIR)
	rm -f $@
	$(AR) $(ARFLAGS) $@ $(addprefix $(OBJDIR)/,$(RT_OFILES))

$(LIBMADC_SHARED): $(LIBMADC_OBJECTS) $(MIRLIB) $(LIBMODE_STAMP) $(RT_LEDGER_INPUTS)
	mkdir -p $(LIBDIR)
	$(CXX) -shared -Wl,-soname,libmadc.so.$(MADC_SOVERSION) -o $@ $(LIBMADC_OBJECTS) $(MIRLIB) $(CXXFLAGS) $(DEFINES) $(LIBS)
# A consumer linked with -lmadc gets DT_NEEDED = the SONAME (libmadc.so.0), so
# the build tree needs that name too or a thin CLI / host binary cannot load
# from lib/ (the install tree ships the real file under the soname and the
# .so as the dev symlink — same names, mirrored). Host modes only: a cross
# mode's foreign .so must never claim the host soname.
ifeq ($(LIBMADC_SHARED),$(LIBDIR)/libmadc.so)
	ln -sf libmadc.so $(LIBDIR)/libmadc.so.$(MADC_SOVERSION)
endif

$(MADDAT_ARCHIVE): $(MADDAT_OBJECTS)
	mkdir -p $(LIBDIR)
	rm -f $@
	$(AR) $(ARFLAGS) $@ $^

# embedded_headers.cpp is regenerated at PARSE time (see EMBED_GEN near the top),
# not by a recipe rule: a recipe-time prereq updates the file too late — make has
# already stat'd embedded_headers.o against the OLD mtime when evaluating the
# graph, so a changed/added/deleted embedded header would not rebuild the object
# until the NEXT make. Parse-time generation makes it correct in one invocation.

# Host system include search paths, captured from the configured compiler.
# Host-specific + gitignored, so generate it if missing (a fresh checkout has
# none); `make clean` removes it to force a refresh when the toolchain changes.
# The generator is a PREREQUISITE, not just the recipe: without it an edit to
# the capture logic left the existing table in place, so the build kept serving
# a table the current generator would never have produced.
sys_include_paths.cpp: ../scripts/gen_sys_includes.sh
	CXX="$(HOSTTAB_CXX)" MADC_SYS_INCLUDE_PREFIX_MAP="$(MADC_SYS_INCLUDE_PREFIX_MAP)" ../scripts/gen_sys_includes.sh

# Host predefined compiler macros, captured from the configured compiler.
# Same host-specific/gitignored/regenerate-if-missing treatment as above.
predefined_macros.cpp: ../scripts/gen_predefined_macros.sh
	CXX="$(HOSTTAB_CXX)" ../scripts/gen_predefined_macros.sh

$(OBJDIR)/%.o: %.cpp $(DEPENDS)
	@mkdir -p $(OBJDIR)
	$(CXX) -c -o $@ $(CXXFLAGS) $(DEFINES) -MMD -MP -MF $(@:.o=.d) $<

# src/rt/*.c — the dual-build C-lane runtime. Driven through $(CXX) with
# `-x c` rather than a separate $(CC) so every MODE's target/sysroot flags
# (the hosted-darwin CXX carries -target/--sysroot) apply unchanged; the
# C++ standard flag from CXXFLAGS must NOT come along, hence its own flag set.
RT_CFLAGS = -x c -std=c11 -Wall $(WERROR_FLAG) -ffunction-sections -fdata-sections $(MODE_OPTFLAGS)
# ...but the C++-ONLY header flags in CXX (-nostdinc++ plus the C++ -isystem
# pair) must not ride a `-x c` compile. gcc says so out loud ("valid for
# C++/ObjC++ but not for C"), and the quiet half is worse: a C++ standard
# library's include dir on a C compile's SYSTEM path lets its wrappers for C
# header names shadow the real ones. CXX_BASE is the same driver carrying the
# same target/sysroot flags WITHOUT that block — the two cross modes define it
# beside their CXX; every other mode adds no C++-only flags, so it is just CXX.
CXX_BASE ?= $(CXX)
# -MMD cannot see command-line flag changes. Bump this version whenever
# RT_CFLAGS or the PIC-only runtime flags change; both runtime object families
# depend on the per-mode stamp so neither can silently retain the old flags.
RT_FLAGS_VERSION = 2
RT_FLAGS_STAMP = $(OBJDIR)/.rt-flags-v$(RT_FLAGS_VERSION)
$(RT_FLAGS_STAMP):
	@mkdir -p $(OBJDIR)
	@rm -f $(OBJDIR)/.rt-flags-v*
	@touch $@
$(addprefix $(OBJDIR)/,$(RT_OFILES)): $(RT_FLAGS_STAMP)
$(addprefix $(PICOBJDIR)/,$(RT_OFILES)): $(RT_FLAGS_STAMP)
$(OBJDIR)/%.o: rt/%.c $(DEPENDS)
	@mkdir -p $(OBJDIR)
	$(CXX_BASE) -c -o $@ $(RT_CFLAGS) $(DEFINES) -MMD -MP -MF $(@:.o=.d) $<

# Apple-target modes compile the PER-MODE embedded_headers.cpp (generated at
# parse time into the obj tree, EMBED_GEN above) — never src/embedded_headers.cpp,
# which stays the committed include/madc/-only table for host builds.
ifdef EMBEDDED_HEADERS_CPP
$(OBJDIR)/embedded_headers.o: $(EMBEDDED_HEADERS_CPP) $(DEPENDS)
	@mkdir -p $(OBJDIR)
	$(CXX) -c -o $@ $(CXXFLAGS) $(DEFINES) -MMD -MP -MF $(@:.o=.d) $<
endif

# Per-mode include-table source (see SYS_INCLUDES_CPP above): same shape as
# embedded_headers.o — the generated .cpp lives in the obj tree, never src/.
# Guarded by the variable, not the mode family: every mode that captures a
# foreign header world (darwin hosted+cross, windows hosted) sets it.
ifdef SYS_INCLUDES_CPP
$(OBJDIR)/sys_include_paths.o: $(SYS_INCLUDES_CPP) $(DEPENDS)
	@mkdir -p $(OBJDIR)
	$(CXX) -c -o $@ $(CXXFLAGS) $(DEFINES) -MMD -MP -MF $(@:.o=.d) $<
endif

# UCRT setjmp compat thunks (W0): plain cpp-driven assembly, no -MMD (the
# one include is windows-lane-frozen); assembled by the mode's own CC so a
# future arm64-windows twin reuses the rule unchanged.
ifeq ($(MODE),hosted-x86-64-windows)
$(OBJDIR)/win_ucrt_compat.o: win_ucrt_compat.S
	@mkdir -p $(OBJDIR)
	$(CC) -c -o $@ $<

# Runtime DLLs staged beside the exe (the darwin dylib-beside-binary
# analogue): the loader searches the exe's directory first, so wine, the
# real-Windows runner, and the release zip all pick these UCRT-flavor
# copies over anything else by name.
../bin/libstdc++-6.dll: $(WIN_UCRT_LIBSTDCXX)/bin/libstdc++-6.dll
	cp -p $< $@

../bin/libwinpthread-1.dll: $(WIN_UCRT_LIBSTDCXX)/bin/libwinpthread-1.dll
	cp -p $< $@

# libmadc_rt.dll (W3.5): the win64 twin of libmadc.so — the runtime
# provider AOT images (madc -o prog.exe) import madc_puts / the
# madc_value_* bridge / the __madc_* helpers from, and the DLL half of
# the external-link story (the import lib serves
# `x86_64-w64-mingw32-gcc prog.o -lmadc_rt.dll`). Whole-archive: loaded
# programs reference these symbols, the DLL itself does not, so plain
# archive semantics would drop their objects. --export-all-symbols (in
# LIBS) is the export surface — plain names, the darwin flat-bind
# analogue. Staged in bin/ beside madc.exe: the PE loader's app-dir rule
# serves madc.exe's own LoadLibrary probe, wine, win_run.sh's
# adjacent-DLL shipping, and the release zip alike.
# The .def rides the link: PE ld auto-excludes libmingwex/oldnames code
# from --export-all-symbols, but AOT images import the mingw-stdio map's
# surface (__mingw_printf family, *l math, dirent) from THIS DLL — the
# .def re-adds exactly the map's names, generated from the one map owner
# so the lists can never drift. --export-all-symbols stays on the line:
# with a .def present ld would otherwise export ONLY the .def names.
../bin/libmadc_rt.dll: $(LIBMADC_STATIC) $(MIRLIB) ../scripts/gen_win_rt_exports.sh $(MIRDIR)/mir-mingw-stdio.h
	bash ../scripts/gen_win_rt_exports.sh $(MIRDIR)/mir-mingw-stdio.h $(OBJDIR)/libmadc_rt.def
	$(CXX) -shared -o $@ $(OBJDIR)/libmadc_rt.def -Wl,--out-implib,../lib/libmadc_rt.dll.a $(WHOLE_ARCHIVE_BEGIN) $(LIBMADC_STATIC) $(WHOLE_ARCHIVE_END) $(MIRLIB) $(CXXFLAGS) $(LIBS)
endif

# Forest-carriers S1: cross-freeze the darwin groves and keep the freezer (the
# same-arch cross madc) fresh via a recursive make. FORCE (the shared rule
# by the variant-lib section) re-runs that make every hosted build (a no-op
# when current); the freeze itself re-runs only when the freezer binary, the
# prelude manifest, or the pack script changed (file-mtime prerequisites —
# the classic if-changed pattern).
ifdef HOSTED_DARWIN_TARGET
$(CROSS_MADC_BIN): FORCE
	@$(MAKE) MODE=cross-$(DARWIN_ARCH)-macos $(CROSS_MADC_BIN)

$(DARWIN_FOREST_BIN): $(CROSS_MADC_BIN) $(DARWIN_PRELUDE_DIR)/.MANIFEST ../scripts/forest_pack_darwin.sh ../scripts/forest_pack_headers_darwin.txt $(RT_LEDGER_INPUTS)
	bash ../scripts/forest_pack_darwin.sh $(CROSS_MADC_BIN) $(DARWIN_PRELUDE_DIR)/.MANIFEST $@
endif

$(PICOBJDIR)/%.o: %.cpp $(DEPENDS)
	@mkdir -p $(PICOBJDIR)
	$(CXX) -c -o $@ $(PIC_CXXFLAGS) $(DEFINES) -MMD -MP -MF $(@:.o=.d) $<

$(PICOBJDIR)/%.o: rt/%.c $(DEPENDS)
	@mkdir -p $(PICOBJDIR)
	$(CXX_BASE) -c -o $@ $(RT_CFLAGS) -fPIC $(DEFINES) -MMD -MP -MF $(@:.o=.d) $<

# Auto-generated per-object header dependencies (from -MMD -MP above). A changed
# .h rebuilds every .o that includes it, preventing stale-object ABI skew (e.g.
# a layout change in cir_builder.h not recompiling madc_cir.o). Missing on a
# clean build (regenerated during it); -include ignores the absent files.
-include $(wildcard $(OBJDIR)/*.d $(PICOBJDIR)/*.d)
