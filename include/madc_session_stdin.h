/* madc_session_stdin.h — Send EOF's platform halves (plan
 * docs/plans/madc-repl-thonny-plan-2026-09-24.md §41.12a).
 *
 * Send EOF ends the backend's stdin: the client closes the pipe's write end,
 * so the running program's read returns its end once what was written before
 * it is read. The next entry reads a fresh pipe: Process::renew_stdin makes
 * it, and this unit hands its read end to the backend, one owner per
 * platform:
 *   - POSIX: the fd crosses a socketpair made with the backend (SCM_RIGHTS),
 *     beside the request wire; the request that follows names nothing.
 *   - Windows: renew_stdin duplicates the read end into the backend, and the
 *     request names its handle value there.
 * The backend takes it when it reads that request — after the running entry
 * ended, since it reads requests in order — and the fresh end becomes fd 0.
 *
 * Its own unit because the Windows half needs <windows.h>, which units that
 * include madc.h cannot (the `interface` macro).
 *
 * Thread contract: a SessionStdinHandOff is used from the client's thread;
 * session_stdin_take runs on the backend's session thread, between entries.
 */
#ifndef __MADC_SESSION_STDIN_H
#define __MADC_SESSION_STDIN_H 1

#include <stdint.h>

namespace madc {

class Process;

// The client's half for one backend.
class SessionStdinHandOff
{
public:
    SessionStdinHandOff();
    ~SessionStdinHandOff();
    // Before the backend starts: POSIX makes the socketpair, whose
    // backend_end() the fork child keeps; Windows needs nothing. False = it
    // could not be made.
    bool open();
    // The fork child's end of the socketpair (POSIX); -1 on Windows.
    int backend_end() const;
    // The backend started: the client closes its copy of the backend's end.
    void started();
    // In the fork child: it keeps the backend's end alone.
    void in_backend();
    // End the stdin of the backend `p` runs and hand it a fresh one.
    // `handle` is what the request names: Windows, the fresh read end's
    // handle in the backend; POSIX, 0. False = `p` has no piped stdin, or
    // the hand-off failed (then no request may follow).
    bool renew(Process &p, uint64_t &handle);
    void close();

private:
    SessionStdinHandOff(const SessionStdinHandOff &);
    SessionStdinHandOff &operator=(const SessionStdinHandOff &);
    int client_end_;
    int backend_end_;
};

// The backend's half: the fresh stdin the request names becomes fd 0 (the
// backend clears stdin's end-of-file state as it takes up its next request).
// `socket_end` is
// the POSIX socketpair's backend end (-1 on Windows); `handle` the
// request's.
bool session_stdin_take(int socket_end, uint64_t handle);

} // namespace madc

#endif
