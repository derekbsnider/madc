// madcide_args.inc — madcide's COMMAND LINE (plan §41.11a step 1): the one
// reader of argv. Flags come anywhere; the first word that is not a flag is
// the file, and a later `ro` opens it read-only. The strings are compared
// only here, at the input boundary: main reads the parsed record.
//
//   madcide <file> [ro] [--profile NAME] [--gui|--line] [-c "<command> [arg]"]
//           [--serve <host:port>] [--mcp] [--lsp] [--attach [<host:port>]]
//           [--no-serve] [--sessions] [--help] [--] [<file>]
//
// `--` ends the flags (a file whose name starts with '-'). An unknown flag, a
// flag missing its value and a second file are refused with the reason and
// the usage (exit 2). `--attach`'s address is optional: the word after it is
// the address unless it is a flag or no file has been named yet (then it is
// the file — `madcide --lsp --attach f.c` discovers the session holding f.c).
//
// Thread contract: main's thread, before any session opens; the record points
// into argv, which lives for the process.

struct ide_args
{
    const char *file;		// NULL = none given
    const char *profile;	// --profile NAME; NULL = not given
    const char *once;		// -c "<command> [arg]"
    const char *serve;		// --serve <host:port>
    const char *attach_addr;	// --attach's address; NULL = discover
    ui::level level;		// --gui (ui::WEB), --line (ui::LINE), else ui::TUI
    bool ro;
    bool mcp;
    bool lsp;
    bool attach;
    bool no_serve;
    bool sessions;		// --sessions: what is running (no file needed)
    bool help;			// --help / -h
};

// The usage text (printed for --help, and after a refusal).
const char *ide_usage()
{
    return "usage: madcide <file> [ro] [--profile NAME] [--gui|--line] [-c \"<command> [arg]\"] [--serve <host:port>] [--mcp] [--lsp [--serve <host:port>]] [--no-serve]\n"
	   "       madcide <file> --lsp|--mcp --attach [<host:port>]  (join a running session; no address = discover one)\n"
	   "       madcide --sessions                                 (what is running, and how to join it)\n"
	   "       --profile NAME   the profile (a bundle of keys, layout, menu and settings; default: settings.json's \"profile\", else default)\n";
}

// A flag's value: the index of the next word, or 0 when there is none (`why`
// says so).
int ide_flag_value(var &why, int argc, const char **argv, int i, const char *what)
{
    if ( i + 1 >= argc )
    {
	why = format("{} needs {}", argv[i], what);
	return 0;
    }
    return i + 1;
}

// What a valued flag takes (its refusal names it).
const char *ide_flag_wants(const char *flag)
{
    if ( strcmp(flag, "-c") == 0 )
	return "\"<command> [arg]\"";
    if ( strcmp(flag, "--serve") == 0 )
	return "<host:port>";
    return "a profile NAME";
}

// Parse argv into `a`. False = refused; `why` says why (the caller prints it
// with the usage and exits 2).
bool ide_args_parse(ide_args &a, var &why, int argc, const char **argv)
{
    a.file = NULL;
    a.profile = NULL;
    a.once = NULL;
    a.serve = NULL;
    a.attach_addr = NULL;
    a.level = ui::TUI;
    a.ro = false;
    a.mcp = false;
    a.lsp = false;
    a.attach = false;
    a.no_serve = false;
    a.sessions = false;
    a.help = false;
    bool flags = true;		// false after `--`
    for ( int i = 1; i < argc; i++ )
    {
	const char *w = argv[i];
	if ( flags && w[0] == '-' && w[1] != 0 )
	{
	    if ( strcmp(w, "--") == 0 )
		flags = false;
	    else if ( strcmp(w, "--gui") == 0 )
		a.level = ui::WEB;
	    else if ( strcmp(w, "--line") == 0 )
		a.level = ui::LINE;
	    else if ( strcmp(w, "--mcp") == 0 )
		a.mcp = true;
	    else if ( strcmp(w, "--lsp") == 0 )
		a.lsp = true;
	    else if ( strcmp(w, "--no-serve") == 0 )
		a.no_serve = true;
	    else if ( strcmp(w, "--sessions") == 0 )
		a.sessions = true;
	    else if ( strcmp(w, "--help") == 0 || strcmp(w, "-h") == 0 )
		a.help = true;
	    else if ( strcmp(w, "-c") == 0 || strcmp(w, "--serve") == 0
		      || strcmp(w, "--profile") == 0 )
	    {
		int v = ide_flag_value(why, argc, argv, i, ide_flag_wants(w));
		if ( v == 0 )
		    return false;
		i = v;
		if ( strcmp(w, "-c") == 0 )
		    a.once = argv[v];
		else if ( strcmp(w, "--serve") == 0 )
		    a.serve = argv[v];
		else
		    a.profile = argv[v];
	    }
	    else if ( strcmp(w, "--attach") == 0 )
	    {
		a.attach = true;
		if ( a.file != NULL && i + 1 < argc && argv[i + 1][0] != '-' )
		{
		    i++;
		    a.attach_addr = argv[i];
		}
	    }
	    else
	    {
		why = format("unknown option '{}'", w);
		return false;
	    }
	}
	else if ( a.file == NULL )
	    a.file = w;
	else if ( strcmp(w, "ro") == 0 )
	    a.ro = true;
	else
	{
	    why = format("unexpected argument '{}' (the file is '{}')", w, a.file);
	    return false;
	}
    }
    return true;
}
