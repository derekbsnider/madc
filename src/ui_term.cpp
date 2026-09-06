///////////////////////////////////////////////////////////////////////////
//								       //
// ui_term — the built-in VT100/xterm TUI target (Track 7.2 R5)	       //
//								       //
// The byte-moving half of the level-1 renderer: raw-mode termios, the //
// alternate screen, CUP/SGR escape emission for the model's dirty     //
// rows, and batched key reads feeding the model's escape parser. All  //
// intelligence (layout, focus, key semantics, diffing) lives in       //
// madcdis/tui_model.h; everything here is I/O.			       //
//								       //
// Hand-rolled by owner decision (2026-08-25) over vendoring ncurses / //
// termbox2 / notcurses: zero dependencies, no terminfo — the VT100+   //
// escape family every modern terminal speaks (the design doc's own    //
// level-1 target list). The escape tables and the bare-ESC timeout    //
// idea are cross-checked against termbox2 and ncurses (ESCDELAY).     //
// Richer targets register beside this one through the same seam.      //
//								       //
// Two platform bodies, ONE byte language: the POSIX body (termios +   //
// poll + SIGWINCH) and the _WIN32 body (console modes + VT processing //
// — owner direction 2026-08-27: Win10+ VT mode, never the DOS-era     //
// console-cell API). Everything that BUILDS bytes — the SGR table,    //
// CUP, the paint-plan emission, the ESC-grace drain policy — lives    //
// once in the shared region below; each body owns only its raw-mode   //
// bookkeeping and wait/read plumbing.				       //
//								       //
// THREAD-SAFETY CONTRACT: one live target per process (it owns THE    //
// terminal), confined to the opening thread. The atexit/live-pointer  //
// pair below exists so a script that forgets tui_close still gets its //
// terminal back at process exit.				       //
//								       //
///////////////////////////////////////////////////////////////////////////

#include "madcdis/tui_provider.h"

#include <cstdio>
#include <string>
#include <vector>

///////////////////////////////////////////////////////////////////////////
// Shared VT byte-building — one implementation, every platform body.    //
///////////////////////////////////////////////////////////////////////////

namespace {

using madc::hub::tui_grid;
using madc::hub::tui_attr;
using madc::hub::tui_keyev;
using madc::hub::tui_key;
using madc::hub::tui_keyparse;
using madc::hub::tui_paint_plan;
using madc::hub::tui_diff_plan;

// Grid-mode entry/exit byte streams: alternate screen, clear, home,
// cursor hidden / SGR reset, cursor shown, primary screen.
const char VT_ENTER_GRID[] = "\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l";
const char VT_LEAVE_GRID[] = "\x1b[0m\x1b[?25h\x1b[?1049l";

void cup(std::string &out, size_t row, size_t col)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%zu;%zuH", row + 1, col + 1);
	out += buf;
}

// THE style->SGR table (AST-2; owner: VT-102 ANSI / JOE parity). A
// non-normal transition RESETS and then sets the target style's full
// parameter list — attributes 1/2/3/4/5/7, fg 30+c, bg 40+c;
// bold-as-bright supplies the 16-colour foreground model (VT-102 /
// 8-colour terminals brighten on bold, exactly JOE's behavior; no
// aixterm 90–97). The historical spellings survive: pure inverse
// entered from normal emits \x1b[7m and any->normal emits \x1b[0m,
// so a grid using only normal/reverse produces the byte stream it
// always did.
void emit_sgr(std::string &out, tui_attr from, tui_attr to)
{
	if ( to.is_normal() )
	{
	    out += "\x1b[0m";
	    return;
	}
	if ( !from.is_normal() )
	    out += "\x1b[0m";
	if ( to.is_reverse() )
	{
	    out += "\x1b[7m";
	    return;
	}
	std::string params;
	char buf[8];
	if ( to.flags & tui_attr::BOLD )	params += "1;";
	if ( to.flags & tui_attr::DIM )		params += "2;";
	if ( to.flags & tui_attr::ITALIC )	params += "3;";
	if ( to.flags & tui_attr::UNDERLINE )	params += "4;";
	if ( to.flags & tui_attr::BLINK )	params += "5;";
	if ( to.flags & tui_attr::INVERSE )	params += "7;";
	if ( to.fg )
	{
	    snprintf(buf, sizeof(buf), "%d;", 29 + (int)to.fg);
	    params += buf;
	}
	if ( to.bg )
	{
	    snprintf(buf, sizeof(buf), "%d;", 39 + (int)to.bg);
	    params += buf;
	}
	if ( params.empty() )
	    return;			// unreachable: normal handled above
	params.erase(params.size() - 1);	// the trailing ';'
	out += "\x1b[";
	out += params;
	out += 'm';
}

