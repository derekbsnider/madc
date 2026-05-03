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
    const std::string &dataset_name() const;
    void set_dataset_name(const std::string &name);
    const std::vector<std::string> &selected_fields() const;
    void set_selected_fields(const std::vector<std::string> &fields);
    bool has_where_equality() const;
    const std::string &where_field() const;
    const value &where_value() const;
    void set_where_equality(const std::string &field, const value &match);
    void clear_where_equality();
    bool has_lower_bound() const;
    const std::string &lower_bound_field() const;
    const value &lower_bound_value() const;
    bool lower_bound_inclusive() const;
    void set_lower_bound(const std::string &field,
			 const value &match,
			 bool inclusive = true);
    void clear_lower_bound();
    bool has_upper_bound() const;
    const std::string &upper_bound_field() const;
    const value &upper_bound_value() const;
    bool upper_bound_inclusive() const;
    void set_upper_bound(const std::string &field,
			 const value &match,
			 bool inclusive = true);
    void clear_upper_bound();
    bool has_limit() const;
    std::size_t row_limit() const;
    void set_limit(std::size_t count);
    void clear_limit();

private:
    kind _kind;
    std::string _text;
    std::string _dataset_name;
    std::vector<std::string> _selected_fields;
    std::string _where_field;
    value _where_value;
    bool _has_where_equality;
    std::string _lower_bound_field;
    value _lower_bound_value;
    bool _has_lower_bound;
    bool _lower_bound_inclusive;
    std::string _upper_bound_field;
    value _upper_bound_value;
    bool _has_upper_bound;
    bool _upper_bound_inclusive;
    std::size_t _row_limit;
    bool _has_limit;
};

class QueryBuilder
{
public:
    QueryBuilder();
    ~QueryBuilder();
    QueryBuilder(QueryBuilder &&other) noexcept;
    QueryBuilder &operator=(QueryBuilder &&other) noexcept;

    QueryBuilder(const QueryBuilder &other) = delete;
    QueryBuilder &operator=(const QueryBuilder &other) = delete;

    QueryBuilder &from(const std::string &dataset_name);
    QueryBuilder &where_eq(const std::string &field, const value &match);
    QueryBuilder &where_gt(const std::string &field, const value &match);
    QueryBuilder &where_gte(const std::string &field, const value &match);
    QueryBuilder &where_lt(const std::string &field, const value &match);
    QueryBuilder &where_lte(const std::string &field, const value &match);
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
