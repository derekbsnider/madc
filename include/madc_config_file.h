///////////////////////////////////////////////////////////////////////////
//                                                                       //
// config_file — a SCHEMA-BLIND `key = value` configuration-file reader.   //
//                                                                       //
// The same split madcdis/snapshot.h makes for the pool container: that    //
// container is CONTENT-blind (a segment is just (kind, bytes) and the     //
// consumer owns what the bytes mean), and this reader is SCHEMA-blind —   //
// it owns the FORMAT (lookup chain, lexing, path semantics, strict        //
// diagnostics) while the consumer registers the keys it accepts. So madc, //
// madcdat and any madcdis-based tool get one lookup rule, one set of path //
// semantics and one diagnostic style instead of a copied parser each      //
// (3R credo; no-parallel-implementations).                               //
//                                                                       //
// Construct it with the application name: `config_file("madcdat")` reads  //
// madcdat.ini, looks in madcdat's config directory, and accepts a         //
// [madcdat] section. Nothing here knows a single madc key spelling.       //
//                                                                       //
// PRIVATE (uninstalled) on purpose: include/madcdis/*.h is installed      //
// wholesale by the build, so putting this there would freeze a first-draft //
// interface into shipped headers. In-tree consumers need no such promise;  //
// promote it to include/madcdis/config_file.h once a second consumer has  //
// actually exercised the interface.                                      //
//                                                                       //
// Reading a config file is a POLICY decision that belongs to the caller,  //
// not to this reader: a file that can redirect where a compiler loads     //
// frozen state from is an attack surface for a sandboxed process, so each //
// consumer gates its own lookup (madc's is the --enable-config-file        //
// configure axis). Having a parser available is not a call path.          //
//                                                                       //
///////////////////////////////////////////////////////////////////////////
#ifndef __MADC_CONFIG_FILE_H
#define __MADC_CONFIG_FILE_H 1

#include <iosfwd>
#include <string>
#include <vector>

namespace madc {
namespace cfg {

// One configuration file: its lookup chain, its grammar, and the schema the
// consumer registered. Register keys, then load() or parse_file().
//
// Grammar: `key = value`, one per line; `#` and `;` comments; an optional
// `[<app>]` section header; values may be quoted (the quotes are syntax, not
// content). Keys are case-insensitive, values are not. A repeated scalar takes
// the LAST value; a list key appends in file order.
//
// STRICT by design: an unknown key, a foreign section, a missing '=', an empty
// value, or an unparsable number/boolean is an ERROR naming file:line and the
// registered key set. A config file is the user's own file, so half-applying it
// is worse than refusing it.
class config_file
{
public:
	// `app` drives everything app-specific: <app>.ini, the <app> config
	// directory, and the accepted section name.
	explicit config_file(const std::string &app);

	// --- schema registration (the consumer's half of the contract) ---
	// Each accept_* names a key and the destination the parsed value lands
	// in. The optional `seen` flag records that the FILE set this key, which
	// is what a precedence rule needs: "the file said 0" and "the file said
	// nothing" are different answers when an environment variable or a baked
	// default sits underneath.
	void accept_text(const char *key, std::string &out, bool *seen = NULL);
	// Repeatable plain text, appended in file order — names, not paths, so
	// the value is taken verbatim (no directory resolution).
	void accept_text_list(const char *key, std::vector<std::string> &out);
	// Path keys resolve a relative value against the CONFIG FILE's own
	// directory and expand a leading `~/`.
	void accept_path(const char *key, std::string &out, bool *seen = NULL);
	void accept_path_list(const char *key, std::vector<std::string> &out);
	// Whole-string unsigned decimal; trailing junk is refused, never
	// truncated (`8G` must say so, not mean 8). `units` is a noun used ONLY
	// in the diagnostic ("needs a whole number of megabytes"), so the reader
	// can present a key well without knowing what it means.
	void accept_count(const char *key, unsigned long &out, bool *seen = NULL,
			  const char *units = NULL);
	// yes/no, true/false, on/off, 1/0 (case-insensitive).
	void accept_flag(const char *key, bool &out, bool *seen = NULL);

	// The lookup chain, in order: ./<app>.ini,
	// $XDG_CONFIG_HOME/<app>/<app>.ini (or ~/.config/<app>/<app>.ini), then
	// <sysconfdir>/<app>.ini. The first EXISTING file wins outright — files
	// are never merged, because a merged chain makes "why is this setting
	// on?" unanswerable.
	std::vector<std::string> search_paths() const;

	// Parse one named file. false = unreadable or malformed, reason already
	// on `err`.
	bool parse_file(const std::string &path, std::ostream &err);

	// A non-NULL explicit_path is the ENTIRE search and must load — a named
	// file that gets ignored is a silent failure. Otherwise the chain above
	// is walked; a complete miss is normal and silent (true, nothing set).
	bool load(const char *explicit_path, std::ostream &err);

	// The file the values came from; empty when none loaded.
	const std::string &source_path() const { return _source_path; }

private:
	enum value_kind { vkText, vkTextList, vkPath, vkPathList, vkCount, vkFlag };

	struct key_spec
	{
		std::string name;
		value_kind kind;
		std::string *text;
		std::vector<std::string> *list;
		unsigned long *count;
		bool *flag;
		bool *seen;
		const char *units;	// diagnostic-only noun for vkCount
	};

	void add_key(const char *key, value_kind kind, std::string *text,
		     std::vector<std::string> *list, unsigned long *count,
		     bool *flag, bool *seen, const char *units = NULL);
	const key_spec *find_key(const std::string &name) const;
	std::string accepted_keys() const;
	bool assign(const key_spec &spec, const std::string &value,
		    const std::string &path, int lineno, std::ostream &err);

	std::string _app;
	std::string _source_path;
	std::vector<key_spec> _keys;
};

}   // namespace cfg
}   // namespace madc

#endif
