// madcide_tests.inc — the EVIDENCE axis (Nexus L4d records, L4e runs; design
// docs/plans/2026-09-13-nexus-L4-design.md §3.10, §3.11; plan
// 2026-09-14-nexus-L4e-verification-plan.md). Tests are RECORDS (test.discover
// folds the project's families from the SAME conventions the canonical runner
// reads: tests/<name>.mad + fixtures, skipping a .helper-owned unit;
// tests/unit/*.cpp), runs are EVENTS in the one stream tagged by the node
// that ran them, and the canonical runner (scripts/run_tests.sh) stays the
// ONE harness — the Nexus is its CLIENT: test.run spawns it per test
// directory with --report=json and reads one JSON object per verdict; it
// never re-reads a fixture convention. Rule #7: no per-test knowledge here.
// Included by madcide_mcp.inc after madcide_nexus.inc.
//
// Which structure answers what (the index-is-not-the-graph law):
//   discover   — the tests directory's conventions -> test records (upsert)
//   list       — the fold's test records + the LATEST testrun per test
//   candidates — the fold's `tests` links (explicit, by declaration); none =
//                the mad family's whole suite NAMED as the fallback, never run
//   run        — the runner spawned per directory (mad), an external record's
//                command through build_subst, one testrun event per verdict
//   results    — the testrun fold (filters: test, since, node)
// Thread contract: a run's child lives inside the one verb call on the
// session thread (cooperative poll_state + sleep_ms; cancel on the idle
// deadline — never a blocking read); every write is one clog_append.

// Upsert one test record: an existing (origin, ext_key) gets an update, else a
// create. Answers 1 for created, 0 for updated.
long test_record_upsert(long w, long es, long adoc, long self_id, const char *name0, long family,
			const char *asset0)
{
    var name = name0;			// own both ring texts first
    var asset = asset0;
    var fam = test_family_name(family);
    var fields = { "name": name, "family": fam, "asset": asset, "command": "" };
    var origin = "convention";
    var f;
    nexus_fold(f, w);
    var recs = f["records"];
    for ( var idv : f["ids"] )
    {
	var key = format("{}", idv);
	var r;
	keyed_get(r, recs, key.c_str());
	if ( r.is_null() )
	    continue;
	if ( r["origin"] == origin && r["ext_key"] == asset )
	{
	    record_update(w, es, adoc, self_id, idv.as_integer(), fields, rsNONE, roUPDATE);
	    return 0;
	}
    }
    record_create(w, es, adoc, self_id, rkTEST, fields, origin.c_str(), asset.c_str(), rsOPEN);
    return 1;
}

// test.discover(dir?): the .mad family (a .helper sibling = a unit owned by
// another test — skipped, the runner's rule) and the unit family.
void test_discover(var &out, long w, long es, long adoc, long self_id, const char *dir0)
{
    var dir = dir0;
    if ( strlen(dir.c_str()) == 0 )
	dir = "tests";
    long created = 0;
    long seen = 0;
    array mads;
    var pat = format("{}/*.mad", dir);
    perl::glob(mads, pat.c_str());
    php::sort(mads);
    var prefix = format("{}/", dir);
    for ( var p : mads )
    {
	var base;
	path_sans_ext(base, p);
	var helper = format("{}.helper", base);
	if ( php::file_exists(helper) )
	    continue;
	var name = php::str_replace(prefix.c_str(), "", base.c_str());
	seen = seen + 1;
	created = created + test_record_upsert(w, es, adoc, self_id, name.c_str(), tfMAD, p.c_str());
    }
    array units;
    var upat = format("{}/unit/*.cpp", dir);
    perl::glob(units, upat.c_str());
    php::sort(units);
    var uprefix = format("{}/unit/", dir);
    for ( var p : units )
    {
	var base;
	path_sans_ext(base, p);
	var name = php::str_replace(uprefix.c_str(), "", base.c_str());
	seen = seen + 1;
	created = created + test_record_upsert(w, es, adoc, self_id, name.c_str(), tfUNIT, p.c_str());
    }
    long updated = seen - created;
    out = { "ok": true, "directory": dir, "discovered": seen, "created": created, "updated": updated };
}

