// madc_sema.h — Semantic pre-pass result types
//
// SemaInfo holds the type tables populated by madc_sema_collect().
// The C emitter queries these tables instead of doing inline tracking.

#ifndef __MADC_SEMA_H
#define __MADC_SEMA_H 1

#include <string>
#include <map>
#include <set>

// Type classification enum — used for cout format selection and
// general type-category dispatch.  Integer comparisons, not strings.
enum TypeClass {
    TC_INT    = 0,   // int, long, short, bool, enum values
    TC_CHAR   = 1,   // char (single byte, prints as character)
    TC_STRING = 2,   // char*, const char*, string (prints as string)
    TC_DOUBLE = 3,   // double, float (prints as %g)
    TC_VOID   = 4,   // void
    TC_CLASS  = 5,   // class instance (user-defined or built-in like std::string)
    TC_PTR    = 6,   // generic pointer (not char*)
};

// Per-class type information
struct SemaClassInfo {
    std::string name;
    std::set<std::string> fields;                             // field names
    std::map<std::string, TypeClass> field_types;             // field → type
    std::set<std::string> methods;                            // method names
    std::map<std::string, TypeClass> method_ret_types;        // method → return type
    bool has_ctor;
    bool has_dtor;

    SemaClassInfo() : has_ctor(false), has_dtor(false) {}
};

// Complete semantic analysis result
struct SemaInfo {
    // Variable types: name → type classification
    std::map<std::string, TypeClass> var_types;

    // Function return types: name → type class
    std::map<std::string, TypeClass> func_ret_types;

    // Function return type strings (for emit_type-level use)
    std::map<std::string, std::string> func_type_strs;

    // Variable → class name mapping (for method dispatch)
    std::map<std::string, std::string> var_class_map;

    // Known class names
    std::set<std::string> class_names;

    // Per-class detailed info
    std::map<std::string, SemaClassInfo> class_info;

    // Class inheritance: derived → base
    std::map<std::string, std::string> class_bases;

    // Classes that define constructors/destructors
    std::set<std::string> classes_with_ctor;
    std::set<std::string> classes_with_dtor;

    // Known struct names
    std::set<std::string> struct_names;

    // Known typedef names (for type vs identifier disambiguation)
    std::set<std::string> typedef_names;

    // Query helpers

    TypeClass get_var_type(const std::string &name) const {
	auto it = var_types.find(name);
	return (it != var_types.end()) ? it->second : TC_INT;
    }

    TypeClass get_func_ret_type(const std::string &name) const {
	auto it = func_ret_types.find(name);
	return (it != func_ret_types.end()) ? it->second : TC_INT;
    }

    bool is_class(const std::string &name) const {
	return class_names.count(name) > 0;
    }

    const SemaClassInfo *get_class(const std::string &name) const {
	auto it = class_info.find(name);
	return (it != class_info.end()) ? &it->second : nullptr;
    }

    std::string get_var_class(const std::string &name) const {
	auto it = var_class_map.find(name);
	return (it != var_class_map.end()) ? it->second : "";
    }

    bool is_typedef(const std::string &name) const {
	return typedef_names.count(name) > 0;
    }
};

// Public API
struct gp_tree_node;
SemaInfo *madc_sema_collect(gp_tree_node *root);
void madc_sema_free(SemaInfo *info);

#endif // __MADC_SEMA_H
