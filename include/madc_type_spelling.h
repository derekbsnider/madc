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
// namespaces), and only reads them, so it may run inside an entry
// transaction. A NULL Program spells as C with no aliases.
//
// Thread contract: as the Program it reads (one thread, between entries).

#include <string>

class Program;
class DataDef;
class DataDefCLASS;
class FuncDef;

class TypeSpeller
{
public:
    explicit TypeSpeller(const Program *pgm) : pgm(pgm) {}
    // A value's type, as D10's show writes it before the value.
    std::string shown(DataDef *dd) const;
    // A class: the source's name for an instantiation (`std::string`),
    // else its canonical spelling, else `struct X` / `union U`.
    std::string class_word(DataDefCLASS *cls) const;
    // The source's own name for a type, found by identity in the datatype
    // maps; empty when nothing names it.
    std::string alias(DataDef *dd) const;
    // An inline namespace is not part of the name anybody writes.
    std::string strip_inline_namespaces(const std::string &spelling) const;
    // A scalar's word, from its DataType (var_dump's canonical word).
    static std::string scalar_word(DataDef *dd);
private:
    std::string parameter_list(FuncDef *fd) const;
    const Program *pgm;
};

#endif