// The whole paint plan as one byte string (the platform body just
// writes it): diff, DECSTBM+DL/IL scroll of a shifted band, per-span
// SGR-tracked cell emission with the EL tail, final cursor placement.
std::string vt_paint_bytes(const tui_grid &prev, const tui_grid &next)
{
	std::string out;
	out += "\x1b[?25l";	// hidden while rows repaint
	tui_paint_plan plan = tui_diff_plan(prev, next);
	if ( plan.shifted )
	{
	    // The model found a vertical shift: scroll the band with
	    // DECSTBM + DL/IL (VT100-core, JOE's dl/al) and repaint only
	    // the rows the plan lists. SGR reset first — the blank lines
	    // DL/IL insert take the CURRENT attributes.
	    char buf[32];
	    snprintf(buf, sizeof(buf), "\x1b[0m\x1b[%zu;%zur",
		     plan.top + 1, plan.bot + 1);
	    out += buf;
	    cup(out, plan.top, 0);
	    snprintf(buf, sizeof(buf), "\x1b[%zu%c\x1b[r",
		     plan.delta, plan.up ? 'M' : 'L');
	    out += buf;
	}
	for ( size_t i = 0; i < plan.spans.size(); ++i )
	{
	    const madc::hub::tui_row_span &s = plan.spans[i];
	    cup(out, s.row, s.c0);
	    // A span whose tail reaches into the row's normal-space run is
	    // finished by one EL; cells right of the span already match.
	    size_t pe = next.row_paint_end(s.row);
	    bool   el = pe <= s.c1;
	    size_t end = el ? (pe > s.c0 ? pe : s.c0) : s.c1 + 1;
	    tui_attr cur = tui_attr::normal();
	    for ( size_t c = s.c0; c < end; ++c )
	    {
		const madc::hub::tui_cell &cell = next.at(s.row, c);
		if ( cell.attr != cur )
		{
		    emit_sgr(out, cur, cell.attr);
		    cur = cell.attr;
		}
		out += cell.ch;
	    }
	    if ( cur != tui_attr::normal() )
		out += "\x1b[0m";
	    if ( el )
		out += "\x1b[K";	// the normal-space tail, one erase
	}
	if ( next.cursor_visible )
	{
	    cup(out, next.cursor_row, next.cursor_col);
	    out += "\x1b[?25h";
	}
	return out;
}

// The batch-drain + bare-ESC grace policy (§7.5; the termbox2/ESCDELAY
// idea): drain what is immediately available — the batch printable
// coalescing rides — then give a split escape sequence one short grace
// read; an unambiguous batch pays no added latency. ReadyFn(timeout_ms)
// -> readable now; ReadFn(parse, out) -> false when the source ended or
// would block.
template <class ReadyFn, class ReadFn>
void vt_drain_pending(tui_keyparse &parse, std::vector<tui_keyev> &out,
		      ReadyFn input_ready, ReadFn read_some)
{
	for (;;)
	{
	    while ( input_ready(0) )
	    {
		if ( !read_some(parse, out) )
		    break;
	    }
	    if ( !parse.pending() )
		break;
	    if ( !input_ready(25) )
	    {
		parse.flush(out);
		break;
	    }
	}
}

} // namespace (shared VT helpers)

