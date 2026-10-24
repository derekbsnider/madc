#ifndef __LIBMADC_CLI_OPTIONS_H
#define __LIBMADC_CLI_OPTIONS_H 1

// libmadc/cli_options.h — THE command-line option reader (design:
// docs/plans/2026-10-16-cli-options-design.md). A program declares its
// options once, as a table of rows; parse() reads a command line against the
// table, and the writers derive the help, a JSON description and shell
// completion scripts from the same rows. madc's own command line
// (src/madc_cli.cpp) is one table; the dialect faces are others.
//
// A table is constant data: rows are aggregates of literals, so a program's
// table costs nothing to build (madc reads its own at every start).
//
// THREAD-SAFETY CONTRACT: a table is immutable and may be shared across
// threads; parse() and the writers only read it. parse() is reentrant: it
// reads its arguments, the response files they name and, when asked, the
// environment through the caller's function, and returns a new result, a
// value the caller owns. An occurrence's `spelled` points into the table:
// keep the table alive while reading a result. No global state.

#include <cstddef>
#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "madcdis/istring.h"

namespace madc {
namespace cli {

// How one spelling takes its value.
enum class value_form : uint8_t
{
	none,			// -c, --verbose
	separate,		// -o FILE: the next argument, whatever it holds
	joined,			// -lNAME: glued on, not empty
	prefix,			// -O, -O2, -Werror=x: glued on, may be empty
	joined_or_separate,	// -IDIR or -I DIR
	equals,			// --std=VALUE
	optional_equals,	// --run-frozen or --run-frozen=FILE
	equals_or_separate	// --output=FILE or --output FILE (getopt_long)
};

// What a value must look like; anything else is a parse error.
enum class value_type : uint8_t
{
	text,			// anything (the default)
	integer,		// a whole decimal number, optionally signed
	number,			// a decimal number
	boolean,		// yes/no, true/false, on/off, 1/0 (any case)
	path			// a path, stored in generic form (host_path_generic)
};

// A row's flags: what the reader does around it.
enum row_flag : uint32_t
{
	rf_none		= 0,
	rf_ends_options	= 1u << 0,	// every argument after it is the remainder
	rf_hidden	= 1u << 1,	// read; never shown in help or completions
	rf_negatable	= 1u << 2,	// a "--name" flag also reads "--no-name"
	rf_required	= 1u << 3,	// a parse that never sets it fails
	rf_doc		= 1u << 4	// a help line only (an operand form, an
					// environment variable): never read
};

const size_t max_spellings = 4;

struct spelling
{
	const char *text;	// "-o", "--std": an equals form adds the '='
	value_form form;
};

// One option. An aggregate of literals: fields left out of an initializer are
// zero — no further spelling, no metavar, text type, no default.
struct row
{
	int id;				// the caller's code (an enum value)
	const char *name;		// a stable key: --help=json, sys.flags
	spelling spellings[max_spellings];	// the first is the primary one
	const char *metavar;		// the value's name in help ("<file>")
	unsigned section;		// index into table::sections
	const char *help;		// '\n' separates paragraphs; the writer wraps each
	const char *usage;		// help's left column, when the spellings can't say it
	uint32_t flags;			// row_flag bits
	uint32_t tags;			// the caller's own bits; the reader never reads them
	value_type type;
	const char *const *choices;	// NULL-terminated; non-NULL: the value is one of them
	const char *default_value;	// NULL: none
	const char *env;		// an environment variable that supplies the value
	const char *config_key;		// its key in the program's config file
};

struct section
{
	const char *title;
	const char *note;		// printed under the title, before its rows
};

enum class group_rule : uint8_t
{
	exclusive,			// at most one of the ids
	requires_rest			// ids[0] given: every other id must be too
};

const size_t max_group_ids = 8;

struct group
{
	group_rule rule;
	size_t nids;
	int ids[max_group_ids];
};

// Whether options may follow an operand.
enum class operand_rule : uint8_t
{
	permute,		// yes, anywhere (GNU getopt, gcc)
	stop_at_first,		// no: the first operand starts the remainder (POSIX)
	caller			// table::operand_continues decides at each operand
};

struct table;
struct result;

// At operand words[at]: true = options may still follow it.
typedef bool (*operand_policy)(const table &t, const result &so_far,
			       const std::vector<std::string> &words, size_t at);

struct table
{
	const char *program;		// "madc": the usage line, completions
	const char *usage;		// the usage line after the program name
	const char *description;	// help's prologue (NULL: none)
	const char *epilog;		// help's epilogue (NULL: none)
	const section *sections;
	size_t nsections;
	const row *rows;
	size_t nrows;
	const group *groups;
	size_t ngroups;
	bool bundling;			// -abc reads as -a -b -c
	bool response_files;		// @file expands in place (gcc, MSVC)
	operand_rule operands;
	operand_policy operand_continues;	// operand_rule::caller only
	const void *context;		// the policy's own data
};

enum class value_source : uint8_t { command_line, environment, config, default_value };

struct occurrence
{
	int id;
	std::string value;		// empty for a flag
	std::string word;		// the argument as written: "-O2", "--std=c17", "-o"
	const char *spelled;		// the spelling it matched (into the table); NULL off the command line
	bool negated;			// read through --no-<name>
	value_source source;
};

enum class error_kind : uint8_t
{
	none,
	unknown_option,		// -fbogus
	missing_value,		// -o at the end of the line
	unexpected_value,	// --verbose=1
	bad_value,		// not of the row's type, or not among its choices
	conflict,		// two options of an exclusive group
	missing_required,	// a required option, or a group's requirement
	response_file		// an @file that cannot be expanded
};

struct result
{
	std::vector<occurrence> options;	// the command line in order, then the other sources
	std::vector<std::string> operands;	// operands read while options could still follow
	std::vector<std::string> remainder;	// every argument after the options ended, verbatim
	std::vector<std::string> option_words;	// the words the options consumed, @file expanded
	error_kind error;
	std::string message;			// the diagnostic sentence, without a program prefix

