///////////////////////////////////////////////////////////////////////////
//                                                                     //
// madc STL container helpers                                          //
//                                                                     //
// C++ helper functions for typed containers (vector, map, set, list). //
// Called from JIT code via cc.invoke().                                //
//                                                                     //
///////////////////////////////////////////////////////////////////////////

#include <string>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <list>
#include <queue>
#include <stack>
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include <asmjit/x86.h>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

using namespace std;

extern "C" {

//
// vector<int64_t> helpers
//
void *vector_int_construct(void *ptr)
{
    DBG(cout << "vector_int_construct(" << (uint64_t)ptr << ')' << endl);
    return new(ptr) std::vector<int64_t>;
}

void vector_int_destruct(void *ptr)
{
    DBG(cout << "vector_int_destruct(" << (uint64_t)ptr << ')' << endl);
    ((std::vector<int64_t> *)ptr)->~vector();
}

void vector_int_push_back(void *ptr, int64_t val)
{
    ((std::vector<int64_t> *)ptr)->push_back(val);
}

void vector_int_pop_back(void *ptr)
{
    ((std::vector<int64_t> *)ptr)->pop_back();
}

int64_t vector_int_at(void *ptr, int64_t index)
{
    std::vector<int64_t> &v = *(std::vector<int64_t> *)ptr;
    if ( index < 0 || (size_t)index >= v.size() ) return 0;
    return v[(size_t)index];
}

int64_t vector_int_size(void *ptr)
{
    return (int64_t)((std::vector<int64_t> *)ptr)->size();
}

void vector_int_clear(void *ptr)
{
    ((std::vector<int64_t> *)ptr)->clear();
}

int64_t vector_int_empty(void *ptr)
{
    return ((std::vector<int64_t> *)ptr)->empty() ? 1 : 0;
}

void vector_int_set(void *ptr, int64_t index, int64_t val)
{
    std::vector<int64_t> &v = *(std::vector<int64_t> *)ptr;
    if ( index >= 0 && (size_t)index < v.size() )
        v[(size_t)index] = val;
}

//
// vector<string> helpers
//
void *vector_str_construct(void *ptr)
{
    DBG(cout << "vector_str_construct(" << (uint64_t)ptr << ')' << endl);
    return new(ptr) std::vector<std::string>;
}

void vector_str_destruct(void *ptr)
{
    DBG(cout << "vector_str_destruct(" << (uint64_t)ptr << ')' << endl);
    ((std::vector<std::string> *)ptr)->~vector();
}

void vector_str_push_back(void *ptr, void *str)
{
    ((std::vector<std::string> *)ptr)->push_back(*(std::string *)str);
}

void vector_str_pop_back(void *ptr)
{
    ((std::vector<std::string> *)ptr)->pop_back();
}

void *vector_str_at(void *result, void *ptr, int64_t index)
{
    std::vector<std::string> &v = *(std::vector<std::string> *)ptr;
    std::string &res = *(std::string *)result;
    if ( index < 0 || (size_t)index >= v.size() ) { res.clear(); return result; }
    res = v[(size_t)index];
    return result;
}

int64_t vector_str_size(void *ptr)
{
    return (int64_t)((std::vector<std::string> *)ptr)->size();
}

void vector_str_clear(void *ptr)
{
    ((std::vector<std::string> *)ptr)->clear();
}

int64_t vector_str_empty(void *ptr)
{
    return ((std::vector<std::string> *)ptr)->empty() ? 1 : 0;
}

void vector_str_set(void *ptr, int64_t index, void *str)
{
    std::vector<std::string> &v = *(std::vector<std::string> *)ptr;
    if ( index >= 0 && (size_t)index < v.size() )
        v[(size_t)index] = *(std::string *)str;
}

//
// map<string, int64_t> helpers
//
void *map_str_int_construct(void *ptr)
{
    return new(ptr) std::map<std::string, int64_t>;
}

void map_str_int_destruct(void *ptr)
{
    ((std::map<std::string, int64_t> *)ptr)->~map();
}

void map_str_int_set(void *ptr, void *key, int64_t val)
{
    (*(std::map<std::string, int64_t> *)ptr)[*(std::string *)key] = val;
}

int64_t map_str_int_get(void *ptr, void *key)
{
    std::map<std::string, int64_t> &m = *(std::map<std::string, int64_t> *)ptr;
    auto it = m.find(*(std::string *)key);
    return it != m.end() ? it->second : 0;
}

int64_t map_str_int_contains(void *ptr, void *key)
{
    std::map<std::string, int64_t> &m = *(std::map<std::string, int64_t> *)ptr;
    return m.find(*(std::string *)key) != m.end() ? 1 : 0;
}

void map_str_int_erase(void *ptr, void *key)
{
    ((std::map<std::string, int64_t> *)ptr)->erase(*(std::string *)key);
}

int64_t map_str_int_size(void *ptr)
{
    return (int64_t)((std::map<std::string, int64_t> *)ptr)->size();
}

void map_str_int_clear(void *ptr)
{
    ((std::map<std::string, int64_t> *)ptr)->clear();
}

//
// map<string, string> helpers
//
void *map_str_str_construct(void *ptr)
{
    return new(ptr) std::map<std::string, std::string>;
}

void map_str_str_destruct(void *ptr)
{
    ((std::map<std::string, std::string> *)ptr)->~map();
}

void map_str_str_set(void *ptr, void *key, void *val)
{
    (*(std::map<std::string, std::string> *)ptr)[*(std::string *)key] = *(std::string *)val;
}

void *map_str_str_get(void *result, void *ptr, void *key)
{
    std::map<std::string, std::string> &m = *(std::map<std::string, std::string> *)ptr;
    std::string &res = *(std::string *)result;
    auto it = m.find(*(std::string *)key);
    res = it != m.end() ? it->second : "";
    return result;
}

int64_t map_str_str_contains(void *ptr, void *key)
{
    std::map<std::string, std::string> &m = *(std::map<std::string, std::string> *)ptr;
    return m.find(*(std::string *)key) != m.end() ? 1 : 0;
}

int64_t map_str_str_size(void *ptr)
{
    return (int64_t)((std::map<std::string, std::string> *)ptr)->size();
}

//
// set<string> helpers
//
void *set_str_construct(void *ptr)
{
    return new(ptr) std::set<std::string>;
}

void set_str_destruct(void *ptr)
{
    ((std::set<std::string> *)ptr)->~set();
}

void set_str_insert(void *ptr, void *str)
{
    ((std::set<std::string> *)ptr)->insert(*(std::string *)str);
}

int64_t set_str_contains(void *ptr, void *str)
{
    return ((std::set<std::string> *)ptr)->count(*(std::string *)str) ? 1 : 0;
}

void set_str_erase(void *ptr, void *str)
{
    ((std::set<std::string> *)ptr)->erase(*(std::string *)str);
}

int64_t set_str_size(void *ptr)
{
    return (int64_t)((std::set<std::string> *)ptr)->size();
}

void set_str_clear(void *ptr)
{
    ((std::set<std::string> *)ptr)->clear();
}

//
// set<int64_t> helpers
//
void *set_int_construct(void *ptr)
{
    return new(ptr) std::set<int64_t>;
}

void set_int_destruct(void *ptr)
{
    ((std::set<int64_t> *)ptr)->~set();
}

void set_int_insert(void *ptr, int64_t val)
{
    ((std::set<int64_t> *)ptr)->insert(val);
}

int64_t set_int_contains(void *ptr, int64_t val)
{
    return ((std::set<int64_t> *)ptr)->count(val) ? 1 : 0;
}

int64_t set_int_size(void *ptr)
{
    return (int64_t)((std::set<int64_t> *)ptr)->size();
}

//
// list<int64_t> helpers
//
void *list_int_construct(void *ptr)
{
    return new(ptr) std::list<int64_t>;
}

void list_int_destruct(void *ptr)
{
    ((std::list<int64_t> *)ptr)->~list();
}

void list_int_push_back(void *ptr, int64_t val)
{
    ((std::list<int64_t> *)ptr)->push_back(val);
}

void list_int_push_front(void *ptr, int64_t val)
{
    ((std::list<int64_t> *)ptr)->push_front(val);
}

int64_t list_int_size(void *ptr)
{
    return (int64_t)((std::list<int64_t> *)ptr)->size();
}

void list_int_clear(void *ptr)
{
    ((std::list<int64_t> *)ptr)->clear();
}

//
// list<string> helpers
//
void *list_str_construct(void *ptr)
{
    return new(ptr) std::list<std::string>;
}

void list_str_destruct(void *ptr)
{
    ((std::list<std::string> *)ptr)->~list();
}

void list_str_push_back(void *ptr, void *str)
{
    ((std::list<std::string> *)ptr)->push_back(*(std::string *)str);
}

void list_str_push_front(void *ptr, void *str)
{
    ((std::list<std::string> *)ptr)->push_front(*(std::string *)str);
}

int64_t list_str_size(void *ptr)
{
    return (int64_t)((std::list<std::string> *)ptr)->size();
}

} // extern "C"
