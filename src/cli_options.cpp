// src/cli_options.cpp — THE command-line option reader. Contract and
// thread-safety: include/libmadc/cli_options.h; design:
// docs/plans/2026-10-16-cli-options-design.md.

#include "libmadc/cli_options.h"
#include "madc_posix_io.h"	// host_path_generic
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace madc {
namespace cli {

namespace {

bool takes_bare_word(value_form f)
{
	return f == value_form::none || f == value_form::separate
		|| f == value_form::prefix || f == value_form::joined_or_separate
		|| f == value_form::optional_equals
		|| f == value_form::equals_or_separate;
}

bool takes_equals(value_form f)
{
	return f == value_form::equals || f == value_form::optional_equals
		|| f == value_form::equals_or_separate;
}

bool takes_glued(value_form f)
{
	return f == value_form::joined || f == value_form::prefix
		|| f == value_form::joined_or_separate;
}

// A bare spelling of this form reads its value from the next word.
bool value_follows(value_form f)
{
	return f == value_form::separate || f == value_form::joined_or_separate
		|| f == value_form::equals_or_separate;
}

bool starts_with(const std::string &s, const char *p)
{
	return s.compare(0, strlen(p), p) == 0;
}

std::string quoted(const std::string &s)
{
	return "'" + s + "'";
}

bool fail(result &r, error_kind kind, const std::string &message)
{
	r.error = kind;
	r.message = message;
	return false;
}

// Every spelling a word could be read as, in table order; f returns true
// to stop.
template <typename F>
void each_spelling(const table &t, F f)
{
	for ( size_t r = 0; r < t.nrows; r++ )
	{
		const row &rw = t.rows[r];
		if ( rw.flags & rf_doc )
			continue;
		for ( size_t s = 0; s < max_spellings && rw.spellings[s].text; s++ )
			if ( f(rw, rw.spellings[s]) )
				return;
	}
}

// Optimal-string-alignment distance: insertions, deletions, substitutions
// and adjacent transpositions each cost one.
size_t edit_distance(const std::string &a, const std::string &b)
{
	const size_t n = a.size(), m = b.size();
	std::vector<std::vector<size_t> > d(n + 1, std::vector<size_t>(m + 1));
	for ( size_t i = 0; i <= n; i++ )
		d[i][0] = i;
	for ( size_t j = 0; j <= m; j++ )
		d[0][j] = j;
	for ( size_t i = 1; i <= n; i++ )
		for ( size_t j = 1; j <= m; j++ )
		{
			const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
			size_t v = std::min(std::min(d[i - 1][j] + 1, d[i][j - 1] + 1),
					    d[i - 1][j - 1] + cost);
			if ( i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] )
				v = std::min(v, d[i - 2][j - 2] + 1);
			d[i][j] = v;
		}
	return d[n][m];
}

// How far a suggestion may be, after gcc's spellcheck: none for one-letter
// words; a third of the longer length when the lengths are close, else a
// little more.
size_t edit_cutoff(size_t goal, size_t candidate)
{
	const size_t hi = std::max(goal, candidate), lo = std::min(goal, candidate);
	if ( hi <= 1 )
		return 0;
	if ( hi - lo <= 1 )
		return std::max(hi / 3, (size_t)1);
	return (hi + 2) / 3;
}

// The spelling nearest `word`, as a user would type it; "" when none is
// near enough. A NAME=VALUE word is compared by its "NAME=".
std::string nearest_spelling(const table &t, const std::string &word)
{
	const size_t eq = word.find('=');
	const std::string goal = eq == std::string::npos ? word : word.substr(0, eq + 1);
	std::string best;
	size_t best_d = (size_t)-1;
	each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
		if ( rw.flags & rf_hidden )
			return false;
		std::string cand(sp.text);
		if ( sp.form == value_form::equals )
			cand += '=';
		// The distance is at least the difference in length, so a
		// spelling that far off is never compared (gcc's spellcheck
		// tests it first); it also keeps edit_distance's table to the
		// spellings' own lengths, whatever word an @file holds.
		const size_t gap = goal.size() > cand.size() ? goal.size() - cand.size()
							     : cand.size() - goal.size();
		if ( gap > edit_cutoff(goal.size(), cand.size()) )
			return false;
		const size_t d = edit_distance(goal, cand);
		if ( d <= edit_cutoff(goal.size(), cand.size()) && d < best_d )
		{
			best_d = d;
			best = cand;
		}
		return false;
	});
	return best;
}

// Why `word` names no option: a flag given a value, or nothing at all.
bool fail_unknown(const table &t, const std::string &word, result &r)
{
	const size_t eq = word.find('=');
	if ( eq != std::string::npos )
	{
		const std::string head = word.substr(0, eq);
		bool flag = false;
		each_spelling(t, [&](const row &, const spelling &sp) -> bool {
			flag = sp.form == value_form::none && head == sp.text;
			return flag;
		});
		if ( flag )
			return fail(r, error_kind::unexpected_value,
				    "option " + quoted(head) + " doesn't allow an argument");
	}
	std::string message = "unrecognized command-line option " + quoted(word);
	const std::string near = nearest_spelling(t, word);
	if ( !near.empty() )
		message += "; did you mean " + quoted(near) + "?";
	return fail(r, error_kind::unknown_option, message);
}

