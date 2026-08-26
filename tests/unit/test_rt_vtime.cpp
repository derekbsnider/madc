// MT-4 virtual time — the pluggable time source's test gate (recon
// amendment 4: deterministic time-based gates). Under MADC_TASK_VTIME=1
// the scheduler's clock JUMPS to the earliest deadline whenever only
// timer-parked tasks exist: seconds of madc-visible sleeping complete in
// wall-clock milliseconds, in exact deadline order.
//
// Own binary ON PURPOSE: the vtime knob is read once and cached at the
// runtime's first use, so the env must be set before ANY task call in the
// process — sharing a binary with real-time task cases would race the
// cache.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/rt/rt_task.h"

namespace {

std::string g_order;

struct Nap { long long ms; char tag; };

void napper(void *arg)
{
    Nap *n = (Nap *)arg;
    __madc_task_sleep_ms(n->ms);
    g_order += n->tag;
}

} // namespace

TEST_CASE("virtual time: seconds of sleep, deadline order, no wall cost") {
    setenv("MADC_TASK_VTIME", "1", 1);	// before ANY runtime use (cached)

    Nap a{3000, 'a'}, b{1000, 'b'}, c{2000, 'c'};
    auto t0 = std::chrono::steady_clock::now();
    __madc_go(napper, &a);
    __madc_go(napper, &b);
    __madc_go(napper, &c);
    __madc_task_sleep_ms(4000);		// main sleeps LONGEST: all wake first
    __madc_task_join_all();
    double wall = std::chrono::duration<double>(
	std::chrono::steady_clock::now() - t0).count();

    // Deadline order, not spawn order.
    CHECK(g_order == "bca");
    // 10 madc-seconds of sleeping; virtual time must make it (near-)free.
    CHECK(wall < 1.0);
}
