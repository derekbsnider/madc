/* madc_session.h — the persistent interactive session (plan §41.2a).
 *
 * One Program accepts appended entries (D1). Each entry lowers to its own MIR
 * module, and every module links into ONE live MIR context, so a later entry
 * calls the functions and reads the globals an earlier entry defined, in
 * place. Nothing is replayed.
 *
 * This is the core, not the REPL: the REPL is one client of it, madcide's
 * panel another (§40, D2). The core is host-agnostic and renders nothing; a
 * refused entry's diagnostics are the Program's.
 *
 * Thread contract: one session is driven by one thread. Concurrent clients
 * reach it through the serialized verbs of D9.
 */

#ifndef __MADC_SESSION_H
#define __MADC_SESSION_H 1

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "madc/bits/session_enums"	// offer_state: the verdict's one enum text
#include "libmadc/value.h"		// madc::value: a bindings row
#include "madcdis/istring.h"	// a buffer's name and path (BufferSource)
#include "highlight_class.h"	// code_range: the shown text that is code
#include "madc_module_commands.h"	// a module's REPL command rows (1B.10)

class Program;
class Variable;
class CirJitSession;

// The session a REPL loop drives (madc_repl_run, madc_repl_edit; plan
// §41.9a slice 4): in this process (InteractiveSession) or in a backend
// process (BackendSession, madc_session_client.h). The loop asks only this.
class ReplSession
{
public:
    // Called once an offered entry is final, before its diagnostics render
    // and before it runs: a line editor hands the terminal back there.
    typedef std::function<void()> TakenHook;
    // The verdict's one text is <bits/session_enums>, the dialect's too.
    typedef ::madc::offer_state OfferState;
    struct Offered
    {
	OfferState state;
	bool ok;
    };
    virtual ~ReplSession() {}
    // The standard in force by its canonical name ("c17"): the prompt's
    // (D22) and the history's mode.
    virtual std::string standard_name() = 0;
    virtual bool submit(const std::string &text,
			const TakenHook &taken = TakenHook()) = 0;
    virtual Offered offer(const std::string &text,
			  const TakenHook &taken = TakenHook()) = 0;
    virtual const std::string &shown() const = 0;
    // The byte ranges of shown() that are code (`%type`'s type, `%pinfo`'s
    // declarations, split arc 1B.9), in order: a host may colour them.
    // Empty when nothing shown is code.
    virtual const std::vector<madc::code_range> &shown_code() const = 0;
    // The source the last taken entry added to the session, for a host's
    // REPL buffer (colon mode at the prompt edits it; owner 2026-10-12): a
    // code entry's text once the session kept it (its unit linked — a run
    // that stopped included, D27), or the text of the file %load kept (an
    // open buffer's live text when a buffer holds it). Empty after anything
    // else: a refused entry, any other command.
    virtual const std::string &taken_source() const = 0;
    // The host's REPL buffer was edited (owner 2026-10-12, the live
    // reparse): `before` and `after` are its text either side of the edit.
    // Each entry the edit changed or added is read again as definitions
    // only — a function it defines is redefined (D5), its statements never
    // run, and it shows nothing. False when one was refused (its diagnostics
    // render as an entry's do).
    virtual bool buffer_edited(const std::string &before, const std::string &after)
    { (void)before; (void)after; return true; }
    virtual std::vector<std::string> complete(const std::string &text,
					      size_t caret, size_t &start) = 0;
    // Does LINE continue an if that ended an entry (D11)? Its first word is
    // the standard's `else`.
    virtual bool continues_if(const std::string &line) = 0;
    // Load a program file and call its main when it defines one, with argv
    // (the path, then the program's arguments): `madc -i file`, the core of
    // %run (D25). False when the file was refused.
    virtual bool run_file(int argc, char **argv) = 0;
    // The end of a REPL-buffer rebuild (%run, owner 2026-10-12): the
    // buffer's main runs, as %run runs a file's, when the session defines
    // main and no unit names it (an entry that calls main ran it already).
    // `name` is the buffer's: main's argv[0]. False when main stopped.
    virtual bool run_rebuilt_main(const std::string &name)
    { (void)name; return true; }
    // The session ended, and its status: an exit(n) in an entry ended a
    // backend's process (in this process the exit already happened), or
    // %quit asked to end it (status 0).
    virtual bool ended(int &status) const { (void)status; return false; }
    // A host's buffers (split arc 1B.8, design decision 1): `name` — a FILE
    // typed, "" for the host's CURRENT buffer — to the path and the LIVE
    // text a command on it reads (has_text). A FILE no buffer holds: true,
    // the name as the path, has_text false (the disk's). False, with why,
    // when there is nothing to read: no current buffer, an untitled one.
    // The name and the path are names (istrings); the text and the refusal
    // are built text.
    typedef std::function<bool(const madc::dis::istring &name,
			       madc::dis::istring &path, std::string &text,
			       bool &has_text, std::string &why)> BufferSource;
    // A host that holds buffers hands them to the session it drives (the
    // session's buffer question: InteractiveSession::buffer_text).
    virtual void set_buffers(const BufferSource &source) { (void)source; }
    // The editor a command asks for (%open, %edit; split arc 1C.12): a host
    // that shows files — an IDE, an LSP client — hands its own to the session
    // it drives, as it hands its buffers. True when `path` was shown (at
    // `line` when > 0); false with `why`. None: the terminal's
    // (madc::run_terminal_editor). One request at a time, on the session's
    // thread.
    typedef std::function<bool(const std::string &path, int line,
			       std::string &why)> Editor;
    virtual void set_editor(const Editor &editor) { (void)editor; }
    // Input the session's program read from a stdin it shares with the host
    // past what it consumed (a backend's C runtime reads ahead on a pipe —
    // Windows), handed back: the host reads it before its own input. Taking
    // it clears it. Empty when nothing was read ahead.
    virtual std::string take_returned_input() { return std::string(); }
};

