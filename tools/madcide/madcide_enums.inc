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
    cmdVIEWCLOSE, cmdVIEWDOCK, cmdVIEWFOCUS, cmdVIEWOPEN, cmdVIEWSIZE,
    cmdVIEWSPLIT, cmdVIEWSYNC, cmdVIEWWINDOW,
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
    cmdGOTO, cmdGOTODEF, cmdW, cmdF, cmdQ, cmdOPT_TABWIDTH, cmdOPT_SCHEME,
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
    rows[] = { "name": "viewclose", "code": cmdVIEWCLOSE };
    rows[] = { "name": "viewdock", "code": cmdVIEWDOCK };
    rows[] = { "name": "viewfocus", "code": cmdVIEWFOCUS };
    rows[] = { "name": "viewopen", "code": cmdVIEWOPEN };
    rows[] = { "name": "viewsize", "code": cmdVIEWSIZE };
    rows[] = { "name": "viewsplit", "code": cmdVIEWSPLIT };
    rows[] = { "name": "viewsync", "code": cmdVIEWSYNC };
    rows[] = { "name": "viewwindow", "code": cmdVIEWWINDOW };
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
    rows[] = { "name": "gotodef", "code": cmdGOTODEF };
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

// ---- the IDE's STATE discriminators (V0.5d) -------------------------------
// The bag's slots — pane, pmode, vimode — and the kinds that ride
// a request, a row or the manifest are INTEGERS (es_int reads them, a switch
// dispatches them); a NAME appears only where text leaves the program: a
// probe's print, a message, the page's region hook, the host's dialog mode,
// the manifest file — through the converters here, never a literal at the
// site. Prefixed unscoped enums with a fixed base (the owner's spelling).

// The POPUP pane a client shows (paneNONE = none up): the pick lists — help,
// options, the build palette, the modes palette, the project window. The
// tool Views (problems, outline, output, terminal) are not panes here: they
// live in the layout's chrome panes (V2, design §2.2) and show through
// show_view.
enum ide_pane : unsigned char
{
    paneNONE = 0, paneHELP, paneOPTIONS, paneBUILD, paneMODES, panePROJECT
};
const char *pane_name(long p)
{
    switch ( p )
    {
	case paneHELP: return "help";
	case paneOPTIONS: return "options";
	case paneBUILD: return "build";
	case paneMODES: return "modes";
	case panePROJECT: return "project";
    }
    return "";
}

// The prompt that is up (pmNONE = none): what enter's answer means.
enum ide_prompt : unsigned char
{
    pmNONE = 0, pmCONFIRM, pmFIND, pmGOTOLINE, pmINSERTFILE, pmEDITFILE,
    pmSAVEAS, pmOPENPROJECT, pmBUFSEL, pmPROJADD, pmTABWIDTH, pmTHEME, pmCOLON
};
const char *prompt_name(long m)
{
    switch ( m )
    {
	case pmCONFIRM: return "confirm";
	case pmFIND: return "find";
	case pmGOTOLINE: return "gotoline";
	case pmINSERTFILE: return "insertfile";
	case pmEDITFILE: return "editfile";
	case pmSAVEAS: return "saveas";
	case pmOPENPROJECT: return "openproject";
	case pmBUFSEL: return "bufsel";
	case pmPROJADD: return "projadd";
	case pmTABWIDTH: return "tabwidth";
	case pmTHEME: return "theme";
	case pmCOLON: return "colon";
    }
    return "";
}

// The vi mode (viOFF = not modal).
enum ide_vimode : unsigned char { viOFF = 0, viNORMAL, viINSERT };
const char *vimode_name(long m)
{
    switch ( m )
    {
	case viNORMAL: return "normal";
	case viINSERT: return "insert";
    }
    return "";
}

// A parked terminal request's kind (the session says WHAT it needs; the
// client executes it).
enum ide_req : unsigned char
{
    rqNONE = 0, rqREFRESH, rqFILEDIALOG, rqRUN, rqPROJRUN, rqCMD, rqSHELL
};
const char *req_name(long k)
{
    switch ( k )
    {
	case rqREFRESH: return "refresh";
	case rqFILEDIALOG: return "filedialog";
	case rqRUN: return "run";
	case rqPROJRUN: return "projrun";
	case rqCMD: return "cmd";
	case rqSHELL: return "shell";
    }
    return "";
}