bool operand_continues(const table &t, const result &r,
		       const std::vector<std::string> &words, size_t at)
{
	switch ( t.operands )
	{
	case operand_rule::permute:
		return true;
	case operand_rule::stop_at_first:
		return false;
	case operand_rule::caller:
		return t.operand_continues ? t.operand_continues(t, r, words, at) : true;
	}
	return true;
}

bool parse_boolean(const std::string &s, bool &out)
{
	std::string l;
	for ( size_t i = 0; i < s.size(); i++ )
		l += (char)tolower((unsigned char)s[i]);
	if ( l == "1" || l == "yes" || l == "true" || l == "on" )
	{
		out = true;
		return true;
	}
	if ( l == "0" || l == "no" || l == "false" || l == "off" )
	{
		out = false;
		return true;
	}
	return false;
}

// libiberty's limit (expandargv): a file that names itself stops here.
const unsigned response_file_limit = 2000;

// A response file's words, by libiberty's buildargv (gcc's @file): blanks
// separate words; '...' and "..." quote; a backslash makes the next
// character literal everywhere, inside quotes too; "" is an empty word.
std::vector<std::string> split_response_text(const std::string &s)
{
	std::vector<std::string> out;
	size_t i = 0;
	for ( ;; )
	{
		while ( i < s.size() && isspace((unsigned char)s[i]) )
			i++;
		if ( i >= s.size() )
			return out;
		std::string arg;
		bool squote = false, dquote = false, bsquote = false;
		for ( ; i < s.size(); i++ )
		{
			const char c = s[i];
			if ( isspace((unsigned char)c) && !squote && !dquote && !bsquote )
				break;
			if ( bsquote )
			{
				bsquote = false;
				arg += c;
			}
			else if ( c == '\\' )
				bsquote = true;
			else if ( squote )
			{
				if ( c == '\'' )
					squote = false;
				else
					arg += c;
			}
			else if ( dquote )
			{
				if ( c == '"' )
					dquote = false;
				else
					arg += c;
			}
			else if ( c == '\'' )
				squote = true;
			else if ( c == '"' )
				dquote = true;
			else
				arg += c;
		}
		out.push_back(arg);
	}
}

bool whole_integer(const std::string &s)
{
	if ( s.empty() )
		return false;
	errno = 0;
	char *end = NULL;
	strtoll(s.c_str(), &end, 10);
	return errno == 0 && end && *end == '\0';
}

bool whole_number(const std::string &s)
{
	if ( s.empty() )
		return false;
	errno = 0;
	char *end = NULL;
	strtod(s.c_str(), &end);
	return errno == 0 && end && *end == '\0';
}

// A command-line occurrence carries a value to check: never a flag, nor the
// bare word of a form whose value may be absent.
bool carries_value(value_form f, const std::string &value)
{
	if ( f == value_form::none )
		return false;
	return !value.empty() || (f != value_form::prefix && f != value_form::optional_equals);
}

// The value a row needs: its type, then one of its choices. A path is stored
// in generic form. `what` names where the value came from.
bool check_value(const row &rw, occurrence &o, const std::string &what, result &r)
{
	const char *expected = NULL;
	bool b;
	switch ( rw.type )
	{
	case value_type::text:
		break;
	case value_type::integer:
		if ( !whole_integer(o.value) )
			expected = "an integer";
		break;
	case value_type::number:
		if ( !whole_number(o.value) )
			expected = "a number";
		break;
	case value_type::boolean:
		if ( !parse_boolean(o.value, b) )
			expected = "yes or no";
		break;
	case value_type::path:
		o.value = madc::detail::host_path_generic(o.value);
		break;
	}
	if ( expected )
		return fail(r, error_kind::bad_value, "invalid value " + quoted(o.value)
			    + " for " + what + ": expected " + expected);
	if ( !rw.choices )
		return true;
	std::string list;
	for ( const char *const *c = rw.choices; *c; c++ )
	{
		if ( o.value == *c )
			return true;
		list += (list.empty() ? "" : ", ") + std::string(*c);
	}
	return fail(r, error_kind::bad_value, "invalid value " + quoted(o.value)
		    + " for " + what + ": expected one of " + list);
}

bool given_on_command_line(const result &r, int id)
{
	for ( size_t k = 0; k < r.options.size(); k++ )
		if ( r.options[k].id == id && !r.options[k].negated
		     && r.options[k].source == value_source::command_line )
			return true;
	return false;
}

// How a message names `id`: the spelling the command line used, else the
// row's primary spelling.
std::string spelled_name(const table &t, const result &r, int id)
{
	for ( size_t k = 0; k < r.options.size(); k++ )
		if ( r.options[k].id == id && r.options[k].spelled )
			return quoted(r.options[k].spelled);
	const row *rw = find_row(t, id);
	return quoted(rw && rw->spellings[0].text ? rw->spellings[0].text : "?");
}

