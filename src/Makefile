CXX=g++
CXXFLAGS=-O3 -std=c++11 -Wall
#CFLAGS = -O3 -std=c++14 -stdlib=libc++ -Wall -Wno-nested-anon-types -Wno-gnu-anonymous-struct -Wunused
HFILES	= madc.h datadef.h tokens.h datatokens.h
OFILES	= lexer.o parser.o compiler.o madc.o
INCDIR	= ../include
BINDIR	= ../bin
LIBDIR	= ../lib
OBJDIR	= ../obj
DEFINES	= -I$(INCDIR)
#LIBS	= -lreadline -lasmjit
LIBS	= -lasmjit
DEPENDS	= $(patsubst %,$(INCDIR)/%,$(HFILES))
OBJECTS = $(patsubst %,$(OBJDIR)/%,$(OFILES))

all:
	make ../bin/madc
clean:
	rm $(OBJECTS)

../bin/madc: $(OBJECTS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(DEFINES) $(LIBS)

$(OBJDIR)/%.o: %.cpp $(DEPENDS)
	$(CXX) -c -o $@ $(CXXFLAGS) $(DEFINES) $<