// The IDE's own dialog kind: what a native file dialog's answer MEANS. The
// host speaks only the dialog MODE (ui::dialog_mode's names, open | save) —
// dlg_host_mode is that wire word; the kind waits on the bag for the answer.
enum ide_dlg : unsigned char { dlgOPEN = 0, dlgSAVE, dlgOPENPROJECT };
const char *dlg_host_mode(long d)
{
    return d == dlgSAVE ? "save" : "open";
}

// The project's kind — the manifest's `kind` word (console | gui) converted
// where the manifest is READ; written back as the file's word.
enum ide_projkind : unsigned char { pkCONSOLE = 0, pkGUI };
const char *projkind_name(long k)
{
    return k == pkGUI ? "gui" : "console";
}
long projkind_of(var &k)
{
    if ( k.is_null() )
	return pkCONSOLE;
    return k == "gui" ? pkGUI : pkCONSOLE;
}

// A build row's run mode — the manifest command's `mode` word (capture |
// terminal) converted where the rows are built.
enum ide_bmode : unsigned char { bmCAPTURE = 0, bmTERMINAL };
long bmode_of(var &m)
{
    if ( m.is_null() )
	return bmCAPTURE;
    return m == "terminal" ? bmTERMINAL : bmCAPTURE;
}

// A layout SLOT (V2, design §2.2): where a pane sits — a popup (a pick list
// floating over the workbench), the sidebar or the panel (the fixed-slot
// chrome, docked by the client's layout), or the editor region. Placement is
// the layout's data (profiles/default.layout); slot_name is its wire word
// (the page docks by `region`).
enum ide_slot : unsigned char { slotNONE = 0, slotPOPUP, slotSIDEBAR, slotPANEL, slotEDITOR };
const char *slot_name(long s)
{
    switch ( s )
    {
	case slotPOPUP: return "popup";
	case slotSIDEBAR: return "sidebar";
	case slotPANEL: return "panel";
	case slotEDITOR: return "editor";
    }
    return "";
}
// The INPUT converter for a `.layout` line's slot word (V2): the chrome
// slots and the editor region; slotNONE = not a slot word (the refusal).
long slot_of(var &name)
{
    if ( name.is_null() )
	return slotNONE;
    for ( long k = slotPOPUP; k <= slotEDITOR; k = k + 1 )
	if ( name == slot_name(k) )
	    return k;
    return slotNONE;
}

// ---- containers (client-server arc V2; design doc §2.2) -------------------
// A layout node's KIND: a pane (a region holding Views, in `tabs` or `stack`
// mode) or a split (children side by side or one above the other — the
// direction is the engine's ui::split, <bits/ui_enums>, a chrome pane's side
// its ui::side). The layout file's words convert once at load (parse_layout,
// madcide_core.inc); a misspelling refuses the profile with its line.
enum ide_container : unsigned char { ctNONE = 0, ctPANE, ctSPLIT };
const char *container_name(long c)
{
    switch ( c )
    {
	case ctPANE: return "pane";
	case ctSPLIT: return "split";
    }
    return "";
}
long container_of(var &name)
{
    if ( name.is_null() )
	return ctNONE;
    for ( long k = ctPANE; k <= ctSPLIT; k = k + 1 )
	if ( name == container_name(k) )
	    return k;
    return ctNONE;
}

// A pane's stacking MODE: `tabs` shows one View at a time behind a strip;
// `stack` shows every View one above the other, each with its header (the
// S5 window stack's shape).
enum ide_pmode : unsigned char { lmNONE = 0, lmTABS, lmSTACK };
const char *pmode_name(long m)
{
    switch ( m )
    {
	case lmTABS: return "tabs";
	case lmSTACK: return "stack";
    }
    return "";
}
long pmode_of(var &name)
{
    if ( name.is_null() )
	return lmNONE;
    for ( long k = lmTABS; k <= lmSTACK; k = k + 1 )
	if ( name == pmode_name(k) )
	    return k;
    return lmNONE;
}

