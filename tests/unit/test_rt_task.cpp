#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "../../src/rt/rt_task.h"

#include <string>

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

// The MT-1 substrate contract: single OS thread, strict FIFO run-to-yield —
// so every interleaving below is DETERMINISTIC and pinned exactly.

namespace {

std::string g_trace;

struct NamedHop {
	char name;
	int hops;
};

void hopper(void *p)
{
	NamedHop *h = static_cast<NamedHop *>(p);
	for (int i = 0; i < h->hops; i++) {
		g_trace += h->name;
		__madc_yield();
	}
	g_trace += '.';
}

void spawner(void *)
{
	g_trace += 'S';
	static NamedHop child = { 'c', 1 };
	__madc_go(hopper, &child);
	g_trace += 's';
}

} // namespace

TEST_CASE("spawn does not switch; join drains in FIFO order")
{
	g_trace.clear();
	static NamedHop a = { 'a', 2 };
	static NamedHop b = { 'b', 2 };
	__madc_go(hopper, &a);
	__madc_go(hopper, &b);
	// The spawner keeps running (Go semantics).
	g_trace += 'M';
	CHECK(__madc_task_live() == 2);
	__madc_task_join_all();
	// FIFO run-to-yield: a b a b, then each finishes with '.'.
	CHECK(g_trace == "Mabab..");
	CHECK(__madc_task_live() == 0);
}

TEST_CASE("main yield interleaves with ready tasks")
{
	g_trace.clear();
	static NamedHop a = { 'x', 1 };
	__madc_go(hopper, &a);
	g_trace += '1';
	__madc_yield();          // runs x up to its first yield
	g_trace += '2';
	__madc_task_join_all();  // x finishes ('.')
	CHECK(g_trace == "1x2.");
}

TEST_CASE("tasks can spawn tasks; join drains transitively")
{
	g_trace.clear();
	__madc_go(spawner, nullptr);
	__madc_task_join_all();
	CHECK(g_trace == "Ssc.");
	CHECK(__madc_task_live() == 0);
}

TEST_CASE("yield and join are no-ops without live tasks")
{
	__madc_yield();
	__madc_task_join_all();
	CHECK(__madc_task_live() == 0);
}
