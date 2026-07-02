#include <stdlib.h>
#include <string.h>

thread_local bool madc_verbose __attribute__((weak)) = false;
thread_local int madc_opt_level __attribute__((weak)) = 1;

// See the declaration in include/datadef.h. Default ON; "0" opts out.
bool madc_tsubst_dep_parse_enabled()
{
	const char *v = getenv("MADC_XTEST_DEP_PARSE");
	return !v || strcmp(v, "0") != 0;
}
