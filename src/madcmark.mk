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
#   Windows and macOS — the cross-target arms join in the next slice (the
#     same stage, built per target).
#   THE PIN: CMARK_GFM_TAG below, the newest upstream release; the staged paths
#     carry it, so a bump re-stages.
#
# The module binds libmadc's own symbols (madc::value) at LOAD, from the image
# that imported it: undefined at link on ELF (bin/madc exports them).
MADCMARK_SRC = modules/madcmark/madcmark.cpp
MADCMARK_HDRS = $(INCDIR)/madcdis/markdown.h $(INCDIR)/madc/bits/markdown_enums $(INCDIR)/madc/madcmark.h
MADCMARK_BUILD_DIR = ../obj/madcmark/$(MODE)
MADCMARK_DEFAULT =
# Where scripts/stage_cmark_gfm.sh stages the per-target static cmark-gfm,
# one directory per pinned tag (the LIBGIT2_DIR discipline).
CMARK_GFM_DIR ?= /workspace/cmark-gfm
CMARK_GFM_TAG := 0.29.0.gfm.13
CMARK_GFM_STAGE = $(CMARK_GFM_DIR)/$(CMARK_GFM_TAG)

ifeq ($(MODE),hosted-x86-64-windows)
MADCMARK_LIBRARY =
else ifdef HOSTED_DARWIN_TARGET
MADCMARK_LIBRARY =
else
# --- Linux/host: the staged static cmark-gfm for this host, else a system
# cmark-gfm (shared). Static archives stay internal: --exclude-libs keeps
# cmark_* out of the module's exports (modules load RTLD_GLOBAL — an exported
# copy would enter the process's global scope; gate: check-module-exports.sh).
# CMARK_GFM_STATIC_DEFINE: the stage's export header then declares the API
# plainly, as the archives were compiled.
MADCMARK_STAGED = $(CMARK_GFM_STAGE)/$(STAGE_HOST_TARGET)
ifneq ($(wildcard $(MADCMARK_STAGED)/libcmark-gfm.a),)
MADCMARK_AVAILABLE := 1
MADCMARK_CFLAGS = -I$(MADCMARK_STAGED)/include -DCMARK_GFM_STATIC_DEFINE -DCMARK_GFM_EXTENSIONS_STATIC_DEFINE
MADCMARK_LIBS = $(MADCMARK_STAGED)/libcmark-gfm-extensions.a $(MADCMARK_STAGED)/libcmark-gfm.a
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

.PHONY: libmadcmark madcmark-deps

libmadcmark: $(MADCMARK_LIBRARY)

madcmark-deps:
	@test -n "$(MADCMARK_AVAILABLE)" || { echo 'madcmark needs cmark-gfm: $(MADCMARK_STAGED) (scripts/stage_cmark_gfm.sh host), or a system cmark-gfm (pkg-config libcmark-gfm)' >&2; exit 1; }

# A command stamp makes flag/toolchain changes rebuild the library, while
# identical commands leave the artifact alone (the madcgit.mk discipline).
$(MADCMARK_BUILD_DIR)/command: FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(CXX) $(MADCMARK_LINK_FLAGS) $(CXXFLAGS) $(DEFINES) $(MADCMARK_CFLAGS) $(MADCMARK_LIBS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

ifneq ($(MADCMARK_LIBRARY),)
$(MADCMARK_LIBRARY): $(MADCMARK_SRC) $(MADCMARK_HDRS) $(MADCMARK_BUILD_DIR)/command | madcmark-deps
	mkdir -p $(dir $@)
	$(CXX) $(MADCMARK_LINK_FLAGS) -o $@ $(CXXFLAGS) $(DEFINES) -I. $(MADCMARK_CFLAGS) $(MADCMARK_SRC) $(MADCMARK_LIBS)
endif

$(BINDIR)/test_markdown: $(TESTDIR)/test_markdown.cpp $(MADCMARK_SRC) $(MADCMARK_HDRS) $(DEPENDS) $(LIBMADC_STATIC) $(MIRLIB) | madcmark-deps
	$(CXX) -o $@ $(CXXFLAGS) $(DEFINES) -I. $(MADCMARK_CFLAGS) $< $(MADCMARK_SRC) $(WHOLE_ARCHIVE_BEGIN) $(LIBMADC_STATIC) $(WHOLE_ARCHIVE_END) $(MIRLIB) $(MADCMARK_LIBS) $(LIBS)