namespace madc {
// Is `text` blank — white space only (a blank line is no entry)?
bool blank_text(const std::string &text);
// A line without a CRLF text's '\r'.
void drop_cr(std::string &line);
}

// The REPL's grouping of typed lines into entries (D11): the ONE rule for
// every host that feeds lines, the cooked loop (madc_repl.cpp) and a rebuild's
// replay of the REPL buffer, in this process or in a backend. A line joins
// the pending entry, offered until the session takes it; an if that ended an
// entry takes a following `else` line, and any other line runs it first; a
// blank line between entries is none; at the end of input the pending entry
// is final. A host reads its own command lines (colon mode, the rebuild's
// line) only between entries (idle). After each entry the session took,
// `taken` gets its text and whether it was ok; false from it stops the lines
// (the session ended). `running`, when given, gets an entry's text once it
// is final, before it runs (a backend's running row).
class EntryLines
{
public:
    typedef std::function<bool(const std::string &entry, bool ok)> Taken;
    typedef std::function<void(const std::string &entry)> Running;
    explicit EntryLines(ReplSession &session, const Running &running = Running());
    bool idle() const { return pending.empty(); }
    // Before `line` joins: an if waiting for its else runs unless `line`
    // continues it.
    bool settle(const std::string &line, const Taken &taken);
    // `line` joins the entry.
    bool add(const std::string &line, const Taken &taken);
    // The end of input.
    bool finish(const Taken &taken);
    // A text's lines (a CRLF text's included), taken one entry at a time:
    // a rebuild's replay, in step with its host.
    void load(const std::string &text);
    // The loaded text's next entry: true once one was taken (`taken` was
    // called), false at the text's end.
    bool step(const Taken &taken);
private:
    ReplSession &session;
    Running running;
    std::string pending;	// the entry so far
    bool extendable;		// pending is an if waiting for its else
    std::vector<std::string> lines;	// load's
    size_t next;		// the next of them to join
    bool settled;		// lines[next] passed settle
    bool finished;		// the loaded text's end was taken
    ReplSession::TakenHook hook(const std::string &entry) const;
    bool take(const std::string &entry, bool ok, const Taken &taken);
};

class InteractiveSession : public ReplSession
{
public:
    InteractiveSession();
    // Adopt a Program its host configured: the command line's options and
    // madc.ini hold in the session as in a file (plan §41.5a, D20).
    explicit InteractiveSession(std::unique_ptr<Program> configured);
    ~InteractiveSession();

    // Start the session. `std_option` is a `--std=` spelling; empty keeps
    // the Program's default standard. False when the session cannot start;
    // an unknown standard is said on the Program's error stream.
    bool begin(const std::string &std_option = std::string());

