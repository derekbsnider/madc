// madcide_tests.inc — the EVIDENCE axis's records (Nexus L4d; design §3.10):
// test.discover folds the project's test families from the SAME conventions
// the canonical runner reads (tests/<name>.mad + its fixtures, skipping a
// .helper-owned unit; tests/unit/*.cpp) into `test` records — origin
// "convention", ext_key = the path, upserted like a sync. Rule #7: no per-test
// knowledge here. L4e adds test.list / candidates / run / results (the runner
// seat) to this file. Included by madcide_mcp.inc after madcide_nexus.inc.

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
    out = { "ok": true, "dir": dir, "discovered": seen, "created": created, "updated": updated };
}
