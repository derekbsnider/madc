CXX=g++
CXXFLAGS= -std=c++11 -Wall
#CFLAGS = -O3 -std=c++14 -stdlib=libc++ -Wall -Wno-nested-anon-types -Wno-gnu-anonymous-struct -Wunused
HFILES	= madc.h datadef.h tokens.h datatokens.h
OFILES	= lexer.o parser.o typesafe.o compiler.o ns_php.o ns_perl.o ns_python.o ns_ruby.o ns_js.o ns_stl.o madc.o
INCDIR	= ../include
BINDIR	= ../bin
LIBDIR	= ../lib
OBJDIR	= ../obj
DEFINES	= -I$(INCDIR)
#LIBS	= -lreadline -lasmjit
LIBS	= -L/usr/local/lib -Wl,-rpath,/usr/local/lib -lasmjit -ldl
DEPENDS	= $(patsubst %,$(INCDIR)/%,$(HFILES))
OBJECTS = $(patsubst %,$(OBJDIR)/%,$(OFILES))

TESTDIR  = ../tests/unit
TESTSRCS = $(wildcard $(TESTDIR)/*.cpp)
TESTBINS = $(patsubst $(TESTDIR)/%.cpp,$(TESTDIR)/%,$(TESTSRCS))
# Link tests against all objects except madc.o (which has main())
TESTOBJ  = $(filter-out $(OBJDIR)/madc.o, $(OBJECTS))

all:
	make ../bin/madc
debug:
	make CXXFLAGS="-std=c++11 -Wall -g -O0" ../bin/madc
clean:
	rm -f $(OBJECTS)
	rm -f $(TESTBINS)

test: $(TESTBINS)
	@for t in $(TESTBINS); do echo "Running $$t..."; $$t; done

$(TESTDIR)/%: $(TESTDIR)/%.cpp $(DEPENDS) $(TESTOBJ)
	$(CXX) -o $@ $(CXXFLAGS) $(DEFINES) $< $(TESTOBJ) $(LIBS)

../bin/madc: $(OBJECTS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(DEFINES) $(LIBS)

$(OBJDIR)/%.o: %.cpp $(DEPENDS)
	$(CXX) -c -o $@ $(CXXFLAGS) $(DEFINES) $<
