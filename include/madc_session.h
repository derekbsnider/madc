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

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "madc/bits/session_enums"	// offer_state: the verdict's one enum text
#include "libmadc/value.h"		// madc::value: a bindings row

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
    virtual std::vector<std::string> complete(const std::string &text,
					      size_t caret, size_t &start) = 0;
    // Does LINE continue an if that ended an entry (D11)? Its first word is
    // the standard's `else`.
    virtual bool continues_if(const std::string &line) = 0;
    // Load a program file and call its main when it defines one, with argv
    // (the path, then the program's arguments): `madc -i file`, the core of
    // %run (D25). False when the file was refused.
    virtual bool run_file(int argc, char **argv) = 0;
    // The session ended, and its status: an exit(n) in an entry ended a
    // backend's process (in this process the exit already happened), or
    // %quit asked to end it (status 0).
    virtual bool ended(int &status) const { (void)status; return false; }
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
    std::string standard_name() override;
    bool continues_if(const std::string &line) override;
    // %quit ended the session (plan §7f): status 0.
    bool ended(int &status) const override;

    // The session's commands (plan §41.8a, D13/D24): an entry whose first
    // line starts with `%name`, `:name` or `.name` is a command, never C.
    // The typed name, a command's or an alias's (cling's `.L`, plan §7f),
    // becomes one of these codes once, at input. `?NAME` is %pinfo NAME, and
    // `?` alone %help (IPython).
    enum class Command : unsigned char { help, type, pinfo, whos, load, run, quit };
    // What the last taken command asks its host (IPython's payloads; the
    // codes are <bits/session_enums>' session_payload), with payload_argv()
    // — the file, then its arguments. A host that honors them says so with
    // host_honors_payloads(true) (the backend server: its clients read the
    // file, an IDE's open buffer as its live text, and only a client can
    // start a FRESH session for %run, D16). With any other host the session
    // loads %load's and %run -i's file itself, and %run refuses, naming
    // %run -i. %quit's payload has no argv; a host that honors no payloads
    // reads %quit from ended().
    madc::session_payload payload() const { return payload_kind; }
    const std::vector<std::string> &payload_argv() const { return payload_args; }
    void host_honors_payloads(bool on) { payload_host = on; }
    // The last file loaded defined main (a main an earlier unit defined is
    // not the file's: a second main is refused): only then does running the
    // file run main (run_file, and a host's run after its load).
    bool loaded_main() const { return load_main; }
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

private:
    Offered enter(const std::string &text, bool final, const TakenHook &taken);
    // The recorded diagnostics nothing has rendered yet, rendered.
    void render_pending_diagnostics();
    // A command entry's run: its output goes to command_output. False when
    // it is refused (its diagnostics are the Program's).
    bool run_command(const std::string &text, const std::string &name);
    bool type_command(const std::string &expression, const std::string &name);
    bool pinfo_command(const std::string &argument, const std::string &name);
    void whos_command();
    // %load FILE and %run [-i] FILE [ARGS] (D25, D16): ARGS split by the
    // shell's rules (ns_common::shell_words).
    bool load_command(const std::string &argument, const std::string &name);
    bool run_file_command(const std::string &argument, const std::string &name);
    bool quit_command(const std::string &argument, const std::string &name);
    // A file command's file, once it opens: the payload `kind` for a host
    // that honors payloads, else loaded (and run) here.
    bool file_command(madc::session_payload kind, std::vector<std::string> &words);
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
    bool showed_command;		// the last entry was a command
    madc::session_payload payload_kind;	// the last command's ask of its host
    std::vector<std::string> payload_args;
    bool payload_host;			// the host honors payloads
    bool load_main;			// the last loaded file defined main
    bool quit_asked;			// %quit ended the session
    unsigned quiet_count;		// the quiet entries' units, each its own
    InteractiveSession(const InteractiveSession &);
    InteractiveSession &operator=(const InteractiveSession &);
};

#endif
