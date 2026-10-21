# markdown.mk — the markdown MODULE (include/madc_module.h): GitHub-flavoured
# Markdown parsed by cmark-gfm into a value tree (plan docs/plans/2026-10-03-
# chthonia-windows-macos.md §7d: Help's topics, Markdown Preview, .md
# highlighting). A program says `import markdown;`; madc finds the module by
# name in its module directory (lib/madc/modules/markdown.<dso>) and the
# module describes itself — nothing about it lives in the core. cmark-gfm is a
# dependency, never part of madc (owner 2026-10-04) — no subtree, never linked
# into libmadc or any madc image; the engine's export surface carries no
# cmark_* symbol.
#
# THE FACE: modules/markdown/markdown.madi; its declaration table is generated
# by madc (madc_modules.mk's madc_module_table_rules) and compiled into the
# module (markdown.cpp includes it).
#
# HOW cmark-gfm REACHES EACH TARGET (the git.mk shape):
#   Linux/host — the pinned static cmark-gfm scripts/stage_cmark_gfm.sh builds
#     for the host ($(CMARK_GFM_STAGE)/<target>/), linked INTO
#     markdown.so; its COPYING ships as the notice. Nothing named cmark ships
#     beside the module, and no package depends on a system cmark-gfm. With no
#     stage, a SYSTEM cmark-gfm (pkg-config libcmark-gfm, which names the
#     extensions library too) builds the module, linked shared — a dev box, or
#     Homebrew's formula, which depends on its own cmark-gfm; the system package
#     carries its own notice. A release (package_release.sh) requires the stage.
#   Windows — lib/madc/modules/markdown.dll, the x86-64-windows stage linked
#     in; macOS — lib/madc/modules/<arch>-macos/markdown.dylib (and a flat
#     lib/madc/modules/markdown.dylib of the arch just built), the <arch>-macos
#     stage linked in. Both ride their platform's release target.
#   THE PIN: CMARK_GFM_TAG below, the newest upstream release; the staged paths
#     carry it, so a bump re-stages.
#
# The module binds libmadc's own symbols (madc::value, the registration API its
# generated builders call) at LOAD, from the image that imported it: undefined
# at link on ELF and Mach-O (bin/madc exports them; darwin uses -undefined
# dynamic_lookup). A Windows PE DLL cannot carry undefined symbols, so there
# the module links libmadc's import lib (../lib/libmadc.dll.a from
# ../bin/libmadc-0.dll).
MARKDOWN_SRC = modules/markdown/markdown.cpp
MARKDOWN_FACE = modules/markdown/markdown.madi
MARKDOWN_HDRS = modules/markdown/markdown.h modules/markdown/markdown_enums $(INCDIR)/madc_module.h
MARKDOWN_BUILD_DIR = ../obj/markdown/$(MODE)
MARKDOWN_DEFAULT =
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
MARKDOWN_STAGE_CFLAGS = -I$(MARKDOWN_STAGED)/include -DCMARK_GFM_STATIC_DEFINE -DCMARK_GFM_EXTENSIONS_STATIC_DEFINE
MARKDOWN_ARCHIVES = $(MARKDOWN_STAGED)/libcmark-gfm-extensions.a $(MARKDOWN_STAGED)/libcmark-gfm.a
MARKDOWN_LINK_PREREQ =

ifeq ($(MODE),hosted-x86-64-windows)
# --- Windows bundle: lib/madc/modules/markdown.dll, on the SAME UCRT
# libstdc++/pthread runtime as madc (the git.mk discipline).
MARKDOWN_STAGED = $(CMARK_GFM_STAGE)/x86-64-windows
MARKDOWN_AVAILABLE := 1
MARKDOWN_CFLAGS = $(MARKDOWN_STAGE_CFLAGS)
MARKDOWN_LIBRARY = $(MODULE_LIBDIR)/markdown.dll
# --export-all-symbols: the markdown:: publics madc resolves by mangled name
# are the DLL's exports (git.mk); --exclude-libs keeps cmark's out.
MARKDOWN_LINK_FLAGS = -shared -static-libgcc -L$(WIN_UCRT_LIBSTDCXX)/lib -Wl,--export-all-symbols -Wl,--exclude-libs,ALL
MARKDOWN_LIBS = $(MARKDOWN_ARCHIVES) -L../lib -lmadc.dll -lpthread
MARKDOWN_LINK_PREREQ = ../bin/libmadc-0.dll
MARKDOWN_NOTICE = $(CMARK_GFM_STAGE)/src/COPYING
else ifdef HOSTED_DARWIN_TARGET
# --- macOS bundle: lib/madc/modules/<arch>-macos/markdown.dylib.
MARKDOWN_STAGED = $(CMARK_GFM_STAGE)/$(DARWIN_ARCH)-macos
MARKDOWN_AVAILABLE := 1
MARKDOWN_CFLAGS = $(MARKDOWN_STAGE_CFLAGS)
MARKDOWN_LIBRARY = $(MODULE_LIBDIR)/$(DARWIN_ARCH)-macos/markdown.dylib
MARKDOWN_LINK_FLAGS = $(DARWIN_LD_FLAGS) -dynamiclib -Wl,-install_name,@rpath/markdown.dylib -undefined dynamic_lookup
MARKDOWN_LIBS = -Wl,-load_hidden,$(MARKDOWN_STAGED)/libcmark-gfm-extensions.a -Wl,-load_hidden,$(MARKDOWN_STAGED)/libcmark-gfm.a
MARKDOWN_NOTICE = $(CMARK_GFM_STAGE)/src/COPYING
else
# --- Linux/host: the staged static cmark-gfm for this host, else a system
# cmark-gfm (shared).
MARKDOWN_STAGED = $(CMARK_GFM_STAGE)/$(STAGE_HOST_TARGET)
ifneq ($(wildcard $(MARKDOWN_STAGED)/libcmark-gfm.a),)
MARKDOWN_AVAILABLE := 1
MARKDOWN_CFLAGS = $(MARKDOWN_STAGE_CFLAGS)
MARKDOWN_LIBS = $(MARKDOWN_ARCHIVES)
MARKDOWN_NOTICE = $(CMARK_GFM_STAGE)/src/COPYING
else
MARKDOWN_CFLAGS := $(shell pkg-config --cflags libcmark-gfm 2>/dev/null)
MARKDOWN_AVAILABLE := $(shell pkg-config --exists libcmark-gfm 2>/dev/null && echo 1)
# cmark-gfm's pkg-config file names both libraries (-lcmark-gfm
# -lcmark-gfm-extensions).
MARKDOWN_LIBS := $(shell pkg-config --libs libcmark-gfm 2>/dev/null)
MARKDOWN_NOTICE =
endif
MARKDOWN_LIBRARY = $(MODULE_LIBDIR)/markdown.so
MARKDOWN_LINK_FLAGS = -shared -fPIC -Wl,-soname,markdown.so -Wl,--exclude-libs,ALL
ifeq ($(MARKDOWN_AVAILABLE),1)
MARKDOWN_DEFAULT = $(MARKDOWN_LIBRARY)
# The module's unit test: the module's objects + cmark-gfm.
TESTBINS += $(BINDIR)/test_markdown
endif
endif