    // Submit one complete entry. Its declarations persist, its module is
    // linked into the live context, and its statements run once (D25). False
    // when the entry is refused, or when its init or its run stops at a use
    // of a function no entry defines yet (plan §42 D27); its diagnostics are
    // program().diagnostics. A refused entry leaves the session as it was,
    // the Program and the live context alike (plan §41.3). A stopped one is
    // linked (entries() counts it) and keeps its definitions.
    //
    // `taken`, when given, is called once the entry is final, before its
    // diagnostics render and before it runs: a line editor finishes its
    // display there and hands the terminal back (plan §41.7a).
    bool submit(const std::string &text, const TakenHook &taken = TakenHook()) override;

    // Offer the text typed so far (plan §41.5a): the classifier runs inside
    // the entry transaction (§41.1a), so there is one parse per line.
    //   taken: the entry is final — numbered, its parse's diagnostics
    //     rendered, and submitted as submit() would (ok is its result);
    //   incomplete: keep reading; nothing is kept, numbered or rendered;
    //   extendable: a finished if with no else; D11 waits one line (the
    //     client submits it, or offers it again with the line).
    // `taken` is submit()'s, called only when the entry is taken. The enum's
    // one text is <bits/session_enums>, the dialect's too (ReplSession's).
    Offered offer(const std::string &text, const TakenHook &taken = TakenHook()) override;

    // Load a program file into the session (plan §41.5a, slice 2; the core
    // of %load, D25): one unit in its own grammar (a file is read as gcc
    // reads it), on everything the session declared. The file and the
    // session are one unit, so its statics are session names. Its
    // diagnostics cite its path; it takes no REPL[N]. False when it is
    // refused, and then it leaves nothing (plan §41.3).
    bool load(const std::string &path);
    // The same for a file's text, named `path` (an editor's buffer, unsaved
    // edits included: madcide's F5, plan §41.10a). load() reads the file and
    // calls this.
    bool load_text(const std::string &text, const std::string &path);
    // Call the session's main(argc, argv) at the entry boundary (the rest of
    // %run, D16/D25; `madc -i file`, python -i). False when there is no main,
    // or a use of an undefined name returned; its return value goes to
    // *status. It does not end the session.
    bool run_main(int argc, char **argv, int *status);
    // load(argv[0]), then run_main when the session defines main.
    bool run_file(int argc, char **argv) override;
    bool run_rebuilt_main(const std::string &name) override;
    // The same with main's argv (a backend's: the name, then %args).
    bool run_rebuilt_main(int argc, char **argv, int *status);
    // Will the rebuild's main run: the session defines main and no unit
    // names it.
    bool rebuilt_main_due();

    // Tab's question (plan §41.7a, slice 3): the names that complete the
    // word before `caret` in the text typed so far, sorted; the word starts
    // at `start`. The query leaves the session as it was, and a word in a
    // string or a comment completes nothing.
    std::vector<std::string> complete(const std::string &text, size_t caret,
				      size_t &start) override;

    // The live address of a session function / global, by emitted name.
    // NULL when no linked entry defines it.
    void *function(const char *name);
    void *data(const char *name);

    Program &program() { return *prog; }
    // The last entry's shown value (D10, plan §41.4a): an entry whose final
    // statement omits its `;` shows it, in re-enterable syntax (`30`,
    // `"abc"`, `(int *) 0x7ffd...`). After a command (plan §41.8a), the
    // command's output. Empty when the entry showed nothing. The core
    // renders nothing: a client prints it.
    const std::string &shown() const override;
    const std::vector<madc::code_range> &shown_code() const override;
    const std::string &taken_source() const override { return kept_source; }
    bool buffer_edited(const std::string &before, const std::string &after) override;
    // The text of the file unit the last load_text kept ("" when it was
    // refused): the backend server's load reply carries it, so a host that
    // honors %load's payload can keep it in its REPL buffer.
    const std::string &loaded_source() const { return unit_text; }
    // The host's buffers (split arc 1B.10, design decision 4): the source a
    // host in this process hands it, or the backend server's, which asks the
    // host over the wire. None: every FILE is the disk's.
    void set_buffers(const BufferSource &source) override { buffers = source; }
    // The host's editor (split arc 1C.12): %open / %edit in this process.
    void set_editor(const Editor &e) override { editor = e; }
    // The session's buffer question (madc::resolve_buffer's rule over its
    // source): what a command running in the session — a module's — reads
    // for FILE ("" the host's current buffer).
    bool buffer_text(const madc::dis::istring &name, madc::dis::istring &path,
		     std::string &text, bool &has_text, std::string &why);
    std::string standard_name() override;
    bool continues_if(const std::string &line) override;
    // %quit ended the session (plan §7f): status 0.
    bool ended(int &status) const override;

