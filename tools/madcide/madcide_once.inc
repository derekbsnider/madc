// madcide_once.inc — the ui::NONE CLIENT (client-server design
// 2026-09-09 §2.3b, slice V1.5): `madcide <file> -c "<command> [arg]"`.
// A client at the NONE level has no rendered surface and expects no input
// (<bits/ui_enums>): it opens the session, posts ONE registry command,
// prints the resulting projection through the level-0 typesetter
// (ui::render_tree — the headless harness's shape, tests/testmadcide.mad)
// and answers with an exit status. No target is opened — nothing to open
// at this level; the session is driven directly, exactly as the harness
// drives it. The command NAME converts ONCE here, the command-line
// boundary, through the registry's one table (cmd_of — the same table the
// profiles and menus load through); the rest of the line is the command's
// ARGUMENT (IdeSession::command: what the prompt the command opens would
// have been typed). Facts this client pushes: no terminal (a request that
// needs one refuses on the status line, as the headless runs always have).
//
// THE VERDICT (the exit status):
//   0  the command ran; the projection is on stdout; the problems
//      projection carries no error
//   1  the file could not be opened (the reason on stderr), or the problems
//      projection carries ERROR rows after the command (`-c check` on a
//      broken file — the check's verdict; warnings never count)
//   2  the command line refused: an unknown command (stderr, nothing on
//      stdout — no session opens), or an argument for a command that
//      opened no prompt (the status line says so)
// A command's OWN refusal ("No terminal to suspend.") speaks on the status
// line inside the projection and reads as 0 until the api seat's per-reply
// verdicts land (design §2.7, V6).
long run_once(var &out, const char *path, bool ro, const char *cmdline)
{
    out = "";
    var line = php::trim(cmdline);
    var name = line;
    var arg = "";
    long sp = line.index(" ");
    if ( sp >= 0 )
    {
	name = perl::substr(line, 0, sp);
	var rest = perl::substr(line, sp + 1, strlen(line.c_str()) - sp - 1);
	arg = php::trim(rest.c_str());
    }
    long code = cmd_of(name.c_str());
    if ( code == cmdNONE )
    {
	println(stderr, "madcide: unknown command '{}' (the names are the registry's: tools/madcide/profiles/default.menu)", name);
	return 2;
    }
    IdeSession S;
    long doc = S.open(path, ro);
    if ( doc == 0 )
	println(stderr, "madcide: cannot read {}", path);
    if ( doc <= 0 )
	return 1;			// -1 already reported on stderr
    S.terminal(false);		// the level says so: nothing to hand over
    long adoc = S.active(doc);
    bool cont = true;
    long rc = 0;
    if ( !S.command(adoc, code, arg.c_str(), cont) )
	rc = 2;
    else if ( S.error_count() > 0 )
	rc = 1;
    var tree;
    S.compose_ide_tree(tree, S.active(adoc));
    ui::render_tree(out, S.world(), tree);
    S.close();
    return rc;
}
