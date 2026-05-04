-include ../config.mk

CXX ?= g++
CXXFLAGS ?= -std=c++11 -Wall
AR ?= ar
ARFLAGS ?= rcs
INSTALL ?= install
INSTALL_DATA ?= $(INSTALL) -m 644
INSTALL_DIR ?= $(INSTALL) -d
#CFLAGS = -O3 -std=c++14 -stdlib=libc++ -Wall -Wno-nested-anon-types -Wno-gnu-anonymous-struct -Wunused
HFILES	= madc.h datadef.h tokens.h datatokens.h
CORE_OFILES = lexer.o parser.o typesafe.o compiler.o madc_ir.o madc_value.o madc_error.o ns_common.o ns_php.o ns_perl.o ns_python.o ns_ruby.o ns_js.o ns_rust.o ns_stl.o va_helpers.o embedded_headers.o madc.o
MADDAT_OFILES = $(MADC_STORAGE_DB_OFILES)
MADHDRS = $(wildcard ../include/madc/*)
LIBMADDHDRS = $(wildcard ../include/libmadc/*.h)
MADDATHDRS = $(wildcard ../include/madcdat/*.h)
INCDIR	= ../include
BINDIR	= ../bin
LIBDIR	= ../lib
OBJDIR	= ../obj
FEATURE_DEFINES ?=
OPTIONAL_CPPFLAGS ?=
OPTIONAL_LIBS ?=
MADC_STORAGE_DB_OFILES ?=
HAVE_MADCDAT ?= 1
DEFINES	= -I$(INCDIR) $(FEATURE_DEFINES) $(OPTIONAL_CPPFLAGS)
#LIBS	= -lreadline -lasmjit
LIBS	= -L/usr/local/lib -Wl,-rpath,/usr/local/lib -rdynamic -lasmjit -ldl $(OPTIONAL_LIBS)
DEPENDS	= $(patsubst %,$(INCDIR)/%,$(HFILES)) $(LIBMADDHDRS) $(MADDATHDRS)

ifeq ($(HAVE_MADCDAT),1)
OFILES = $(CORE_OFILES) $(MADDAT_OFILES)
else
OFILES = $(CORE_OFILES)
endif

OBJECTS = $(patsubst %,$(OBJDIR)/%,$(OFILES))
MADDAT_OBJECTS = $(patsubst %,$(OBJDIR)/%,$(MADDAT_OFILES))
MADDAT_ARCHIVE = $(LIBDIR)/libmadcdat.a

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

all:
	$(MAKE) ../bin/madc

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
	rm -f ../obj/madc_storage.o ../obj/madc_storage_bdb.o ../obj/madc_storage_gdbm.o ../obj/madc_storage_qdbm.o ../obj/madc_storage_sqlite.o
	rm -f $(ALL_TESTBINS)
	rm -f $(MADDAT_ARCHIVE)

test: $(TESTBINS)
	@for t in $(TESTBINS); do echo "Running $$t..."; $$t; done

fulltest: ../bin/madc test
	@cd .. && bash scripts/run_tests.sh

$(BINDIR)/%: $(TESTDIR)/%.cpp $(DEPENDS) $(TESTOBJ)
	$(CXX) -o $@ $(CXXFLAGS) $(DEFINES) $< $(TESTOBJ) $(LIBS)

../bin/madc: $(OBJECTS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(DEFINES) $(LIBS)

$(MADDAT_ARCHIVE): $(MADDAT_OBJECTS)
	mkdir -p $(LIBDIR)
	rm -f $@
	$(AR) $(ARFLAGS) $@ $^

embedded_headers.cpp: $(MADHDRS)
	../scripts/gen_embedded_headers.sh

$(OBJDIR)/%.o: %.cpp $(DEPENDS)
	$(CXX) -c -o $@ $(CXXFLAGS) $(DEFINES) $<