// ---- Views (client-server arc V1; design doc §2.1) -------------------------
// A View's projection KIND — what a View shows. A code View's LANGUAGE is the
// engine's file-kind vocabulary (<bits/file_kinds>, madc::fk*; its names are
// the engine's, madc::file_kind_name / file_kind_of); the two fields are the
// representation. These names are OUTPUT (a probe's print, a hint a client
// shows) and, from V2, the layout profile's INPUT words (`views source
// problems`) — view_of converts once at load; a misspelling is viewNONE, the
// caller's refusal, never a dead View.
enum ide_view : unsigned char
{
    viewNONE = 0, viewSOURCE, viewCODE, viewPROBLEMS, viewOUTPUT, viewTERMINAL,
    viewPROJECT, viewOUTLINE, viewHELP
};
const char *view_name(long k)
{
    switch ( k )
    {
	case viewSOURCE: return "source";
	case viewCODE: return "code";
	case viewPROBLEMS: return "problems";
	case viewOUTPUT: return "output";
	case viewTERMINAL: return "terminal";
	case viewPROJECT: return "project";
	case viewOUTLINE: return "outline";
	case viewHELP: return "help";
    }
    return "";
}
long view_of(var &name)
{
    if ( name.is_null() )
	return viewNONE;
    for ( long k = viewSOURCE; k <= viewHELP; k = k + 1 )
	if ( name == view_name(k) )
	    return k;
    return viewNONE;
}

// The registry command that SHOWS a tool View (V2): what a chrome pane's tab
// posts when clicked — the same command the profiles bind. cmdNONE = a kind
// no command shows (a source View: the buffer ring's own commands).
long view_show_cmd(long kind)
{
    switch ( kind )
    {
	case viewPROBLEMS: return cmdPROBLEMS;
	case viewOUTPUT: return cmdOUTPUT;
	case viewTERMINAL: return cmdTERMINAL;
	case viewOUTLINE: return cmdOUTLINE;
	case viewPROJECT: return cmdPROJECT;
	case viewHELP: return cmdHELP;
    }
    return cmdNONE;
}

// A rendered View's GENERATOR — the compiler that produced its text: genNONE
// for stored text, genMADC for every render this compiler makes, gcc / clang
// for the external-assembly Views (V7).
enum ide_gen : unsigned char { genNONE = 0, genMADC, genGCC, genCLANG };
const char *gen_name(long g)
{
    switch ( g )
    {
	case genMADC: return "madc";
	case genGCC: return "gcc";
	case genCLANG: return "clang";
    }
    return "";
}

// ---- the CLIENT vocabularies (client-server design §2.3, groundwork
// invariant #1) ---------------------------------------------------------
// A client of the session is transport/level/capability-GENERAL: a local
// window, a WebSocket peer and an api / LSP editor differ only in their
// `transport` and capability set; the `tier` bounds what the client may
// do. Both are ENUMS (owner law, "enums not strings") — a wire handshake's
// text converts to a code ONCE at the connection boundary, and an unknown
// spelling is a refusal there, never a silent mis-route. Dispatch switches
// on the code.
enum ide_transport : unsigned char { trLOCAL = 0, trWS, trAPI };
// The permission ladder (the gateway vision's tiers, born low for a remote
// peer — V6): an OBSERVER watches, a PROPOSER may not mutate but may STORE a
// proposal for an editor to land (Nexus L4c, design §3.6 — the fourth level
// rides every existing gate; every `<` / `>=` keeps its meaning), an EDITOR
// may mutate, an OWNER holds the session. A local window is the OWNER; a
// fresh remote peer starts OBSERVER and is raised by the owner.
enum ide_tier : unsigned char { tierOBSERVER = 0, tierPROPOSER, tierEDITOR, tierOWNER };

// ---- tier <-> name (V6a tiers) -----------------------------------------
// Text only at the wire boundary: convert ONCE, refuse a misspelling, never
// mis-route (the enums-not-strings law). tier_name spells a code for a
// reply/refusal; tier_of_name resolves clienttier's <tier> argument (-1 =
// unknown spelling).
const char *tier_name(long tier)
{
    switch ( tier )
    {
    case tierOBSERVER: return "observer";
    case tierPROPOSER: return "proposer";
    case tierEDITOR:   return "editor";
    case tierOWNER:    return "owner";
    }
    return "?";
}

long tier_of_name(const char *name)
{
    if ( strcmp(name, "observer") == 0 ) return tierOBSERVER;
    if ( strcmp(name, "proposer") == 0 ) return tierPROPOSER;
    if ( strcmp(name, "editor") == 0 )   return tierEDITOR;
    if ( strcmp(name, "owner") == 0 )    return tierOWNER;
    return -1;
}