bool check_groups(const table &t, result &r)
{
	for ( size_t g = 0; g < t.ngroups; g++ )
	{
		const group &gr = t.groups[g];
		if ( gr.rule == group_rule::exclusive )
		{
			const int *first = NULL;
			for ( size_t k = 0; k < gr.nids; k++ )
			{
				if ( !given_on_command_line(r, gr.ids[k]) )
					continue;
				if ( first )
					return fail(r, error_kind::conflict, spelled_name(t, r, gr.ids[k])
						    + " cannot be used with " + spelled_name(t, r, *first));
				first = &gr.ids[k];
			}
		}
		else if ( gr.nids > 0 && r.has(gr.ids[0]) )
			for ( size_t k = 1; k < gr.nids; k++ )
				if ( !r.has(gr.ids[k]) )
					return fail(r, error_kind::missing_required, spelled_name(t, r, gr.ids[0])
						    + " requires " + spelled_name(t, r, gr.ids[k]));
	}
	return true;
}

bool check_required(const table &t, result &r)
{
	for ( size_t k = 0; k < t.nrows; k++ )
	{
		const row &rw = t.rows[k];
		if ( (rw.flags & rf_required) && !(rw.flags & rf_doc) && !r.last(rw.id) )
			return fail(r, error_kind::missing_required,
				    "the option " + quoted(rw.spellings[0].text) + " is required");
	}
	return true;
}

bool takes_value(const row &rw)
{
	for ( size_t s = 0; s < max_spellings && rw.spellings[s].text; s++ )
		if ( rw.spellings[s].form != value_form::none )
			return true;
	return false;
}

// What the sources below the command line give a row it left unset — the
// environment, then config, then the row's default — with that source and a
// name for messages; NULL when none does.
const char *source_value(const row &rw, const sources &from, value_source &src,
			 std::string &what)
{
	if ( from.environment && rw.env )
	{
		const char *v = from.environment(rw.env);
		if ( v && *v )
		{
			src = value_source::environment;
			what = quoted(rw.env);
			return v;
		}
	}
	if ( from.config && rw.config_key )
	{
		std::map<madc::dis::istring, std::string>::const_iterator it =
			from.config->find(madc::dis::istring(rw.config_key));
		if ( it != from.config->end() )
		{
			src = value_source::config;
			what = quoted(rw.config_key);
			return it->second.c_str();
		}
	}
	if ( !from.skip_defaults && rw.default_value )
	{
		src = value_source::default_value;
		what = "the default of " + quoted(rw.spellings[0].text ? rw.spellings[0].text : rw.name);
		return rw.default_value;
	}
	return NULL;
}

// Every row the command line left unset takes its value from the sources;
// a flag's value from them is a yes or a no.
bool apply_sources(const table &t, const sources &from, result &r)
{
	for ( size_t k = 0; k < t.nrows; k++ )
	{
		const row &rw = t.rows[k];
		if ( (rw.flags & rf_doc) || r.last(rw.id) )
			continue;
		occurrence o;
		o.id = rw.id;
		o.spelled = NULL;
		o.negated = false;
		std::string what;
		const char *v = source_value(rw, from, o.source, what);
		if ( !v )
			continue;
		o.value = v;
		if ( takes_value(rw) )
		{
			if ( !check_value(rw, o, what, r) )
				return false;
			r.options.push_back(o);
			continue;
		}
		bool on;
		if ( !parse_boolean(o.value, on) )
			return fail(r, error_kind::bad_value, "invalid value " + quoted(o.value)
				    + " for " + what + ": expected yes or no");
		o.value.clear();
		o.negated = !on;
		if ( on || (rw.flags & rf_negatable) )
			r.options.push_back(o);
	}
	return true;
}

const size_t help_column = 26;

std::string metavar_of(const row &rw)
{
	return rw.metavar ? rw.metavar : "<value>";
}

std::string spelling_display(const row &rw, const spelling &sp)
{
	std::string s(sp.text);
	switch ( sp.form )
	{
	case value_form::none:
		break;
	case value_form::separate:
		s += " " + metavar_of(rw);
		break;
	case value_form::joined:
	case value_form::joined_or_separate:
		s += metavar_of(rw);
		break;
	case value_form::prefix:
		if ( rw.metavar )
			s += "[" + metavar_of(rw) + "]";
		break;
	case value_form::equals:
	case value_form::equals_or_separate:
		s += "=" + metavar_of(rw);
		break;
	case value_form::optional_equals:
		s += "[=" + metavar_of(rw) + "]";
		break;
	}
	return s;
}

// Help's left column for a row: its usage, else its spellings.
std::string row_display(const row &rw)
{
	if ( rw.usage )
		return rw.usage;
	std::string s;
	for ( size_t k = 0; k < max_spellings && rw.spellings[k].text; k++ )
	{
		if ( k )
			s += ", ";
		s += spelling_display(rw, rw.spellings[k]);
	}
	if ( rw.flags & rf_negatable )
		for ( size_t k = 0; k < max_spellings && rw.spellings[k].text; k++ )
			if ( rw.spellings[k].form == value_form::none
			     && starts_with(rw.spellings[k].text, "--") )
				s += ", --no-" + std::string(rw.spellings[k].text + 2);
	return s;
}

