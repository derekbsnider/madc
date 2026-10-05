/* madc_session_interrupt.h — the session interrupt's platform halves (D8,
 * plan docs/plans/madc-repl-thonny-plan-2026-09-24.md §41.12a).
 *
 * The interrupt itself is the session runtime's (madc_cir.cpp:
 * madc_session_interrupt_raise marks one pending, the entry's loops poll it
 * and return to the entry's boundary). This unit only carries it between
 * processes, one owner per platform:
 *   - POSIX: SIGINT. The client signals the backend's pid; a terminal's
 *     Ctrl-C reaches the backend already (the host ignores it while an entry
 *     runs).
 *   - Windows (no signals): an auto-reset event named from the session's
 *     token, which the client sets and a watcher thread in the backend waits
 *     on; and the console's Ctrl+C for a backend on the host's terminal.
 * A second interrupt before the entry polled the first takes the default:
 * the backend ends, and the client starts a fresh one.
 *
 * Its own unit because the Windows half needs <windows.h>, which units that
 * include madc.h cannot (the `interface` macro).
 *
 * Thread contract: arm_backend runs once, on the backend's session thread,
 * before its first request; the watcher thread touches only the pending
 * flag. A SessionInterruptor is used from the client's thread.
 */
#ifndef __MADC_SESSION_INTERRUPT_H
#define __MADC_SESSION_INTERRUPT_H 1

#include <string>

namespace madc {

class Process;

// The backend's half: take interrupts from now on. `token` names the
// Windows event (the session token the backend connected with); POSIX
// ignores it.
void session_interrupt_arm_backend(const std::string &token);

// The client's half for one backend.
class SessionInterruptor
{
public:
    SessionInterruptor();
    ~SessionInterruptor();
    // Before the backend starts: Windows makes the event `token` names;
    // POSIX needs nothing. False = it could not be made.
    bool open(const std::string &token);
    // Interrupt the backend `p` runs. False = no backend, or no way to reach
    // it.
    bool send(Process &p);
    void close();

private:
    SessionInterruptor(const SessionInterruptor &);
    SessionInterruptor &operator=(const SessionInterruptor &);
    void *event_;
};

// While a child runs in the foreground — a session entry, a fork Run of a
// parse handle, a project run (src/madc_program.cpp) — the terminal's
// interrupt keys are the child's: the host ignores them for the guard's
// lifetime (POSIX SIGINT and SIGQUIT, as system(3) does while its child runs;
// Windows the console's Ctrl+C), as a shell leaves them to its foreground job.
// The one owner of that rule (scripts/check-one-host-interrupt-guard.sh).
class HostIgnoresInterrupt
{
public:
    HostIgnoresInterrupt();
    ~HostIgnoresInterrupt();

private:
    HostIgnoresInterrupt(const HostIgnoresInterrupt &);
    HostIgnoresInterrupt &operator=(const HostIgnoresInterrupt &);
    void *saved_;
};

} // namespace madc

#endif