// The minimum tier a command requires (V6a tiers). Read-only queries and
// navigation are tierOBSERVER (the default); commands that MUTATE the
// document/project, write files, or spend build/run resources are tierEDITOR;
// arbitrary shell/terminal execution is tierOWNER. The tier POLICY, one
// switch over the command enum (the shape cmd_table is), listing only the
// elevated codes — everything else observes. First pass; refined as the
// surface grows (e.g. per-argument gating of the `:` line's sub-verbs).
long cmd_min_tier(long code)
{
    switch ( code )
    {
    // owner — arbitrary execution
    case cmdSHELL:
    case cmdTERMINAL:
	return tierOWNER;
    // editor — text mutation
    case cmdBLOCKDEL:
    case cmdBLOCKMOVE:
    case cmdDELLINE:
    case cmdDELWORD:
    case cmdBACKSPACE:
    case cmdDEL:
    case cmdENTER:
    case cmdDISCARD:
    case cmdINSERTFILE:
    case cmdUNDO:
    case cmdREDO:
    case cmdVIDELETE:
    case cmdVICHANGE:
    case cmdVIINSERT:
    case cmdVIINSERTBOL:
    case cmdVIAPPEND:
    case cmdVIAPPENDEOL:
    case cmdVIOPENBELOW:
    case cmdVIOPENABOVE:
    // editor — file writes
    case cmdSAVE:
    case cmdSAVEAS:
    case cmdSAVEQUIT:
    // editor — project mutation / opening
    case cmdOPENPROJECT:
    case cmdEDITFILE:
    case cmdPROJADDCUR:
    case cmdPROJADD:
    case cmdPROJDEL:
    case cmdPROJCLOSE:
    case cmdOPT_PROJKIND:
    // editor — builds / runs (produce artifacts, execute the project)
    case cmdBUILD:
    case cmdBLD_NATIVE:
    case cmdBLD_RUN:
    case cmdBLD_RUN_CMD:
    case cmdBLD_STOP:
    case cmdBLD_PROJ_BUILD:
    case cmdBLD_PROJ_RUN:
    // editor — the `:` line can mutate/save (its `:!` shell gap is a
    // refinement: gate the parsed sub-verb, not the gateway)
    case cmdCOLON:
	return tierEDITOR;
    }
    return tierOBSERVER;
}

// ---- the code-graph MCP tool family (design 2026-09-12) --------------------
// The graph.* verbs as CODES (enums-not-strings): the wire name converts ONCE
// (graph_verb_of, over the descriptor table — one list of names) and every
// dispatch / policy is a switch on the code. gvNONE = unknown verb (a refusal).
enum graph_verb : unsigned char {
    gvNONE = 0,
    gvSYMBOLS, gvDEFINITION, gvNODE, gvTYPE_OF, gvMEMBERS, gvBASES, gvENCLOSING,
    gvBODY, gvCHILDREN,
    gvCALLEES, gvCALLERS, gvREFERENCES, gvSEARCH, gvIMPACT,
    gvSPAN, gvAT, gvINSERT, gvREPLACE, gvDELETE,
    // Nexus L4b (design 2026-09-13 §4.3): the PAST verbs — reads, observer tier.
    gvSTATUS, gvSOURCE, gvHISTORY, gvCOMMITS, gvREVISION, gvDIFF,
    // Nexus L4c (design §3.4, §4.4): the FUTURE verbs — the proposal fold + decisions.
    gvPROPOSALS, gvPROPOSAL, gvACCEPT, gvREJECT, gvWITHDRAW
};

// The annotation kinds madcide writes into the ONE change stream (design
// §3.1); the texteditor layer's reader sees them as ckOTHER. Text on the
// persisted line, a code in the reader (nexus_kind_of) — the enum law. The
// writers: nodeop (L3), proposal / decision (L4c), record / link (L4d),
// testrun / buildrun (L4e — the EVIDENCE events, tagged by the node).
enum nexus_kind : unsigned char {
    nkNONE = 0, nkNODEOP, nkPROPOSAL, nkDECISION, nkRECORD, nkLINK, nkTESTRUN, nkBUILDRUN
};

long nexus_kind_of(var &rec)
{
    var k = rec["kind"];
    if ( k.is_null() )
	return nkNONE;
    if ( k == "nodeop" )   return nkNODEOP;
    if ( k == "proposal" ) return nkPROPOSAL;
    if ( k == "decision" ) return nkDECISION;
    if ( k == "record" )   return nkRECORD;
    if ( k == "link" )     return nkLINK;
    if ( k == "testrun" )  return nkTESTRUN;
    if ( k == "buildrun" ) return nkBUILDRUN;
    return nkNONE;
}

// Revision handles kept open per document (design §3.3: a bounded LRU cache of
// derived state; never persisted).
const long REV_HANDLES_MAX = 8;

// How graph_edit_apply ends (Nexus L4c, design §3.4): APPLY commits the
// validated candidate; PROPOSE stores it as a proposal record and touches
// nothing else. One path up to the candidate's validation; the mode decides.
enum edit_mode : unsigned char { emAPPLY = 0, emPROPOSE };