#ifndef _WIN32

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "rt/rt_task.h"	// stage-2: cooperative wait (runnable probe + yield)
#include "madc_task_io.h"	// MT-4c: the host wait (stdin joins the one poll)

namespace {

// SIGWINCH sets a flag; read_keys turns it into a resize key. sa_flags
// carries no SA_RESTART so the blocking poll wakes with EINTR.
volatile sig_atomic_t g_winch = 0;
void winch_handler(int) { g_winch = 1; }

class term_target;
term_target *g_live = 0;	// the one open target, for atexit recovery
pid_t g_live_pid = 0;		// the OPENING process: a fork child that
				// exit()s must not run the parent's
				// inherited atexit recovery (fork-Run)
void close_live_target();

class term_target : public madc::hub::tui_target
{
    struct termios   _saved;
    struct sigaction _saved_winch;
    bool	     _open;
    bool	     _suspended;
    size_t	     _rows, _cols;
    tui_keyparse     _parse;

    static bool query_size(size_t &rows, size_t &cols)
    {
	struct winsize ws;
	if ( ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 )
	    return false;
	rows = ws.ws_row;
	cols = ws.ws_col;
	return true;
    }
    static void emit(const std::string &s)
    {
	size_t off = 0;
	while ( off < s.size() )
	{
	    ssize_t n = write(STDOUT_FILENO, s.data() + off, s.size() - off);
	    if ( n <= 0 )
	    {
		if ( n < 0 && errno == EINTR )
		    continue;
		return;
	    }
	    off += (size_t)n;
	}
    }
    // Poll stdin; <0 timeout blocks. True = a read would make progress
    // NOW — data, EOF, or an error the read surfaces (POLLHUP/POLLERR;
    // the fd_readable_progress rule taskio's io_probe_readable owns —
    // a dead terminal must reach the read that returns 0, never hang
    // the wait). EINTR returns false so the caller re-checks the
    // resize flag.
    static bool input_ready(int timeout_ms)
    {
	struct pollfd pfd;
	pfd.fd = STDIN_FILENO;
	pfd.events = POLLIN;
	int r = poll(&pfd, 1, timeout_ms);
	return r > 0
	    && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    }

public:
    term_target() : _open(false), _suspended(false), _rows(24), _cols(80) {}
    ~term_target() { close(); }

    // Grid-mode entry/exit, shared by open/close and suspend/resume
    // (one implementation — the two pairs differ only in bookkeeping).
    bool enter_grid_mode()
    {
	struct termios raw = _saved;
	// Raw mode by explicit flags (termios(3)); IXON off is the point
	// — ^S/^Q are editor keys, not flow control. ISIG off delivers
	// ^C/^Z as keys; the application owns quitting.
	raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR
			 | ICRNL | IXON);
	raw.c_oflag &= ~OPOST;
	raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	raw.c_cflag &= ~(CSIZE | PARENB);
	raw.c_cflag |= CS8;
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if ( tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0 )
	{
	    fprintf(stderr, "ui: tcsetattr failed\n");
	    return false;
	}
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = winch_handler;
	sigaction(SIGWINCH, &sa, &_saved_winch);
	query_size(_rows, _cols);
	emit(VT_ENTER_GRID);
	return true;
    }