// A row's help with what the reader knows of it: [default: v] [env: X]
// [config: k].
std::string help_text(const row &rw)
{
	std::string s = rw.help ? rw.help : "";
	if ( rw.default_value )
		s += std::string(s.empty() ? "" : " ") + "[default: " + rw.default_value + "]";
	if ( rw.env )
		s += std::string(s.empty() ? "" : " ") + "[env: " + rw.env + "]";
	if ( rw.config_key )
		s += std::string(s.empty() ? "" : " ") + "[config: " + rw.config_key + "]";
	return s;
}

// `text` as lines of at most `room` columns: each '\n'-separated paragraph
// on its own, a paragraph's leading blanks kept on each of its lines, a word
// wider than the room alone on its line.
std::vector<std::string> wrap(const std::string &text, size_t room)
{
	std::vector<std::string> lines;
	size_t start = 0;
	for ( ;; )
	{
		const size_t nl = text.find('\n', start);
		const std::string para = text.substr(start, nl == std::string::npos
						     ? std::string::npos : nl - start);
		const size_t lead = para.find_first_not_of(' ');
		if ( lead == std::string::npos )
			lines.push_back(std::string());
		else
		{
			const std::string indent = para.substr(0, lead);
			std::istringstream in(para);
			std::string word, line = indent;
			while ( in >> word )
			{
				if ( line.size() > indent.size() && line.size() + 1 + word.size() > room )
				{
					lines.push_back(line);
					line = indent;
				}
				if ( line.size() > indent.size() )
					line += ' ';
				line += word;
			}
			lines.push_back(line);
		}
		if ( nl == std::string::npos )
			return lines;
		start = nl + 1;
	}
}

// One output line, its trailing blanks dropped.
void put_line(std::ostream &os, std::string line)
{
	const size_t end = line.find_last_not_of(' ');
	line.erase(end == std::string::npos ? 0 : end + 1);
	os << line << "\n";
}

void put_wrapped(std::ostream &os, const std::string &text, size_t width,
		 const std::string &indent)
{
	const std::vector<std::string> lines = wrap(text, width - indent.size());
	for ( size_t l = 0; l < lines.size(); l++ )
		put_line(os, indent + lines[l]);
}

const char *form_name(value_form f)
{
	switch ( f )
	{
	case value_form::none:			return "none";
	case value_form::separate:		return "separate";
	case value_form::joined:		return "joined";
	case value_form::prefix:		return "prefix";
	case value_form::joined_or_separate:	return "joined_or_separate";
	case value_form::equals:		return "equals";
	case value_form::optional_equals:	return "optional_equals";
	case value_form::equals_or_separate:	return "equals_or_separate";
	}
	return "none";
}

const char *type_name(value_type v)
{
	switch ( v )
	{
	case value_type::text:		return "text";
	case value_type::integer:	return "integer";
	case value_type::number:	return "number";
	case value_type::boolean:	return "boolean";
	case value_type::path:		return "path";
	}
	return "text";
}

// The words a completion offers for options: each visible spelling, a form
// whose value follows '=' with its '='.
std::vector<std::string> completion_words(const table &t)
{
	std::vector<std::string> out;
	each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
		if ( rw.flags & rf_hidden )
			return false;
		std::string w(sp.text);
		if ( sp.form == value_form::equals || sp.form == value_form::equals_or_separate )
			w += '=';
		out.push_back(w);
		return false;
	});
	return out;
}

// The visible spellings whose bare word takes the next word as its value.
std::vector<std::string> value_next_words(const table &t)
{
	std::vector<std::string> out;
	each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
		if ( !(rw.flags & rf_hidden) && value_follows(sp.form) )
			out.push_back(sp.text);
		return false;
	});
	return out;
}

// `s` single-quoted for a POSIX shell (bash, zsh): a ' becomes '\''.
std::string sh_quote(const std::string &s)
{
	std::string out = "'";
	for ( size_t i = 0; i < s.size(); i++ )
		out += s[i] == '\'' ? std::string("'\\''") : std::string(1, s[i]);
	return out + "'";
}

// `s` single-quoted for fish: ' and \ escaped with a backslash.
std::string fish_quote(const std::string &s)
{
	std::string out = "'";
	for ( size_t i = 0; i < s.size(); i++ )
	{
		if ( s[i] == '\'' || s[i] == '\\' )
			out += '\\';
		out += s[i];
	}
	return out + "'";
}

// `s` single-quoted for PowerShell: a ' doubled.
std::string ps_quote(const std::string &s)
{
	std::string out = "'";
	for ( size_t i = 0; i < s.size(); i++ )
		out += s[i] == '\'' ? std::string("''") : std::string(1, s[i]);
	return out + "'";
}

// A word of bash's compgen -W list: glob characters escaped (compgen expands
// each word and removes the escapes).
std::string compgen_word(const std::string &s)
{
	std::string out;
	for ( size_t i = 0; i < s.size(); i++ )
	{
		if ( strchr("?*[]\\", s[i]) )
			out += '\\';
		out += s[i];
	}
	return out;
}

