// Unit tests for madc::dis::istring (madcdis/istring.h): the permanent,
// process-wide interned name. Verifies dedup by identity, pointer equality,
// byte ordering (an ordered map walks as it would over std::string), the
// std::string reference view, the empty value, concatenation, and that
// interning is safe from several threads at once.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <map>
#include <sstream>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>
#include "madcdis/istring.h"

using madc::dis::istring;

TEST_CASE("equal spellings are one entry")
{
    std::string built = std::string("std::__cxx11::basic_string") + "<char>";
    istring a("std::__cxx11::basic_string<char>");
    istring b(built);
    istring c("std::vector<int>");
    CHECK(a == b);
    CHECK(a.entry() == b.entry());
    CHECK(a != c);
    CHECK(a.c_str() == b.c_str());	// the same bytes, not merely equal bytes
    CHECK(a.size() == built.size());
}

TEST_CASE("the std::string view is the entry, not a copy")
{
    istring a("operator<<");
    const std::string &r1 = a;
    const std::string &r2 = a.str();
    CHECK(&r1 == &r2);
    CHECK(&r1 == a.entry());
    CHECK(r1 == "operator<<");
}

TEST_CASE("empty")
{
    istring d;
    istring e("");
    istring n((const char *)NULL);
    CHECK(d.empty());
    CHECK(d == e);
    CHECK(d == n);
    CHECK(d.size() == 0u);
    CHECK(std::string(d.c_str()).empty());
}

TEST_CASE("ordering is byte order")
{
    std::vector<std::string> words = { "pair", "_Rb_tree", "map", "Z", "a", "map_" };
    std::map<istring, int> by_name;
    std::map<std::string, int> by_string;
    for ( size_t i = 0; i < words.size(); ++i )
    {
	by_name[istring(words[i])] = (int)i;
	by_string[words[i]] = (int)i;
    }
    std::vector<std::string> a, b;
    for ( const auto &kv : by_name )
	a.push_back(kv.first.str());
    for ( const auto &kv : by_string )
	b.push_back(kv.first);
    CHECK(a == b);
    CHECK(istring("a") < istring("b"));
    CHECK(!(istring("b") < istring("b")));
}

TEST_CASE("comparisons and concatenation with std::string and const char*")
{
    istring a("__ns_std_move__o2");
    std::string s = "__ns_std_move__o2";
    CHECK(a == s);
    CHECK(s == a);
    CHECK(a == "__ns_std_move__o2");
    CHECK("__ns_std_move__o2" == a);
    CHECK(a != "__ns_std_move");
    CHECK(a + "_x" == "__ns_std_move__o2_x");
    CHECK("x_" + a == "x___ns_std_move__o2");
    CHECK(a + istring("!") == "__ns_std_move__o2!");
    CHECK(a.find("move") == 9u);
    CHECK(a.substr(0, 5) == "__ns_");
    CHECK(a.compare(0, 4, "__ns") == 0);
    std::ostringstream os;
    os << a;
    CHECK(os.str() == s);
}

TEST_CASE("hash is identity: an unordered map keyed by istring")
{
    std::unordered_map<istring, int> m;
    m[istring("alpha")] = 1;
    m[istring(std::string("al") + "pha")] += 1;
    CHECK(m.size() == 1u);
    CHECK(m[istring("alpha")] == 2);
}

TEST_CASE("interning from several threads yields one entry per spelling")
{
    const int threads = 4, per = 2000;
    std::vector<std::vector<const std::string *> > seen(threads);
    std::vector<std::thread> pool;
    for ( int t = 0; t < threads; ++t )
	pool.emplace_back([t, per, &seen]() {
	    for ( int i = 0; i < per; ++i )
		seen[t].push_back(istring("thread_name_" + std::to_string(i)).entry());
	});
    for ( std::thread &th : pool )
	th.join();
    for ( int t = 1; t < threads; ++t )
	CHECK(seen[t] == seen[0]);
}

TEST_CASE("a hold: the owner interns lock-free, nests, and another thread waits for it")
{
    using madc::dis::istring_table;
    std::atomic<bool> other_done(false);
    const std::string *seen_by_other = NULL;
    std::thread other;
    {
	istring_table::hold h;
	{
	    istring_table::hold nested;		// same thread: a no-op, no self-deadlock
	    CHECK(istring("held_name_a") == istring("held_name_a"));
	}
	CHECK(istring_table::instance().count() > 0);	// hold-aware, no self-deadlock
	other = std::thread([&]() {
	    seen_by_other = istring("held_name_b").entry();
	    other_done = true;
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	CHECK(!other_done);			// blocked on the held table
	istring mine("held_name_b");
	CHECK(mine.str() == "held_name_b");
    }
    other.join();
    CHECK(other_done);
    CHECK(seen_by_other == istring("held_name_b").entry());	// one entry either way
}

TEST_CASE("hashing: lengths around the 8-byte word stay distinct and equal spellings match")
{
    std::string s;
    std::vector<const std::string *> entries;
    for ( int n = 1; n <= 40; ++n )
    {
	s.push_back((char)('a' + n % 26));
	entries.push_back(istring(s).entry());
	CHECK(istring(s.data(), s.size()).entry() == entries.back());
    }
    for ( size_t i = 1; i < entries.size(); ++i )
	CHECK(entries[i] != entries[i - 1]);
}

TEST_CASE("one character: the table entry IS the interned spelling, every byte value")
{
    for ( int i = 0; i < 256; ++i )
    {
	const char c = (char)i;
	const istring one(1, c);
	CHECK(one.size() == 1u);
	CHECK(one[0] == c);
	CHECK(one.entry() == istring(&c, 1).entry());	// same entry as any other route
    }
    CHECK(istring(1, '(') == istring("("));
    CHECK(istring(3, '-') == istring("---"));		// n != 1 still interns the repeat
    CHECK(istring(0, 'x').empty());
}
