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
    cmdVIEWCLOSE, cmdVIEWDOCK, cmdVIEWFOCUS, cmdVIEWOPEN, cmdVIEWSPLIT,
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
    rows[] = { "name": "viewsplit", "code": cmdVIEWSPLIT };
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