    void leave_grid_mode()
    {
	emit(VT_LEAVE_GRID);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &_saved);
	sigaction(SIGWINCH, &_saved_winch, (struct sigaction *)0);
    }

    virtual bool open(size_t &rows, size_t &cols)
    {
	if ( g_live )
	{
	    fprintf(stderr, "ui: a TUI target is already open\n");
	    return false;
	}
	if ( !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) )
	{
	    fprintf(stderr,
		    "ui: the terminal target needs a tty on stdin/stdout\n");
	    return false;
	}
	if ( tcgetattr(STDIN_FILENO, &_saved) != 0 )
	{
	    fprintf(stderr, "ui: tcgetattr failed\n");
	    return false;
	}
	if ( !enter_grid_mode() )
	    return false;
	_open = true;
	g_live = this;
	g_live_pid = getpid();
	static bool exit_hooked = false;
	if ( !exit_hooked )
	{
	    exit_hooked = true;
	    atexit(close_live_target);
	}
	rows = _rows;
	cols = _cols;
	return true;
    }

    virtual void close()
    {
	if ( !_open )
	    return;
	if ( !_suspended )		// suspended: already restored
	    leave_grid_mode();
	_suspended = false;
	_open = false;
	g_live = 0;
    }

    // Suspend/resume (madcide v2, JOE ^K Z): the terminal goes back to
    // the owner-found state while a child process runs; resume re-raws
    // and re-enters the alternate screen. `_saved` stays the ORIGINAL
    // pre-open state — that is what "restored exactly as found" means,
    // whatever the child did in between.
    virtual bool suspend()
    {
	if ( !_open || _suspended )
	    return false;
	leave_grid_mode();
	_suspended = true;
	return true;
    }

    virtual bool resume()
    {
	if ( !_open || !_suspended )
	    return false;
	if ( !enter_grid_mode() )
	    return false;		// still suspended; caller told
	_suspended = false;
	return true;
    }

    virtual void paint(const tui_grid &prev, const tui_grid &next)
    {
	if ( !_open || _suspended )
	    return;
	emit(vt_paint_bytes(prev, next));	// the shared byte builder
    }

    virtual bool read_keys(std::vector<tui_keyev> &out)
    {
	if ( !_open || _suspended )
	    return false;
	for (;;)
	{
	    if ( g_winch )
	    {
		g_winch = 0;
		size_t r = _rows, c = _cols;
		if ( query_size(r, c) && (r != _rows || c != _cols) )
		{
		    _rows = r;
		    _cols = c;
		    out.push_back(tui_keyev(tui_key::resize));
		    return true;
		}
	    }
	    // Stage-2 cooperative parse + IDE-10b builds: while spawned
	    // tasks are RUNNABLE, never park the OS thread in poll — hand
	    // them the CPU between zero-timeout input polls (each
	    // __madc_yield runs the queue head to its next yield point, so
	    // keystroke latency stays one slice). Every actual run flows
	    // through this branch, so when the queue DRAINS after tasks
	    // ran, synthesize a `wake` event — the application recomposes
	    // (build output repaints without a keystroke). Tasks that are
	    // LIVE but PARKED: this flow parks ON STDIN through taskio
	    // (MT-4c — the host wait), so the scheduler makes ONE blocking
	    // decision over {stdin, io waiters, timers}; a SYNTHETIC wake
	    // (other tasks got the CPU and drained, or EINTR — the resize
	    // check at this loop's head wants the CPU) surfaces as the
	    // same wake event. Zero live tasks = the old blocking wait,
	    // zero new cost.
	    {
		bool ran = false;
		for ( ;; )
		{
		    if ( g_winch || input_ready(0) )
			break;
		    if ( __madc_task_runnable() > 0 )
		    {
			__madc_yield();
			ran = true;
			continue;
		    }
		    if ( ran )
		    {
			out.push_back(tui_keyev(tui_key::wake));
			return true;
		    }
		    if ( __madc_task_live() == 0 )
			break;
		    if ( madc::taskio::host_wait_readable(0)
			 == madc::taskio::host_wake::fired )
			break;			// stdin fired: read below
		    out.push_back(tui_keyev(tui_key::wake));
		    return true;		// synthetic wake (deadline
						// never: unbounded park —
						// EINTR serves resize here)
		}
		if ( g_winch )
		    continue;	// a resize landed mid-drain: handle it first
	    }
	    if ( !input_ready(0) && !input_ready(-1) )
		continue;	// EINTR: re-check the resize flag
	    char buf[64];
	    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
	    if ( n == 0 )
		return false;	// the input source ended
	    if ( n < 0 )
	    {
		if ( errno == EINTR )
		    continue;
		return false;
	    }
	    _parse.feed(buf, (size_t)n, out);
	    vt_drain_pending(_parse, out,
		[](int t) { return input_ready(t); },
		[](tui_keyparse &p, std::vector<tui_keyev> &o) -> bool {
		    char b[64];
		    ssize_t m = read(STDIN_FILENO, b, sizeof(b));
		    if ( m <= 0 )
			return false;
		    p.feed(b, (size_t)m, o);
		    return true;
		});
	    if ( !out.empty() )
		return true;
	}
    }

    virtual void size(size_t &rows, size_t &cols)
    {
	rows = _rows;
	cols = _cols;
    }
};

