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
// POSIX-only body: on _WIN32 this file registers nothing and	       //
// ui::tui_open reports no target (a Console-API target is a later     //
// provider — the deps-later posture).				       //
//								       //
// THREAD-SAFETY CONTRACT: one live target per process (it owns THE    //
// terminal), confined to the opening thread. The atexit/live-pointer  //
// pair below exists so a script that forgets tui_close still gets its //
// terminal back at process exit.				       //
//								       //
///////////////////////////////////////////////////////////////////////////

#include "madcdis/tui_provider.h"

#ifndef _WIN32

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "rt/rt_task.h"	// stage-2: cooperative wait (runnable probe + yield)

namespace {

using madc::hub::tui_grid;
using madc::hub::tui_attr;
using madc::hub::tui_keyev;
using madc::hub::tui_key;
using madc::hub::tui_keyparse;
using madc::hub::tui_paint_plan;
using madc::hub::tui_diff_plan;

// SIGWINCH sets a flag; read_keys turns it into a resize key. sa_flags
// carries no SA_RESTART so the blocking poll wakes with EINTR.
volatile sig_atomic_t g_winch = 0;
void winch_handler(int) { g_winch = 1; }

class term_target;
term_target *g_live = 0;	// the one open target, for atexit recovery
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
    static void cup(std::string &out, size_t row, size_t col)
    {
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%zu;%zuH", row + 1, col + 1);
	out += buf;
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
	// Alternate screen, clear, home, cursor hidden until a paint
	// places it.
	emit("\x1b[?1049h\x1b[2J\x1b[H\x1b[?25l");
	return true;
    }

    void leave_grid_mode()
    {
	emit("\x1b[0m\x1b[?25h\x1b[?1049l");
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

    // THE style->SGR table (AST-2; owner: VT-102 ANSI / JOE parity). A
    // non-normal transition RESETS and then sets the target style's full
    // parameter list — attributes 1/2/3/4/5/7, fg 30+c, bg 40+c;
    // bold-as-bright supplies the 16-colour foreground model (VT-102 /
    // 8-colour terminals brighten on bold, exactly JOE's behavior; no
    // aixterm 90–97). The historical spellings survive: pure inverse
    // entered from normal emits \x1b[7m and any->normal emits \x1b[0m,
    // so a grid using only normal/reverse produces the byte stream it
    // always did.
    static void emit_sgr(std::string &out, tui_attr from, tui_attr to)
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

    virtual void paint(const tui_grid &prev, const tui_grid &next)
    {
	if ( !_open || _suspended )
	    return;
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
	emit(out);
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
	    // keystroke latency stays one slice). Tasks that are LIVE but
	    // PARKED wait on wakes that fire only inside scheduling
	    // decisions (a build pump parked on its child's fd, a
	    // sleeper) — a blocking stdin wait would starve them, so nap
	    // on a 50ms cadence and fire the due set each round
	    // (__madc_task_fire_due: expired timers + the io hook's
	    // zero-timeout probe). Every actual run flows through the
	    // runnable branch, so when the queue DRAINS after tasks ran,
	    // synthesize a `wake` event — the application recomposes
	    // (build output repaints without a keystroke). Zero live
	    // tasks = the old blocking wait, zero new cost. The MT-4c
	    // unification (parking THIS flow on stdin through taskio, one
	    // poll for everything) retires the cadence.
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
		    input_ready(50);		// the cadence nap; stdin cuts it
		    __madc_task_fire_due();	// fd/timer wakes -> runnable
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
	    // Drain what is immediately available — the batch printable
	    // coalescing rides (§7.5) — then give a split escape sequence
	    // one short grace read (the termbox2/ESCDELAY idea; an
	    // unambiguous batch pays no added latency).
	    for (;;)
	    {
		while ( input_ready(0) )
		{
		    n = read(STDIN_FILENO, buf, sizeof(buf));
		    if ( n <= 0 )
			break;
		    _parse.feed(buf, (size_t)n, out);
		}
		if ( !_parse.pending() )
		    break;
		if ( !input_ready(25) )
		{
		    _parse.flush(out);
		    break;
		}
	    }
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

#endif // !_WIN32

namespace madc {
namespace hub {

void register_builtin_tui_targets()
{
    static bool done = false;
    if ( done )
	return;
    done = true;
#ifndef _WIN32
    register_tui_target("term", make_term_target);
#endif
}

} // namespace hub
} // namespace madc
