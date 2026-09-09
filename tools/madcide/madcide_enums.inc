// madcide_enums.inc — the IDE's own vocabularies as ENUMS (owner law
// 2026-09-09, "enums, not strings": a misspelt string code goes undetected;
// an enumerator is a compile-time error). Included FIRST by
// madcide_core.inc; every madcide file sees these through it.
//
// THE COMMAND VOCABULARY is code; its names are the input boundary. Every
// verb the dispatcher implements is an `ide_cmd` enumerator: the registry
// commands (profiles/default.menu rows), the key-named motions (the action
// name IS the key spelling — a profile binds "^a home"), the modal-scope
// actions (the @prompt / @confirm / @project / @modes / @pane / @terminal /
// @insert values), the vi grammar's operators, motions and insert entries,
// and the pane-row verbs (the ^B palette's bld-* rows, the ^T Options rows,
// the diagnostics / outline rows' goto, the shared editor menu's w / f / q).
// A NAME is legal only where text enters: a .keys profile, a .menu file, a
// client that posts a command by name. cmd_of converts it ONCE there and an
// unknown name is a REFUSAL there (the profile / menu does not load; the
// event reports "Unknown action"); cmd_name is the OUTPUT converter (a
// control's wire id, a message). Dispatch is a `switch` on the code.
//
// Prefixed UNSCOPED with a fixed base (the owner's spelling): `switch (code)
// { case cmdSAVE: }` on a `long` read from a bag; `var == cmdSAVE` compares
// as an integer; a misspelt enumerator does not compile.
//
// Thread contract: constants and pure functions over them.
enum ide_cmd : unsigned short
{
    cmdNONE = 0,			// unbound chord / unknown name
    // ---- the command registry (profiles/default.menu) ----
    cmdBEND, cmdBLOCKCOPY, cmdBLOCKDEL, cmdBLOCKMOVE, cmdBOTTOM, cmdBUFSEL,
    cmdBUILD, cmdCHECK, cmdCOLON, cmdDELLINE, cmdDELWORD, cmdDISCARD,
    cmdEDITFILE, cmdFIND, cmdFINDNEXT, cmdGOTOLINE, cmdGROWW, cmdHELP,
    cmdINSERTFILE, cmdKILLWIN, cmdMARK, cmdMODES, cmdNEXTW, cmdONLYWIN,
    cmdOPENPROJECT, cmdOPTIONS, cmdOUTLINE, cmdOUTPUT, cmdPANEL, cmdPREVW,
    cmdPROBLEMS, cmdPROFILE, cmdPROJADDCUR, cmdPROJECT, cmdQUIT, cmdREDO,
    cmdREFRESH, cmdSAVE, cmdSAVEAS, cmdSAVEQUIT, cmdSHELL, cmdSHRINKW,
    cmdSPLITW, cmdTERMINAL, cmdTHEME, cmdTOP, cmdUNDO, cmdVIEW,
    cmdVIMODE, cmdWORDLEFT, cmdWORDRIGHT,
    // ---- motions: the action name IS the key spelling (edit_key's keys) ----
    cmdLEFT, cmdRIGHT, cmdUP, cmdDOWN, cmdHOME, cmdEND,
    cmdPGUP, cmdPGDN, cmdBACKSPACE, cmdDEL, cmdENTER,
    // ---- modal-scope actions (@prompt @confirm @project @modes @pane @terminal @insert) ----
    cmdPCOMMIT, cmdPCANCEL, cmdPDELCHAR, cmdPYES, cmdPROJCLOSE, cmdPROJDELCHAR,
    cmdPROJADD, cmdPROJDEL, cmdPROJOPEN, cmdMODESOPEN, cmdMODESCLOSE, cmdPANECLOSE,
    cmdTERMUNFOCUS, cmdVINORMAL,
    // ---- the vi grammar (@normal): operators, the G motion, insert entries ----
    cmdVIDELETE, cmdVICHANGE, cmdVIGOTO, cmdVIINSERT, cmdVIINSERTBOL, cmdVIAPPEND,
    cmdVIAPPENDEOL, cmdVIOPENBELOW, cmdVIOPENABOVE,
    // ---- pane-row verbs: diagnostics/outline goto, the editor menu's w f q,
    //      the ^T Options rows, the ^B palette's rows ----
    cmdGOTO, cmdW, cmdF, cmdQ, cmdOPT_TABWIDTH, cmdOPT_SCHEME,
    cmdOPT_KEYMAP, cmdOPT_PROJKIND, cmdBLD_CHECK, cmdBLD_NATIVE, cmdBLD_RUN, cmdBLD_RUN_CMD,
    cmdBLD_STOP, cmdBLD_PROJ_CHECK, cmdBLD_PROJ_BUILD, cmdBLD_PROJ_RUN,
    cmdBUILDROW				// a Build MENU row "build-<n>": the
					// NAME carries the 1-based row (an
					// argument, not a discriminator)
};

