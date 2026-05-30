// madc_mangle.cpp — Itanium C++ ABI name mangler.
//
// Produces mangled symbol names following the Itanium ABI spec:
// https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling
//
// Covers: builtin types, pointers, references, const, classes,
// nested names, constructors (C1), destructors (D1).

#include "madc_mangle.h"
#include <cstring>

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

	// Fixed-width aliases (Linux x86-64 mappings)
	if (t == "int8_t")              return "a";
	if (t == "uint8_t")             return "h";
	if (t == "int16_t")             return "s";
	if (t == "uint16_t")            return "t";
	if (t == "int32_t")             return "i";
	if (t == "uint32_t")            return "j";
	if (t == "int64_t")             return "l";
	if (t == "uint64_t")            return "m";
	if (t == "size_t")              return "m";
	if (t == "ssize_t")             return "l";
	if (t == "ptrdiff_t")           return "l";

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

// Split a comma-separated template-arg list at top level (respecting <>).
std::vector<std::string> split_targs(const std::string &s)
{
	std::vector<std::string> out;
	int depth = 0;
	size_t start = 0;
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (c == '<') depth++;
		else if (c == '>') depth--;
		else if (c == ',' && depth == 0) {
			out.push_back(mstrip(s.substr(start, i - start)));
			start = i + 1;
		}
	}
	std::string last = mstrip(s.substr(start));
	if (!last.empty()) out.push_back(last);
	return out;
}

// Split a qualified name "a::b::c<...>" into component strings at top-level
// "::" (respecting <>). Each component may still carry its own <...>.
std::vector<std::string> split_scope(const std::string &s)
{
	std::vector<std::string> out;
	int depth = 0;
	size_t start = 0;
	for (size_t i = 0; i + 1 < s.size(); ++i) {
		char c = s[i];
		if (c == '<') depth++;
		else if (c == '>') depth--;
		else if (c == ':' && s[i + 1] == ':' && depth == 0) {
			out.push_back(s.substr(start, i - start));
			i++;            // skip second ':'
			start = i + 1;
		}
	}
	out.push_back(s.substr(start));
	return out;
}

TypeNode parse_type(const std::string &raw);

// Parse one name component "char_traits<char>" → ident + template args.
NameComponent parse_component(const std::string &raw)
{
	NameComponent nc;
	std::string s = mstrip(raw);
	size_t lt = std::string::npos;
	int depth = 0;
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '<') { if (depth == 0) { lt = i; } depth++; }
		else if (s[i] == '>') depth--;
	}
	if (lt == std::string::npos) {
		nc.ident = s;
		return nc;
	}
	nc.ident = mstrip(s.substr(0, lt));
	// strip the matching outer < >
	size_t gt = s.rfind('>');
	std::string inside = s.substr(lt + 1, gt - lt - 1);
	nc.is_template = true;
	for (const auto &a : split_targs(inside))
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
		std::string enc = encode_core(t);

		// Decorations are listed outermost→innermost; apply innermost first.
		// Track the canonical key alongside, so identical decorated types
		// (e.g. two "const string&" params) share one slot.
		std::string canon = canon_type_core(t);
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
				out += encode_type(parse_type(p));
		}
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
		if (scopedNoArgs == "std::basic_istream")  return "Si";
		if (scopedNoArgs == "std::basic_ostream")  return "So";
		if (scopedNoArgs == "std::basic_iostream") return "Sd";
		return "";
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

		for (size_t i = 0; i < chain.size(); ++i) {
			const NameComponent &nc = chain[i];
			std::string scopedNoArgs = scoped_prefix_noargs(chain, i);

			if (!nc.is_template) {
				std::string canonKey = canon_name_prefix(chain, i);
				std::string ref = find_sub(canonKey);
				std::string abbr = std_abbrev(scopedNoArgs);
				if (!ref.empty()) {
					encoded = ref;
				} else if (!abbr.empty()) {
					encoded = abbr;                 // abbreviation: no slot
				} else {
					encoded += source_name(nc.ident);
					add_sub(canonKey);
					srcNameCount++;
				}
			} else {
				// Template-prefix (name up to this ident, WITHOUT args).
				std::string prefixKey = scopedNoArgs;
				std::string pref = find_sub(prefixKey);
				std::string pabbr = std_abbrev(scopedNoArgs);
				std::string tprefix;
				if (!pref.empty()) {
					tprefix = pref;
				} else if (!pabbr.empty()) {
					tprefix = pabbr;
				} else {
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
				} else {
					encoded = tprefix + "I" + args + "E";
					add_sub(tidKey);
				}
			}
		}

		// Wrap in N..E only for a genuine nested-name used as a standalone type:
		// i.e. when 2+ length-prefixed identifiers were emitted at the top level
		// (the St/Sa/… abbreviations and back-refs absorb the scope without an
		// N..E). A single source-name under std (e.g. St11char_traitsIcE) does
		// not need wrapping.
		if (standalone && srcNameCount >= 2)
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
	ItaniumMangler m;
	m.reset();
	return m.encode_type(parse_type(cpp_type));
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

std::string itanium_mangle_ctor_sub(const std::string &qualified_class,
                                      const std::vector<std::string> &param_types)
{
	ItaniumMangler m;
	return m.mangle_member(qualified_class, "", "C1",
	                       param_types, false);
}

std::string itanium_mangle_dtor_sub(const std::string &qualified_class)
{
	ItaniumMangler m;
	return m.mangle_member(qualified_class, "", "D1",
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

// ---- canonical std:: type spellings -----------------------------------------

std::string std_string_type()
{
	return "std::__cxx11::basic_string<char,std::char_traits<char>,"
	       "std::allocator<char>>";
}

std::string std_vector_type(const std::string &elem)
{
	return "std::vector<" + elem + ",std::allocator<" + elem + ">>";
}

std::string std_map_type(const std::string &key, const std::string &val)
{
	return "std::map<" + key + "," + val + ",std::less<" + key + ">,"
	       "std::allocator<std::pair<const " + key + "," + val + ">>>";
}

std::string std_set_type(const std::string &elem)
{
	return "std::set<" + elem + ",std::less<" + elem + ">,"
	       "std::allocator<" + elem + ">>";
}

std::string std_stringstream_type()
{
	return "std::__cxx11::basic_stringstream<char,std::char_traits<char>,"
	       "std::allocator<char>>";
}
