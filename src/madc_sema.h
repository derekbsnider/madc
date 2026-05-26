// madc_sema.h — Semantic pre-pass result types
//
// SemaInfo holds the type tables populated by madc_sema_collect().
// The C emitter queries these tables instead of doing inline tracking.

#ifndef __MADC_SEMA_H
#define __MADC_SEMA_H 1

#include <string>
#include <map>
#include <set>

// Per-class type information
struct SemaClassInfo {
    std::string name;
    std::set<std::string> fields;                         // field names
    std::map<std::string, char> field_types;              // field → type char
    std::set<std::string> methods;                        // method names
    std::map<std::string, char> method_ret_types;         // method → return type char
    bool has_ctor;
    bool has_dtor;

    SemaClassInfo() : has_ctor(false), has_dtor(false) {}
};

// Complete semantic analysis result
struct SemaInfo {
    // Variable types: name → type classification char
    //   'c' = char, 's' = string/char*, 'd' = double/float, 'i' = int (default)
    std::map<std::string, char> var_types;

    // Function return types: name → type char
    std::map<std::string, char> func_ret_types;

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

    // Get variable type char, or default 'i'
    char get_var_type(const std::string &name) const {
	auto it = var_types.find(name);
	return (it != var_types.end()) ? it->second : 'i';
    }

    // Get function return type char, or default 'i'
    char get_func_ret_type(const std::string &name) const {
	auto it = func_ret_types.find(name);
	return (it != func_ret_types.end()) ? it->second : 'i';
    }

    // Check if name is a known class
    bool is_class(const std::string &name) const {
	return class_names.count(name) > 0;
    }

    // Get class info, or nullptr
    const SemaClassInfo *get_class(const std::string &name) const {
	auto it = class_info.find(name);
	return (it != class_info.end()) ? &it->second : nullptr;
    }

    // Get the class name for a variable, or ""
    std::string get_var_class(const std::string &name) const {
	auto it = var_class_map.find(name);
	return (it != var_class_map.end()) ? it->second : "";
    }

    // Check if name is a known typedef
    bool is_typedef(const std::string &name) const {
	return typedef_names.count(name) > 0;
    }
};

// Public API
struct gp_tree_node;
SemaInfo *madc_sema_collect(gp_tree_node *root);
void madc_sema_free(SemaInfo *info);

#endif // __MADC_SEMA_H
