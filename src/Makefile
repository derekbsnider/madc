C_FLAGS = -ggdb -std=c++11
#C_FLAGS = -O3 -std=c++14 -stdlib=libc++ -Wall -Wno-nested-anon-types -Wno-gnu-anonymous-struct -Wunused

all:
	make test_parser

test_parser: lexer.o parser.o compiler.o test_parser.o
	g++ lexer.o parser.o compiler.o test_parser.o -o test_parser -lreadline -lasmjit

.cpp.o:
	g++ -c $(C_FLAGS) $<
