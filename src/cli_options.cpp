// src/cli_options.cpp — THE command-line option reader. Contract and
// thread-safety: include/libmadc/cli_options.h; design:
// docs/plans/2026-10-16-cli-options-design.md.

#include "libmadc/cli_options.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

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
		: _t(t), _words(args), _r(r) {}
	bool run();

private:
	// words[k] exists and is ready to read.
	bool fetch(size_t k) { return k < _words.size(); }
	bool read_option(size_t &i);
	bool read_bundle(size_t &i);
	bool take_value(const match &m, const std::string &word, size_t &i,
			occurrence &o);

	const table &_t;
	std::vector<std::string> _words;
	result &_r;
};

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

result parse(const table &t, const std::vector<std::string> &args)
{
	result r;
	reader(t, args, r).run();
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