// A shell identifier from the program's name.
std::string ident(const char *program)
{
	std::string out;
	for ( const char *p = program ? program : ""; *p; p++ )
		out += isalnum((unsigned char)*p) ? *p : '_';
	return out;
}

// A row's help, short enough for a completion menu: its first paragraph,
// cut at a word before 60 columns.
std::string short_help(const row &rw)
{
	std::string s = rw.help ? rw.help : "";
	const size_t nl = s.find('\n');
	if ( nl != std::string::npos )
		s.erase(nl);
	if ( s.size() <= 60 )
		return s;
	size_t cut = s.rfind(' ', 57);
	if ( cut == std::string::npos )
		cut = 57;
	return s.substr(0, cut) + "...";
}

void write_bash(std::ostream &os, const table &t)
{
	const std::string fn = "_" + ident(t.program) + "_complete";
	os << "# bash completion for " << t.program << " - generated by `"
	   << t.program << " --completion=bash`\n";
	os << fn << "()\n{\n";
	os << "\tlocal cur=\"${COMP_WORDS[COMP_CWORD]}\"\n";
	os << "\tlocal prev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n";
	const std::vector<std::string> after = value_next_words(t);
	if ( !after.empty() )
	{
		os << "\tcase \"$prev\" in\n\t\t";
		for ( size_t k = 0; k < after.size(); k++ )
			os << (k ? "|" : "") << sh_quote(after[k]);
		os << ") COMPREPLY=( $(compgen -f -- \"$cur\") ); return 0 ;;\n\tesac\n";
	}
	std::string list;
	const std::vector<std::string> all = completion_words(t);
	for ( size_t k = 0; k < all.size(); k++ )
		list += (k ? " " : "") + compgen_word(all[k]);
	os << "\tif [[ \"$cur\" == -* ]]; then\n";
	os << "\t\tCOMPREPLY=( $(compgen -W " << sh_quote(list) << " -- \"$cur\") )\n";
	os << "\t\t[[ \"${COMPREPLY[0]}\" == *= ]] && compopt -o nospace\n";
	os << "\t\treturn 0\n\tfi\n";
	os << "\tCOMPREPLY=( $(compgen -f -- \"$cur\") )\n}\n";
	os << "complete -F " << fn << " " << t.program << "\n";
}

void write_zsh(std::ostream &os, const table &t)
{
	const std::string fn = "_" + ident(t.program);
	std::string plain, equals, after;
	const std::vector<std::string> all = completion_words(t);
	for ( size_t k = 0; k < all.size(); k++ )
		(all[k][all[k].size() - 1] == '=' ? equals : plain) += " " + sh_quote(all[k]);
	const std::vector<std::string> next = value_next_words(t);
	for ( size_t k = 0; k < next.size(); k++ )
		after += " " + sh_quote(next[k]);
	os << "#compdef " << t.program << "\n";
	os << "# zsh completion for " << t.program << " - generated by `"
	   << t.program << " --completion=zsh`\n";
	os << fn << "() {\n";
	os << "\tlocal -a plain equals after\n";
	os << "\tplain=(" << plain << " )\n";
	os << "\tequals=(" << equals << " )\n";
	os << "\tafter=(" << after << " )\n";
	os << "\tif (( ${after[(Ie)${words[CURRENT-1]}]} )); then\n\t\t_files\n";
	os << "\telif [[ $PREFIX == -* ]]; then\n";
	os << "\t\tcompadd -- $plain\n\t\tcompadd -S '' -- $equals\n";
	os << "\telse\n\t\t_files\n\tfi\n}\n";
	os << fn << " \"$@\"\n";
}

void write_fish(std::ostream &os, const table &t)
{
	os << "# fish completion for " << t.program << " - generated by `"
	   << t.program << " --completion=fish`\n";
	each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
		if ( rw.flags & rf_hidden )
			return false;
		const std::string text(sp.text);
		os << "complete -c " << t.program;
		if ( text.compare(0, 2, "--") == 0 )
			os << " -l " << fish_quote(text.substr(2));
		else if ( text.size() == 2 )
			os << " -s " << fish_quote(text.substr(1));
		else
			os << " -o " << fish_quote(text.substr(1));
		if ( sp.form != value_form::none )
		{
			if ( rw.choices )
			{
				std::string list;
				for ( const char *const *c = rw.choices; *c; c++ )
					list += (list.empty() ? "" : " ") + std::string(*c);
				os << " -x -a " << fish_quote(list);
			}
			else if ( value_follows(sp.form) )
				os << " -r -F";
			else
				os << " -r";
		}
		const std::string h = short_help(rw);
		if ( !h.empty() )
			os << " -d " << fish_quote(h);
		os << "\n";
		return false;
	});
}

