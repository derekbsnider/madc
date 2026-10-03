#ifndef __MADC_TYPE_SPELLING_H
#define __MADC_TYPE_SPELLING_H 1
// madc_type_spelling.h — the source's spelling of a type (plan §41.8a).
//
// A type spelled the way the entry's language writes it: `int *` and
// `struct P` in C, `P *`, `std::string` and `std::list<int>` in C++, a
// function pointer as `int (*)(int)`. D10's show bakes it into `(TYPE)
// value`, var_dump names a class by it, and the session's `%type` prints
// it. The one owner of that spelling: CirBuilder's show and var_dump words
// forward here.
//
// It reads the Program's type registries (a class's alias, the inline
// namespaces, a template's defaults), and only reads them, so it may run
// inside an entry transaction; a lookup may thaw a restored template or
// intern a pointer type, the registries' own caches, never a registration.
// A NULL Program spells as C with no aliases.
//
// Thread contract: as the Program it reads (one thread, between entries).

#include <string>

class Program;
class DataDef;
class DataDefSTRUCT;
class DataDefCLASS;
class FuncDef;
class Method;

class TypeSpeller
{
public:
    explicit TypeSpeller(Program *pgm) : pgm(pgm) {}
    // A value's type, as D10's show writes it before the value.
    std::string shown(DataDef *dd) const;
    // A class (or an aggregate a template instantiated): the source's name
    // for an instantiation (`std::string`), else its template-id as g++ and
    // clang++ write it (`std::vector<int>`), else its canonical spelling,
    // else `struct X` / `union U`.
    std::string class_word(DataDefSTRUCT *cls) const;
    // The name an aggregate's word carries, without C's tag: an aggregate a
    // template instantiated by its template-id (`Box<int>`), any other by
    // its tag's name. The show, var_dump and print_r name one with it.
    std::string aggregate_name(DataDefSTRUCT *s) const;
    // The source's own name for a type, found by identity in the datatype
    // maps; empty when nothing names it.
    std::string alias(DataDef *dd) const;
    // An inline namespace is not part of the name anybody writes.
    std::string strip_inline_namespaces(const std::string &spelling) const;
    // A scalar's word, from its DataType (var_dump's canonical word).
    static std::string scalar_word(DataDef *dd);
    // A declaration: the type with `name` where C writes its declarator-id
    // (`int a[3]`, `int (*fp)(int)`); an empty name spells the type alone.
    std::string declared(DataDef *dd, const std::string &name) const;
    // A function's signature, as `?name` prints it (plan §41.8a):
    // `int sq(int n)`, the parameter names from its Method when given, the
    // receiver and varargs slots madc adds left out.
    std::string signature(const std::string &name, FuncDef *fd,
			  const Method *m) const;
private:
    std::string parameter_list(FuncDef *fd, const Method *m) const;
    std::string template_word(const std::string &canon) const;
    std::string argument_word(const std::string &arg) const;
    Program *pgm;
};

#endif