// ---- the runs: testrun EVENTS folded (design §3.10) --------------------------
// rows = every testrun (oldest first) passing the filters (test 0 = any test;
// since = seq > since; node < 0 = any node); latest = test-id-key -> its
// newest row (the derived state).
void testrun_fold(var &out, long w, long test, long since, long node)
{
    var rows = {};
    var latest;				// a null var made the map on its first keyed write
    long le = clog_find(w);
    if ( le > 0 )
    {
	var recs;
	clog_records(recs, w, le);	// the ONE reader, parsed once per revision
	for ( var rec : recs )
	{
	    if ( clog_kind_of(rec) != ckOTHER || nexus_kind_of(rec) != nkTESTRUN )
		continue;
	    if ( test != 0 && rec["test"].as_integer() != test )
		continue;
	    if ( rec["seq"].as_integer() <= since )
		continue;
	    if ( node >= 0 && rec["node"].as_integer() != node )
		continue;
	    rows.push(rec);
	    var key = format("{}", rec["test"]);
	    latest[key.c_str()] = rec;
	}
    }
    out = { "rows": rows, "latest": latest };
}

// Is the served document's buffer what its file on disk holds? The runner
// tests the DISK; a run against an unsaved buffer is labelled, never blocked.
bool doc_synced_to_disk(long w, long doc)
{
    var p;
    ui::get(p, w, doc, "path");
    var disk;
    if ( !php::file_get_contents(disk, p.c_str()) )
	return false;
    var t;
    ui::text(t, w, doc);
    return disk == t;
}

// ONE testrun event per verdict (design §3.10): the test record's id and
// names, the node that ran it, the result word, the exit status, the seconds,
// the runner's detail, the proposal it checks (0 = none).
long testrun_write(long w, long es, long adoc, long self_id, var &rec_test, long node, long result,
		   long exit, long seconds, var &detail, long proposal, bool synced)
{
    var word = test_result_name(result);
    var flds = rec_test["fields"];
    var rec = { "kind": "testrun", "es": es, "doc": adoc, "actor": self_id, "test": rec_test["id"],
		"name": flds["name"], "family": flds["family"], "node": node, "result": word, "exit": exit,
		"seconds": seconds, "detail": detail, "output_ref": "", "synced": synced };
    if ( proposal > 0 )
	rec["proposal"] = proposal;
    return clog_append(w, adoc, rec);
}

// Every stdout line of a child until it drains, under an IDLE deadline: no
// line within idle_ms cancels the child (SIGTERM) and answers false.
// Cooperative — poll_state + sleep_ms, never a blocking read on a silent
// pipe (the mcp_client_call idiom, collecting every line instead of one).
bool child_collect(var &lines, madc::channel &c, long idle_ms)
{
    lines = {};
    long waited = 0;
    for ( ;; )
    {
	long st = c.poll_state();
	if ( st < 0 )
	    return true;		// drained
	if ( st == 0 )
	{
	    if ( waited >= idle_ms )
	    {
		c.cancel();
		return false;
	    }
	    madc::sleep_ms(20);
	    waited = waited + 20;
	    continue;
	}
	var ln;
	if ( !c.readline(ln) )
	    return true;
	waited = 0;
	lines.push(ln);
    }
    return true;
}

// The live test records (family tfNONE = every family).
void test_records(var &out, long w, long family)
{
    out = {};
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
	if ( family != tfNONE && test_family_of(flds["family"]) != family )
	    continue;
	out.push(r);
    }
}

// test.list(family?): every live test record with its LATEST run (`last`,
// null when it never ran) — the derived state of the evidence axis.
void test_list(var &out, long w, long family)
{
    var recs;
    test_records(recs, w, family);
    var runs;
    testrun_fold(runs, w, 0, 0, -1);
    var latest = runs["latest"];
    var rows = {};
    for ( var r : recs )
    {
	var key = format("{}", r["id"]);
	var last;
	keyed_get(last, latest, key.c_str());
	var flds = r["fields"];
	var asset;
	keyed_get(asset, flds, "asset");
	var row = { "id": r["id"], "name": flds["name"], "family": flds["family"], "asset": asset,
		    "state": r["state"], "origin": r["origin"], "last": last };
	rows.push(row);
    }
    out = { "rows": rows };
}

// test.candidates(ref): the test records linked `tests` -> ref (explicit
// links only, ground truth by declaration). None linked: the HONEST fallback
// is the mad family's whole suite — NAMED, never run here (design §3.10; an
// agent runs it with test.run explicitly).
void test_candidates(var &out, long w, var &ref)
{
    var f;
    nexus_fold(f, w);
    var key;
    ref_key(key, ref);
    var recs = f["records"];
    var tests = {};
    var seen;				// record-id-key -> 1 (dynamic keys)
    for ( var l : f["links"] )
    {
	if ( rel_of(l["rel"]) != relTESTS || !(l["to_key"] == key) )
	    continue;
	var from = l["from"];
	if ( ref_kind_of(from["ref"]) != refRECORD )
	    continue;
	var rkey = format("{}", from["id"]);
	var was;
	keyed_get(was, seen, rkey.c_str());
	if ( !was.is_null() )
	    continue;
	var r;
	keyed_get(r, recs, rkey.c_str());
	if ( r.is_null() || record_kind_of(r["rkind"]) != rkTEST )
	    continue;
	seen[rkey.c_str()] = 1;
	tests.push(r);
    }
    if ( php::count(tests) > 0 )
    {
	out = { "ref": ref, "tests": tests, "fallback": false };
	return;
    }
    var mads;
    test_records(mads, w, tfMAD);
    var fam = test_family_name(tfMAD);
    long n = php::count(mads);
    var suite = { "family": fam, "count": n };
    out = { "ref": ref, "tests": tests, "fallback": true, "suite": suite };
}