// A proposal's status — DERIVED from the stream (design §3.4): open until a
// decision record names the proposal. The word is the wire form (a decision
// record's `decision`, a fold row's `status`); converted ONCE by
// proposal_status_of, written only through proposal_status_name.
enum proposal_status : unsigned char { psNONE = 0, psOPEN, psACCEPTED, psREJECTED, psWITHDRAWN };
const char *proposal_status_name(long s)
{
    switch ( s )
    {
	case psOPEN: return "open";
	case psACCEPTED: return "accepted";
	case psREJECTED: return "rejected";
	case psWITHDRAWN: return "withdrawn";
    }
    return "";
}
long proposal_status_of(var &word)
{
    if ( word.is_null() )
	return psNONE;
    for ( long k = psOPEN; k <= psWITHDRAWN; k = k + 1 )
	if ( word == proposal_status_name(k) )
	    return k;
    return psNONE;
}

// The ENVELOPE a JSON line on the connection seat carries (Nexus L4c): the api
// shape {"cmd", "args", "seq"} or a JSON-RPC 2.0 message {"jsonrpc", "id",
// "method", "params"} — the MCP seat over the same socket. Read ONCE at the
// line; the loop switches on the code. envNONE = neither (a refusal).
enum ide_envelope : unsigned char { envNONE = 0, envAPI, envRPC };
long envelope_of(var &req)
{
    if ( !req["jsonrpc"].is_null() )
	return envRPC;
    if ( !req["cmd"].is_null() )
	return envAPI;
    return envNONE;
}

// ---- the INTENT axis's vocabulary (Nexus L4d; design §3.5, §3.7, §3.10,
// §4.5): text on the wire and on the persisted line, a code in every reader.
// Each enum has its OUTPUT converter (X_name) and its INPUT converter (X_of —
// NONE = unknown or null, the caller's refusal). ----------------------------

// A record's kind (the owner's four + milestone + test).
enum record_kind : unsigned char { rkNONE = 0, rkTASK, rkREQUIREMENT, rkDECISION, rkWORKSET, rkMILESTONE, rkTEST };
const char *record_kind_name(long k)
{
    switch ( k )
    {
	case rkTASK: return "task";
	case rkREQUIREMENT: return "requirement";
	case rkDECISION: return "decision";
	case rkWORKSET: return "workset";
	case rkMILESTONE: return "milestone";
	case rkTEST: return "test";
    }
    return "";
}
long record_kind_of(var &word)
{
    if ( word.is_null() )
	return rkNONE;
    for ( long k = rkTASK; k <= rkTEST; k = k + 1 )
	if ( word == record_kind_name(k) )
	    return k;
    return rkNONE;
}

// A record's state (the envelope): the fold's default reads exclude
// superseded / rejected — an obsolete plan is never mistaken for truth.
enum record_state : unsigned char { rsNONE = 0, rsOPEN, rsACTIVE, rsDONE, rsSUPERSEDED, rsREJECTED };
const char *record_state_name(long s)
{
    switch ( s )
    {
	case rsOPEN: return "open";
	case rsACTIVE: return "active";
	case rsDONE: return "done";
	case rsSUPERSEDED: return "superseded";
	case rsREJECTED: return "rejected";
    }
    return "";
}
long record_state_of(var &word)
{
    if ( word.is_null() )
	return rsNONE;
    for ( long k = rsOPEN; k <= rsREJECTED; k = k + 1 )
	if ( word == record_state_name(k) )
	    return k;
    return rsNONE;
}

// A record event's operation.
enum record_op : unsigned char { roNONE = 0, roCREATE, roUPDATE, roCLOSE };
const char *record_op_name(long o)
{
    switch ( o )
    {
	case roCREATE: return "create";
	case roUPDATE: return "update";
	case roCLOSE: return "close";
    }
    return "";
}
long record_op_of(var &word)
{
    if ( word.is_null() )
	return roNONE;
    for ( long k = roCREATE; k <= roCLOSE; k = k + 1 )
	if ( word == record_op_name(k) )
	    return k;
    return roNONE;
}

// A link event's operation.
enum link_op : unsigned char { loNONE = 0, loLINK, loUNLINK };
const char *link_op_name(long o)
{
    switch ( o )
    {
	case loLINK: return "link";
	case loUNLINK: return "unlink";
    }
    return "";
}
long link_op_of(var &word)
{
    if ( word.is_null() )
	return loNONE;
    for ( long k = loLINK; k <= loUNLINK; k = k + 1 )
	if ( word == link_op_name(k) )
	    return k;
    return loNONE;
}

