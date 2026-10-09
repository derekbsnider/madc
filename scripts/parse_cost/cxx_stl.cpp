// parse-cost workload: the common libstdc++ / libc++ headers and a little use
// of each (scripts/parse_cost_gate.sh). Its cost is dominated by parsing and
// instantiating the standard library, which is what the gate watches.
#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <algorithm>
int main()
{
    std::vector<std::string> v;
    v.push_back("b");
    v.push_back("a");
    std::map<std::string, int> m;
    m[v[0]] = 1;
    std::cout << v[0] << m.size() << std::endl;
    return 0;
}
