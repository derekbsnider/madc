/* madc_run_child.cpp — a Run in a child OF SELF where there is no fork: both
 * halves of the request (include/madc_run_child.h).
 *
 * The parent (madcrun:// and madcproj:// on Windows, parse_run and
 * project_run there) names the run in the child's environment; the child —
 * the same executable — serves it before its own main runs and exits with
 * the guest's status. The variable is consumed before the guest starts, so
 * the guest (which may itself include <ns_madc>) and anything it spawns run
 * as ordinary programs.
 *
 * Thread contract: see the header — process start only, no shared state.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"		// MadcEngine: the project child's engine
#include "madc_cir.h"		// madc_cir_execute_frozen
#include "madc_project.h"	// read_project_manifest, madc_project_execute
#include "madc_run_child.h"
#include "madcdis/process.h"	// Process, ProcessOptions

namespace {

const char run_child_variable[] = "MADC_RUN_CHILD";

// The request's words at the environment boundary — ONE spelling owner,
// both directions.
const char *run_child_kind_name(MadcRunChildKind kind)
{
    switch ( kind )
    {
	case rckFrozen:  return "frozen";
	case rckProject: return "project";
    }
    return "";
}

bool run_child_kind_from_name(const std::string &word, MadcRunChildKind &kind)
{
    for ( int k = rckFrozen; k <= rckProject; ++k )
	if ( word == run_child_kind_name((MadcRunChildKind)k) )
	{
	    kind = (MadcRunChildKind)k;
	    return true;
	}
    return false;
}

void forget_run_child_variable()
{
#ifdef _WIN32
    _putenv_s(run_child_variable, "");	// the UCRT's unsetenv
#else
    unsetenv(run_child_variable);
#endif
}

// The guest's status as the CLI reports it (--run-frozen, --project): a
// guest that never ran exits 1.
int run_child_exit_status(int rc)
{
    return rc < 0 ? 1 : rc;
}

} // namespace

std::unique_ptr<madc::Process> madc_run_child_process(
    MadcRunChildKind kind, const std::string &path,
    madc::ProcessOptions options)
{
    options.environment[run_child_variable] =
	std::string(run_child_kind_name(kind)) + " " + path;
    return std::unique_ptr<madc::Process>(new madc::Process(
	madc::DataSource("exec://" + madc_self_exe_path()), options));
}

int64_t madc_serve_run_child()
{
    const char *request = getenv(run_child_variable);
    if ( !request )
	return -1;
    const std::string text(request);
    forget_run_child_variable();

    // "<kind> <path>": the path is the rest of the value (a Windows temp
    // path may hold spaces).
    const std::string::size_type space = text.find(' ');
    MadcRunChildKind kind = rckFrozen;
    if ( space == std::string::npos
      || !run_child_kind_from_name(text.substr(0, space), kind) )
    {
	fprintf(stderr, "madc: %s='%s' names no run\n",
		run_child_variable, text.c_str());
	fflush(NULL);
	exit(1);
    }
    std::string path = text.substr(space + 1);
    char *guest_argv[2] = { &path[0], (char *)0 };	// the CLI's argv[0]
    int rc = -1;
    switch ( kind )
    {
	case rckFrozen:
	    rc = madc_cir_execute_frozen(path.c_str(), 1, guest_argv);
	    break;
	case rckProject:
	{
	    ProjectManifest manifest;
	    std::string err;
	    if ( !read_project_manifest(path, manifest, err) )
	    {
		fprintf(stderr, "madc: %s\n", err.c_str());
		break;
	    }
	    MadcEngine engine;
	    rc = madc_project_execute(engine, manifest, 1, guest_argv);
	    break;
	}
    }
    fflush(NULL);
    std::cout.flush();
    exit(run_child_exit_status(rc));
}