void close_live_target()
{
    // A fork child (fork-Run) inherits this atexit registration; the
    // terminal belongs to the OPENING process only.
    if ( g_live && getpid() == g_live_pid )
	g_live->close();
}

madc::hub::tui_target *make_term_target()
{
    return new term_target();
}

} // namespace

#else // _WIN32 — the Win10+ VT console twin (owner direction 2026-08-27)

#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>

#include "rt/rt_task.h"	// stage-2: cooperative wait (runnable probe + yield)
#include "madc_task_io.h"	// MT-4c host wait — bounded here (no EINTR axis)

namespace {

// No SIGWINCH on Windows: read_keys polls the window extent at its loop
// head, so every blocking wait is bounded to this cadence — a resize
// with no keystroke still surfaces (recon item 3).
const int RESIZE_POLL_MS = 200;

class term_target;
term_target *g_live = 0;	// the one open target, for atexit recovery
void close_live_target();	// no fork on Windows — the POSIX pid guard
				// holds trivially (recon item 5)

class term_target : public madc::hub::tui_target
{
    HANDLE _hin, _hout;
    DWORD  _saved_in, _saved_out;
    UINT   _saved_cp_in, _saved_cp_out;
    bool   _open, _suspended;
    size_t _rows, _cols;
    tui_keyparse _parse;

    bool query_size(size_t &rows, size_t &cols)
    {
	CONSOLE_SCREEN_BUFFER_INFO sbi;
	if ( !GetConsoleScreenBufferInfo(_hout, &sbi) )
	    return false;
	// srWindow is the VISIBLE window — the surface the VT sequences
	// address (the screen buffer behind it can be taller).
	size_t r = (size_t)(sbi.srWindow.Bottom - sbi.srWindow.Top + 1);
	size_t c = (size_t)(sbi.srWindow.Right - sbi.srWindow.Left + 1);
	if ( r == 0 || c == 0 )
	    return false;
	rows = r;
	cols = c;
	return true;
    }
    void emit(const std::string &s)
    {
	size_t off = 0;
	while ( off < s.size() )
	{
	    DWORD n = 0;
	    if ( !WriteFile(_hout, s.data() + off,
			    (DWORD)(s.size() - off), &n, NULL) || n == 0 )
		return;
	    off += n;
	}
    }
    // The poll(STDIN) twin. taskio's probe owns the console-readability
    // rule (fd 0 per its CRT-fd contract; husk records — key-ups, bare
    // modifiers, focus/menu/size events — are drained THERE, so a true
    // here means ReadFile will not block). A timed wait parks on the
    // input handle (a console is a waitable object) and re-probes: a
    // husk signal re-waits within the same budget.
    bool input_ready(int timeout_ms)
    {
	for (;;)
	{
	    if ( madc::taskio::poll_readable(0) )
		return true;
	    if ( timeout_ms == 0 )
		return false;
	    DWORD step = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
	    long long t0 = (long long)GetTickCount64();
	    if ( WaitForSingleObject(_hin, step) != WAIT_OBJECT_0 )
		return false;	// timeout (or a wait error the read owns)
	    if ( timeout_ms > 0 )
	    {
		timeout_ms -= (int)((long long)GetTickCount64() - t0);
		if ( timeout_ms < 0 )
		    timeout_ms = 0;
	    }
	}
    }

public:
    term_target() : _hin(INVALID_HANDLE_VALUE), _hout(INVALID_HANDLE_VALUE),
		    _saved_in(0), _saved_out(0),
		    _saved_cp_in(0), _saved_cp_out(0),
		    _open(false), _suspended(false), _rows(24), _cols(80) {}
    ~term_target() { close(); }