// test.results(test?, since?, node?): the testrun rows + the latest per test.
void test_results(var &out, long w, long test, long since, long node)
{
    testrun_fold(out, w, test, since, node);
}

// The verdict row the runner reported for a test base (its lane rows are
// not the verdict), or left null.
void report_row_of(var &out, var &rows, var &base)
{
    for ( var r : rows )
	if ( r["lane"].is_null() && r["test"] == base )
	{
	    out = r;
	    return;
	}
}

// One verdict's testrun event + answer row; counts the pass / fail tallies.
void run_verdict(var &rows, long &passed, long &failed, long w, long es, long adoc, long self_id, var &r,
		 long node, long result, long exit, long seconds, var &detail, long proposal, bool synced)
{
    long seq = testrun_write(w, es, adoc, self_id, r, node, result, exit, seconds, detail, proposal, synced);
    if ( result == trPASS )
	passed = passed + 1;
    else
	failed = failed + 1;
    var flds = r["fields"];
    var row = { "test": r["id"], "name": flds["name"], "result": test_result_name(result), "exit": exit,
		"seconds": seconds, "detail": detail, "seq": seq };
    rows.push(row);
}

// test.run(ids, target?, proposal?): the mad records grouped by DIRECTORY —
// ONE canonical-runner spawn per directory (`--report=json`, the fixture
// protocol's answer read as data; MADC_BIN = the running madc, the ONE
// compiler); an external record's command through build_subst (exit 0 =
// pass); a family with no runner on this node refuses the WHOLE call
// (nothing half-runs). Every verdict = one testrun event tagged with the node
// (0 = here) and the proposal it checks.
void test_run_ids(var &out, IdeSession &S, long doc, long self_id, var &ids, long node, long proposal)
{
    long w = S.world();
    long es = S.es;
    long adoc = S.active(doc);
    var f;
    nexus_fold(f, w);
    var recs = f["records"];
    var groups;				// directory -> [record…] (dynamic keys)
    var dirs = {};
    var externals = {};
    var runner;
    runner_path_of(runner, tfMAD);
    for ( var idv : ids )
    {
	var key = format("{}", idv);
	var r;
	keyed_get(r, recs, key.c_str());
	if ( r.is_null() || record_kind_of(r["rkind"]) != rkTEST )
	{
	    var why = format("no test record {}", idv);
	    out = { "error": why };
	    return;
	}
	var flds = r["fields"];
	long fam = test_family_of(flds["family"]);
	if ( fam == tfMAD )
	{
	    if ( strlen(runner.c_str()) == 0 )
	    {
		var why = format("no runner for family {} on this node (looked for scripts/run_tests.sh under the project root, posix)", flds["family"]);
		out = { "error": why };
		return;
	    }
	    var dir = php::dirname(flds["asset"]);
	    var have;
	    keyed_get(have, groups, dir.c_str());
	    if ( have.is_null() )
	    {
		have = {};
		dirs.push(dir);
	    }
	    have.push(r);
	    groups[dir.c_str()] = have;
	}
	else if ( fam == tfEXTERNAL )
	{
	    var cmd;
	    keyed_get(cmd, flds, "command");
	    if ( cmd.is_null() || strlen(cmd.c_str()) == 0 )
	    {
		var why = format("test {} (external) names no command", idv);
		out = { "error": why };
		return;
	    }
	    externals.push(r);
	}
	else
	{
	    var why = format("no runner for family {} on this node (make -C src test owns the unit binaries)", flds["family"]);
	    out = { "error": why };
	    return;
	}
    }
    bool synced = doc_synced_to_disk(w, adoc);
    var self = madc::compiler_path();
    var rows = {};
    long passed = 0;
    long failed = 0;
    for ( var dir : dirs )
    {
	var members;
	keyed_get(members, groups, dir.c_str());
	var bases = "";
	for ( var r : members )
	{
	    var flds = r["fields"];
	    var bn = php::basename(flds["asset"], ".mad");
	    bases = format("{} {}", bases, bn);
	}
	var cmd = format("env MADC_BIN={} MADC_TEST_DIR={} bash {} --report=json{}", self, dir, runner, bases);
	var uri = format("exec://{}", cmd);
	madc::channel c(uri.c_str());
	if ( !c.ok() )
	{
	    var why = format("cannot spawn the runner: {}", c.last_error());
	    out = { "error": why };
	    return;
	}
	c.close_write();
	var lines;
	bool drained = child_collect(lines, c, 120000);
	c.close();
	long xs = c.exit_status();
	var reports = {};
	for ( var ln : lines )
	{
	    var rec;
	    if ( js::parse(rec, ln) && !rec["test"].is_null() )
		reports.push(rec);
	}
	for ( var r : members )
	{
	    var flds = r["fields"];
	    var bn = php::basename(flds["asset"], ".mad");
	    var rep;
	    report_row_of(rep, reports, bn);
	    long result = trERROR;
	    long exit = xs;
	    long seconds = 0;
	    var detail = "no report line from the runner";
	    if ( !drained )
		detail = "the runner went silent (cancelled)";
	    if ( !rep.is_null() )
	    {
		result = test_result_of(rep["result"]);
		if ( result == trNONE )
		    result = trERROR;
		if ( !rep["exit"].is_null() )
		    exit = rep["exit"].as_integer();
		if ( !rep["seconds"].is_null() )
		    seconds = rep["seconds"].as_integer();
		detail = "";
		var d = rep["detail"];
		if ( !d.is_null() )
		    detail = d;
	    }
	    run_verdict(rows, passed, failed, w, es, adoc, self_id, r, node, result, exit, seconds, detail, proposal, synced);
	}
    }
    for ( var r : externals )
    {
	var flds = r["fields"];
	var cmd0 = flds["command"];
	var cmd;
	build_subst(cmd, cmd0.c_str(), w, es, adoc);
	var uri = format("exec://{}", cmd);
	madc::channel c(uri.c_str());
	long result = trERROR;
	long exit = -1;
	var detail = "";
	if ( !c.ok() )
	    detail = c.last_error();
	else
	{
	    c.close_write();
	    var lines;
	    bool drained = child_collect(lines, c, 60000);
	    c.close();
	    exit = c.exit_status();
	    if ( !drained )
	    {
		result = trTIMEOUT;
		detail = "silent past the idle deadline (cancelled)";
	    }
	    else if ( exit == 0 )
		result = trPASS;
	    else
	    {
		result = trFAIL;
		detail = format("exit {}", exit);
	    }
	}
	run_verdict(rows, passed, failed, w, es, adoc, self_id, r, node, result, exit, 0, detail, proposal, synced);
    }
    long ran = php::count(rows);
    out = { "ok": true, "node": node, "ran": ran, "passed": passed, "failed": failed,
	    "synced": synced, "rows": rows };
}

