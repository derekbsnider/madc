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
    std::set<std::string> operators;                          // overloaded operator symbols (==, !=, <, etc.)
    bool has_ctor;
    bool has_dtor;

    // Virtual dispatch support
    std::vector<std::string> virtual_methods;                 // ordered vtable slot list
    std::map<std::string, std::string> virtual_ret_types;     // method → C return type string
    std::map<std::string, std::string> virtual_param_types;   // method → C param type string (excl. __this)

    bool has_virtuals() const { return !virtual_methods.empty(); }

    // Find vtable slot index for a method (-1 if not virtual)
    int vtable_slot(const std::string &method) const {
        for (size_t i = 0; i < virtual_methods.size(); i++)
            if (virtual_methods[i] == method) return (int)i;
        return -1;
    }

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

    // Per-struct field type info (struct_name → field_name → TypeClass)
    std::map<std::string, std::map<std::string, TypeClass>> struct_field_types;

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

    // Look up a struct field's type across all known structs.
    // Returns TC_INT if not found.
    TypeClass get_struct_field_type(const std::string &field) const {
	for (auto &kv : struct_field_types) {
	    auto fit = kv.second.find(field);
	    if (fit != kv.second.end()) return fit->second;
	}
	return TC_INT;
    }

    // Look up a field type in a specific struct
    TypeClass get_struct_field_type(const std::string &sname,
				   const std::string &field) const {
	auto sit = struct_field_types.find(sname);
	if (sit != struct_field_types.end()) {
	    auto fit = sit->second.find(field);
	    if (fit != sit->second.end()) return fit->second;
	}
	return TC_INT;
    }

    // Find the root class that declares the vtable for a given class
    // (walks inheritance chain up to find where virtual methods originate)
    std::string vtable_class(const std::string &name) const {
	std::string cur = name;
	std::string result;
	while (!cur.empty()) {
	    const SemaClassInfo *ci = get_class(cur);
	    if (ci && ci->has_virtuals())
		result = cur;
	    auto bit = class_bases.find(cur);
	    if (bit != class_bases.end())
		cur = bit->second;
	    else
		break;
	}
	return result;
    }

    // Check if a method is virtual for a class (including inherited)
    bool is_virtual_method(const std::string &class_name,
			   const std::string &method) const {
	std::string cur = class_name;
	while (!cur.empty()) {
	    const SemaClassInfo *ci = get_class(cur);
	    if (ci && ci->vtable_slot(method) >= 0) return true;
	    auto bit = class_bases.find(cur);
	    if (bit != class_bases.end())
		cur = bit->second;
	    else
		break;
	}
	return false;
    }
};

// Public API
struct gp_tree_node;
SemaInfo *madc_sema_collect(gp_tree_node *root);
void madc_sema_free(SemaInfo *info);

#endif // __MADC_SEMA_H