// A ref's kind — a durable key, never a raw graph id (design §3.5): a record
// by id, a symbol by file + kind + name, a commit by sha, a proposal by seq,
// an external system's row by source + key. The wire shape is {ref: word, …}.
enum ref_kind : unsigned char { refNONE = 0, refRECORD, refSYMBOL, refCOMMIT, refPROPOSAL, refEXTERNAL };
const char *ref_kind_name(long k)
{
    switch ( k )
    {
	case refRECORD: return "record";
	case refSYMBOL: return "symbol";
	case refCOMMIT: return "commit";
	case refPROPOSAL: return "proposal";
	case refEXTERNAL: return "external";
    }
    return "";
}
long ref_kind_of(var &word)
{
    if ( word.is_null() )
	return refNONE;
    for ( long k = refRECORD; k <= refEXTERNAL; k = k + 1 )
	if ( word == ref_kind_name(k) )
	    return k;
    return refNONE;
}

// The relations (the Nexus §2 vocabulary that is INTENT, not code — code edges
// stay the graph's calls / references ground truth).
enum nexus_rel : unsigned char {
    relNONE = 0, relIMPLEMENTS, relTESTS, relFIXES, relAFFECTS, relREQUESTED_BY, relDOCUMENTS,
    relSUPERSEDES, relBLOCKED_BY, relPLANNED_FOR, relREVIEWED_BY, relGENERATED_BY,
    relINTRODUCED_BY, relMODIFIED_BY
};
const char *rel_name(long r)
{
    switch ( r )
    {
	case relIMPLEMENTS: return "implements";
	case relTESTS: return "tests";
	case relFIXES: return "fixes";
	case relAFFECTS: return "affects";
	case relREQUESTED_BY: return "requested_by";
	case relDOCUMENTS: return "documents";
	case relSUPERSEDES: return "supersedes";
	case relBLOCKED_BY: return "blocked_by";
	case relPLANNED_FOR: return "planned_for";
	case relREVIEWED_BY: return "reviewed_by";
	case relGENERATED_BY: return "generated_by";
	case relINTRODUCED_BY: return "introduced_by";
	case relMODIFIED_BY: return "modified_by";
    }
    return "";
}
long rel_of(var &word)
{
    if ( word.is_null() )
	return relNONE;
    for ( long k = relIMPLEMENTS; k <= relMODIFIED_BY; k = k + 1 )
	if ( word == rel_name(k) )
	    return k;
    return relNONE;
}

// The authority that produced an answer row (design §3.7): every history /
// context / explain row names it; nothing is blended silently. pvINFERRED
// exists so a future heuristic is LABELLED — v1 emits none.
enum provenance : unsigned char { pvNONE = 0, pvCOMPILER, pvGIT, pvEVENT, pvRECORD, pvINFERRED };
const char *provenance_name(long p)
{
    switch ( p )
    {
	case pvCOMPILER: return "compiler";
	case pvGIT: return "git";
	case pvEVENT: return "event";
	case pvRECORD: return "record";
	case pvINFERRED: return "inferred";
    }
    return "";
}
long provenance_of(var &word)
{
    if ( word.is_null() )
	return pvNONE;
    for ( long k = pvCOMPILER; k <= pvINFERRED; k = k + 1 )
	if ( word == provenance_name(k) )
	    return k;
    return pvNONE;
}

// The nexus.* verbs (the third tool family) and the test.* verbs (the fourth:
// discover = L4d's records; list / candidates / run / results = L4e's runner
// seat, design §3.10).
enum nexus_verb : unsigned char {
    nvNONE = 0, nvRECORD, nvRECORDS, nvCREATE, nvUPDATE, nvCLOSE, nvLINK, nvUNLINK,
    nvCONTEXT, nvSOURCES, nvSYNC, nvEXPLAIN
};
enum test_verb : unsigned char { tvNONE = 0, tvDISCOVER, tvLIST, tvCANDIDATES, tvRUN, tvRESULTS };

// A test record's family (design §3.10): the .mad family with its fixtures,
// the doctest unit binaries, a manifest-declared external command.
enum test_family : unsigned char { tfNONE = 0, tfMAD, tfUNIT, tfEXTERNAL };
const char *test_family_name(long f)
{
    switch ( f )
    {
	case tfMAD: return "mad";
	case tfUNIT: return "unit";
	case tfEXTERNAL: return "external";
    }
    return "";
}
long test_family_of(var &word)
{
    if ( word.is_null() )
	return tfNONE;
    for ( long k = tfMAD; k <= tfEXTERNAL; k = k + 1 )
	if ( word == test_family_name(k) )
	    return k;
    return tfNONE;
}

