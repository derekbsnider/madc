#include <stdlib.h>
#include <string.h>

thread_local bool madc_verbose __attribute__((weak)) = false;
thread_local int madc_opt_level __attribute__((weak)) = 1;
thread_local bool madc_debug_info __attribute__((weak)) = false;
thread_local bool madc_object_mode __attribute__((weak)) = false;