// A proposal's CHECKS (design §3.10): the tests linked `tests` to every op's
// target symbol, run here and recorded under the proposal (the events carry
// its seq; the fold attaches them). Explicit candidates only — the family's
// whole suite is NAMED as the fallback, never run inside an accept.
// Informing, never blocking.
void proposal_checks(var &out, IdeSession &S, long doc, long self_id, var &ops, long seq)
{
    long w = S.world();
    long adoc = S.active(doc);
    var ids = {};
    var seen;				// test-id-key -> 1 (dynamic keys)
    var suite;
    bool fallback = false;
    for ( var op : ops )
    {
	var ref;
	symbol_ref_of_doc(ref, w, adoc, op["target"]);
	var cands;
	test_candidates(cands, w, ref);
	if ( cands["fallback"].as_boolean() )
	{
	    fallback = true;
	    suite = cands["suite"];
	    continue;
	}
	for ( var t : cands["tests"] )
	{
	    var key = format("{}", t["id"]);
	    var was;
	    keyed_get(was, seen, key.c_str());
	    if ( !was.is_null() )
		continue;
	    seen[key.c_str()] = 1;
	    ids.push(t["id"]);
	}
    }
    if ( php::count(ids) == 0 )
    {
	out = { "ran": 0, "passed": 0, "failed": 0, "fallback": fallback, "suite": suite };
	return;
    }
    var run;
    test_run_ids(run, S, doc, self_id, ids, 0, seq);
    var err = run["error"];
    if ( !err.is_null() )
    {
	out = { "ran": 0, "passed": 0, "failed": 0, "fallback": fallback, "error": err };
	return;
    }
    out = { "ran": run["ran"], "passed": run["passed"], "failed": run["failed"], "fallback": fallback,
	    "suite": suite, "rows": run["rows"] };
}
