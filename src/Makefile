-include ../config.mk

CC ?= clang
CXX ?= clang++
# Default to -O0 during development: optimize madc's *algorithms* first, and
# keep the binary unoptimized so profiling/gdb attribution is honest. The -O2
# default (P0, docs/plans/2026-06-22-front-end-performance-plan.md — a free
# ~1.74x) is a last-lap switch, flipped only once the front-end is doing the
# work properly. Use the `debug` target for -O0 -g.
CXXFLAGS ?= -std=c++11 -Wall -O0
PIC_CXXFLAGS ?= $(CXXFLAGS) -fPIC
AR ?= ar
ARFLAGS ?= rcs
INSTALL ?= install
INSTALL_DATA ?= $(INSTALL) -m 644
INSTALL_PROGRAM ?= $(INSTALL) -m 755
INSTALL_DIR ?= $(INSTALL) -d
#CFLAGS = -O3 -std=c++14 -stdlib=libc++ -Wall -Wno-nested-anon-types -Wno-gnu-anonymous-struct -Wunused
HFILES	= madc.h datadef.h tokens.h datatokens.h
CORE_OFILES = lexer.o parser.o madc_value.o madc_error.o madc_program.o madc_api.o madc_c_api.o madc_globals.o ns_common.o ns_php.o ns_perl.o ns_python.o ns_ruby.o ns_js.o ns_rust.o ns_madc.o va_helpers.o exception_runtime.o pch.o embedded_headers.o sys_include_paths.o predefined_macros.o precompiled_headers.o madc_mir_backend.o madc_mangle.o madc_cir.o cir_builder.o cir_emit_c.o madc_project.o madc.o
MADDAT_OFILES = $(MADC_STORAGE_DB_OFILES)
MADHDRS = $(wildcard ../include/madc/*)
# Regenerate embedded_headers.cpp NOW, at Makefile parse time, before any target
# is evaluated — so an edited/added/DELETED embedded header is reflected in
# embedded_headers.o within a SINGLE make. The generator is idempotent (rewrites
# only on real content change), so this does not churn mtimes or force rebuilds.
EMBED_GEN := $(shell ../scripts/gen_embedded_headers.sh >/dev/null 2>&1; echo done)
LIBMADDHDRS = $(wildcard ../include/libmadc/*.h)
MADDATHDRS = $(wildcard ../include/madcdat/*.h)
INCDIR	= ../include
BINDIR	= ../bin
LIBDIR	= ../lib
OBJDIR	= ../obj
PICOBJDIR = ../obj/pic
FEATURE_DEFINES ?=
OPTIONAL_CPPFLAGS ?=
OPTIONAL_LIBS ?=
MADC_STORAGE_DB_OFILES ?= madcdat_storage.o
HAVE_MADCDAT ?= 1
MIRDIR	= /workspace/mir
# MADC_VERSION (computed below from ../VERSION) is threaded into the binary as a
# string literal so the built-in MADC_VERSION macro tracks the release version —
# single source of truth, no hardcoded copy in the lexer.
DEFINES	= -I$(INCDIR) -I$(MIRDIR) -I$(MIRDIR)/c2mir $(FEATURE_DEFINES) $(OPTIONAL_CPPFLAGS) -DMADC_VERSION_STR='"$(MADC_VERSION)"'
#LIBS	= -lreadline -lasmjit
LIBS	= -rdynamic -ldl -lz -lm -lpthread $(OPTIONAL_LIBS)
MIRLIB	= $(MIRDIR)/libmir.a
DEPENDS	= $(patsubst %,$(INCDIR)/%,$(HFILES)) $(LIBMADDHDRS) $(MADDATHDRS)

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
# Objects for the library: everything except the main() driver
LIBMADC_STATIC_OBJS = $(filter-out $(OBJDIR)/madc.o, $(OBJECTS))
pkgconfigdir ?= $(libdir)/pkgconfig

TESTDIR  = ../tests/unit
TESTSRCS = $(wildcard $(TESTDIR)/*.cpp)
MADDAT_TESTSRCS = $(TESTDIR)/test_libmadc_bdb.cpp $(TESTDIR)/test_libmadc_dsv.cpp $(TESTDIR)/test_libmadc_flr.cpp $(TESTDIR)/test_libmadc_gdbm.cpp $(TESTDIR)/test_libmadc_qdbm.cpp $(TESTDIR)/test_libmadc_relation.cpp $(TESTDIR)/test_libmadc_sqlite.cpp $(TESTDIR)/test_libmadc_storage_contract.cpp $(TESTDIR)/test_libmadc_vlr.cpp
ifeq ($(HAVE_MADCDAT),1)
ACTIVE_TESTSRCS = $(TESTSRCS)
else
ACTIVE_TESTSRCS = $(filter-out $(MADDAT_TESTSRCS), $(TESTSRCS))
endif
ALL_TESTBINS = $(patsubst $(TESTDIR)/%.cpp,$(BINDIR)/%,$(TESTSRCS))
TESTBINS = $(patsubst $(TESTDIR)/%.cpp,$(BINDIR)/%,$(ACTIVE_TESTSRCS))
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
DEFAULT_TARGETS = $(LIBMADC_STATIC) ../bin/madc $(LIBMADC_SHARED) $(TESTBINS)
else
DEFAULT_TARGETS = $(LIBMADC_STATIC) ../bin/madc $(LIBMADC_SHARED)
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
	$(INSTALL_PROGRAM) $(LIBMADC_SHARED) $(DESTDIR)$(libdir)/libmadc.so.$(MADC_VERSION)
	cd $(DESTDIR)$(libdir) && ln -sf libmadc.so.$(MADC_VERSION) libmadc.so.$(MADC_SOVERSION)
	cd $(DESTDIR)$(libdir) && ln -sf libmadc.so.$(MADC_SOVERSION) libmadc.so
	$(INSTALL_DATA) ../include/madc_api.h $(DESTDIR)$(includedir)/madc_api.h
	@for h in $(LIBMADDHDRS); do $(INSTALL_DATA) $$h $(DESTDIR)$(includedir)/libmadc/; done
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
	$(MAKE) CXXFLAGS="-std=c++11 -Wall -g -O0" ../bin/madc
clean:
	rm -f $(OBJECTS)
	rm -f ../obj/madcdat_storage.o ../obj/madcdat_storage_bdb.o ../obj/madcdat_storage_gdbm.o ../obj/madcdat_storage_qdbm.o ../obj/madcdat_storage_sqlite.o
	rm -f ../obj/pic/*.o
	rm -f $(ALL_TESTBINS)
	rm -f $(MADDAT_ARCHIVE)
	rm -f $(LIBMADC_STATIC)
	rm -f $(LIBMADC_SHARED)
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

fulltest: ../bin/madc $(LIBMADC_SHARED) test
	@cd .. && bash scripts/run_tests.sh
	@cd .. && bash scripts/check-no-std-hardcoding.sh
	@cd .. && bash scripts/check-call-emit-symbol.sh
	@cd .. && bash scripts/warn_census.sh --check
	@cd .. && bash scripts/tag_arith_burndown.sh --check
	@cd .. && bash scripts/tsubst_flagon_gate.sh --check

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
WHOLE_ARCHIVE_BEGIN = -Wl,--whole-archive
WHOLE_ARCHIVE_END   = -Wl,--no-whole-archive

$(BINDIR)/%: $(TESTDIR)/%.cpp $(DEPENDS) $(LIBMADC_STATIC) $(MIRLIB)
	$(CXX) -o $@ $(CXXFLAGS) $(DEFINES) $< $(WHOLE_ARCHIVE_BEGIN) $(LIBMADC_STATIC) $(WHOLE_ARCHIVE_END) $(MIRLIB) $(LIBS)

../bin/madc: $(OBJDIR)/madc.o $(LIBMADC_STATIC) $(MIRLIB)
	$(CXX) -o $@ $(OBJDIR)/madc.o $(WHOLE_ARCHIVE_BEGIN) $(LIBMADC_STATIC) $(WHOLE_ARCHIVE_END) $(MIRLIB) $(CXXFLAGS) $(DEFINES) $(LIBS)

$(LIBMADC_STATIC): $(LIBMADC_STATIC_OBJS)
	mkdir -p $(LIBDIR)
	rm -f $@
	$(AR) $(ARFLAGS) $@ $^

$(LIBMADC_SHARED): $(LIBMADC_OBJECTS)
	mkdir -p $(LIBDIR)
	$(CXX) -shared -Wl,-soname,libmadc.so.$(MADC_SOVERSION) -o $@ $^ $(MIRLIB) $(CXXFLAGS) $(DEFINES) $(LIBS)

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
sys_include_paths.cpp:
	CXX="$(CXX)" ../scripts/gen_sys_includes.sh

# Host predefined compiler macros, captured from the configured compiler.
# Same host-specific/gitignored/regenerate-if-missing treatment as above.
predefined_macros.cpp:
	CXX="$(CXX)" ../scripts/gen_predefined_macros.sh

$(OBJDIR)/%.o: %.cpp $(DEPENDS)
	$(CXX) -c -o $@ $(CXXFLAGS) $(DEFINES) -MMD -MP -MF $(@:.o=.d) $<

$(PICOBJDIR)/%.o: %.cpp $(DEPENDS)
	mkdir -p $(PICOBJDIR)
	$(CXX) -c -o $@ $(PIC_CXXFLAGS) $(DEFINES) -MMD -MP -MF $(@:.o=.d) $<

# Auto-generated per-object header dependencies (from -MMD -MP above). A changed
# .h rebuilds every .o that includes it, preventing stale-object ABI skew (e.g.
# a layout change in cir_builder.h not recompiling madc_cir.o). Missing on a
# clean build (regenerated during it); -include ignores the absent files.
-include $(wildcard $(OBJDIR)/*.d $(PICOBJDIR)/*.d)