// The ONE name <-> code table (rows, source order = the enum's groups). Built
// on demand — the callers that convert many names (parse_keys, load_menu)
// build it once per load; the rest convert one name at a time.
void cmd_table(var &t)
{
    var rows = {};
    rows[] = { "name": "bend", "code": cmdBEND };
    rows[] = { "name": "blockcopy", "code": cmdBLOCKCOPY };
    rows[] = { "name": "blockdel", "code": cmdBLOCKDEL };
    rows[] = { "name": "blockmove", "code": cmdBLOCKMOVE };
    rows[] = { "name": "bottom", "code": cmdBOTTOM };
    rows[] = { "name": "bufsel", "code": cmdBUFSEL };
    rows[] = { "name": "build", "code": cmdBUILD };
    rows[] = { "name": "check", "code": cmdCHECK };
    rows[] = { "name": "colon", "code": cmdCOLON };
    rows[] = { "name": "delline", "code": cmdDELLINE };
    rows[] = { "name": "delword", "code": cmdDELWORD };
    rows[] = { "name": "discard", "code": cmdDISCARD };
    rows[] = { "name": "editfile", "code": cmdEDITFILE };
    rows[] = { "name": "find", "code": cmdFIND };
    rows[] = { "name": "findnext", "code": cmdFINDNEXT };
    rows[] = { "name": "gotoline", "code": cmdGOTOLINE };
    rows[] = { "name": "groww", "code": cmdGROWW };
    rows[] = { "name": "help", "code": cmdHELP };
    rows[] = { "name": "insertfile", "code": cmdINSERTFILE };
    rows[] = { "name": "killwin", "code": cmdKILLWIN };
    rows[] = { "name": "mark", "code": cmdMARK };
    rows[] = { "name": "modes", "code": cmdMODES };
    rows[] = { "name": "nextw", "code": cmdNEXTW };
    rows[] = { "name": "onlywin", "code": cmdONLYWIN };
    rows[] = { "name": "openproject", "code": cmdOPENPROJECT };
    rows[] = { "name": "options", "code": cmdOPTIONS };
    rows[] = { "name": "outline", "code": cmdOUTLINE };
    rows[] = { "name": "output", "code": cmdOUTPUT };
    rows[] = { "name": "panel", "code": cmdPANEL };
    rows[] = { "name": "prevw", "code": cmdPREVW };
    rows[] = { "name": "problems", "code": cmdPROBLEMS };
    rows[] = { "name": "profile", "code": cmdPROFILE };
    rows[] = { "name": "projaddcur", "code": cmdPROJADDCUR };
    rows[] = { "name": "project", "code": cmdPROJECT };
    rows[] = { "name": "quit", "code": cmdQUIT };
    rows[] = { "name": "redo", "code": cmdREDO };
    rows[] = { "name": "refresh", "code": cmdREFRESH };
    rows[] = { "name": "save", "code": cmdSAVE };
    rows[] = { "name": "saveas", "code": cmdSAVEAS };
    rows[] = { "name": "savequit", "code": cmdSAVEQUIT };
    rows[] = { "name": "shell", "code": cmdSHELL };
    rows[] = { "name": "shrinkw", "code": cmdSHRINKW };
    rows[] = { "name": "splitw", "code": cmdSPLITW };
    rows[] = { "name": "terminal", "code": cmdTERMINAL };
    rows[] = { "name": "theme", "code": cmdTHEME };
    rows[] = { "name": "top", "code": cmdTOP };
    rows[] = { "name": "undo", "code": cmdUNDO };
    rows[] = { "name": "view", "code": cmdVIEW };
    rows[] = { "name": "vimode", "code": cmdVIMODE };
    rows[] = { "name": "wordleft", "code": cmdWORDLEFT };
    rows[] = { "name": "wordright", "code": cmdWORDRIGHT };
    rows[] = { "name": "left", "code": cmdLEFT };
    rows[] = { "name": "right", "code": cmdRIGHT };
    rows[] = { "name": "up", "code": cmdUP };
    rows[] = { "name": "down", "code": cmdDOWN };
    rows[] = { "name": "home", "code": cmdHOME };
    rows[] = { "name": "end", "code": cmdEND };
    rows[] = { "name": "pgup", "code": cmdPGUP };
    rows[] = { "name": "pgdn", "code": cmdPGDN };
    rows[] = { "name": "backspace", "code": cmdBACKSPACE };
    rows[] = { "name": "del", "code": cmdDEL };
    rows[] = { "name": "enter", "code": cmdENTER };
    rows[] = { "name": "pcommit", "code": cmdPCOMMIT };
    rows[] = { "name": "pcancel", "code": cmdPCANCEL };
    rows[] = { "name": "pdelchar", "code": cmdPDELCHAR };
    rows[] = { "name": "pyes", "code": cmdPYES };
    rows[] = { "name": "projclose", "code": cmdPROJCLOSE };
    rows[] = { "name": "projdelchar", "code": cmdPROJDELCHAR };
    rows[] = { "name": "projadd", "code": cmdPROJADD };
    rows[] = { "name": "projdel", "code": cmdPROJDEL };
    rows[] = { "name": "projopen", "code": cmdPROJOPEN };
    rows[] = { "name": "modesopen", "code": cmdMODESOPEN };
    rows[] = { "name": "modesclose", "code": cmdMODESCLOSE };
    rows[] = { "name": "paneclose", "code": cmdPANECLOSE };
    rows[] = { "name": "termunfocus", "code": cmdTERMUNFOCUS };
    rows[] = { "name": "vinormal", "code": cmdVINORMAL };
    rows[] = { "name": "videlete", "code": cmdVIDELETE };
    rows[] = { "name": "vichange", "code": cmdVICHANGE };
    rows[] = { "name": "vigoto", "code": cmdVIGOTO };
    rows[] = { "name": "viinsert", "code": cmdVIINSERT };
    rows[] = { "name": "viinsertbol", "code": cmdVIINSERTBOL };
    rows[] = { "name": "viappend", "code": cmdVIAPPEND };
    rows[] = { "name": "viappendeol", "code": cmdVIAPPENDEOL };
    rows[] = { "name": "viopenbelow", "code": cmdVIOPENBELOW };
    rows[] = { "name": "viopenabove", "code": cmdVIOPENABOVE };
    rows[] = { "name": "goto", "code": cmdGOTO };
    rows[] = { "name": "w", "code": cmdW };
    rows[] = { "name": "f", "code": cmdF };
    rows[] = { "name": "q", "code": cmdQ };
    rows[] = { "name": "opt-tabwidth", "code": cmdOPT_TABWIDTH };
    rows[] = { "name": "opt-scheme", "code": cmdOPT_SCHEME };
    rows[] = { "name": "opt-keymap", "code": cmdOPT_KEYMAP };
    rows[] = { "name": "opt-projkind", "code": cmdOPT_PROJKIND };
    rows[] = { "name": "bld-check", "code": cmdBLD_CHECK };
    rows[] = { "name": "bld-native", "code": cmdBLD_NATIVE };
    rows[] = { "name": "bld-run", "code": cmdBLD_RUN };
    rows[] = { "name": "bld-run-cmd", "code": cmdBLD_RUN_CMD };
    rows[] = { "name": "bld-stop", "code": cmdBLD_STOP };
    rows[] = { "name": "bld-proj-check", "code": cmdBLD_PROJ_CHECK };
    rows[] = { "name": "bld-proj-build", "code": cmdBLD_PROJ_BUILD };
    rows[] = { "name": "bld-proj-run", "code": cmdBLD_PROJ_RUN };
    t = rows;
}

