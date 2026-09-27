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

class Program;
class CirJitSession;

class InteractiveSession
{
public:
    InteractiveSession();
    // Adopt a Program its host configured: the command line's options and
    // madc.ini hold in the session as in a file (plan §41.5a, D20).
    explicit InteractiveSession(std::unique_ptr<Program> configured);
    ~InteractiveSession();

    // Start the session. `std_option` is a `--std=` spelling; empty keeps
    // the Program's default standard. False when the session cannot start.
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
    typedef std::function<void()> TakenHook;
    bool submit(const std::string &text, const TakenHook &taken = TakenHook());

    // Offer the text typed so far (plan §41.5a): the classifier runs inside
    // the entry transaction (§41.1a), so there is one parse per line.
    //   Taken: the entry is final — numbered, its parse's diagnostics
    //     rendered, and submitted as submit() would (ok is its result);
    //   Incomplete: keep reading; nothing is kept, numbered or rendered;
    //   Extendable: a finished if with no else; D11 waits one line (the
    //     client submits it, or offers it again with the line).
    // `taken` is submit()'s, called only when the entry is Taken.
    enum class OfferState : unsigned char { Taken, Incomplete, Extendable };
    struct Offered
    {
	OfferState state;
	bool ok;
    };
    Offered offer(const std::string &text, const TakenHook &taken = TakenHook());

    // Load a program file into the session (plan §41.5a, slice 2; the core
    // of %load, D25): one unit in its own grammar (a file is read as gcc
    // reads it), on everything the session declared. The file and the
    // session are one unit, so its statics are session names. Its
    // diagnostics cite its path; it takes no REPL[N]. False when it is
    // refused, and then it leaves nothing (plan §41.3).
    bool load(const std::string &path);
    // Call the session's main(argc, argv) at the entry boundary (the rest of
    // %run, D16/D25; `madc -i file`, python -i). False when there is no main,
    // or a use of an undefined name returned; its return value goes to
    // *status. It does not end the session.
    bool run_main(int argc, char **argv, int *status);

    // Tab's question (plan §41.7a, slice 3): the names that complete the
    // word before `caret` in the text typed so far, sorted; the word starts
    // at `start`. The query leaves the session as it was, and a word in a
    // string or a comment completes nothing.
    std::vector<std::string> complete(const std::string &text, size_t caret,
				      size_t &start);

    // The live address of a session function / global, by emitted name.
    // NULL when no linked entry defines it.
    void *function(const char *name);
    void *data(const char *name);

    Program &program() { return *prog; }
    // The last entry's shown value (D10, plan §41.4a): an entry whose final
    // statement omits its `;` shows it, in re-enterable syntax (`30`,
    // `"abc"`, `(int *) 0x7ffd...`). Empty when the entry showed nothing. The
    // core renders nothing: a client prints it.
    const std::string &shown() const;
    // The entries linked into the live context.
    unsigned entries() const { return entry_count; }
    // The entries submitted, refused ones included: entry N is REPL[N].
    unsigned submitted() const { return submit_count; }

private:
    Offered enter(const std::string &text, bool final, const TakenHook &taken);
    void render_parse_diagnostics();
    std::unique_ptr<Program> prog;
    std::unique_ptr<CirJitSession> jit;
    unsigned entry_count;
    unsigned submit_count;
    InteractiveSession(const InteractiveSession &);
    InteractiveSession &operator=(const InteractiveSession &);
};

#endif