    // Grid-mode entry/exit, shared by open/close and suspend/resume —
    // the termios twins (recon item 1). Raw mode: line/echo/processed
    // off, VT INPUT on (the console then DELIVERS VT byte sequences, so
    // the shared tui_keyparse consumes them unchanged — recon item 2);
    // output: VT processing on + newline auto-return off (the OPOST-off
    // twin). SetConsoleMode failing = no VT console (pre-Win10-1809).
    bool enter_grid_mode()
    {
	DWORD rawin = _saved_in, rawout = _saved_out;
	rawin &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT
		   | ENABLE_PROCESSED_INPUT);
	rawin |= ENABLE_VIRTUAL_TERMINAL_INPUT;
	rawout |= ENABLE_VIRTUAL_TERMINAL_PROCESSING
		| DISABLE_NEWLINE_AUTO_RETURN;
	if ( !SetConsoleMode(_hout, rawout)
	     || !SetConsoleMode(_hin, rawin) )
	{
	    SetConsoleMode(_hout, _saved_out);	// undo a half-applied pair
	    fprintf(stderr, "ui: this console has no VT mode"
			    " (Windows 10 1809+ required)\n");
	    return false;
	}
	// UTF-8 codepages: the grid's cell text is UTF-8 bytes and the
	// parser expects UTF-8 input (the Windows Terminal convention).
	// [validate-win]
	_saved_cp_in  = GetConsoleCP();
	_saved_cp_out = GetConsoleOutputCP();
	SetConsoleCP(CP_UTF8);
	SetConsoleOutputCP(CP_UTF8);
	query_size(_rows, _cols);
	emit(VT_ENTER_GRID);
	return true;
    }

    void leave_grid_mode()
    {
	emit(VT_LEAVE_GRID);
	SetConsoleMode(_hin, _saved_in);
	SetConsoleMode(_hout, _saved_out);
	if ( _saved_cp_in )
	    SetConsoleCP(_saved_cp_in);
	if ( _saved_cp_out )
	    SetConsoleOutputCP(_saved_cp_out);
    }

    virtual bool open(size_t &rows, size_t &cols)
    {
	if ( g_live )
	{
	    fprintf(stderr, "ui: a TUI target is already open\n");
	    return false;
	}
	_hin = GetStdHandle(STD_INPUT_HANDLE);
	_hout = GetStdHandle(STD_OUTPUT_HANDLE);
	// GetConsoleMode succeeding IS the isatty twin (recon item 1).
	if ( _hin == INVALID_HANDLE_VALUE || _hout == INVALID_HANDLE_VALUE
	     || !GetConsoleMode(_hin, &_saved_in)
	     || !GetConsoleMode(_hout, &_saved_out) )
	{
	    fprintf(stderr,
		    "ui: the terminal target needs a console on"
		    " stdin/stdout\n");
	    return false;
	}
	if ( !enter_grid_mode() )
	    return false;
	_open = true;
	g_live = this;
	static bool exit_hooked = false;
	if ( !exit_hooked )
	{
	    exit_hooked = true;
	    atexit(close_live_target);
	}
	rows = _rows;
	cols = _cols;
	return true;
    }

    virtual void close()
    {
	if ( !_open )
	    return;
	if ( !_suspended )		// suspended: already restored
	    leave_grid_mode();
	_suspended = false;
	_open = false;
	g_live = 0;
    }

