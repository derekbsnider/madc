// madcide_line.inc — `madcide FILE --line` (split arc 1D.18, decided
// 2026-10-09): madc's line mode. The line-oriented face — the ex core at the
// REPL prompt (D24: colon mode, the buffer, the session) — is madc's own,
// `madc -i FILE`, so madcide hands its terminal to the running madc with the
// file and the session's standard and ends with it. madcide keeps no line
// grammar of its own.
long run_line(const char *path, bool ro, var &more, long std = 0)
{
    var p = path;			// ring discipline: own the text first
    if ( ro )
	println(stderr, "madcide --line: madc -i has no read-only buffer; {} opens writable.", p);
    var args = {};
    if ( std != 0 )
	args.push(format("--std={}", madc::file_kind_name(std)));
    args.push("-i");
    if ( strlen(p.c_str()) > 0 )
	args.push(p);
    if ( php::count(more) > 0 )
	println(stderr, "madcide --line: one file (madc -i FILE); the rest are not opened.");
    return madc::compiler_run(args);
}
