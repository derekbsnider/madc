# madcmark.mk — the madcmark MODULE: GitHub-flavoured Markdown parsed by
# cmark-gfm into a value tree (plan docs/plans/2026-10-03-chthonia-windows-
# macos.md §7d: Help's topics, Markdown Preview, .md highlighting). The row in
# src/madc_modules.cpp is LAZY: a program that says `markdown::…`
# (<ns_markdown> imports the module) compiles and runs without it and asks
# markdown::available() first. cmark-gfm is a dependency, never part of madc
# (owner 2026-10-04) — no subtree, never linked into libmadc or any madc
# image; the engine's export surface carries no cmark_* symbol.
#
# HOW cmark-gfm REACHES EACH TARGET (the madcgit.mk shape):
#   Linux/host — the pinned static cmark-gfm scripts/stage_cmark_gfm.sh builds
#     for the host ($(CMARK_GFM_STAGE)/<target>/), linked INTO
#     libmadcmark.so; its COPYING ships as the notice. Nothing named cmark
#     ships beside the module, and no package depends on a system cmark-gfm.
#     With no stage, a SYSTEM cmark-gfm (pkg-config libcmark-gfm, which
#     names the extensions library too) builds the module, linked shared — a dev
#     box, or Homebrew's formula, which depends on its own cmark-gfm; the
#     system package carries its own notice. A release (package_release.sh)
#     requires the stage.
#   Windows — madcmark.dll beside madc.exe, the x86-64-windows stage linked
#     in; macOS — lib/madcmark/<arch>-macos/libmadcmark.dylib (and a flat
#     lib/libmadcmark.dylib of the arch just built), the <arch>-macos stage
#     linked in. Both ride their platform's release target.
#   THE PIN: CMARK_GFM_TAG below, the newest upstream release; the staged paths
#     carry it, so a bump re-stages.
#
# The module binds libmadc's own symbols (madc::value) at LOAD, from the image
# that imported it: undefined at link on ELF and Mach-O (bin/madc exports
# them; darwin uses -undefined dynamic_lookup). A Windows PE DLL cannot carry
# undefined symbols, so there the module links libmadc's import lib
# (../lib/libmadc.dll.a from ../bin/libmadc-0.dll).
MADCMARK_SRC = modules/madcmark/madcmark.cpp
MADCMARK_HDRS = $(INCDIR)/madcdis/markdown.h $(INCDIR)/madc/bits/markdown_enums $(INCDIR)/madc/madcmark.h
MADCMARK_BUILD_DIR = ../obj/madcmark/$(MODE)
MADCMARK_DEFAULT =
# Where scripts/stage_cmark_gfm.sh stages the per-target static cmark-gfm,
# one directory per pinned tag (the LIBGIT2_DIR discipline).
CMARK_GFM_DIR ?= /workspace/cmark-gfm
CMARK_GFM_TAG := 0.29.0.gfm.13
CMARK_GFM_STAGE = $(CMARK_GFM_DIR)/$(CMARK_GFM_TAG)

# Every staged arm: the stage's headers, with CMARK_GFM_STATIC_DEFINE so its
# export header declares the API as the archives were compiled (on Windows it
# would otherwise say dllimport), and the stage's COPYING as the notice. The
# archives' symbols stay internal to the module: --exclude-libs (ELF, PE) and
# -load_hidden (Mach-O) keep cmark_* out of its exports (modules load
# RTLD_GLOBAL — an exported copy would enter the process's global scope; gate:
# check-module-exports.sh).
MADCMARK_STAGE_CFLAGS = -I$(MADCMARK_STAGED)/include -DCMARK_GFM_STATIC_DEFINE -DCMARK_GFM_EXTENSIONS_STATIC_DEFINE
MADCMARK_ARCHIVES = $(MADCMARK_STAGED)/libcmark-gfm-extensions.a $(MADCMARK_STAGED)/libcmark-gfm.a
MADCMARK_LINK_PREREQ =