    // Suspend/resume (madcide v2, JOE ^K Z): the console goes back to
    // the owner-found modes while a child process runs; resume re-raws
    // and re-enters the alternate screen. The saved modes stay the
    // ORIGINAL pre-open state — "restored exactly as found".
    virtual bool suspend()
    {
	if ( !_open || _suspended )
	    return false;
	leave_grid_mode();
	_suspended = true;
	return true;
    }

    virtual bool resume()
    {
	if ( !_open || !_suspended )
	    return false;
	if ( !enter_grid_mode() )
	    return false;		// still suspended; caller told
	_suspended = false;
	return true;
    }

    virtual void paint(const tui_grid &prev, const tui_grid &next)
    {
	if ( !_open || _suspended )
	    return;
	emit(vt_paint_bytes(prev, next));	// the shared byte builder
    }

    virtual bool read_keys(std::vector<tui_keyev> &out)
    {
	if ( !_open || _suspended )
	    return false;
	for (;;)
	{
	    // The g_winch twin: poll the window extent at the loop head
	    // (one cheap call per wake — recon item 3); every blocking
	    // wait below is bounded to RESIZE_POLL_MS so this line is
	    // reached on a cadence even with no keystroke.
	    {
		size_t r = _rows, c = _cols;
		if ( query_size(r, c) && (r != _rows || c != _cols) )
		{
		    _rows = r;
		    _cols = c;
		    out.push_back(tui_keyev(tui_key::resize));
		    return true;
		}
	    }
	    // Stage-2 cooperative gate — the POSIX body's policy with the
	    // EINTR axis replaced by the bounded host wait: a DEADLINE
	    // wake re-runs the resize poll above (no wake event), a
	    // SYNTHETIC wake surfaces as the wake event (recompose).
	    {
		bool ran = false;
		bool repoll = false;
		for ( ;; )
		{
		    if ( input_ready(0) )
			break;
		    if ( __madc_task_runnable() > 0 )
		    {
			__madc_yield();
			ran = true;
			continue;
		    }
		    if ( ran )
		    {
			out.push_back(tui_keyev(tui_key::wake));
			return true;
		    }
		    if ( __madc_task_live() == 0 )
			break;
		    madc::taskio::host_wake hw =
			madc::taskio::host_wait_readable(0, RESIZE_POLL_MS);
		    if ( hw == madc::taskio::host_wake::fired )
			break;			// stdin fired: read below
		    if ( hw == madc::taskio::host_wake::deadline )
		    {
			repoll = true;		// resize cadence
			break;
		    }
		    out.push_back(tui_keyev(tui_key::wake));
		    return true;		// synthetic: recompose
		}
		if ( repoll )
		    continue;
	    }
	    // Zero-task blocking wait, bounded for the resize cadence.
	    if ( !input_ready(0) && !input_ready(RESIZE_POLL_MS) )
		continue;
	    char buf[64];
	    DWORD n = 0;
	    if ( !ReadFile(_hin, buf, sizeof(buf), &n, NULL) || n == 0 )
		return false;	// the input source ended
	    _parse.feed(buf, (size_t)n, out);
	    HANDLE hin = _hin;
	    vt_drain_pending(_parse, out,
		[this](int t) { return input_ready(t); },
		[hin](tui_keyparse &p, std::vector<tui_keyev> &o) -> bool {
		    char b[64];
		    DWORD m = 0;
		    if ( !ReadFile(hin, b, sizeof(b), &m, NULL) || m == 0 )
			return false;
		    p.feed(b, (size_t)m, o);
		    return true;
		});
	    if ( !out.empty() )
		return true;
	}
    }

    virtual void size(size_t &rows, size_t &cols)
    {
	rows = _rows;
	cols = _cols;
    }
};

void close_live_target()
{
    if ( g_live )
	g_live->close();
}

madc::hub::tui_target *make_term_target()
{
    return new term_target();
}

} // namespace

#endif // _WIN32

namespace madc {
namespace hub {

void register_builtin_tui_targets()
{
    static bool done = false;
    if ( done )
	return;
    done = true;
    register_tui_target("term", make_term_target);
}

} // namespace hub
} // namespace madc
