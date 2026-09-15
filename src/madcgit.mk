# madcgit.mk — the madcgit MODULE: lib/libmadcgit.so, madc's READ-ONLY view of
# a local git repository over the SYSTEM libgit2 (owner ruling 2026-09-15:
# libgit2 is a dependency of the IDE's nexus, never part of madc — no subtree,
# never linked into libmadc or any madc image; the engine's export surface
# carries no git_* symbol, scripts/check-c-abi-surface.sh). The row in
# src/madc_modules.cpp is LAZY: a program that says `git::…` (<ns_git> imports
# the module) compiles and runs without the library and asks
# madc::module_available first.
#
# Built where pkg-config finds libgit2 (libgit2-dev on the container,
# scripts/provision_container.sh; brew libgit2 on a mac host) — `all` includes
# it then (MADCGIT_DEFAULT) so the suite has it; absent, nothing is built and
# nothing is said (the nexus degrades to "no repository"). The hosted cross
# modes (windows / macos from the container) have no libgit2 for their target
# and build nothing here; a fetched prebuilt for those targets is the named
# follow-up (the WebView2 SDK fetch is the precedent).
#
# The module binds libmadc's own symbols (madc::value, madc::error, the error
# composer, the path canonicalizer) at LOAD, from the image that imported it —
# bin/madc exports them (-rdynamic); at link they stay undefined on purpose.
MADCGIT_SRC = modules/madcgit/madcgit.cpp
MADCGIT_HDRS = $(INCDIR)/madcdis/git_repo.h $(INCDIR)/madc/madcgit.h $(INCDIR)/handle_table.h
MADCGIT_BUILD_DIR = ../obj/madcgit/$(MODE)
MADCGIT_AVAILABLE := $(shell pkg-config --exists libgit2 2>/dev/null && echo 1)
MADCGIT_CFLAGS := $(shell pkg-config --cflags libgit2 2>/dev/null)
MADCGIT_LIBS := $(shell pkg-config --libs libgit2 2>/dev/null)
MADCGIT_LIBRARY = $(LIBDIR)/libmadcgit.so
MADCGIT_DEFAULT =

ifeq (,$(filter cross-% hosted-%,$(MODE)))
ifeq ($(MADCGIT_AVAILABLE),1)
MADCGIT_DEFAULT = $(MADCGIT_LIBRARY)
# The module's unit test: the module's objects + libgit2, not the engine's
# generic unit link (its fixture repository is built through libgit2's own
# write API — the oracle, never a second owner).
TESTBINS += $(BINDIR)/test_gitrepo
endif
endif

.PHONY: libmadcgit madcgit-deps

libmadcgit: $(MADCGIT_LIBRARY)

madcgit-deps:
	@pkg-config --exists libgit2 || { echo 'madcgit needs libgit2 (libgit2-dev — scripts/provision_container.sh)' >&2; exit 1; }

# A command stamp makes flag/toolchain changes rebuild the library, while
# identical commands leave the artifact alone (the webview.mk discipline).
$(MADCGIT_BUILD_DIR)/command: FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(CXX) $(CXXFLAGS) $(DEFINES) $(MADCGIT_CFLAGS) $(MADCGIT_LIBS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

# -I. : the module lives one directory down and reads the engine's src-local
# headers (madc_datachannel_internal.h, madc_posix_io.h) like any engine file.
$(MADCGIT_LIBRARY): $(MADCGIT_SRC) $(MADCGIT_HDRS) $(MADCGIT_BUILD_DIR)/command | madcgit-deps
	mkdir -p $(dir $@)
	$(CXX) -shared -fPIC -Wl,-soname,libmadcgit.so -o $@ $(CXXFLAGS) $(DEFINES) -I. $(MADCGIT_CFLAGS) $(MADCGIT_SRC) $(MADCGIT_LIBS)

$(BINDIR)/test_gitrepo: $(TESTDIR)/test_gitrepo.cpp $(MADCGIT_SRC) $(MADCGIT_HDRS) $(DEPENDS) $(LIBMADC_STATIC) $(MIRLIB) | madcgit-deps
	$(CXX) -o $@ $(CXXFLAGS) $(DEFINES) -I. $(MADCGIT_CFLAGS) $< $(MADCGIT_SRC) $(WHOLE_ARCHIVE_BEGIN) $(LIBMADC_STATIC) $(WHOLE_ARCHIVE_END) $(MIRLIB) $(MADCGIT_LIBS) $(LIBS)
