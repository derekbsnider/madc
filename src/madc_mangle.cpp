// madc_mangle.cpp — Itanium C++ ABI name mangler.
//
// Produces mangled symbol names following the Itanium ABI spec:
// https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling
//
// Covers: builtin types, pointers, references, const, classes,
// nested names, constructors (C1), destructors (D1).

#include "madc_mangle.h"
#include "spelling_delim.h"
#include <cstring>
#include <typeinfo>
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "datadef.h"

// Length-prefixed source name: "Foo" → "3Foo"
static std::string source_name(const std::string &name)
{
	return std::to_string(name.size()) + name;
}

// Strip leading/trailing whitespace
static std::string strip(const std::string &s)
{
	size_t start = s.find_first_not_of(" \t");
	if (start == std::string::npos) return "";
	size_t end = s.find_last_not_of(" \t");
	return s.substr(start, end - start + 1);
}

// Check if a string ends with a suffix
static bool ends_with(const std::string &s, const std::string &suffix)
{
	if (suffix.size() > s.size()) return false;
	return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Try to match a builtin type name, return its Itanium code or empty string
// A NON-TYPE template argument is an expression, not a type: Itanium encodes it
// `L <type> <value> E`. parse_component sends every template argument through
// parse_type, so `moneypunct<char,false>` mangled the VALUE `false` as if it
// were a type name — `_ZNSt10moneypunctIc5falseE2idE` where libstdc++ exports
// `_ZNSt10moneypunctIcLb0EE2idE`. A wrong symbol links against nothing, which
// is the whole failure mode Rule #1 exists to prevent.
//
// Only literals whose TYPE is unambiguous from their spelling are encoded here.
// `true`/`false` are bool by definition, so `Lb1E`/`Lb0E` are exact.
//
// An INTEGER literal deliberately falls through: Itanium takes the type from
// the template PARAMETER's declaration, not the argument, so the same spelling
// `8` is `Li8E` for `int`, `Lm8E` for `size_t` (std::array, std::bitset),
// `Lj8E` for `unsigned`. The mangler is handed only a spelling and cannot tell
// them apart — guessing would trade a visibly-wrong symbol for an
// invisibly-wrong one. Fixing it means carrying the parameter's declared type
// into the spelling the caller passes; see the KG gap.
static std::string nontype_literal_code(const std::string &t)
{
	if (t == "false") return "Lb0E";
	if (t == "true")  return "Lb1E";
	return std::string();
}

static std::string builtin_code(const std::string &t)
{
	if (t == "void")                return "v";
	if (t == "bool")                return "b";
	if (t == "char")                return "c";
	if (t == "signed char")         return "a";
	if (t == "unsigned char")       return "h";
	if (t == "short")               return "s";
	if (t == "short int")           return "s";
	if (t == "unsigned short")      return "t";
	if (t == "unsigned short int")  return "t";
	if (t == "int")                 return "i";
	if (t == "unsigned int")        return "j";
	if (t == "unsigned")            return "j";
	if (t == "long")                return "l";
	if (t == "long int")            return "l";
	if (t == "unsigned long")       return "m";
	if (t == "unsigned long int")   return "m";
	if (t == "long long")           return "x";
	if (t == "long long int")       return "x";
	if (t == "unsigned long long")  return "y";
	if (t == "float")               return "f";
	if (t == "double")              return "d";
	if (t == "long double")         return "e";
	if (t == "wchar_t")             return "w";
	if (t == "...")                 return "z";   // trailing ellipsis

	// Fixed-width / width-carrying aliases. The 64-bit rows follow the
	// target data model (datadef.h): LP64 spells them through `long`
	// (l/m), LLP64 through `long long` (x/y) — x86_64-w64-mingw32-g++
	// mangles f(size_t) as _Z1fy where Linux g++ says _Z1fm. The letter
	// is the TYPE the typedef resolves to on the target, so it moves
	// with the model; the plain long/long long rows above never do.
	if (t == "int8_t")              return "a";
	if (t == "uint8_t")             return "h";
	if (t == "int16_t")             return "s";
	if (t == "uint16_t")            return "t";
	if (t == "int32_t")             return "i";
	if (t == "uint32_t")            return "j";
	if (t == "int64_t")             return target_llp64() ? "x" : "l";
	if (t == "uint64_t")            return target_llp64() ? "y" : "m";
	if (t == "size_t")              return target_llp64() ? "y" : "m";
	if (t == "std::size_t")         return target_llp64() ? "y" : "m";
	if (t == "ssize_t")             return target_llp64() ? "x" : "l";
	if (t == "ptrdiff_t")           return target_llp64() ? "x" : "l";
	if (t == "std::ptrdiff_t")      return target_llp64() ? "x" : "l";

	return "";
}

std::string itanium_encode_type(const std::string &raw_type)
{
	std::string t = strip(raw_type);
	if (t.empty()) return "v";

	// Pointer: strip trailing * and recurse
	if (ends_with(t, "*")) {
		std::string inner = strip(t.substr(0, t.size() - 1));
		return "P" + itanium_encode_type(inner);
	}

	// Reference: strip trailing & and recurse
	if (ends_with(t, "&")) {
		std::string inner = strip(t.substr(0, t.size() - 1));
		return "R" + itanium_encode_type(inner);
	}

	// Const: strip leading "const " and wrap with K
	if (t.substr(0, 6) == "const ") {
		std::string inner = strip(t.substr(6));
		return "K" + itanium_encode_type(inner);
	}

	// Builtin type
	std::string code = builtin_code(t);
	if (!code.empty()) return code;

	// User-defined type: length-prefixed name
	return source_name(t);
}

std::string itanium_encode_params(const std::vector<std::string> &param_types)
{
	if (param_types.empty()) return "v";
	std::string result;
	for (const auto &pt : param_types)
		result += itanium_encode_type(pt);
	return result;
}

std::string itanium_mangle(const std::string &func_name,
                            const std::vector<std::string> &param_types)
{
	return "_Z" + source_name(func_name) + itanium_encode_params(param_types);
}

std::string itanium_mangle_method(const std::string &class_name,
                                   const std::string &method_name,
                                   const std::vector<std::string> &param_types)
{
	return "_ZN" + source_name(class_name) + source_name(method_name)
	       + "E" + itanium_encode_params(param_types);
}

std::string itanium_mangle_ctor(const std::string &class_name,
                                 const std::vector<std::string> &param_types)
{
	return "_ZN" + source_name(class_name) + "C1"
	       + "E" + itanium_encode_params(param_types);
}

std::string itanium_mangle_dtor(const std::string &class_name)
{
	return "_ZN" + source_name(class_name) + "D1Ev";
}

static std::string operator_code(const std::string &op)
{
	// madc dialect operators — Itanium vendor-extended operator-name
	// encoding `v <arity> <source-name>` (no standard code exists).
	if (op == "===") return "v23eq3";
	if (op == "!==") return "v23ne3";
	if (op == "==") return "eq";
	if (op == "!=") return "ne";
	if (op == "<")  return "lt";
	if (op == ">")  return "gt";
	if (op == "<=") return "le";
	if (op == ">=") return "ge";
	if (op == "+")  return "pl";
	if (op == "-")  return "mi";
	if (op == "*")  return "ml";
	if (op == "/")  return "dv";
	if (op == "%")  return "rm";
	if (op == "=")  return "aS";
	if (op == "+=") return "pL";
	if (op == "-=") return "mI";
	if (op == "*=") return "mL";
	if (op == "/=") return "dV";
	if (op == "<<") return "ls";
	if (op == ">>") return "rs";
	if (op == "[]") return "ix";
	if (op == "()") return "cl";
	if (op == "++") return "pp";
	if (op == "--") return "mm";
	if (op == "!")  return "nt";
	if (op == "~")  return "co";
	if (op == "&")  return "an";
	if (op == "|")  return "or";
	if (op == "^")  return "eo";
	if (op == "new")      return "nw";
	if (op == "new[]")    return "na";
	if (op == "delete")   return "dl";
	if (op == "delete[]") return "da";
	return "";
}

std::string itanium_mangle_operator(const std::string &class_name,
                                     const std::string &op,
                                     const std::vector<std::string> &param_types)
{
	std::string code = operator_code(op);
	if (code.empty()) return "";
	return "_ZN" + source_name(class_name) + code
	       + "E" + itanium_encode_params(param_types);
}

std::string itanium_mangle_nested(const std::vector<std::string> &qualifiers,
                                   const std::string &name,
                                   const std::vector<std::string> &param_types)
{
	std::string result = "_ZN";
	for (const auto &q : qualifiers)
		result += source_name(q);
	result += source_name(name);
	result += "E" + itanium_encode_params(param_types);
	return result;
}

// RTTI symbols for an un-namespaced user class (S5b.1).
std::string itanium_typeinfo_sym(const std::string &class_name)
{
	return "_ZTI" + source_name(class_name);
}

// Real Itanium vtable / typeinfo symbols for a class named by its canonical C++
// spelling (e.g. "std::bad_alloc"), using the full St-aware type encoding. These
// are the symbols libstdc++ actually exports — used when madc DEFERS an
// externally-defined class to the library (it references the real _ZTVSt.../
// _ZTISt... instead of synthesizing its own). For an un-namespaced name the
// encoding equals source_name, so itanium_typeinfo_sym_cpp("C") == _ZTI1C.
//   "std::bad_alloc" -> _ZTVSt9bad_alloc / _ZTISt9bad_alloc
std::string itanium_vtable_sym_cpp(const std::string &cpp_spelling)
{
	return "_ZTV" + itanium_encode_type_sub(cpp_spelling);
}

std::string itanium_typeinfo_sym_cpp(const std::string &cpp_spelling)
{
	return "_ZTI" + itanium_encode_type_sub(cpp_spelling);
}

std::string itanium_typeinfo_name_sym(const std::string &class_name)
{
	return "_ZTS" + source_name(class_name);
}

std::string itanium_typeinfo_name_string(const std::string &class_name)
{
	return source_name(class_name);
}

// ===========================================================================
// Substitution-aware (template-id capable) mangling.
//
// Implements the real Itanium substitution algorithm: an ordered candidate
// table is maintained as a name/type is mangled. The first candidate is
// referenced as "S_", the second "S0_", the third "S1_", … (i.e. "S" then
// base-36 of (index-1) then "_", with index 0 written as bare "S_").
//
// The standard abbreviations (St=std, Sa=std::allocator, Sb=std::basic_string,
// Ss=std::string [pre-cxx11 only], Si/So/Sd=basic_istream/ostream/iostream<char>)
// are substitution *shorthands*: they may be referenced like a back-ref but do
// NOT consume a numbered slot. They only match the exact std-direct name.
// ===========================================================================

namespace {

// ---- type / name AST -------------------------------------------------------

struct TypeNode;

// A qualified-name component: e.g. "char_traits" possibly with template args.
struct NameComponent {
	std::string ident;                 // e.g. "char_traits", "__cxx11", "std"
	bool is_template = false;          // has <...>
	std::vector<TypeNode> targs;       // template arguments (if is_template)
};

// A parsed type: a chain of name components (a::b::c<...>) plus pointer/ref/
// const decorations applied outermost, OR a builtin.
struct TypeNode {
	// Outer decorations, applied innermost-last. We model as flags + the
	// inner core. To keep it simple we store decoration codes in order from
	// outermost to innermost: each is "P", "R", or "K".
	std::vector<std::string> decos;    // e.g. {"R","K"} for "const X&"
	bool is_builtin = false;
	std::string builtin;               // Itanium code if builtin (e.g. "c")
	int tparam = -1;                   // >=0 if this type is template-param #N ($Tn)
	bool is_funcptr = false;           // pointer-to-function "R (*)(P,…)" → PF…E
	std::vector<TypeNode> fp_ret;      // [0] = return type (if is_funcptr)
	std::vector<TypeNode> fp_params;   // parameter types (if is_funcptr)
	std::vector<NameComponent> name;   // qualified name chain (if !is_builtin)
};

// ---- parsing ---------------------------------------------------------------

std::string mstrip(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t");
	if (a == std::string::npos) return "";
	size_t b = s.find_last_not_of(" \t");
	return s.substr(a, b - a + 1);
}

// Split a comma-separated template-arg list at top level. The depth rule is
// the shared one (include/spelling_delim.h) — the local copy tracked only
// `<`/`>`, so a `<` inside `(...)` or after a non-name character (`operator<`)
// counted as a template open.
std::vector<std::string> split_targs(const std::string &s)
{
	return split_template_args_spelling(s);
}

// Split a qualified name "a::b::c<...>" into component strings at top-level
// "::" (respecting <>). Each component may still carry its own <...>.
std::vector<std::string> split_scope(const std::string &s)
{
	return split_scope_spelling(s);
}

TypeNode parse_type(const std::string &raw);

// Parse one name component "char_traits<char>" → ident + template args.
NameComponent parse_component(const std::string &raw)
{
	NameComponent nc;
	std::vector<std::string> targs;
	// A component arrives already split at top-level "::", so its template-id
	// is the only one — first and last coincide. Ignore is the historical
	// policy for anything trailing the closing '>'; the local copy expressed
	// it by taking rfind('>') as the close.
	if (!split_template_id_parts(mstrip(raw), nc.ident, targs,
				     SpellingTail::Ignore))
		return nc;
	nc.is_template = true;
	for (const auto &a : targs)
		nc.targs.push_back(parse_type(a));
	return nc;
}

TypeNode parse_type(const std::string &raw)
{
	TypeNode t;
	std::string s = mstrip(raw);

	// Peel outer decorations. Trailing * / & first (outermost), then leading
	// "const". We recurse by re-parsing the inner string, then prepend deco.
	// Repeat until no decoration remains.
	for (;;) {
		s = mstrip(s);
		if (!s.empty() && s.back() == '*') {
			t.decos.push_back("P");
			s = s.substr(0, s.size() - 1);
			continue;
		}
		// rvalue reference "&&" → O (must be tested before single &)
		if (s.size() >= 2 && s.compare(s.size() - 2, 2, "&&") == 0) {
			t.decos.push_back("O");
			s = s.substr(0, s.size() - 2);
			continue;
		}
		if (!s.empty() && s.back() == '&') {
			t.decos.push_back("R");
			s = s.substr(0, s.size() - 1);
			continue;
		}
		if (s.size() >= 6 && s.compare(0, 6, "const ") == 0) {
			t.decos.push_back("K");
			s = s.substr(6);
			continue;
		}
		// trailing " const" form
		if (s.size() >= 6 && s.compare(s.size() - 6, 6, " const") == 0) {
			t.decos.push_back("K");
			s = s.substr(0, s.size() - 6);
			continue;
		}
		break;
	}

	// Template-param placeholder: "$T0" → template-param #0, etc.
	if (s.size() >= 3 && s[0] == '$' && s[1] == 'T') {
		t.tparam = std::stoi(s.substr(2));
		return t;
	}

	// Function-pointer type "<ret> (*)(<params>)" → PF<ret><params>E.
	size_t fp = s.find("(*)(");
	if (fp != std::string::npos) {
		t.is_funcptr = true;
		t.decos.push_back("P");                 // pointer to the function type
		t.fp_ret.push_back(parse_type(mstrip(s.substr(0, fp))));
		size_t pend = s.rfind(')');
		std::string ps = s.substr(fp + 4, pend - (fp + 4));
		for (const auto &a : split_targs(ps))
			if (!a.empty() && a != "void") t.fp_params.push_back(parse_type(a));
		return t;
	}

	// A non-type argument rides the builtin channel: encode_core emits
	// `builtin` verbatim and, unlike a class type, registers NO substitution
	// candidate — which is exactly right, since an expression literal is not
	// a substitutable <type> under the ABI.
	std::string lit = nontype_literal_code(s);
	if (!lit.empty()) {
		t.is_builtin = true;
		t.builtin = lit;
		return t;
	}

	std::string code = builtin_code(s);
	if (!code.empty()) {
		t.is_builtin = true;
		t.builtin = code;
		return t;
	}

	for (const auto &comp : split_scope(s))
		t.name.push_back(parse_component(comp));
	return t;
}

TypeNode parse_param_type(const std::string &raw)
{
	TypeNode t = parse_type(raw);
	// Top-level cv-qualification is not part of a function parameter type in
	// the Itanium ABI (`const size_t` by value encodes as `m`). Keep pointee
	// const (`const char*` -> `PKc`) and reference-to-const (`const T&` -> `RK...`).
	if (t.decos.size() == 1 && t.decos[0] == "K")
		t.decos.clear();
	return t;
}

// ---- stdlib flavor state ----------------------------------------------------
// Declared HERE, above the encoder, because the SCOPE of a std:: entity depends
// on the active flavor: under libstdc++ `std::terminate` really is in `std`,
// while under libc++ it is in the inline namespace `std::__1`, and a mangled
// name that gets that scope wrong does not resolve. The setters and the
// source-spelling helpers stay below, next to the rest of the flavor surface.
// The "__cxx11" spelling and the LLVM namespace policy live in this file and
// only in this file — the mangler is the one permitted home for std:: symbol
// knowledge. The LLVM namespace is never a literal: it arrives from the parsed
// _LIBCPP_ABI_NAMESPACE via madc_mangle_set_stdlib_llvm().

static MangleStdlib g_std_stdlib = mstdlibGnu;
static std::string g_std_abi_ns = "__cxx11";	// build default: GNU cxx11 ABI
static unsigned g_std_abi_gen = 0;		// bumped on change; caches key on it

// The inline namespace a std:: ENTITY lives in for the active flavor, or empty
// when it lives directly in `std`. GNU's `__cxx11` is an ABI TAG on selected
// components (basic_string et al), not a scope every name sits in — so only the
// LLVM flavor answers non-empty here. Keeping that distinction in one predicate
// is what stops `__cxx11` from leaking into unrelated GNU symbols.
static const std::string &std_entity_inline_ns()
{
	static const std::string none;
	return g_std_stdlib == mstdlibLlvm ? g_std_abi_ns : none;
}

// ---- the encoder -----------------------------------------------------------
//
// One ordered candidate table per top-level symbol. Each candidate is keyed by
// a CANONICAL string (independent of substitution state) so the same entity
// always matches regardless of where it appears. The slot index alone decides
// the back-ref spelling (S_, S0_, …). The encoder follows the Itanium rule:
// a candidate is added for every prefix level of a name, every template-id,
// and every (non-builtin) type built from decorations — in completion order.

class ItaniumMangler {
public:
	// Encode a <type>, registering substitution candidates as it goes.
	std::string encode_type(const TypeNode &t)
	{
		// Build innermost core first, then wrap decorations outward. Each
		// decorated layer is itself a substitution candidate (checked first).
		std::string canon = canon_type_core(t);
		std::string enc;
		if (t.tparam >= 0) {
			// A <template-param> is a substitution candidate: the first use
			// emits T_/T0_/… and registers it; later uses are back-refs.
			std::string ref = find_sub(canon);
			if (!ref.empty()) enc = ref;
			else { enc = tparam_ref(t.tparam); add_sub(canon); }
		} else if (t.is_funcptr) {
			// Function type F<ret><params>E (a substitution candidate). The
			// pointer "P" deco is applied by the loop below → PF…E.
			std::string ref = find_sub(canon);
			if (!ref.empty()) enc = ref;
			else {
				std::string r = encode_type(t.fp_ret[0]);
				std::string ps;
				if (t.fp_params.empty()) ps = "v";
				else for (const auto &p : t.fp_params) ps += encode_type(p);
				enc = "F" + r + ps + "E";
				add_sub(canon);
			}
		} else {
			enc = encode_core(t);
		}

		// Decorations are listed outermost→innermost; apply innermost first.
		// Track the canonical key alongside, so identical decorated types
		// (e.g. two "const string&" params) share one slot.
		for (auto it = t.decos.rbegin(); it != t.decos.rend(); ++it) {
			canon = *it + canon;          // e.g. "R" + "K" + "<string>"
			std::string ref = find_sub(canon);
			if (!ref.empty()) { enc = ref; continue; }
			enc = *it + enc;
			add_sub(canon);
		}
		return enc;
	}

	// Reset candidate table (each top-level symbol starts fresh).
	void reset() { keys_.clear(); }

	// Mangle a member / ctor / dtor / operator symbol on a (template-id) class.
	std::string mangle_member(const std::string &qualified_class,
	                          const std::string &unqualified,
	                          const std::string &special,    // C1 / D1 / op code
	                          const std::vector<std::string> &params,
	                          bool const_method)
	{
		reset();
		TypeNode cls = parse_type(qualified_class);

		// The class name is encoded as the enclosing prefix of an N..E symbol;
		// the outer N..E is supplied here, so the name itself is NOT wrapped.
		std::string clsenc = encode_name(cls.name, /*standalone=*/false);

		std::string out = "_ZN";
		if (const_method) out += "K";
		out += clsenc;
		out += special.empty() ? source_name(unqualified) : special;
		out += "E";

		if (params.empty()) {
			out += "v";
		} else {
			for (const auto &p : params)
				out += encode_type(parse_param_type(p));
		}
		return out;
	}

	std::string mangle_member_template(const std::string &qualified_class,
	                          const std::string &unqualified,
	                          const std::vector<std::string> &targs,
	                          const std::string &ret,
	                          const std::vector<std::string> &params,
	                          bool const_method)
	{
		reset();
		TypeNode cls = parse_type(qualified_class);
		std::string clsenc = encode_name(cls.name, /*standalone=*/false);
		add_sub("@member-template:" + qualified_class + "::" + unqualified);

		std::string out = "_ZN";
		if (const_method) out += "K";
		out += clsenc;
		out += source_name(unqualified);
		out += "I";
		for (const auto &a : targs)
			out += encode_type(parse_type(a));
		out += "E";
		out += "E";
		out += encode_type(parse_type(ret));
		for (const auto &p : params)
			out += encode_type(parse_param_type(p));
		return out;
	}

	// Mangle a non-member std:: function template:
	//   _ZSt <opOrName> I<targs>E <ret> <params...>   (one substitution table)
	// Function templates encode the return type. opOrName is already the
	// operator code (e.g. "ls") or a length-prefixed source name (e.g. "7getline").
	std::string mangle_std_free_template(const std::string &opOrName,
	        const std::vector<std::string> &targs,
	        const std::string &ret,
	        const std::vector<std::string> &params)
	{
		reset();
		// The SCOPE is encoded FIRST because under libc++ it is a nested-name
		// whose std::__1 prefix is substitution candidate #0 — which is what
		// shifts every later slot by one (the same operator+ reads S6_/S9_
		// under libc++ and S5_/S8_ under libstdc++).
		bool nested = false;
		std::string scope = std_entity_scope(nested);
		// The function-template NAME is the next substitution candidate (per the
		// Itanium ABI: "<template-prefix>" of a function template is substitutable;
		// e.g. the spec's `first<Duo>` example registers `first` as S_). It is
		// rarely back-referenced but it shifts every later slot by one.
		add_sub("@fn:" + opOrName);
		std::string out = (nested ? "_ZN" : "_Z") + scope + opOrName + "I";
		for (const auto &a : targs) out += encode_type(parse_type(a));
		out += "E";			// close the template-args
		if (nested) out += "E";		// close the <nested-name>
		out += encode_type(parse_type(ret));
		for (const auto &p : params) out += encode_type(parse_param_type(p));
		return out;
	}

	// A namespace-scope std:: VARIABLE (cout, cin, …). Same scope rule as the
	// functions above, so it shares the one owner rather than spelling the
	// prefix again: GNU _ZSt4cout, libc++ _ZNSt3__14coutE.
	std::string mangle_std_var(const std::string &name)
	{
		reset();
		bool nested = false;
		std::string scope = std_entity_scope(nested);
		std::string out = (nested ? "_ZN" : "_Z") + scope + source_name(name);
		if (nested) out += "E";
		return out;
	}

	std::string mangle_nested_function(const std::vector<std::string> &qualifiers,
	                                   const std::string &name,
	                                   const std::vector<std::string> &params)
	{
		reset();
		// An operator name encodes as its Itanium operator-name code in
		// EVERY scope — ::operator new(size_t) is _Znwm, std::operator<<
		// is _ZStls…, N::operator+ is _ZN1NplE…. Only the global branch
		// had this; the std and qualified branches fell to source_name,
		// producing invalid symbols like _ZSt10operator<<.
		std::string opcode;
		if (name.compare(0, 8, "operator") == 0)
			opcode = operator_code(name.substr(8));
		// GLOBAL-scope function: _Z<name><params> with no N..E nesting.
		if (qualifiers.empty()) {
			std::string out = "_Z" + (opcode.empty() ? source_name(name)
			                                         : opcode);
			if (params.empty())
				out += "v";
			else
				for (const auto &p : params)
					out += encode_type(parse_param_type(p));
			return out;
		}
		// A function whose DECLARED scope is exactly `std`. The qualifier
		// list here is PARSE-FAITHFUL (current_namespace() / FuncDef::
		// namespace_name carry the inline-namespace model), so a plain
		// {"std"} means the UNVERSIONED namespace in BOTH flavors — an
		// Itanium <unscoped-name> with the St abbreviation and NO
		// nested-name wrapper (_ZSt9terminatev). libc++ deliberately
		// declares its ABI-stable entry points there (<new>'s
		// __throw_bad_alloc, in a `namespace std // purposefully not
		// using versioning namespace` block, exports as
		// _ZSt17__throw_bad_allocv); widening it to std::__1 via
		// std_entity_scope minted an import libc++.so does not export.
		// An entity that really lives in the versioning namespace
		// arrives as {"std","__1"} and takes the chain path below.
		// std_entity_scope remains the rule ONLY for the flavor-agnostic
		// W2 spellings (mangle_std_var / mangle_std_free_template),
		// whose callers never tracked the inline namespace.
		if (qualifiers.size() == 1 && qualifiers[0] == "std") {
			std::string out = "_ZSt"
			                + (opcode.empty() ? source_name(name) : opcode);
			if (params.empty())
				out += "v";
			else
				for (const auto &p : params)
					out += encode_type(parse_param_type(p));
			return out;
		}
		std::vector<NameComponent> chain;
		for (const auto &q : qualifiers)
			chain.push_back(parse_component(q));
		std::string out = "_ZN";
		out += encode_name(chain, /*standalone=*/false);
		out += opcode.empty() ? source_name(name) : opcode;
		out += "E";
		if (params.empty())
			out += "v";
		else
			for (const auto &p : params)
				out += encode_type(parse_param_type(p));
		return out;
	}

	std::string mangle_nested_variable(const std::vector<std::string> &qualifiers,
	                                   const std::string &name)
	{
		reset();
		std::vector<NameComponent> chain;
		for (const auto &q : qualifiers)
			chain.push_back(parse_component(q));
		chain.push_back(parse_component(name));
		std::string out = "_Z";
		out += encode_name(chain, /*standalone=*/true);
		return out;
	}

private:
	std::vector<std::string> keys_;   // canonical keys, in candidate order

	// ---- substitution table -------------------------------------------------

	static std::string subref(size_t n)
	{
		if (n == 0) return "S_";
		size_t v = n - 1;
		std::string digits;
		const char *D = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
		if (v == 0) digits = "0";
		while (v > 0) { digits = std::string(1, D[v % 36]) + digits; v /= 36; }
		return "S" + digits + "_";
	}

	// Template-param reference: index 0 → "T_", 1 → "T0_", 2 → "T1_", …
	static std::string tparam_ref(int n)
	{
		if (n == 0) return "T_";
		int v = n - 1;
		std::string d;
		const char *D = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
		if (v == 0) d = "0";
		while (v > 0) { d = std::string(1, D[v % 36]) + d; v /= 36; }
		return "T" + d + "_";
	}

	std::string find_sub(const std::string &canonKey)
	{
		for (size_t i = 0; i < keys_.size(); ++i)
			if (keys_[i] == canonKey) return subref(i);
		return "";
	}
	void add_sub(const std::string &canonKey)
	{
		for (const auto &k : keys_) if (k == canonKey) return;
		keys_.push_back(canonKey);
	}

	// ---- canonical (substitution-independent) keys ---------------------------
	//
	// A stable spelling for an entity, used purely as a map key. The exact
	// characters don't matter as long as distinct entities differ and equal
	// entities match. We use the C++-ish scoped spelling.

	static std::string canon_name_prefix(const std::vector<NameComponent> &chain,
	                                      size_t upto)
	{
		std::string s;
		for (size_t i = 0; i <= upto; ++i) {
			if (i) s += "::";
			s += chain[i].ident;
			if (chain[i].is_template) {
				s += "<";
				for (size_t j = 0; j < chain[i].targs.size(); ++j) {
					if (j) s += ",";
					s += canon_type(chain[i].targs[j]);
				}
				s += ">";
			}
		}
		return s;
	}

	static std::string canon_type_core(const TypeNode &t)
	{
		if (t.tparam >= 0) return "$T" + std::to_string(t.tparam);
		if (t.is_funcptr) {
			std::string c = "F" + canon_type(t.fp_ret[0]);
			for (const auto &p : t.fp_params) c += canon_type(p);
			return c + "E";
		}
		if (t.is_builtin) return "#" + t.builtin;
		return canon_name_prefix(t.name, t.name.size() - 1);
	}

	static std::string canon_type(const TypeNode &t)
	{
		std::string c = canon_type_core(t);
		for (auto it = t.decos.rbegin(); it != t.decos.rend(); ++it)
			c = *it + c;
		return c;
	}

	// ---- standard std-direct abbreviations -----------------------------------
	// Only EXACT std-direct names qualify (e.g. std::__cxx11::basic_string does
	// NOT). Abbreviations are referenceable shorthands that consume no slot.

	static std::string std_abbrev(const std::string &scopedNoArgs)
	{
		if (scopedNoArgs == "std")                 return "St";
		if (scopedNoArgs == "std::allocator")      return "Sa";
		if (scopedNoArgs == "std::basic_string")   return "Sb";
		// Ss (pre-cxx11 std::string) intentionally NOT used for __cxx11.
		// NOTE: Si/So/Sd are NOT prefix abbreviations — they denote the COMPLETE
		// specialization only (see std_complete_abbrev). A basic_ostream with a
		// non-canonical arg (e.g. a template param) must spell out St13basic_ostream.
		return "";
	}

	// Complete-specialization std abbreviations (Ss/Si/So/Sd): the abbreviation
	// stands for the ENTIRE specialization (template name + its canonical
	// char/char_traits[/allocator] args), so it is emitted standalone with NO
	// I..E expansion and consumes no slot. (St/Sa/Sb are prefix/template
	// abbreviations that DO take further args — handled by std_abbrev.)
	// Ss is the pre-C++11 std::basic_string ONLY (NOT std::__cxx11::basic_string).
	static std::string std_complete_abbrev(const std::vector<NameComponent> &chain,
	                                        size_t i)
	{
		const NameComponent &nc = chain[i];
		if (!nc.is_template) return "";
		std::string scope = scoped_prefix_noargs(chain, i);
		const std::vector<TypeNode> &a = nc.targs;
		bool ct = a.size() >= 2
		          && canon_type(a[0]) == "#c"
		          && canon_type(a[1]) == "std::char_traits<#c>";
		if (scope == "std::basic_istream"  && a.size() == 2 && ct) return "Si";
		if (scope == "std::basic_ostream"  && a.size() == 2 && ct) return "So";
		if (scope == "std::basic_iostream" && a.size() == 2 && ct) return "Sd";
		if (scope == "std::basic_string"   && a.size() == 3 && ct
		    && canon_type(a[2]) == "std::allocator<#c>")           return "Ss";
		return "";
	}

	// The mangled scope a std:: ENTITY sits in under the active stdlib flavor,
	// and whether the name therefore needs the nested-name `N…E` wrapper.
	// Registers the substitution candidate the scope creates, so a caller must
	// invoke this BEFORE encoding anything else in the symbol.
	//
	// GNU:  `St` — the Itanium abbreviation for ::std::, an <unscoped-name>, so
	//       NO wrapper. libstdc++ exports _ZSt9terminatev, _ZStplIc…E…
	// LLVM: the entity really lives in the inline namespace std::__1, so the
	//       scope is a two-component <nested-name> `St3__1` and the wrapper IS
	//       required. libc++ exports _ZNSt3__1plIc…EE… — and that prefix is
	//       also substitution candidate S_, which is why libc++'s inner names
	//       read NS_11char_traitsIcEE and every later slot sits exactly one
	//       higher than the GNU symbol's (S6_/S9_ vs S5_/S8_ on operator+).
	std::string std_entity_scope(bool &needs_nesting)
	{
		const std::string &ins = std_entity_inline_ns();
		needs_nesting = !ins.empty();
		if (!needs_nesting)
			return "St";		// abbreviation: consumes no slot
		add_sub("std::" + ins);		// the key the type encoder looks up
		return "St" + source_name(ins);
	}

	// ---- name encoding -------------------------------------------------------
	//
	// Encodes a qualified-name chain, registering a substitution candidate for
	// each prefix level and template-id. Returns the mangled name WITHOUT the
	// enclosing N..E; the caller wraps when `standalone` and wrapping is needed.

	std::string encode_name(const std::vector<NameComponent> &chain,
	                        bool standalone)
	{
		std::string encoded;          // mangled prefix accumulated so far
		size_t srcNameCount = 0;      // length-prefixed idents emitted at top
		bool refPrefixed = false;     // a general S-ref absorbed a PREFIX level
		bool bareRef = false;         // encoded is exactly a back-ref right now

		for (size_t i = 0; i < chain.size(); ++i) {
			const NameComponent &nc = chain[i];
			std::string scopedNoArgs = scoped_prefix_noargs(chain, i);

			if (!nc.is_template) {
				std::string canonKey = canon_name_prefix(chain, i);
				std::string ref = find_sub(canonKey);
				std::string abbr = std_abbrev(scopedNoArgs);
				if (!ref.empty()) {
					encoded = ref;
					refPrefixed = false;            // whole name so far IS the ref
					bareRef = true;
				} else if (!abbr.empty()) {
					encoded = abbr;                 // abbreviation: no slot
					bareRef = false;
				} else {
					if (bareRef) {
						refPrefixed = true;     // S-ref now acts as a prefix
						bareRef = false;
					}
					encoded += source_name(nc.ident);
					add_sub(canonKey);
					srcNameCount++;
				}
			} else {
				// Complete-specialization std abbreviation (Ss/Si/So/Sd): emit
				// the abbreviation standalone — it already includes the template
				// args, so do NOT expand I..E and do NOT add a slot.
				std::string whole = std_complete_abbrev(chain, i);
				if (!whole.empty()) {
					encoded = whole;
					refPrefixed = false;
					bareRef = false;
					continue;
				}
				// Template-prefix (name up to this ident, WITHOUT args).
				std::string prefixKey = scopedNoArgs;
				std::string pref = find_sub(prefixKey);
				std::string pabbr = std_abbrev(scopedNoArgs);
				std::string tprefix;
				bool tprefixIsRef = false;
				if (!pref.empty()) {
					tprefix = pref;
					tprefixIsRef = true;
				} else if (!pabbr.empty()) {
					tprefix = pabbr;
				} else {
					if (bareRef) {
						refPrefixed = true;     // S-ref now acts as a prefix
						bareRef = false;
					}
					tprefix = encoded + source_name(nc.ident);
					add_sub(prefixKey);
					srcNameCount++;
				}

				// Template args (each recurses, registering its own candidates).
				std::string args;
				for (const auto &a : nc.targs)
					args += encode_type(a);

				// Whole template-id candidate, keyed canonically.
				std::string tidKey = canon_name_prefix(chain, i);
				std::string tidRef = find_sub(tidKey);
				if (!tidRef.empty()) {
					encoded = tidRef;
					refPrefixed = false;            // whole name so far IS the ref
					bareRef = true;
				} else {
					encoded = tprefix + "I" + args + "E";
					add_sub(tidKey);
					if (tprefixIsRef)
						refPrefixed = true;     // substituted template-prefix
					bareRef = false;
				}
			}
		}

		// Wrap in N..E for a genuine nested-name used as a standalone type:
		// 2+ length-prefixed identifiers emitted at the top level, OR a general
		// S-ref back-reference used as a PREFIX (Itanium grammar: only St may
		// stand unwrapped as an <unscoped-name> prefix — NS_5valueE, never
		// RS_5value). The St/Sa/… abbreviations and a back-ref that IS the
		// whole name absorb the scope without an N..E; a single source-name
		// under std (e.g. St11char_traitsIcE) does not need wrapping.
		if (standalone && (srcNameCount >= 2 || refPrefixed))
			return "N" + encoded + "E";
		return encoded;
	}

	// "std::char_traits" for chain[..i] ignoring template args (for abbrev /
	// template-prefix lookups).
	static std::string scoped_prefix_noargs(const std::vector<NameComponent> &chain,
	                                         size_t i)
	{
		std::string s;
		for (size_t k = 0; k <= i; ++k) {
			if (k) s += "::";
			s += chain[k].ident;
		}
		return s;
	}

public:
	// Encode a (possibly decorated, possibly template-id) type standalone.
	std::string encode_core(const TypeNode &t)
	{
		if (t.tparam >= 0) return tparam_ref(t.tparam);
		if (t.is_builtin) return t.builtin;
		return encode_name(t.name, /*standalone=*/true);
	}
};

std::string op_special(const std::string &op)
{
	return operator_code(op);
}

} // anonymous namespace

// ---- public substitution-aware API -----------------------------------------

std::string itanium_encode_type_sub(const std::string &cpp_type)
{
	// MEMOIZED. Encoding a type means parsing it back out of its C++ spelling
	// character by character (parse_type -> parse_component ->
	// SpellingDelimDepth) and then mangling the resulting tree — cheap once,
	// ruinous repeated. DataDef::marshals_value_text() calls this to answer a
	// yes/no question ("is this the flavor's std::string?") for EVERY parameter
	// and return of EVERY emitted symbol, and callers re-ask for the same
	// handful of types constantly. Under -stdlib=libc++ each spelling also
	// carries the std::__1:: inline-namespace tag on every nested component, so
	// the strings are several times longer than the libstdc++ ones and the same
	// call count costs far more: this chain measured 88.7% of a 19.9 s
	// front-end run (tests/testsubscript.mad, 2026-08-02) against 1.2 s for the
	// same file on the default flavor.
	//
	// The result is NOT a pure function of the input: std_entity_scope() reads
	// the flavor globals (g_std_stdlib / g_std_abi_ns), so a spelling encodes
	// differently after an ABI switch — which --project performs per TU. The
	// generation stamp is therefore part of the key, the convention g_std_abi_gen
	// is declared for ("bumped on change; caches key on it") and the one
	// marshals_value_text's carrier cache already follows.
	static unsigned memo_gen = ~0u;
	static std::map<std::string, std::string> memo;
	if (memo_gen != g_std_abi_gen) {
		memo.clear();
		memo_gen = g_std_abi_gen;
	}
	std::map<std::string, std::string>::const_iterator hit = memo.find(cpp_type);
	if (hit != memo.end())
		return hit->second;
	ItaniumMangler m;
	m.reset();
	std::string encoded = m.encode_type(parse_type(cpp_type));
	memo[cpp_type] = encoded;
	return encoded;
}

std::string itanium_mangle_member_sub(const std::string &qualified_class,
                                       const std::string &member,
                                       const std::vector<std::string> &param_types,
                                       bool const_method)
{
	ItaniumMangler m;
	return m.mangle_member(qualified_class, member, "",
	                       param_types, const_method);
}

std::string itanium_mangle_member_template_sub(const std::string &qualified_class,
                                       const std::string &member,
                                       const std::vector<std::string> &template_arg_types,
                                       const std::string &return_type,
                                       const std::vector<std::string> &param_types,
                                       bool const_method)
{
	ItaniumMangler m;
	return m.mangle_member_template(qualified_class, member,
	                                template_arg_types, return_type,
	                                param_types, const_method);
}

std::string itanium_mangle_ctor_sub(const std::string &qualified_class,
                                      const std::vector<std::string> &param_types)
{
	ItaniumMangler m;
	return m.mangle_member(qualified_class, "", "C1",
	                       param_types, false);
}

std::string itanium_mangle_dtor_sub(const std::string &qualified_class,
                                    const char *flavor)
{
	ItaniumMangler m;
	return m.mangle_member(qualified_class, "", flavor,
	                       {}, false);
}

std::string itanium_mangle_operator_sub(const std::string &qualified_class,
                                         const std::string &op,
                                         const std::vector<std::string> &param_types,
                                         bool const_method)
{
	std::string code = op_special(op);
	if (code.empty()) return "";
	ItaniumMangler m;
	return m.mangle_member(qualified_class, "", code,
	                       param_types, const_method);
}

std::string itanium_mangle_std_free_template(const std::string &name,
        const std::vector<std::string> &targs,
        const std::string &ret,
        const std::vector<std::string> &params)
{
	std::string code = op_special(name);
	std::string opOrName = code.empty() ? source_name(name) : code;
	ItaniumMangler m;
	return m.mangle_std_free_template(opOrName, targs, ret, params);
}

std::string itanium_mangle_nested_sub(const std::vector<std::string> &qualifiers,
                                      const std::string &name,
                                      const std::vector<std::string> &param_types)
{
	ItaniumMangler m;
	return m.mangle_nested_function(qualifiers, name, param_types);
}

std::string itanium_mangle_nested_var(const std::vector<std::string> &qualifiers,
                                      const std::string &name)
{
	ItaniumMangler m;
	return m.mangle_nested_variable(qualifiers, name);
}

// ---- stdlib flavor / std ABI inline namespace --------------------------------
// See madc_mangle.h. The state itself is declared ABOVE the encoder (it decides
// what scope a std:: name is mangled in); this is the rest of the surface.

void madc_mangle_set_stdlib_gnu(bool cxx11_abi)
{
	const std::string ns = cxx11_abi ? "__cxx11" : "";
	if (g_std_stdlib == mstdlibGnu && g_std_abi_ns == ns)
		return;
	g_std_stdlib = mstdlibGnu;
	g_std_abi_ns = ns;
	++g_std_abi_gen;
}

void madc_mangle_set_stdlib_llvm(const std::string &abi_ns)
{
	if (g_std_stdlib == mstdlibLlvm && g_std_abi_ns == abi_ns)
		return;
	g_std_stdlib = mstdlibLlvm;
	g_std_abi_ns = abi_ns;
	++g_std_abi_gen;
}

MangleStdlib madc_mangle_active_stdlib()
{
	return g_std_stdlib;
}

MangleHostFlavorScope::MangleHostFlavorScope()
	: saved_stdlib(g_std_stdlib), saved_abi_ns(g_std_abi_ns)
{
	madc_mangle_set_stdlib_gnu(true);	// the host build's ABI (GNU cxx11)
}

MangleHostFlavorScope::~MangleHostFlavorScope()
{
	if (saved_stdlib == mstdlibLlvm)
		madc_mangle_set_stdlib_llvm(saved_abi_ns);
	else
		madc_mangle_set_stdlib_gnu(saved_abi_ns == "__cxx11");
}

// "std::" prefix for components INSIDE the versioned inline namespace (GNU:
// only the cxx11-tagged components; LLVM: everything).
static std::string std_prefix_tagged()
{
	return g_std_abi_ns.empty() ? "std::" : "std::" + g_std_abi_ns + "::";
}

// "std::" prefix for the supporting templates (allocator, char_traits, less,
// pair, and the containers): directly in std under GNU, inside the ABI
// namespace under LLVM.
static std::string std_prefix_untagged()
{
	return g_std_stdlib == mstdlibLlvm ? std_prefix_tagged() : "std::";
}

std::string itanium_mangle_std_var(const std::string &name)
{
	// GNU: namespace-scope std vars (cout, cin, ...) live directly in std
	// (never in __cxx11): _ZSt <length-prefixed-name>. LLVM: everything
	// lives in the ABI namespace: _ZN St <ns> <name> E. Both spellings come
	// from ItaniumMangler::std_entity_scope, the one owner of that rule —
	// this used to build the prefix itself, which is how the FUNCTION
	// manglers came to disagree with it.
	ItaniumMangler m;
	return m.mangle_std_var(name);
}

// ---- canonical std:: type spellings -----------------------------------------

std::string std_string_type()
{
	return std_prefix_tagged() + "basic_string<char,"
	       + std_prefix_untagged() + "char_traits<char>,"
	       + std_prefix_untagged() + "allocator<char>>";
}

// Marshalling-boundary predicate (declared on DataDef in datadef.h, defined
// HERE because the mangler is the one permitted home for std:: symbol
// knowledge): is this type the class carrying madc::value's text kind?
// Spellings compare through the Itanium encoding, never raw strings:
// spacing/substitution variants normalize, while genuinely distinct ABI
// types (pre-C++11 std::basic_string vs std::__cxx11::) stay distinct.
// Itanium desugaring for a PLAIN SCALAR dd (see datadef.h). A namespace or
// class-scope scalar typedef mints an alias DataDef whose canonical spelling
// is the ALIAS ("std::streamoff") — right for source fidelity, wrong inside a
// mangled symbol (libstdc++ exports seekoff as ...El..., never
// ...ESt9streamoff...). The typeid guard keeps this to EXACT DataDef
// instances: every derived kind (enum, class, pointer, complex, SIMD, ...)
// spells itself. char-width types stay out — dtCHAR == dtINT8, so `c` vs `a`
// is not recoverable from the DataType alone.
std::string DataDef::mangle_scalar_spelling() const
{
	if (typeid(*this) != typeid(DataDef))
		return "";
	if (basetype() != BaseType::btSimple)
		return "";
	switch (rawtype()) {
	case DataType::dtBOOL:    return "bool";
	case DataType::dtUINT8:   return "unsigned char";
	case DataType::dtINT16:   return "short";
	case DataType::dtUINT16:  return "unsigned short";
	case DataType::dtINT32:   return "int";
	case DataType::dtUINT32:  return "unsigned int";
	// The 64-bit dds spell through the target data model: the served
	// headers themselves say `long` on LP64 and `long long` on LLP64
	// (win64 size_t/int64_t/streamsize all resolve there), so template-
	// argument identity, member-call mangling, and this desugar agree
	// with the target's own libstdc++ exports (_Znwy, not _Znwm).
	case DataType::dtINT64:   return target_llp64() ? "long long"
							 : "long";
	case DataType::dtUINT64:  return target_llp64() ? "unsigned long long"
							 : "unsigned long";
	case DataType::dtFLOAT:   return "float";
	case DataType::dtDOUBLE:  return "double";
	case DataType::dtLDOUBLE: return "long double";
	default:                  return "";
	}
}

bool DataDef::marshals_value_text() const
{
	const std::string &spelling =
		canonical_cpp_spelling().empty() ? name : canonical_cpp_spelling();
	if (spelling.empty())
		return false;
	// Cache keyed on the stdlib-flavor generation, not computed once: the
	// carrier spelling follows the parsed ABI config, which can land AFTER
	// the first call (and --project TUs may re-sync it per TU).
	static unsigned carrier_gen = ~0u;
	static std::string text_carrier;
	if (carrier_gen != g_std_abi_gen) {
		text_carrier = itanium_encode_type_sub(std_string_type());
		carrier_gen = g_std_abi_gen;
	}
	return itanium_encode_type_sub(spelling) == text_carrier;
}

std::string std_vector_type(const std::string &elem)
{
	const std::string ns = std_prefix_untagged();
	return ns + "vector<" + elem + "," + ns + "allocator<" + elem + ">>";
}

std::string std_map_type(const std::string &key, const std::string &val)
{
	const std::string ns = std_prefix_untagged();
	return ns + "map<" + key + "," + val + "," + ns + "less<" + key + ">,"
	       + ns + "allocator<" + ns + "pair<const " + key + "," + val + ">>>";
}

std::string std_set_type(const std::string &elem)
{
	const std::string ns = std_prefix_untagged();
	return ns + "set<" + elem + "," + ns + "less<" + elem + ">,"
	       + ns + "allocator<" + elem + ">>";
}

std::string std_stringstream_type()
{
	return std_prefix_tagged() + "basic_stringstream<char,"
	       + std_prefix_untagged() + "char_traits<char>,"
	       + std_prefix_untagged() + "allocator<char>>";
}