    // The session's commands (plan §41.8a, D13/D24; the modes, owner
    // 2026-10-09, split arc 1A.2): an entry whose first line starts with
    // `%`, `.`, `?` or `:` is a command, never C — except `.` and a digit,
    // `::` and `%:`. The first character is the MODE: `%name` and `.name`
    // are the REPL's commands (each mode with its own aliases: cling's
    // `.L`, plan §7f), `?NAME` is %pinfo NAME, `??NAME` %pinfo2 NAME and
    // `?` alone %help (IPython), and `:` is colon mode — ed / ex / vim commands, the host's,
    // which the session refuses. A typed name becomes its row once, at
    // input. The codes' one text is <bits/session_enums>, the dialect's too.
    typedef ::madc::session_command Command;
    typedef ::madc::command_mode Mode;
    // What the pinfo command shows of a name, its row's detail (IPython's
    // _inspect levels): info (%pinfo, ?NAME), info and source (%pinfo2,
    // ??NAME), source alone (%psource).
    enum class Inspect : unsigned char { info, info_source, source };
    // What `text` is as an entry (a host asks before it offers the entry:
    // colon mode is its own): false when it is no command; else its mode,
    // the name typed (`L`; `pinfo`, `pinfo2` or `help` for `?NAME`, `??NAME`
    // and `?`; empty for
    // a `:` text, and when no name follows `%` or `.`), the BUILT-IN command
    // it names (none: none — a registered one, or a typo) and the rest of
    // its first line after the name (after the `:` for colon mode), without
    // the blanks before it.
    static bool command_of(const std::string &text, Mode &mode, std::string &word,
			   Command &code, std::string &argument);

