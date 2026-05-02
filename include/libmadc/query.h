#ifndef __LIBMADC_QUERY_H
#define __LIBMADC_QUERY_H 1

#include "libmadc/error.h"
#include "libmadc/value.h"

#include <memory>
#include <string>
#include <vector>

namespace madc {

class Query
{
public:
    enum class kind
    {
	builder,
	sql,
	gql
    };

    Query();
    explicit Query(kind k);

    kind query_kind() const;
    const std::string &text() const;
    void set_text(const std::string &text);

private:
    kind _kind;
    std::string _text;
};

class QueryBuilder
{
public:
    QueryBuilder();

    QueryBuilder &from(const std::string &dataset_name);
    QueryBuilder &where_eq(const std::string &field, const value &match);
    QueryBuilder &select(const std::vector<std::string> &fields);
    QueryBuilder &limit(std::size_t count);
    Query build() const;

private:
    struct impl;
    std::unique_ptr<impl> _;
};

QueryBuilder query();

Query parse_sql(const std::string &text);
Query parse_gql(const std::string &text);

} // namespace madc

#endif // __LIBMADC_QUERY_H
