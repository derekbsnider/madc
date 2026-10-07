# madcgit.mk — the madcgit MODULE: madc's READ-ONLY view of a LOCAL git
# repository over libgit2. The row in src/madc_modules.cpp is LAZY: a program
# that says `git::…` (<ns_git> imports the module) compiles and runs without
# the library and asks madc::module_available first. libgit2 is a dependency
# of the IDE's nexus, never part of madc — no subtree, never linked into
# libmadc or any madc image; the engine's export surface carries no git_*
# symbol (scripts/check-c-abi-surface.sh).
#
# HOW libgit2 REACHES EACH TARGET (docs/plans/2026-09-15-madcgit-cross-targets-plan.md):
#   every target, Linux included — libgit2 is a BUILD REQUIREMENT statically
#     linked INTO libmadcgit: the minimal read-only static archive
#     scripts/stage_libgit2.sh builds for the target
#     ($(LIBGIT2_STAGE)/libgit2-<target>.a). Nothing named libgit2 ships, and
#     no package depends on a system libgit2. The cross arms mirror
#     webview.mk's per-mode pattern. One exception, on the Linux/host arm
#     only: with no stage, a SYSTEM libgit2 at the floor builds the module
#     (Homebrew's formula depends on its own libgit2).
#   THE FLOOR (owner 2026-10-04): libgit2 1.8.7 or 1.9.7, or newer — the
#     releases carrying libgit2's security fixes. LIBGIT2_TAG below is the ONE
#     pin; the staged paths carry it, so a bump re-stages.
#     modules/madcgit/libgit2_floor.h is the ONE statement of the floor: the
#     module refuses to compile below it, and the host arm preprocesses it to
#     judge a system libgit2. Ubuntu 24.04's system libgit2 is 1.7.2, below the
#     floor, which is why Linux links the staged archive too.
#
# The module binds libmadc's own symbols (madc::value, madc::error, the error
# composer, the path canonicalizer) at LOAD, from the image that imported it.
# On ELF/Mach-O they stay UNDEFINED at link (bin/madc exports them; darwin uses
# -undefined dynamic_lookup). A Windows PE DLL cannot carry undefined symbols,
# so there the module links libmadc's import lib (../lib/libmadc.dll.a from
# ../bin/libmadc-0.dll); at runtime those imports bind to the loaded libmadc-0.dll.
MADCGIT_SRC = modules/madcgit/madcgit.cpp
MADCGIT_FLOOR = modules/madcgit/libgit2_floor.h
MADCGIT_HDRS = $(INCDIR)/madcdis/git_repo.h $(INCDIR)/madc/bits/git_enums $(INCDIR)/madc/madcgit.h $(INCDIR)/handle_table.h $(MADCGIT_FLOOR)
MADCGIT_BUILD_DIR = ../obj/madcgit/$(MODE)
MADCGIT_DEFAULT =
# Where scripts/stage_libgit2.sh stages the per-target minimal static libgit2
# and the pinned source's public headers (mirrors DARWIN_ZSTD_DIR), one
# directory per pinned tag.
LIBGIT2_DIR ?= /workspace/libgit2
LIBGIT2_TAG := v1.9.7
LIBGIT2_STAGE = $(LIBGIT2_DIR)/$(LIBGIT2_TAG)
LIBGIT2_INCLUDE = $(LIBGIT2_STAGE)/src/include