    // The registry's extension point (split arc 1A.2; design
    // docs/plans/2026-10-09-command-registry-design.md): a REPL command
    // registered by a module running IN the session (its handler runs here)
    // or by the HOST driving the session (taken, it answers the `command`
    // payload with the row's code: the host runs it). A registered name is
    // reachable as `%name` and `.name`, listed by %help and completed. False
    // when `name` is no whole command name, or is a built-in's, an alias's
    // or a registered row's already (first registration wins).
    //
    // A handler: true — `out`, a string, is what the command shows; false —
    // the command is refused, and `out`, a non-empty string, is its error.
    // It runs inside the Program's runtime scope (a JIT'd dialect function,
    // a native module's alike).
    typedef bool (*CommandHandler)(madc::value &out, const char *argument);
    // `name` can be registered as far as the table itself says: a whole
    // command name (a word run not starting with a digit), no built-in's and
    // no alias's in any mode. (A session also refuses its registered names.)
    static bool registrable_name(const std::string &name);
    bool add_command(const std::string &name, const std::string &usage,
		     const std::string &summary, CommandHandler handler);
    bool add_host_command(const std::string &name, const std::string &usage,
			  const std::string &summary, int64_t code);
    // A module's rows (split arc 1B.10, madc_module_commands.h): each that
    // registers (add_command) as a session row; the count registered.
    // `rows_of` NULL brings none. The session calls it for each module that
    // enters its Program (register_module_commands); a test hands its own.
    size_t add_module_commands(madc_module_commands_fn rows_of);
    // The host's rows replaced by `rows` ({name, usage, summary, code} each;
    // the wire's `commands` request, sent after every backend start). A row
    // that cannot register is skipped; the count registered is returned.
    size_t set_host_commands(const madc::value &rows);
    // What the last taken command asks its host (IPython's payloads; the
    // codes are <bits/session_enums>' session_payload), with payload_argv()
    // — the file, then its arguments. A host that honors them says so with
    // host_honors_payloads(true) (the backend server: its clients read the
    // file, an IDE's open buffer as its live text, and only a client can
    // start a FRESH session for %run, D16). With any other host the session
    // loads %load's and %run -i's file itself, and %run refuses, naming
    // %run -i. %call's payload is FILE, then the call's text: the host loads
    // FILE, then asks for the call (call_file). %build's is FILE, then OUT:
    // the host hands FILE's text (an open buffer's) to build_file. %open's
    // and %edit's is FILE, then the line when one is named: an IDE opens
    // its editor there; the terminal runs madc::run_terminal_editor. %quit's payload has no argv;
    // a host that honors no payloads reads %quit from ended(). %reset's
    // (`reset`, no argv) and %std NAME's (`standard`, argv: NAME as `--std=`
    // spells it) ask for a FRESH session, which only such a host can start;
    // any other host refuses them, as %run. %project's (`project`, argv: the
    // open manifest, "" when closed) asks the host to keep the project
    // across restarts (set_project in the fresh session); an IDE opens it.
    madc::session_payload payload() const { return payload_kind; }
    const std::vector<std::string> &payload_argv() const { return payload_args; }
    // The `command` payload's row code (a host row's, add_host_command).
    int64_t payload_code() const { return payload_command; }
    void host_honors_payloads(bool on) { payload_host = on; }
    // The last file loaded defined main (a main an earlier unit defined is
    // not the file's: a second main is refused): only then does running the
    // file run main (run_file, and a host's run after its load).
    bool loaded_main() const { return load_main; }
    // %call's call (cling's .x; plan §7f), once FILE (`path`) is loaded: the
    // function named after FILE (its name without the directory and the last
    // extension), else FILE's main, with the arguments `call` holds — the
    // command's line with everything before its `(` blanked, so a diagnostic
    // cites the column typed. The call is an entry under the command's
    // REPL[N]; its value is shown() and kept by none (D12). FILE's main with
    // no arguments runs as %run -i runs it (argv: FILE; its return value to
    // *status). False when the session defines neither, or the call was
    // refused or stopped.
    bool call_file(const std::string &path, const std::string &call, int *status);
    // %build's build (plan §7f; madcide's Build menu): FILE (`path`) — its
    // `text`, or the file when `text` is empty — compiled to the native
    // executable `out` under the session's standard, through the live-tree
    // build (madc_parse_build over a parse of FILE). Its diagnostics are the
    // Program's, rendered; shown() says what was built. False when it was
    // refused.
    bool build_file(const std::string &path, const std::string &text,
		    const std::string &out);
    // The same build from `text` when non-null — a host's buffer, an empty
    // one included — else the file (split arc 1B.8).
    bool build_text(const std::string &path, const std::string *text,
		    const std::string &out);
    // %check's check (split arc 1B.8, design decision 2): FILE (`path`) — its
    // `text` when non-null, else the file — parsed under the session's
    // standard in a parse of its own (nothing joins the session), its
    // diagnostics rendered as an entry's are; shown() is "FILE: clean" or
    // "FILE: N errors, M warnings". False when no parse opened (an unreadable
    // FILE).
    bool check_text(const std::string &path, const std::string *text);
    // The names the session defined (plan §41.11a step 3d): the one owner
    // %whos and the bindings wire op read. A row per object and function a
    // session unit (an entry, a loaded file) defined, sorted by name:
    // {name, kind (a madc::name_kind code), type (the source's spelling),
    // value (an object's, the show's row form: no type spelled, no pointer
    // followed but a character pointer's text through the fault-safe copy,
    // `0x… "Test"`, at most 16 elements of an aggregate, 80 columns), file,
    // line}. The values come from one quiet entry, which takes no number,
    // keeps no result and says nothing; its module stays loaded, as every
    // entry's does.
    void bindings(madc::value &rows);
    // The entries linked into the live context.
    unsigned entries() const { return entry_count; }
    // The entries submitted, refused ones included: entry N is REPL[N].
    unsigned submitted() const { return submit_count; }
    // The history store %history -l / -g read (madc_history.h; split arc
    // 1B.7): the one its host names ("" names none), else the default store.
    void set_history_file(const std::string &path) { history_file = path; }
    // The project %project opened (split arc 1B.8), its manifest's path ("":
    // none) — a host restores it in a fresh session.
    void set_project(const std::string &path) { project_file = path; }
    const std::string &project() const { return project_file; }

private:
    Offered enter(const std::string &text, bool final, const TakenHook &taken);
    // The recorded diagnostics nothing has rendered yet, rendered.
    void render_pending_diagnostics();
    // A command entry's run: its output goes to command_output. False when
    // it is refused (its diagnostics are the Program's).
    bool run_command(const std::string &text, const std::string &name);
    bool type_command(const std::string &expression, const std::string &name);
    // `%pinfo NAME` / `?NAME`, `%pinfo2 NAME` / `??NAME` and `%psource NAME`:
    // IPython's one inspector at its detail levels (`command`, the row's
    // name, cites a refusal).
    bool pinfo_command(const std::string &argument, const std::string &name,
		       Inspect detail, const char *command);
    // The source of each definition of `word` the session holds (%psource,
    // ??NAME; IPython's inspect.getsource): its lines from its unit's text —
    // REPL[N]'s entry, or the file it was loaded from as the host's buffer
    // of it holds it, else the disk — one after another. False when none.
    bool binding_source(const std::string &word, std::string &out);
    void whos_command();
    // %load FILE and %run [-i] FILE [ARGS] (D25, D16): ARGS split by the
    // shell's rules (ns_common::shell_words).
    bool load_command(const std::string &argument, const std::string &name);
    bool run_file_command(const std::string &argument, const std::string &name);
    bool build_command(const std::string &argument, const std::string &name);
    bool call_command(const std::string &argument, const std::string &name);
    bool open_command(const std::string &argument, const std::string &name);
    bool edit_command(const std::string &argument, const std::string &name);
    // FILE in its host's editor at `line` (0: none): the `open` payload,
    // else madc::run_terminal_editor.
    bool open_in_editor(const std::string &path, int line, const std::string &name);
    bool quit_command(const std::string &argument, const std::string &name);
    bool history_command(const std::string &argument, const std::string &name);
    bool reset_command(const std::string &argument, const std::string &name);
    bool std_command(const std::string &argument, const std::string &name);
    bool check_command(const std::string &argument, const std::string &name);
    bool args_command(const std::string &argument, const std::string &name);
    bool project_command(const std::string &argument, const std::string &name);
    void project_changed();
    bool project_show(const std::string &name);
    bool project_check(const std::string &name);
    bool project_build(const std::string &out, const std::string &name);
    bool project_run(const std::vector<std::string> &args, const std::string &name);
    // A session unit (an entry, a loaded file) defines a function NAME.
    bool defines_function(const std::string &name);
    // A file command's file, once it opens: the payload `kind` for a host
    // that honors payloads, else loaded (and run) here.
    bool file_command(madc::session_payload kind, std::vector<std::string> &words);
    // Does a buffer of the host's hold FILE's live text (the buffer
    // question)? A host that honors payloads reads such a FILE from its
    // buffer, so the disk need not have it yet (an LSP client's unsaved new
    // file).
    bool buffer_holds(const std::string &file);
    // A command's output begins: the entry was a command, and nothing it
    // shows yet (no text, no code).
    void begin_command_output();
    // The REPL commands of each module the Program has loaded and this
    // session has not looked at yet (split arc 1B.10, design §2.3): the
    // configured Program's (-l, the ini's modules) at begin, a kept unit's
    // imports after it. A module whose map row names `commands` is opened (a
    // LAZY one here, as its first call would; under --no-auto-load never)
    // and its rows added; one that does not open brings none.
    void register_module_commands();
    // A command's usage error, cited at the entry's first column.
    void command_error(const std::string &message, const std::string &name);
    // The quiet entry: each object shown through the row form, the texts in
    // order. False when it did not run (then the texts are fewer).
    bool show_rows(const std::vector<Variable *> &objects,
		   std::vector<std::string> &texts);
    std::unique_ptr<Program> prog;
    std::unique_ptr<CirJitSession> jit;
    unsigned entry_count;
    unsigned submit_count;
    std::string command_output;		// the last command's output
    std::vector<madc::code_range> command_code;	// its ranges that are code
    std::vector<madc::code_range> no_code;	// an entry's shown value: none
    bool showed_command;		// the last entry was a command
    std::set<madc::dis::istring> modules_seen;	// register_module_commands' spellings
    madc::session_payload payload_kind;	// the last command's ask of its host
    std::vector<std::string> payload_args;
    int64_t payload_command;		// the `command` payload's row code
    bool payload_host;			// the host honors payloads
    // The registered rows (split arc 1A.2), in registration order: a
    // session row has a handler, a host row a code.
    struct Registered
    {
	std::string name, usage, summary;
	CommandHandler handler;		// NULL: a host row
	int64_t code;
    };
    std::vector<Registered> registered;
    bool add_row(const std::string &name, const std::string &usage,
		 const std::string &summary, CommandHandler handler, int64_t code);
    // A registered row's run: a session row's handler, or a host row's
    // payload. False when it was refused.
    bool run_registered(const Registered &row, const std::string &argument,
			const std::string &name);
    bool load_main;			// the last loaded file defined main
    bool quit_asked;			// %quit ended the session
    unsigned quiet_count;		// the quiet entries' units, each its own
    std::vector<std::string> inputs;	// REPL[N]'s text is inputs[N - 1]
    // The host's REPL buffer as entries (buffer_edited): the text of each
    // entry and file the session kept, in the order the host appended them.
    std::vector<std::string> buffer_entries;
    // A kept unit: one of the session's from here on.
    void unit_linked(const std::string &name);
    // Is `text` a whole entry, as the REPL groups typed lines? `extendable`:
    // an if ended it, so a following `else` line continues it (D11). The
    // trial leaves the session as it was.
    bool entry_whole(const std::string &text, bool &extendable);
    // The entries lines [from, to) hold, each [first, last + 1).
    std::vector<std::pair<size_t, size_t> >
	buffer_split(const std::vector<std::string> &lines, size_t from, size_t to);
    // An entry of the edited REPL buffer read as definitions only;
    // `functions_only`: read again for a type's new version, an entry that
    // declares an object is left as it was (true, nothing shown).
    bool redefine(const std::string &text, bool functions_only = false);
    std::string kept_source;		// taken_source()
    std::string unit_text;		// loaded_source()
    std::string history_file;		// %history's store ("" none)
    std::string project_file;		// %project's manifest ("" none)
    BufferSource buffers;		// the host's (set_buffers); empty: the disk
    Editor editor;			// the host's (set_editor); empty: the terminal's
    InteractiveSession(const InteractiveSession &);
    InteractiveSession &operator=(const InteractiveSession &);
};