void write_powershell(std::ostream &os, const table &t)
{
	os << "# PowerShell completion for " << t.program << " - generated by `"
	   << t.program << " --completion=powershell`\n";
	os << "Register-ArgumentCompleter -Native -CommandName " << t.program
	   << " -ScriptBlock {\n";
	os << "\tparam($wordToComplete, $commandAst, $cursorPosition)\n";
	os << "\tif (-not \"$wordToComplete\".StartsWith('-')) { return }\n";
	os << "\t$options = @(";
	const std::vector<std::string> all = completion_words(t);
	for ( size_t k = 0; k < all.size(); k++ )
		os << (k ? ", " : "") << ps_quote(all[k]);
	os << ")\n";
	os << "\t$options | Where-Object { $_.StartsWith(\"$wordToComplete\") } | ForEach-Object {\n";
	os << "\t\t[System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterName', $_)\n";
	os << "\t}\n}\n";
}

occurrence command_line_occurrence(const row &rw, const spelling &sp,
				   const std::string &word, bool negated)
{
	occurrence o;
	o.id = rw.id;
	o.word = word;
	o.spelled = sp.text;
	o.negated = negated;
	o.source = value_source::command_line;
	return o;
}

// One pass over a command line. The words are a copy: a response file
// (Task 2) splices its words in place.
class reader
{
public:
	reader(const table &t, const std::vector<std::string> &args, result &r)
		: _t(t), _words(args), _r(r), _expansions(0) {}
	bool run();

private:
	// words[k] exists and is ready to read (an @file there expanded).
	bool fetch(size_t k);
	bool read_option(size_t &i);
	bool read_bundle(size_t &i);
	bool take_value(const match &m, const std::string &word, size_t &i,
			occurrence &o);

	const table &_t;
	std::vector<std::string> _words;
	result &_r;
	unsigned _expansions;	// @files expanded so far
};

// words[k] exists, ready to read. A "@file" there is first replaced by the
// file's words — repeatedly, so one file may name another (gcc's
// expandargv). A name that cannot be opened stays as written (gcc), and so
// does one that is not a regular file: gcc reads a file's size, which a
// device or a FIFO has none of, and reading one might never end. A
// directory, or a file past response_file_limit expansions, is an error.
bool reader::fetch(size_t k)
{
	while ( _t.response_files && k < _words.size()
		&& _words[k].size() > 1 && _words[k][0] == '@' )
	{
		const std::string path = _words[k].substr(1);
		struct stat st;
		if ( stat(path.c_str(), &st) != 0 )
			return true;
		if ( S_ISDIR(st.st_mode) )
			return fail(_r, error_kind::response_file,
				    "@-file refers to a directory: " + quoted(path));
		if ( !S_ISREG(st.st_mode) )
			return true;
		std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
		if ( !in )
			return true;
		if ( ++_expansions > response_file_limit )
			return fail(_r, error_kind::response_file, "too many @-files encountered");
		std::string text((size_t)st.st_size, '\0');
		in.read(&text[0], (std::streamsize)text.size());
		text.resize((size_t)in.gcount());
		const std::vector<std::string> got = split_response_text(text);
		_words.erase(_words.begin() + k);
		_words.insert(_words.begin() + k, got.begin(), got.end());
	}
	return k < _words.size();
}

bool reader::run()
{
	bool reading = true;
	size_t i = 0;
	for ( ;; )
	{
		if ( !reading )
		{
			// The remainder is the program's own: verbatim, never expanded.
			_r.remainder.insert(_r.remainder.end(), _words.begin() + i, _words.end());
			return true;
		}
		if ( !fetch(i) )
			return _r.ok();
		const std::string word = _words[i];
		if ( word == "--" )
		{
			reading = false;
			i++;
			continue;
		}
		if ( word.size() < 2 || word[0] != '-' )
		{
			if ( !operand_continues(_t, _r, _words, i) )
			{
				reading = false;
				continue;
			}
			_r.operands.push_back(word);
			i++;
			continue;
		}
		if ( !read_option(i) )
			return false;
		const row *rw = find_row(_t, _r.options.back().id);
		if ( rw && (rw->flags & rf_ends_options) )
			reading = false;
	}
}

bool reader::read_option(size_t &i)
{
	const std::string word = _words[i];
	const match m = match_word(_t, word);
	if ( !m.option )
	{
		if ( _t.bundling && word[1] != '-' )
			return read_bundle(i);
		return fail_unknown(_t, word, _r);
	}
	occurrence o = command_line_occurrence(*m.option, *m.spelled, word, m.negated);
	_r.option_words.push_back(word);
	if ( !take_value(m, word, i, o) )
		return false;
	if ( carries_value(m.spelled->form, o.value)
	     && !check_value(*m.option, o, quoted(o.spelled), _r) )
		return false;
	_r.options.push_back(o);
	i++;
	return true;
}

// The occurrence's value: the one the word carries, else the next word for a
// form that reads one (i moves onto it).
bool reader::take_value(const match &m, const std::string &word, size_t &i,
			occurrence &o)
{
	const value_form f = m.spelled->form;
	if ( m.has_value )
	{
		if ( f == value_form::joined && m.value.empty() )
			return fail(_r, error_kind::missing_value, "missing argument to " + quoted(word));
		o.value = m.value;
		return true;
	}
	if ( !value_follows(f) )
		return true;
	if ( !fetch(i + 1) )
		return _r.ok() ? fail(_r, error_kind::missing_value,
				      "missing argument to " + quoted(word))
			       : false;
	o.value = _words[i + 1];
	_r.option_words.push_back(o.value);
	i++;
	return true;
}

