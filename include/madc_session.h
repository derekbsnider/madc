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

#include <memory>
#include <string>

class Program;
class CirJitSession;

class InteractiveSession
{
public:
    InteractiveSession();
    ~InteractiveSession();

    // Start the session. `std_option` is a `--std=` spelling; empty keeps
    // the Program's default standard. False when the session cannot start.
    bool begin(const std::string &std_option = std::string());

    // Submit one complete entry. Its declarations persist, its module is
    // linked into the live context, and its statements run once (D25). False
    // when the entry is refused; its diagnostics are program().diagnostics.
    // A refused entry leaves the live context as it was, and its
    // definitions are never defined by a later entry (plan §41.3).
    bool submit(const std::string &text);

    // The live address of a session function / global, by emitted name.
    // NULL when no linked entry defines it.
    void *function(const char *name);
    void *data(const char *name);

    Program &program() { return *prog; }
    // The entries linked into the live context.
    unsigned entries() const { return entry_count; }

private:
    bool run_entry();
    bool refuse();
    std::unique_ptr<Program> prog;
    std::unique_ptr<CirJitSession> jit;
    unsigned entry_count;
    InteractiveSession(const InteractiveSession &);
    InteractiveSession &operator=(const InteractiveSession &);
};

#endif