	result() : error(error_kind::none) {}
	bool ok() const { return error == error_kind::none; }
	const occurrence *last(int id) const;		// the last occurrence, negated or not
	bool has(int id) const;				// it exists and is not negated
	size_t count(int id) const;			// non-negated occurrences (-vvv)
	std::vector<std::string> values(int id) const;	// their values, in order
	long long integer(int id, long long fallback) const;
	double number(int id, double fallback) const;
	bool boolean(int id, bool fallback) const;
};

// The sources beside the command line, below it in precedence: the
// environment, then the program's config file, then each row's default.
// Value-initialized (`sources()`): no environment, no config, defaults on.
struct sources
{
	const char *(*environment)(const char *name);	// getenv-shaped; NULL: none
	const std::map<madc::dis::istring, std::string> *config;	// key -> value; NULL: none
	bool skip_defaults;				// true: rows' defaults are not applied
};

// THE reader.
result parse(const table &t, const std::vector<std::string> &args,
	     const sources &from = sources());

// What one word names as an option.
struct match
{
	const row *option;		// NULL: no option
	const spelling *spelled;
	bool has_value;			// the word carries its value (glued or after '=')
	std::string value;
	bool negated;
	match() : option(NULL), spelled(NULL), has_value(false), negated(false) {}
};
match match_word(const table &t, const std::string &word);

// The row of `id`; NULL when the table has none.
const row *find_row(const table &t, int id);

// --- The writers: the same rows as help, JSON and completion scripts ---

// The help: the usage line, the description, each section (its note, then
// its rows and help lines), the epilogue, wrapped to `width` columns (at
// least 40). Hidden rows are left out.
void write_help(std::ostream &os, const table &t, size_t width);

// The table as JSON — what `madc --help=json` prints and madcide reads:
// {schema, program, usage, description, sections: [{title, note, options:
// [{name, spellings: [{text, form}], usage, help, type, metavar, choices,
// default, env, config_key, required, negatable, ends_options}], entries:
// [{usage, help}]}]}. Hidden rows are left out.
std::string describe_json(const table &t);

enum class shell : uint8_t { bash, zsh, fish, powershell };
// A shell's name ("bash", "zsh", "fish", "powershell") as its code.
bool parse_shell(const std::string &name, shell &out);
// A completion script for `sh` from the table's visible spellings: options
// after a '-', files after a spelling whose value is the next word, files
// otherwise.
void write_completion(std::ostream &os, const table &t, shell sh);

} // namespace cli
} // namespace madc

#endif
