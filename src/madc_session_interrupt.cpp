// madc_session_interrupt.cpp — the session interrupt's platform halves (D8,
// plan docs/plans/madc-repl-thonny-plan-2026-09-24.md §41.12a); the contract
// is include/madc_session_interrupt.h's. The interrupt itself is the session
// runtime's (madc_cir.cpp): this unit only carries it between processes.

#include <signal.h>
#include <string>

#include "madc_session_interrupt.h"
#include "madcdis/process.h"

bool madc_session_interrupt_raise();	// madc_cir.cpp (madc_cir.h needs madc.h)

#if defined(_WIN32)
#include <windows.h>

namespace {

// The event's name: the session's token, which only the client and the
// backend it started hold.
std::wstring event_name(const std::string &token)
{
    std::wstring w = L"Local\\madc-session-interrupt-";
    for ( size_t i = 0; i < token.size(); ++i )
	w += (wchar_t)(unsigned char)token[i];
    return w;
}

// One interrupt in the backend: pending, or — one already pending — the
// default, as a second Ctrl+C takes it (the process ends with the console's
// interrupt status; the client starts a fresh backend).
void backend_interrupt()
{
    if ( madc_session_interrupt_raise() )
	TerminateProcess(GetCurrentProcess(), STATUS_CONTROL_C_EXIT);
}

DWORD WINAPI watch_event(LPVOID ev)
{
    while ( WaitForSingleObject((HANDLE)ev, INFINITE) == WAIT_OBJECT_0 )
	backend_interrupt();
    return 0;
}

// A backend on the host's terminal: the console's Ctrl+C reaches every
// process attached to it, this one included.
BOOL WINAPI on_console_ctrl(DWORD kind)
{
    if ( kind != CTRL_C_EVENT )
	return FALSE;
    backend_interrupt();
    return TRUE;
}

} // namespace

namespace madc {

void session_interrupt_arm_backend(const std::string &token)
{
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    if ( token.empty() )
	return;
    HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, event_name(token).c_str());
    if ( ev == NULL )
	return;
    HANDLE th = CreateThread(NULL, 0, watch_event, ev, 0, NULL);
    if ( th == NULL )
	CloseHandle(ev);
    else
	CloseHandle(th);	// it runs for the backend's life
}

SessionInterruptor::SessionInterruptor() : event_(NULL) {}

SessionInterruptor::~SessionInterruptor()
{
    close();
}

bool SessionInterruptor::open(const std::string &token)
{
    close();
    event_ = CreateEventW(NULL, FALSE, FALSE, event_name(token).c_str());
    return event_ != NULL;
}

bool SessionInterruptor::send(Process &p)
{
    (void)p;
    return event_ != NULL && SetEvent((HANDLE)event_) != 0;
}

void SessionInterruptor::close()
{
    if ( event_ != NULL )
	CloseHandle((HANDLE)event_);
    event_ = NULL;
}

HostIgnoresInterrupt::HostIgnoresInterrupt() : saved_(NULL)
{
    SetConsoleCtrlHandler(NULL, TRUE);	// this process ignores Ctrl+C
}

HostIgnoresInterrupt::~HostIgnoresInterrupt()
{
    SetConsoleCtrlHandler(NULL, FALSE);
}

} // namespace madc

#else

namespace {

// One interrupt in the backend: pending, or — one already pending — the
// default action, as a second Ctrl-C takes it (the backend ends; the client
// starts a fresh one). Async-signal-safe: a flag, signal(2), raise(3).
void on_sigint(int)
{
    if ( madc_session_interrupt_raise() )
    {
	signal(SIGINT, SIG_DFL);
	raise(SIGINT);
    }
}

} // namespace

namespace madc {

void session_interrupt_arm_backend(const std::string &)
{
    struct sigaction sa;
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    // Restarted: the backend's own wait for its next request must not end on
    // an interrupt sent while no entry runs. A program's sleep still returns
    // early (nanosleep is never restarted) and its loop polls; a read the
    // program blocks in waits for the second interrupt.
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
}

SessionInterruptor::SessionInterruptor() : event_(NULL) {}

SessionInterruptor::~SessionInterruptor() {}

bool SessionInterruptor::open(const std::string &)
{
    return true;
}

bool SessionInterruptor::send(Process &p)
{
    return p.interrupt();
}

void SessionInterruptor::close() {}

// The terminal's interrupt keys: Ctrl-C (SIGINT) and Ctrl-\ (SIGQUIT), the
// two system(3) ignores while its child runs.
HostIgnoresInterrupt::HostIgnoresInterrupt() : saved_(new struct sigaction[2])
{
    struct sigaction ign;
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    ign.sa_flags = 0;
    struct sigaction *saved = (struct sigaction *)saved_;
    sigaction(SIGINT, &ign, &saved[0]);
    sigaction(SIGQUIT, &ign, &saved[1]);
}

HostIgnoresInterrupt::~HostIgnoresInterrupt()
{
    struct sigaction *saved = (struct sigaction *)saved_;
    sigaction(SIGINT, &saved[0], NULL);
    sigaction(SIGQUIT, &saved[1], NULL);
    delete[] saved;
}

} // namespace madc

#endif