ifeq ($(MODE),hosted-x86-64-windows)
# --- Windows bundle: madcgit.dll (row .windows) beside madc.exe. Self-contained
# libgit2 (bundled zlib in the archive); madc:: resolves through libmadc's
# import lib. ws2_32: libgit2 references winsock symbols even with the network
# backends off. Uses the SAME UCRT libstdc++/pthread runtime as madc (webview
# discipline) — never an MSVCRT libstdc++ copy.
MADCGIT_LIBRARY = ../bin/madcgit.dll
MADCGIT_LIBGIT2 = $(LIBGIT2_STAGE)/libgit2-x86-64-windows.a
MADCGIT_CFLAGS = -I$(LIBGIT2_INCLUDE)
# --exclude-libs,ALL: the DLL has no dllexport, so ld auto-exports every
# global — the archives' too, libgit2's whole API among them. Excluded, the
# module exports its own madcgit_* only (gate: check-module-exports.sh).
MADCGIT_LINK_FLAGS = -shared -static-libgcc -L$(WIN_UCRT_LIBSTDCXX)/lib -Wl,--exclude-libs,ALL
MADCGIT_LIBS = $(MADCGIT_LIBGIT2) -L../lib -lmadc.dll -lws2_32 -lsecur32 -lpthread
MADCGIT_LINK_PREREQ = ../bin/libmadc-0.dll
else ifdef HOSTED_DARWIN_TARGET
# --- macOS bundle: lib/madcgit/<arch>-macos/libmadcgit.dylib. libgit2 static
# (compiled against the SDK's zlib); -lz is resolved here (a system lib on
# every mac). madc:: stays undefined -> -undefined dynamic_lookup, bound at
# dlopen from madc (which exports its globals to dlsym without -rdynamic).
MADCGIT_LIBRARY = $(LIBDIR)/madcgit/$(DARWIN_ARCH)-macos/libmadcgit.dylib
MADCGIT_LIBGIT2 = $(LIBGIT2_STAGE)/libgit2-$(DARWIN_ARCH)-macos.a
MADCGIT_CFLAGS = -I$(LIBGIT2_INCLUDE)
MADCGIT_LINK_FLAGS = $(DARWIN_LD_FLAGS) -dynamiclib -Wl,-install_name,@rpath/libmadcgit.dylib -undefined dynamic_lookup
# -load_hidden: the archive's symbols load with hidden visibility, so the
# dylib exports its own madcgit_* only, never libgit2's API (a script's dlsym
# fallback would otherwise find them; gate: check-module-exports.sh).
MADCGIT_LIBS = -Wl,-load_hidden,$(MADCGIT_LIBGIT2) -lz
MADCGIT_LINK_PREREQ =
else
# --- Linux/host: the staged static libgit2 for this host
# (scripts/stage_libgit2.sh host), linked into libmadcgit.so with the system
# zlib madc already depends on. With no stage, a system libgit2 (pkg-config)
# builds the module when the floor header preprocesses cleanly against its
# flags; MADCGIT_LIBGIT2 stays empty then (no archive linked, no notice to
# ship: the system package carries its own). `all` builds the module where
# either is present; a release (package_release.sh) requires the stage.
MADCGIT_STAGED = $(LIBGIT2_STAGE)/libgit2-$(STAGE_HOST_TARGET).a
ifneq ($(wildcard $(MADCGIT_STAGED)),)
MADCGIT_LIBGIT2 = $(MADCGIT_STAGED)
MADCGIT_AVAILABLE := 1
MADCGIT_CFLAGS = -I$(LIBGIT2_INCLUDE)
MADCGIT_LIBS = $(MADCGIT_LIBGIT2) -lz -lpthread
else
MADCGIT_LIBGIT2 =
MADCGIT_CFLAGS := $(shell pkg-config --cflags libgit2 2>/dev/null)
MADCGIT_AVAILABLE := $(shell pkg-config --exists libgit2 2>/dev/null && $(CXX) -E -x c++ $(MADCGIT_CFLAGS) $(MADCGIT_FLOOR) >/dev/null 2>&1 && echo 1)
MADCGIT_LIBS := $(shell pkg-config --libs libgit2 2>/dev/null)
endif
MADCGIT_LIBRARY = $(LIBDIR)/libmadcgit.so
# --exclude-libs,ALL: the static archive's git_* stay internal to the module.
# Modules load RTLD_GLOBAL (madc_dl_open_global), so an exported copy would
# enter the process's global scope beside any libgit2 a program loads itself
# (gate: check-module-exports.sh).
MADCGIT_LINK_FLAGS = -shared -fPIC -Wl,-soname,libmadcgit.so -Wl,--exclude-libs,ALL
MADCGIT_LINK_PREREQ =
ifeq ($(MADCGIT_AVAILABLE),1)
MADCGIT_DEFAULT = $(MADCGIT_LIBRARY)
# The module's unit test: the module's objects + libgit2, not the engine's
# generic unit link (its fixture repository is built through libgit2's own
# write API — the oracle, never a second owner).
TESTBINS += $(BINDIR)/test_gitrepo
endif
endif

