// madcide_layers.inc — asset LAYERS (Nexus L4e; design
// docs/plans/2026-09-13-nexus-L4-design.md §3.9, §3.11; plan
// 2026-09-14-nexus-L4e-verification-plan.md). Every axis — text, git,
// tracking, lexing, parsing, execution, testing — is a layer an asset has or
// lacks; ONE function computes the set from facts the engine already holds
// (the document's kind from <bits/file_kinds>, the repository above it, the
// session's project, the test records and this node's runners) — no verb
// assumes a layer below it exists. Included by madcide_mcp.inc after
// madcide_past.inc (git_handle_of / git_relpath_of) and before the verb
// families; the test-record read reaches madcide_nexus.inc's fold through the
// declaration below (the L4d precedent: a declaration resolves a later body).
//
// Thread contract: pure reads of session-thread bag state per call (no
// cache); node_offers writes the es bag once per session.

void nexus_fold(var &out, long w);		// madcide_nexus.inc (included later)

// The compiler's kinds: the C, C++ and madc ranges of <bits/file_kinds> —
// what the tokenizer lexes and the parser accepts. <bits/file_kinds> makes
// depth of support a table per layer owned by that layer; this predicate is
// the seat's table for BOTH the lexed and the parsed layer today (they part
// the day a lexer-only kind gains rows). Beside it, not over it:
// cxx_view_applies (madcide_core.inc) answers the narrower C / C++ FAMILY
// question for the c++ render — a different concept, its own range test.
bool kind_compiler_range(long k)
{
    if ( k >= madc::fkC && k <= madc::fkC_LAST )
	return true;
    if ( k >= madc::fkCPP && k <= madc::fkCPP_LAST )
	return true;
    if ( k >= madc::fkMADC && k <= madc::fkMADC_LAST )
	return true;
    return false;
}

// An ASSET is a file; a pseudo-buffer ([build], [terminal]) is bytes only.
bool path_is_asset(var &p)
{
    if ( p.is_null() )
	return false;
    const char *s = p.c_str();
    if ( strlen(s) == 0 )
	return false;
    return s[0] != '[';
}

// The runner THIS node has for a test family ("" = none): the canonical
// runner for the mad family (bash, under the project root = the cwd; posix
// only — the env prefix and bash are not win32's); an external record
// carries its own command; the unit family runs through make -C src test —
// no seat here (v1).
void runner_path_of(var &out, long family)
{
    out = "";
    if ( family != tfMAD )
	return;
    if ( strcmp(madc::sys.platform, "win32") == 0 )
	return;
    var rp = "scripts/run_tests.sh";
    if ( php::file_exists(rp) )
	out = rp;
}

// Does any LIVE test record have a runner on this node?
bool tests_runnable_here(long w)
{
    var f;
    nexus_fold(f, w);
    var recs = f["records"];
    for ( var idv : f["ids"] )
    {
	var key = format("{}", idv);
	var r;
	keyed_get(r, recs, key.c_str());
	if ( r.is_null() || record_kind_of(r["rkind"]) != rkTEST )
	    continue;
	long st = record_state_of(r["state"]);
	if ( st == rsSUPERSEDED || st == rsREJECTED )
	    continue;
	var flds = r["fields"];
	long fam = test_family_of(flds["family"]);
	if ( fam == tfEXTERNAL )
	{
	    var cmd;
	    keyed_get(cmd, flds, "command");
	    if ( !cmd.is_null() && strlen(cmd.c_str()) > 0 )
		return true;
	    continue;
	}
	var rp;
	runner_path_of(rp, fam);
	if ( strlen(rp.c_str()) > 0 )
	    return true;
    }
    return false;
}

// THE layer owner (design §3.9). The bytes always; tracking for every asset
// of the session (the implicit single-file project IS a project, a manifest
// member too — never a pseudo-buffer); the git layer when a repository is
// above the file and the file is inside its work tree; lexing and parsing by
// the compiler's kinds; on THIS node (0) execution = what madc parses it
// runs, testing = live test records with a runner here. Another node has no
// offers yet (v1): its node-relative layers are 0.
long asset_layers_of(long w, long es, long doc, long node)
{
    long layers = alTEXT;
    var p;
    ui::get(p, w, doc, "path");
    if ( !path_is_asset(p) )
	return layers;
    layers = layers | alMANAGED;
    if ( kind_compiler_range(doc_kind(w, doc)) )
	layers = layers | alLEXABLE | alPARSEABLE;
    if ( git_handle_of(w, es, doc) > 0 )
    {
	var rel;
	git_relpath_of(rel, w, doc);
	if ( strlen(rel.c_str()) > 0 )
	    layers = layers | alVERSIONED;
    }
    if ( node == 0 )
    {
	if ( (layers & alPARSEABLE) != 0 )
	    layers = layers | alEXECUTABLE;
	if ( tests_runnable_here(w) )
	    layers = layers | alTESTABLE;
    }
    return layers;
}

// This node's Client record (design §3.11; client-server §2.8 invariants 1–2):
// what it offers — its platform, its toolchains (the running madc), the
// services it serves. Computed once onto the es bag ("offers"); graph.status
// reports it; a refusal of a foreign target quotes it.
void node_offers(var &out, long w, long es)
{
    ui::get(out, w, es, "offers");
    if ( !out.is_null() )
	return;
    var plat = madc::sys.platform;
    var toolchains = {};
    toolchains.push(gen_name(genMADC));
    var serves = {};
    for ( long s = nsBUILD; s <= nsTEST; s = s + 1 )
	serves.push(node_serve_name(s));
    out = { "node": 0, "platform": plat, "toolchains": toolchains, "serves": serves };
    ui::set(w, es, "offers", out);
}

// The command envelope's `target` (design §3.11, invariant 3): null / "" /
// 0 / this node's platform word = here (node 0); anything else names a node
// no one offers — refused with prose, never run locally by surprise.
bool target_node_of(long &node, var &why, var &target, long w, long es)
{
    node = 0;
    if ( target.is_null() )
	return true;
    var offers;
    node_offers(offers, w, es);
    if ( target.is_integer() )
    {
	if ( target.as_integer() == 0 )
	    return true;
    }
    else
    {
	if ( strlen(target.c_str()) == 0 || target == offers["platform"] )
	    return true;
    }
    var sv = php::implode(", ", offers["serves"]);
    why = format("no node offers '{}' (this node: {}; serves {})", target, offers["platform"], sv);
    return false;
}

// The layer refusal every family shares: the missing layer by name, the
// asset's kind, the verb, and what the asset HAS.
void layer_refuse(var &out, var &nm, long miss, long have, long w, long doc)
{
    var words;
    layer_words(words, have);
    var kw = madc::file_kind_name(doc_kind(w, doc));
    var has = php::implode(", ", words);
    var why = format("no {} layer for kind {} ('{}' needs it; this asset has: {})", layer_name(miss), kw, nm, has);
    tool_refuse(out, why);
}
