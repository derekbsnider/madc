-include ../config.mk

CC ?= clang
CXX ?= clang++
CXXFLAGS ?= -std=c++11 -Wall
PIC_CXXFLAGS ?= $(CXXFLAGS) -fPIC
AR ?= ar
ARFLAGS ?= rcs
INSTALL ?= install
INSTALL_DATA ?= $(INSTALL) -m 644
INSTALL_PROGRAM ?= $(INSTALL) -m 755
INSTALL_DIR ?= $(INSTALL) -d
#CFLAGS = -O3 -std=c++14 -stdlib=libc++ -Wall -Wno-nested-anon-types -Wno-gnu-anonymous-struct -Wunused
HFILES	= madc.h datadef.h tokens.h datatokens.h
CORE_OFILES = lexer.o parser.o madc_value.o madc_error.o madc_program.o madc_api.o madc_c_api.o madc_globals.o ns_common.o ns_php.o ns_perl.o ns_python.o ns_ruby.o ns_js.o ns_rust.o va_helpers.o exception_runtime.o pch.o embedded_headers.o precompiled_headers.o madc_mir_backend.o madc_mangle.o madc_cir.o cir_builder.o cir_emit_c.o madc_stream_runtime.o madc.o
MADDAT_OFILES = $(MADC_STORAGE_DB_OFILES)
MADHDRS = $(wildcard ../include/madc/*)
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
DEFINES	= -I$(INCDIR) -I$(MIRDIR) -I$(MIRDIR)/c2mir $(FEATURE_DEFINES) $(OPTIONAL_CPPFLAGS)
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

all:
	$(MAKE) $(LIBMADC_STATIC) ../bin/madc $(LIBMADC_SHARED)

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

test: $(TESTBINS)
	@for t in $(TESTBINS); do echo "Running $$t..."; LD_LIBRARY_PATH="$(LIBDIR):/usr/local/lib:$$LD_LIBRARY_PATH" $$t || exit $$?; done

fulltest: ../bin/madc $(LIBMADC_SHARED) test
	@cd .. && bash scripts/run_tests.sh

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

embedded_headers.cpp: $(MADHDRS)
	../scripts/gen_embedded_headers.sh

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