namespace madc {
// The terminal's editor (%open, %edit; IPython's %edit): $EDITOR (its words
// split by the shell's rules), else — only when stdin is a terminal — vi
// (Microsoft Edit, `edit`, on Windows), given `+LINE` before `path` when
// `line` > 0 (never the Windows default), on this terminal, waited for.
// False when it did not start or no default may run (`why` says so).
bool run_terminal_editor(const std::string &path, int line, std::string &why);
// The editor a session's host gave it (ReplSession::set_editor), else the
// terminal's: the ONE choice behind %open and %edit, in process and in a
// backend's host.
bool show_in_editor(const ReplSession::Editor &editor, const std::string &path,
		    int line, std::string &why);
// <ns_madc>'s command_add (split arc 1A.2): a REPL command registered by
// code running IN a session (a module's own registration) — the running
// Program's session (Program::owning_session) adds it as a session row
// (InteractiveSession::add_command). False outside a session, or for a name
// that cannot register.
bool command_add(const char *name, const char *usage, const char *summary,
		 bool (*handler)(value &out, const char *argument));
// THE buffer rule (split arc 1B.8's BufferSource contract), asked by a host
// before it sends a payload's request or by the session for a command running
// in it: `source` answers when there is one; with none, or for a FILE no
// buffer holds, the name is the path and the disk is read (has_text false).
// False, with `why`, when there is nothing to read: no FILE and no current
// buffer, an untitled one.
bool resolve_buffer(const ReplSession::BufferSource &source,
		    const dis::istring &name, dis::istring &path,
		    std::string &text, bool &has_text, std::string &why);
// The running session's buffer question (InteractiveSession::buffer_text):
// what a module's command reads for FILE ("" the current buffer). Outside a
// session, the thread's host buffers (set_host_buffers), else the disk's (no
// FILE: refused).
bool session_buffer(const std::string &name, std::string &path,
		    std::string &text, bool &has_text, std::string &why);
// The buffers a program that holds its own (an editor calling a module's verb
// in its process) hands to session_buffer outside a session; an empty source
// is the disk's. Per thread: the calling thread's questions only.
void set_host_buffers(const ReplSession::BufferSource &source);
// <ns_madc>'s face of it: {path, text, has_text}, or {error}.
bool session_buffer(value &out, const char *name);
// <ns_madc>'s host_documents: the thread's host buffers are ui world `w`'s
// documents (document_buffer), `current` the current one; w 0 = the disk's.
// False for a world handle that names no world.
bool host_documents(int64_t w, int64_t current);
// An entry's unit name — Julia's REPL[N], the one spelling an entry's
// diagnostics cite, a result's description and a binding's origin name —
// and its inverse: the entry number a unit name spells, 0 for any other
// unit (a file's).
std::string session_entry_unit(size_t entry);
size_t session_entry_of_unit(const std::string &unit);
}

#endif