# The face's generated table: one file for every target (the builders name
# DataDefs, never sizes or symbols), generated by the build host's madc.
$(eval $(call madc_module_table_rules,markdown,$(MARKDOWN_FACE)))
MARKDOWN_TABLE = $(MODULE_TABLE_DIR)/markdown/markdown.inc

.PHONY: markdown markdown-deps markdown-windows markdown-macos markdown-arm64-macos markdown-x86-64-macos

markdown: $(MARKDOWN_LIBRARY)
ifeq ($(MODE),hosted-x86-64-windows)
markdown: ../bin/libstdc++-6.dll ../bin/libwinpthread-1.dll
endif

markdown-windows:
	$(MAKE) MODE=hosted-x86-64-windows markdown
markdown-macos: markdown-arm64-macos markdown-x86-64-macos
markdown-arm64-macos markdown-x86-64-macos:
	$(MAKE) MODE=hosted-$(patsubst markdown-%,%,$@) markdown

markdown-deps:
ifeq ($(MODE),hosted-x86-64-windows)
	@test -f $(MARKDOWN_STAGED)/libcmark-gfm.a || { echo 'markdown ($(MODE)) needs $(MARKDOWN_STAGED) — scripts/stage_cmark_gfm.sh x86-64-windows' >&2; exit 1; }
	@test -f $(WIN_UCRT_LIBSTDCXX)/lib/libstdc++.dll.a || { echo 'markdown win needs the UCRT stage: scripts/build_win_ucrt_libstdcxx.sh' >&2; exit 1; }
else ifdef HOSTED_DARWIN_TARGET
	@test -f $(MARKDOWN_STAGED)/libcmark-gfm.a || { echo 'markdown ($(MODE)) needs $(MARKDOWN_STAGED) — scripts/stage_cmark_gfm.sh $(DARWIN_ARCH)-macos' >&2; exit 1; }
else
	@test -n "$(MARKDOWN_AVAILABLE)" || { echo 'markdown needs cmark-gfm: $(MARKDOWN_STAGED) (scripts/stage_cmark_gfm.sh host), or a system cmark-gfm (pkg-config libcmark-gfm)' >&2; exit 1; }
endif

# A command stamp makes flag/toolchain changes rebuild the library, while
# identical commands leave the artifact alone (the git.mk discipline).
$(MARKDOWN_BUILD_DIR)/command: FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(CXX) $(MARKDOWN_LINK_FLAGS) $(CXXFLAGS) $(DEFINES) $(MARKDOWN_CFLAGS) $(MARKDOWN_LIBS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

ifneq ($(MARKDOWN_LIBRARY),)
$(MARKDOWN_LIBRARY): $(MARKDOWN_SRC) $(MARKDOWN_HDRS) $(MARKDOWN_TABLE) $(MARKDOWN_LINK_PREREQ) $(MARKDOWN_BUILD_DIR)/command | markdown-deps
	mkdir -p $(dir $@)
	$(CXX) $(MARKDOWN_LINK_FLAGS) -o $@ $(CXXFLAGS) $(DEFINES) -I. -I$(dir $(MARKDOWN_TABLE)) $(MARKDOWN_CFLAGS) $(MARKDOWN_SRC) $(MARKDOWN_LIBS)
ifdef HOSTED_DARWIN_TARGET
	# The flat copy of THIS arch, where the module path looks
	# ($(MODULE_LIBDIR)/markdown.dylib) — git.mk's discipline.
	cp -f $@ $(MODULE_LIBDIR)/markdown.dylib
endif
endif

$(BINDIR)/test_markdown: $(TESTDIR)/test_markdown.cpp $(MARKDOWN_SRC) $(MARKDOWN_HDRS) $(MARKDOWN_TABLE) $(DEPENDS) $(LIBMADC_STATIC) $(MIRLIB) | markdown-deps
	$(CXX) -o $@ $(CXXFLAGS) $(DEFINES) -I. -I$(dir $(MARKDOWN_TABLE)) $(MARKDOWN_CFLAGS) $< $(MARKDOWN_SRC) $(WHOLE_ARCHIVE_BEGIN) $(LIBMADC_STATIC) $(WHOLE_ARCHIVE_END) $(MIRLIB) $(MARKDOWN_LIBS) $(LIBS)