ifeq ($(MODE),hosted-x86-64-windows)
# --- Windows bundle: madcmark.dll (row .windows) beside madc.exe, on the SAME
# UCRT libstdc++/pthread runtime as madc (the madcgit.mk discipline).
MADCMARK_STAGED = $(CMARK_GFM_STAGE)/x86-64-windows
MADCMARK_AVAILABLE := 1
MADCMARK_CFLAGS = $(MADCMARK_STAGE_CFLAGS)
MADCMARK_LIBRARY = ../bin/madcmark.dll
MADCMARK_LINK_FLAGS = -shared -static-libgcc -L$(WIN_UCRT_LIBSTDCXX)/lib -Wl,--exclude-libs,ALL
MADCMARK_LIBS = $(MADCMARK_ARCHIVES) -L../lib -lmadc.dll -lpthread
MADCMARK_LINK_PREREQ = ../bin/libmadc-0.dll
MADCMARK_NOTICE = $(CMARK_GFM_STAGE)/src/COPYING
else ifdef HOSTED_DARWIN_TARGET
# --- macOS bundle: lib/madcmark/<arch>-macos/libmadcmark.dylib.
MADCMARK_STAGED = $(CMARK_GFM_STAGE)/$(DARWIN_ARCH)-macos
MADCMARK_AVAILABLE := 1
MADCMARK_CFLAGS = $(MADCMARK_STAGE_CFLAGS)
MADCMARK_LIBRARY = $(LIBDIR)/madcmark/$(DARWIN_ARCH)-macos/libmadcmark.dylib
MADCMARK_LINK_FLAGS = $(DARWIN_LD_FLAGS) -dynamiclib -Wl,-install_name,@rpath/libmadcmark.dylib -undefined dynamic_lookup
MADCMARK_LIBS = -Wl,-load_hidden,$(MADCMARK_STAGED)/libcmark-gfm-extensions.a -Wl,-load_hidden,$(MADCMARK_STAGED)/libcmark-gfm.a
MADCMARK_NOTICE = $(CMARK_GFM_STAGE)/src/COPYING
else
# --- Linux/host: the staged static cmark-gfm for this host, else a system
# cmark-gfm (shared).
MADCMARK_STAGED = $(CMARK_GFM_STAGE)/$(STAGE_HOST_TARGET)
ifneq ($(wildcard $(MADCMARK_STAGED)/libcmark-gfm.a),)
MADCMARK_AVAILABLE := 1
MADCMARK_CFLAGS = $(MADCMARK_STAGE_CFLAGS)
MADCMARK_LIBS = $(MADCMARK_ARCHIVES)
MADCMARK_NOTICE = $(CMARK_GFM_STAGE)/src/COPYING
else
MADCMARK_CFLAGS := $(shell pkg-config --cflags libcmark-gfm 2>/dev/null)
MADCMARK_AVAILABLE := $(shell pkg-config --exists libcmark-gfm 2>/dev/null && echo 1)
# cmark-gfm's pkg-config file names both libraries (-lcmark-gfm
# -lcmark-gfm-extensions).
MADCMARK_LIBS := $(shell pkg-config --libs libcmark-gfm 2>/dev/null)
MADCMARK_NOTICE =
endif
MADCMARK_LIBRARY = $(LIBDIR)/libmadcmark.so
MADCMARK_LINK_FLAGS = -shared -fPIC -Wl,-soname,libmadcmark.so -Wl,--exclude-libs,ALL
ifeq ($(MADCMARK_AVAILABLE),1)
MADCMARK_DEFAULT = $(MADCMARK_LIBRARY)
# The module's unit test: the module's objects + cmark-gfm.
TESTBINS += $(BINDIR)/test_markdown
endif
endif

.PHONY: libmadcmark madcmark-deps madcmark-windows madcmark-macos madcmark-arm64-macos madcmark-x86-64-macos

libmadcmark: $(MADCMARK_LIBRARY)
ifeq ($(MODE),hosted-x86-64-windows)
libmadcmark: ../bin/libstdc++-6.dll ../bin/libwinpthread-1.dll
endif

madcmark-windows:
	$(MAKE) MODE=hosted-x86-64-windows libmadcmark
madcmark-macos: madcmark-arm64-macos madcmark-x86-64-macos
madcmark-arm64-macos madcmark-x86-64-macos:
	$(MAKE) MODE=hosted-$(patsubst madcmark-%,%,$@) libmadcmark

madcmark-deps:
ifeq ($(MODE),hosted-x86-64-windows)
	@test -f $(MADCMARK_STAGED)/libcmark-gfm.a || { echo 'madcmark ($(MODE)) needs $(MADCMARK_STAGED) — scripts/stage_cmark_gfm.sh x86-64-windows' >&2; exit 1; }
	@test -f $(WIN_UCRT_LIBSTDCXX)/lib/libstdc++.dll.a || { echo 'madcmark win needs the UCRT stage: scripts/build_win_ucrt_libstdcxx.sh' >&2; exit 1; }
else ifdef HOSTED_DARWIN_TARGET
	@test -f $(MADCMARK_STAGED)/libcmark-gfm.a || { echo 'madcmark ($(MODE)) needs $(MADCMARK_STAGED) — scripts/stage_cmark_gfm.sh $(DARWIN_ARCH)-macos' >&2; exit 1; }
else
	@test -n "$(MADCMARK_AVAILABLE)" || { echo 'madcmark needs cmark-gfm: $(MADCMARK_STAGED) (scripts/stage_cmark_gfm.sh host), or a system cmark-gfm (pkg-config libcmark-gfm)' >&2; exit 1; }
endif

# A command stamp makes flag/toolchain changes rebuild the library, while
# identical commands leave the artifact alone (the madcgit.mk discipline).
$(MADCMARK_BUILD_DIR)/command: FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(CXX) $(MADCMARK_LINK_FLAGS) $(CXXFLAGS) $(DEFINES) $(MADCMARK_CFLAGS) $(MADCMARK_LIBS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

ifneq ($(MADCMARK_LIBRARY),)
$(MADCMARK_LIBRARY): $(MADCMARK_SRC) $(MADCMARK_HDRS) $(MADCMARK_LINK_PREREQ) $(MADCMARK_BUILD_DIR)/command | madcmark-deps
	mkdir -p $(dir $@)
	$(CXX) $(MADCMARK_LINK_FLAGS) -o $@ $(CXXFLAGS) $(DEFINES) -I. $(MADCMARK_CFLAGS) $(MADCMARK_SRC) $(MADCMARK_LIBS)
ifdef HOSTED_DARWIN_TARGET
	# The flat copy of THIS arch, where the module loader looks
	# ($(LIBDIR)/libmadcmark.dylib, beside the binary's lib/) — madcgit.mk's.
	cp -f $@ $(LIBDIR)/libmadcmark.dylib
endif
endif

$(BINDIR)/test_markdown: $(TESTDIR)/test_markdown.cpp $(MADCMARK_SRC) $(MADCMARK_HDRS) $(DEPENDS) $(LIBMADC_STATIC) $(MIRLIB) | madcmark-deps
	$(CXX) -o $@ $(CXXFLAGS) $(DEFINES) -I. $(MADCMARK_CFLAGS) $< $(MADCMARK_SRC) $(WHOLE_ARCHIVE_BEGIN) $(LIBMADC_STATIC) $(WHOLE_ARCHIVE_END) $(MIRLIB) $(MADCMARK_LIBS) $(LIBS)