// The INPUT converter: a command NAME to its code, against a built table
// (cmd_of_in) or one built here (cmd_of). cmdNONE = unknown — the caller's
// refusal. "build-<n>" is the Build menu's row shape: cmdBUILDROW, the name
// kept beside the code as the row argument.
long cmd_of_in(var &t, const char *name)
{
    var nm = name;			// own the text (ring discipline)
    if ( strlen(nm.c_str()) == 0 )
	return cmdNONE;
    if ( nm.index("build-") == 0 )
	return cmdBUILDROW;
    for ( var r : t )
	if ( r["name"] == nm )
	    return r["code"].as_integer();
    return cmdNONE;
}

long cmd_of(const char *name)
{
    var t;
    cmd_table(t);
    return cmd_of_in(t, name);
}

// The OUTPUT converter: a code to the name a wire or a message spells.
// "" = cmdNONE or a code the table does not carry (cmdBUILDROW: the row's
// own id is the name — the composer writes it).
void cmd_name(var &out, long code)
{
    var t;
    cmd_table(t);
    for ( var r : t )
	if ( r["code"] == code )
	{
	    out = r["name"];
	    return;
	}
    out = "";
}

// An event's command code (action / choose events): the engine stamps it
// (a code-bound chord, a control's `code` hint); a client that posts by
// NAME has it filled at the session's entry (apply_ide_event). cmdNONE
// when absent.
long ev_code(var &ev)
{
    var c = ev["action_code"];
    if ( c.is_null() )
	return cmdNONE;
    return c.as_integer();
}

// The key a MOTION command stands for: the dispatcher synthesizes the key
// event edit_key implements (one movement implementation — the action name
// IS the key spelling). ui::key::none = not a motion.
ui::key motion_key_of(long code)
{
    switch ( code )
    {
	case cmdLEFT: return ui::key::left;
	case cmdRIGHT: return ui::key::right;
	case cmdUP: return ui::key::up;
	case cmdDOWN: return ui::key::down;
	case cmdHOME: return ui::key::home;
	case cmdEND: return ui::key::end;
	case cmdPGUP: return ui::key::pgup;
	case cmdPGDN: return ui::key::pgdn;
	case cmdBACKSPACE: return ui::key::backspace;
	case cmdDEL: return ui::key::del;
	case cmdENTER: return ui::key::enter;
    }
    return ui::key::none;
}
