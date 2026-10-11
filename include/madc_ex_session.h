//////////////////////////////////////////////////////////////////////////
//									//
// madc::ex_session — the ex core's dialect face (split arc 1D, plan	//
// 2026-10-09-madc-madcide-split-arc.md item 18): madc's colon mode	//
// (include/madc_ex.h, madc::ex::session) over a ui:: world handle's	//
// documents, for a host that edits in a script — madcide's colon line	//
// runs ex's commands through it and keeps only the commands that need	//
// its views. Declaration-only in <ns_madc>; resolved mangled-direct	//
// here. LAYOUT CONTRACT with <ns_madc>: a single pointer member,	//
// append-only. A command is a <bits/ex_commands> ex_command code.	//
//									//
// Thread contract: madc::ex::session's — a session and the world it	//
// edits are confined to the world's thread.				//
//									//
//////////////////////////////////////////////////////////////////////////

#ifndef __MADC_EX_SESSION_H
#define __MADC_EX_SESSION_H 1

#include <cstdint>

#include "libmadc/value.h"

namespace madc {

namespace ex { class session; }

class ex_session
{
public:
    // Over the world `w` names (an unknown handle: a session that refuses
    // every command).
    explicit ex_session(int64_t w);
    ~ex_session();
    // The current buffer: making `doc` current, and which it is (0: none).
    bool use(int64_t doc);
    int64_t current();
    // The current line `.` (1-based): set (false when not one of the
    // buffer's lines) and read (0: no buffer, or an empty one).
    bool set_line(int64_t line);
    int64_t line();
    // One command line read, never run: {ok, error, command, force,
    // argument} — `command` its ex_command code.
    value &parse(value &out, const char *text);
    // One command line run: {ok, shown, error, wants_text, quit, command} —
    // `shown` the lines it prints, each ending in a line break.
    value &run(value &out, const char *text);
    // A pending a / i / c's text: `lines` (an array of strings) without
    // their line breaks and without the closing `.`; the same reply.
    value &text(value &out, value &lines);
    bool wants_text();

private:
    // The session and the world handle it edits (src/madc_ex.cpp): one
    // pointer, the layout contract. live(): the session while its world is
    // open, else NULL — a closed world refuses every command.
    struct face;
    face *_f;
    ex::session *live();

    ex_session(const ex_session &);
    ex_session &operator=(const ex_session &);
};

} // namespace madc

#endif // __MADC_EX_SESSION_H