// -abc over one-letter spellings: -a -b -c. A letter that takes a value takes
// the rest of the word, else the next word (getopt).
bool reader::read_bundle(size_t &i)
{
	const std::string word = _words[i];
	std::vector<occurrence> got;
	std::vector<std::string> consumed(1, word);
	for ( size_t k = 1; k < word.size(); k++ )
	{
		const std::string letter = std::string("-") + word[k];
		const row *owner = NULL;
		const spelling *sp = NULL;
		each_spelling(_t, [&](const row &rw, const spelling &s) -> bool {
			if ( letter != s.text )
				return false;
			owner = &rw;
			sp = &s;
			return true;
		});
		if ( !sp || takes_equals(sp->form) )
			return fail_unknown(_t, word, _r);
		occurrence o = command_line_occurrence(*owner, *sp, word, false);
		if ( sp->form == value_form::none )
		{
			got.push_back(o);
			continue;
		}
		o.value = word.substr(k + 1);
		if ( o.value.empty() && value_follows(sp->form) )
		{
			if ( !fetch(i + 1) )
				return _r.ok() ? fail(_r, error_kind::missing_value,
						      "missing argument to " + quoted(letter))
					       : false;
			o.value = _words[++i];
			consumed.push_back(o.value);
		}
		else if ( o.value.empty() && sp->form == value_form::joined )
			return fail(_r, error_kind::missing_value, "missing argument to " + quoted(letter));
		if ( carries_value(sp->form, o.value) && !check_value(*owner, o, quoted(letter), _r) )
			return false;
		got.push_back(o);
		break;
	}
	_r.options.insert(_r.options.end(), got.begin(), got.end());
	_r.option_words.insert(_r.option_words.end(), consumed.begin(), consumed.end());
	i++;
	return true;
}

} // namespace

match match_word(const table &t, const std::string &word)
{
	match m;
	if ( word.size() < 2 || word[0] != '-' )
		return m;
	// 1. The whole word is a spelling.
	each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
		if ( !takes_bare_word(sp.form) || word != sp.text )
			return false;
		m.option = &rw;
		m.spelled = &sp;
		return true;
	});
	if ( m.option )
		return m;
	// 2. NAME=VALUE.
	const size_t eq = word.find('=');
	if ( eq != std::string::npos )
	{
		each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
			if ( !takes_equals(sp.form) || word.compare(0, eq, sp.text) != 0 )
				return false;
			m.option = &rw;
			m.spelled = &sp;
			m.has_value = true;
			m.value = word.substr(eq + 1);
			return true;
		});
		if ( m.option )
			return m;
	}
	// 3. A glued value: the longest spelling the word starts with.
	size_t longest = 0;
	each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
		const size_t n = strlen(sp.text);
		if ( takes_glued(sp.form) && n > longest && starts_with(word, sp.text) )
		{
			longest = n;
			m.option = &rw;
			m.spelled = &sp;
		}
		return false;
	});
	if ( m.option )
	{
		m.has_value = true;
		m.value = word.substr(longest);
		return m;
	}
	// 4. --no-NAME for a negatable flag.
	if ( starts_with(word, "--no-") )
	{
		const std::string positive = "--" + word.substr(5);
		each_spelling(t, [&](const row &rw, const spelling &sp) -> bool {
			if ( !(rw.flags & rf_negatable) || sp.form != value_form::none
			     || positive != sp.text )
				return false;
			m.option = &rw;
			m.spelled = &sp;
			m.negated = true;
			return true;
		});
	}
	return m;
}

const row *find_row(const table &t, int id)
{
	for ( size_t r = 0; r < t.nrows; r++ )
		if ( t.rows[r].id == id && !(t.rows[r].flags & rf_doc) )
			return &t.rows[r];
	return NULL;
}

void write_help(std::ostream &os, const table &t, size_t width)
{
	if ( width < 40 )
		width = 40;
	os << "Usage: " << (t.program ? t.program : "") << " "
	   << (t.usage ? t.usage : "[options]") << "\n";
	if ( t.description )
	{
		os << "\n";
		put_wrapped(os, t.description, width, "");
	}
	for ( size_t s = 0; s < t.nsections; s++ )
	{
		bool any = t.sections[s].note != NULL;
		for ( size_t k = 0; k < t.nrows && !any; k++ )
			any = t.rows[k].section == s && !(t.rows[k].flags & rf_hidden);
		if ( !any )
			continue;
		os << "\n" << t.sections[s].title << ":\n";
		if ( t.sections[s].note )
			put_wrapped(os, t.sections[s].note, width, "  ");
		for ( size_t k = 0; k < t.nrows; k++ )
		{
			const row &rw = t.rows[k];
			if ( rw.section != s || (rw.flags & rf_hidden) )
				continue;
			std::string left = "  " + row_display(rw);
			const std::string help = help_text(rw);
			if ( left.size() + 2 > help_column )
			{
				put_line(os, left);
				left.clear();
				if ( help.empty() )
					continue;
			}
			const std::vector<std::string> lines = wrap(help, width - help_column);
			for ( size_t l = 0; l < lines.size(); l++ )
			{
				std::string col = l == 0 ? left : std::string();
				col.resize(help_column, ' ');
				put_line(os, col + lines[l]);
			}
		}
	}
	if ( t.epilog )
	{
		os << "\n";
		put_wrapped(os, t.epilog, width, "");
	}
}

