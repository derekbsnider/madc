//////////////////////////////////////////////////////////////////////////
//									//
// madc file-stream runtime wrappers					//
//									//
//////////////////////////////////////////////////////////////////////////
//
// C++-linkage wrappers for the file-stream methods (open/close/eof/good/
// is_open). The parser registers these as builtins via
//   addFunction((fVOIDFUNC)ifstream_open, ...)
// and declares them with C++ linkage and a void* filename argument
//   extern void ifstream_open(void *, void *);
// (the void* points at a std::string).
//
// These were previously defined in compiler.cpp, which was removed when
// the asmjit JIT codegen backend was dropped. The CIR → c2mir → MIR
// backend resolves stream methods through the extern "C" wrappers in
// madc_mir_backend.cpp (which take const char *) via dlsym; those have
// distinct C linkage, so they live in a separate translation unit to
// avoid same-name C/C++ redefinition conflicts.
//
// std::ios is a virtual base class, so each concrete stream type needs
// its own typed wrapper — casting a void* straight to std::ios* would
// use the wrong pointer offset.

#include <cstdint>
#include <fstream>
#include <string>

void ifstream_open(void *ptr, void *filename)
	{ ((std::ifstream *)ptr)->open(((std::string *)filename)->c_str()); }
void ifstream_close(void *ptr)
	{ ((std::ifstream *)ptr)->close(); }
void ofstream_open(void *ptr, void *filename)
	{ ((std::ofstream *)ptr)->open(((std::string *)filename)->c_str()); }
void ofstream_close(void *ptr)
	{ ((std::ofstream *)ptr)->close(); }
void fstream_open(void *ptr, void *filename)
	{ ((std::fstream *)ptr)->open(((std::string *)filename)->c_str()); }
void fstream_close(void *ptr)
	{ ((std::fstream *)ptr)->close(); }

int64_t ifstream_eof(void *ptr)     { return ((std::ifstream *)ptr)->eof() ? 1 : 0; }
int64_t ifstream_good(void *ptr)    { return ((std::ifstream *)ptr)->good() ? 1 : 0; }
int64_t ifstream_is_open(void *ptr) { return ((std::ifstream *)ptr)->is_open() ? 1 : 0; }
int64_t ofstream_good(void *ptr)    { return ((std::ofstream *)ptr)->good() ? 1 : 0; }
int64_t ofstream_is_open(void *ptr) { return ((std::ofstream *)ptr)->is_open() ? 1 : 0; }
int64_t fstream_eof(void *ptr)      { return ((std::fstream *)ptr)->eof() ? 1 : 0; }
int64_t fstream_good(void *ptr)     { return ((std::fstream *)ptr)->good() ? 1 : 0; }
int64_t fstream_is_open(void *ptr)  { return ((std::fstream *)ptr)->is_open() ? 1 : 0; }