.PHONY: libmadcgit madcgit-deps madcgit-windows madcgit-macos madcgit-arm64-macos madcgit-x86-64-macos

libmadcgit: $(MADCGIT_LIBRARY)
ifeq ($(MODE),hosted-x86-64-windows)
libmadcgit: ../bin/libstdc++-6.dll ../bin/libwinpthread-1.dll
endif

madcgit-windows:
	$(MAKE) MODE=hosted-x86-64-windows libmadcgit
madcgit-macos: madcgit-arm64-macos madcgit-x86-64-macos
madcgit-arm64-macos madcgit-x86-64-macos:
	$(MAKE) MODE=hosted-$(patsubst madcgit-%,%,$@) libmadcgit

madcgit-deps:
ifeq ($(MODE),hosted-x86-64-windows)
	@test -f $(MADCGIT_LIBGIT2) || { echo 'madcgit ($(MODE)) needs $(MADCGIT_LIBGIT2) — scripts/stage_libgit2.sh x86-64-windows' >&2; exit 1; }
	@test -f $(WIN_UCRT_LIBSTDCXX)/lib/libstdc++.dll.a || { echo 'madcgit win needs the UCRT stage: scripts/build_win_ucrt_libstdcxx.sh' >&2; exit 1; }
else ifdef HOSTED_DARWIN_TARGET
	@test -f $(MADCGIT_LIBGIT2) || { echo 'madcgit ($(MODE)) needs $(MADCGIT_LIBGIT2) — scripts/stage_libgit2.sh $(DARWIN_ARCH)-macos' >&2; exit 1; }
else
	@test -n "$(MADCGIT_AVAILABLE)" || { echo 'madcgit needs libgit2 1.8.7, 1.9.7 or newer: $(MADCGIT_STAGED) (scripts/stage_libgit2.sh host), or a system libgit2 at the floor' >&2; exit 1; }
endif

# A command stamp makes flag/toolchain changes rebuild the library, while
# identical commands leave the artifact alone (the webview.mk discipline).
$(MADCGIT_BUILD_DIR)/command: FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(CXX) $(MADCGIT_LINK_FLAGS) $(CXXFLAGS) $(DEFINES) $(MADCGIT_CFLAGS) $(MADCGIT_LIBS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

# -I. : the module lives one directory down and reads the engine's src-local
# headers (madc_datachannel_internal.h, madc_posix_io.h) like any engine file.
$(MADCGIT_LIBRARY): $(MADCGIT_SRC) $(MADCGIT_HDRS) $(MADCGIT_LINK_PREREQ) $(MADCGIT_BUILD_DIR)/command | madcgit-deps
	mkdir -p $(dir $@)
	$(CXX) $(MADCGIT_LINK_FLAGS) -o $@ $(CXXFLAGS) $(DEFINES) -I. $(MADCGIT_CFLAGS) $(MADCGIT_SRC) $(MADCGIT_LIBS)
ifdef HOSTED_DARWIN_TARGET
	# Linux-parity: the module loader (madc_module_open) looks for the image
	# FLAT beside the binary — $(LIBDIR)/libmadcgit.dylib — the same place the
	# Linux build puts libmadcgit.so. The darwin build nests it per-arch (for
	# the two-arch cross build), so also place a flat copy of THIS arch, so the
	# un-packaged release binary (the darwin suite; a from-source mac dev) finds
	# it exactly as on Linux. Packaging installs it flat the same way.
	cp -f $@ $(LIBDIR)/libmadcgit.dylib
endif

$(BINDIR)/test_gitrepo: $(TESTDIR)/test_gitrepo.cpp $(MADCGIT_SRC) $(MADCGIT_HDRS) $(DEPENDS) $(LIBMADC_STATIC) $(MIRLIB) | madcgit-deps
	$(CXX) -o $@ $(CXXFLAGS) $(DEFINES) -I. $(MADCGIT_CFLAGS) $< $(MADCGIT_SRC) $(WHOLE_ARCHIVE_BEGIN) $(LIBMADC_STATIC) $(WHOLE_ARCHIVE_END) $(MIRLIB) $(MADCGIT_LIBS) $(LIBS)