// An MCP source manifest's vocabulary: the operations the Nexus asks an
// external system for; the manifest maps each to that server's tool. An
// unknown key refuses the manifest at load (the enum-law boundary).
enum nexus_op : unsigned char { noNONE = 0, noLIST_TASKS, noGET_TASK, noCREATE_TASK, noUPDATE_TASK, noCOMMENT };
const char *nexus_op_name(long o)
{
    switch ( o )
    {
	case noLIST_TASKS: return "list_tasks";
	case noGET_TASK: return "get_task";
	case noCREATE_TASK: return "create_task";
	case noUPDATE_TASK: return "update_task";
	case noCOMMENT: return "comment";
    }
    return "";
}
long nexus_op_of(var &word)
{
    if ( word.is_null() )
	return noNONE;
    for ( long k = noLIST_TASKS; k <= noCOMMENT; k = k + 1 )
	if ( word == nexus_op_name(k) )
	    return k;
    return noNONE;
}

// An MCP source's transport: stdio (a child over exec://) today; http waits on
// a madcdis channel (a stdio<->HTTP bridge process serves meanwhile).
enum mcp_transport : unsigned char { mtNONE = 0, mtSTDIO, mtHTTP };
const char *mcp_transport_name(long t)
{
    switch ( t )
    {
	case mtSTDIO: return "stdio";
	case mtHTTP: return "http";
    }
    return "";
}
long mcp_transport_of(var &word)
{
    if ( word.is_null() )
	return mtNONE;
    for ( long k = mtSTDIO; k <= mtHTTP; k = k + 1 )
	if ( word == mcp_transport_name(k) )
	    return k;
    return mtNONE;
}

// nexus.explain's detail (the L2 `detail` increment shares it).
enum explain_detail : unsigned char { edSUMMARY = 0, edFULL };
const char *explain_detail_name(long d)
{
    return d == edFULL ? "full" : "summary";
}
long explain_detail_of(var &word)
{
    if ( word.is_null() )
	return edSUMMARY;
    if ( word == explain_detail_name(edFULL) )
	return edFULL;
    if ( word == explain_detail_name(edSUMMARY) )
	return edSUMMARY;
    return -1;
}

// The nexus.* ladder (design §3.6's table): reads observe; a record is
// reviewable DATA, not source — create / update / close / link / unlink need
// the proposer tier; sync (pull from an external source) needs an editor;
// sources(dir) — a reload from another directory — is owner-only inside the
// verb (the plain listing observes).
long nexus_min_tier(long code)
{
    switch ( code )
    {
    case nvCREATE:
    case nvUPDATE:
    case nvCLOSE:
    case nvLINK:
    case nvUNLINK:
	return tierPROPOSER;
    case nvSYNC:
	return tierEDITOR;
    }
    return tierOBSERVER;
}

// The test.* ladder: discover writes records (proposer); run spends build /
// run resources (editor — cmd_min_tier's rule); list / candidates / results
// observe.
long test_min_tier(long code)
{
    switch ( code )
    {
    case tvDISCOVER:
	return tierPROPOSER;
    case tvRUN:
	return tierEDITOR;
    }
    return tierOBSERVER;
}

// ---- asset LAYERS (Nexus L4e; design §3.9) ----------------------------------
// Every axis is a layer an asset has or lacks, computed by ONE function
// (asset_layers_of, madcide_layers.inc) from facts the engine holds — never
// assumed by a verb. A BITSET: the wire words are the review's discovery
// vocabulary, converted once (layer_words out, layer_of in). executable and
// testable are NODE-relative (this node's offers); the rest are the asset's.
enum asset_layer : unsigned char {
    alTEXT = 1, alVERSIONED = 2, alMANAGED = 4, alLEXABLE = 8, alPARSEABLE = 16,
    alEXECUTABLE = 32, alTESTABLE = 64
};
const char *layer_name(long bit)
{
    switch ( bit )
    {
	case alTEXT: return "text";
	case alVERSIONED: return "versioned";
	case alMANAGED: return "managed";
	case alLEXABLE: return "lexable";
	case alPARSEABLE: return "parseable";
	case alEXECUTABLE: return "executable";
	case alTESTABLE: return "testable";
    }
    return "";
}
long layer_of(var &word)
{
    if ( word.is_null() )
	return 0;
    for ( long b = alTEXT; b <= alTESTABLE; b = b * 2 )
	if ( word == layer_name(b) )
	    return b;
    return 0;
}
// The set as wire words, ascending bit order.
void layer_words(var &out, long bits)
{
    out = {};
    for ( long b = alTEXT; b <= alTESTABLE; b = b * 2 )
	if ( (bits & b) != 0 )
	    out.push(layer_name(b));
}
// The first layer `need` asks for that `have` lacks; 0 = every layer present.
long layer_missing(long have, long need)
{
    for ( long b = alTEXT; b <= alTESTABLE; b = b * 2 )
	if ( (need & b) != 0 && (have & b) == 0 )
	    return b;
    return 0;
}