std::string describe_json(const table &t)
{
	nlohmann::json d;
	d["schema"] = 1;
	d["program"] = t.program ? t.program : "";
	d["usage"] = t.usage ? t.usage : "";
	if ( t.description )
		d["description"] = t.description;
	d["sections"] = nlohmann::json::array();
	for ( size_t s = 0; s < t.nsections; s++ )
	{
		nlohmann::json js;
		js["title"] = t.sections[s].title ? t.sections[s].title : "";
		if ( t.sections[s].note )
			js["note"] = t.sections[s].note;
		js["options"] = nlohmann::json::array();
		js["entries"] = nlohmann::json::array();
		for ( size_t k = 0; k < t.nrows; k++ )
		{
			const row &rw = t.rows[k];
			if ( rw.section != s || (rw.flags & rf_hidden) )
				continue;
			if ( rw.flags & rf_doc )
			{
				nlohmann::json e;
				e["usage"] = row_display(rw);
				e["help"] = rw.help ? rw.help : "";
				js["entries"].push_back(e);
				continue;
			}
			nlohmann::json o;
			o["name"] = rw.name ? rw.name : "";
			o["spellings"] = nlohmann::json::array();
			for ( size_t p = 0; p < max_spellings && rw.spellings[p].text; p++ )
			{
				nlohmann::json sp;
				sp["text"] = rw.spellings[p].text;
				sp["form"] = form_name(rw.spellings[p].form);
				o["spellings"].push_back(sp);
			}
			o["usage"] = row_display(rw);
			o["help"] = rw.help ? rw.help : "";
			o["type"] = type_name(rw.type);
			if ( rw.metavar )
				o["metavar"] = rw.metavar;
			if ( rw.choices )
			{
				o["choices"] = nlohmann::json::array();
				for ( const char *const *c = rw.choices; *c; c++ )
					o["choices"].push_back(*c);
			}
			if ( rw.default_value )
				o["default"] = rw.default_value;
			if ( rw.env )
				o["env"] = rw.env;
			if ( rw.config_key )
				o["config_key"] = rw.config_key;
			o["required"] = (rw.flags & rf_required) != 0;
			o["negatable"] = (rw.flags & rf_negatable) != 0;
			o["ends_options"] = (rw.flags & rf_ends_options) != 0;
			js["options"].push_back(o);
		}
		d["sections"].push_back(js);
	}
	return d.dump(2);
}

bool parse_shell(const std::string &name, shell &out)
{
	if ( name == "bash" )
		out = shell::bash;
	else if ( name == "zsh" )
		out = shell::zsh;
	else if ( name == "fish" )
		out = shell::fish;
	else if ( name == "powershell" )
		out = shell::powershell;
	else
		return false;
	return true;
}

void write_completion(std::ostream &os, const table &t, shell sh)
{
	switch ( sh )
	{
	case shell::bash:
		write_bash(os, t);
		return;
	case shell::zsh:
		write_zsh(os, t);
		return;
	case shell::fish:
		write_fish(os, t);
		return;
	case shell::powershell:
		write_powershell(os, t);
		return;
	}
}

result parse(const table &t, const std::vector<std::string> &args, const sources &from)
{
	result r;
	if ( reader(t, args, r).run() && apply_sources(t, from, r) && check_groups(t, r) )
		check_required(t, r);
	return r;
}

const occurrence *result::last(int id) const
{
	for ( size_t k = options.size(); k-- > 0; )
		if ( options[k].id == id )
			return &options[k];
	return NULL;
}

bool result::has(int id) const
{
	const occurrence *o = last(id);
	return o && !o->negated;
}

size_t result::count(int id) const
{
	size_t n = 0;
	for ( size_t k = 0; k < options.size(); k++ )
		if ( options[k].id == id && !options[k].negated )
			n++;
	return n;
}

std::vector<std::string> result::values(int id) const
{
	std::vector<std::string> out;
	for ( size_t k = 0; k < options.size(); k++ )
		if ( options[k].id == id && !options[k].negated )
			out.push_back(options[k].value);
	return out;
}

long long result::integer(int id, long long fallback) const
{
	const occurrence *o = last(id);
	if ( !o || o->negated || o->value.empty() )
		return fallback;
	return strtoll(o->value.c_str(), NULL, 10);
}

double result::number(int id, double fallback) const
{
	const occurrence *o = last(id);
	if ( !o || o->negated || o->value.empty() )
		return fallback;
	return strtod(o->value.c_str(), NULL);
}

bool result::boolean(int id, bool fallback) const
{
	const occurrence *o = last(id);
	if ( !o )
		return fallback;
	if ( o->negated )
		return false;
	if ( o->value.empty() )
		return true;		// a flag
	bool b;
	return parse_boolean(o->value, b) ? b : fallback;
}

} // namespace cli
} // namespace madc
