#ifndef __MADC_SYS_INCLUDES_H
#define __MADC_SYS_INCLUDES_H 1

// The generated host include-search tables — src/sys_include_paths.cpp, written
// by scripts/gen_sys_includes.sh from the configured compiler at build time.
//
// There is one table PER C++ STANDARD LIBRARY FLAVOR, because `-stdlib=` selects
// a whole search list rather than a prefix. `clang -stdlib=libc++` does not put
// libstdc++'s directories on the path at ALL, and putting libc++ merely FIRST is
// not equivalent: libc++'s <cstdlib> reaches the C library through
// `#include_next <stdlib.h>`, which with the GNU dirs still present walks into
// /usr/include/c++/NN/stdlib.h and dies on its `using std::abort;`.
//
// Which flavors exist is a property of the BUILD HOST — whatever the generator
// could probe — never a hardcoded list. The default is the flavor $(CXX) itself
// uses; an alternate is recorded when some probe reports a different one. The
// names are the libraries' own (read from _LIBCPP_VERSION / __GLIBCXX__), which
// is also clang's `-stdlib=` spelling.
struct madc_stdlib_flavor
{
	const char *name;			// "libstdc++" / "libc++" — the -stdlib= spelling
	const char *const *paths;		// NULL-terminated <...> search list
	const char *compiler_owned_dir;		// this flavor's compiler resource dir ("" if unknown)
};

// NULL-name terminated. Entry 0 is always the default flavor, even when its path
// list is empty because no compiler was available at build time.
extern const madc_stdlib_flavor madc_stdlib_flavors[];

// Name of the flavor $(CXX) uses — the one selected when -stdlib= is absent.
// Empty when detection failed.
extern const char *madc_default_stdlib_flavor;

#endif