// The layers a verb family needs, DATA beside its tier (design §3.9): the
// gate runs at graph_call / nexus_call / test_call entry after the tier gate,
// before any structure is touched. graph.status needs only the bytes (it
// REPORTS the layers); the commit-level history verb needs the repository
// (a binary member has commit history); the line-level PAST verbs need the
// tree too; every other graph verb reads the tree.
long graph_min_layer(long code)
{
    switch ( code )
    {
    case gvSTATUS:
	return alTEXT;
    case gvCOMMITS:
	return alVERSIONED;
    case gvHISTORY:
    case gvREVISION:
    case gvDIFF:
	return alVERSIONED | alPARSEABLE;
    }
    return alPARSEABLE;
}
// Records, links, context and the sources need tracking (a project — every
// session asset has it); explain projects the tree too.
long nexus_min_layer(long code)
{
    if ( code == nvEXPLAIN )
	return alMANAGED | alPARSEABLE;
    return alMANAGED;
}
// Test records need tracking; RUNNING needs a runner on the target node.
long test_min_layer(long code)
{
    if ( code == tvRUN )
	return alMANAGED | alTESTABLE;
    return alMANAGED;
}

// A test run's verdict (design §3.10) — the runner's word converted once
// (the boundary), the enum in every reader and on every testrun event.
enum test_result : unsigned char { trNONE = 0, trPASS, trFAIL, trSKIP, trTIMEOUT, trERROR };
const char *test_result_name(long r)
{
    switch ( r )
    {
	case trPASS: return "pass";
	case trFAIL: return "fail";
	case trSKIP: return "skip";
	case trTIMEOUT: return "timeout";
	case trERROR: return "error";
    }
    return "";
}
long test_result_of(var &word)
{
    if ( word.is_null() )
	return trNONE;
    for ( long k = trPASS; k <= trERROR; k = k + 1 )
	if ( word == test_result_name(k) )
	    return k;
    return trNONE;
}

// What a node OFFERS (design §3.11, client-server §2.8 invariant 2): the
// services its Client record advertises. This node serves all three.
enum node_serve : unsigned char { nsNONE = 0, nsBUILD, nsRUN, nsTEST };
const char *node_serve_name(long s)
{
    switch ( s )
    {
	case nsBUILD: return "build";
	case nsRUN: return "run";
	case nsTEST: return "test";
    }
    return "";
}
long node_serve_of(var &word)
{
    if ( word.is_null() )
	return nsNONE;
    for ( long k = nsBUILD; k <= nsTEST; k = k + 1 )
	if ( word == node_serve_name(k) )
	    return k;
    return nsNONE;
}

// graph.insert's placement relative to its target; text only on the wire.
enum graph_where : unsigned char { gwNONE = 0, gwBEFORE, gwAFTER };

long graph_where_of(const char *name)
{
    if ( strcmp(name, "before") == 0 ) return gwBEFORE;
    if ( strcmp(name, "after") == 0 )  return gwAFTER;
    return gwNONE;
}

// The minimum tier a graph verb requires — the SAME ladder cmd_min_tier is
// (V6a tiers): reads and navigation observe; the three mutations need the
// PROPOSER tier (Nexus L4c, design §3.6) — at exactly that tier graph_call
// STORES a proposal, above it the edit lands; accept / reject are editor
// decisions; withdraw is the proposer's own or an editor's. An MCP client is
// born observer and promoted by clienttier — design §2.5.
long graph_min_tier(long code)
{
    switch ( code )
    {
    case gvINSERT:
    case gvREPLACE:
    case gvDELETE:
    case gvWITHDRAW:
	return tierPROPOSER;
    case gvACCEPT:
    case gvREJECT:
	return tierEDITOR;
    }
    return tierOBSERVER;
}
