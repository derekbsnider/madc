// MT-4b io-wait seat — rt-level gates: a task parked on an fd wakes through
// the scheduler's io hook (task_next_or_wait's wait arm), poll_readable's
// edges (empty / data / EOF — EOF IS readable progress: the read surfaces
// it), and the idempotent-enqueue belt (a task unparked twice must run
// ONCE; the blind relink used to truncate the ready queue and a double-run
// on one stack is catastrophic).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>
#include <unistd.h>

#include "../src/rt/rt_task.h"
#include "../src/madc_task_io.h"

namespace {

std::string g_order;
void *g_parked;

struct Rd { int fd; char tag; };

void reader(void *arg)
{
    Rd *r = (Rd *)arg;
    madc::taskio::wait_readable(r->fd);
    char b[8];
    ssize_t n = ::read(r->fd, b, sizeof b);
    g_order += r->tag;
    g_order += (n > 0) ? '+' : '0';
}

void parker(void *arg)
{
    (void)arg;
    g_parked = __madc_task_current();
    __madc_task_park();
    g_order += 'P';
}

} // namespace

TEST_CASE("io wait: a parked reader wakes when the fd turns readable") {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    CHECK(!madc::taskio::poll_readable(fds[0]));
    Rd r{fds[0], 'a'};
    g_order.clear();
    __madc_go(reader, &r);
    __madc_yield();			// reader probes, registers, parks
    CHECK(g_order == "");		// parked, not done
    CHECK(::write(fds[1], "x", 1) == 1);
    CHECK(madc::taskio::poll_readable(fds[0]));
    __madc_task_join_all();		// the io wait fires the fd
    CHECK(g_order == "a+");
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("io wait: EOF is readable progress (the read surfaces it)") {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    Rd r{fds[0], 'e'};
    g_order.clear();
    __madc_go(reader, &r);
    __madc_yield();			// parked on the empty pipe
    ::close(fds[1]);			// EOF arrives while parked
    CHECK(madc::taskio::poll_readable(fds[0]));
    __madc_task_join_all();
    CHECK(g_order == "e0");		// woke; read returned 0
    ::close(fds[0]);
}

TEST_CASE("idempotent enqueue: two unparks run the task once") {
    g_order.clear();
    g_parked = 0;
    __madc_go(parker, 0);
    __madc_yield();			// parker records itself and parks
    REQUIRE(g_parked != 0);
    __madc_task_unpark(g_parked);
    __madc_task_unpark(g_parked);	// the belt: second enqueue no-ops
    __madc_task_join_all();
    CHECK(g_order == "P");
    CHECK(__madc_task_live() == 0);
}
