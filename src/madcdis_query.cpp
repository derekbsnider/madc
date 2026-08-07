// Core query IR and builder implementation.

#include "madcdis/query.h"

#include <sstream>
#include <utility>

namespace madc {

Query::Query()
    : _kind(kind::builder),
      _match_mode(match_mode::all),
      _has_where_equality(false),
      _has_where_inequality(false),
      _has_where_in(false),
      _has_where_not_in(false),
      _has_where_like(false),
      _has_lower_bound(false),
      _lower_bound_inclusive(true),
      _has_upper_bound(false),
      _upper_bound_inclusive(true),
      _row_limit(0),
      _has_limit(false)
{
}

Query::Query(kind k)
    : _kind(k),
      _match_mode(match_mode::all),
      _has_where_equality(false),
      _has_where_inequality(false),
      _has_where_in(false),
      _has_where_not_in(false),
      _has_where_like(false),
      _has_lower_bound(false),
      _lower_bound_inclusive(true),
      _has_upper_bound(false),
      _upper_bound_inclusive(true),
      _row_limit(0),
      _has_limit(false)
{
}

Query::kind Query::query_kind() const
{
    return _kind;
}

Query::match_mode Query::predicate_match_mode() const
{
    return _match_mode;
}

void Query::set_predicate_match_mode(match_mode mode)
{
    _match_mode = mode;
}

const std::string &Query::text() const
{
    return _text;
}

void Query::set_text(const std::string &text)
{
    _text = text;
}

const std::string &Query::dataset_name() const
{
    return _dataset_name;
}

void Query::set_dataset_name(const std::string &name)
{
    _dataset_name = name;
}

const std::vector<std::string> &Query::selected_fields() const
{
    return _selected_fields;
}

void Query::set_selected_fields(const std::vector<std::string> &fields)
{
    _selected_fields = fields;
}

bool Query::has_where_equality() const
{
    return _has_where_equality;
}

const std::string &Query::where_field() const
{
    return _where_field;
}

const value &Query::where_value() const
{
    return _where_value;
}

void Query::set_where_equality(const std::string &field, const value &match)
{
    _where_field = field;
    _where_value = match;
    _has_where_equality = true;
}

void Query::clear_where_equality()
{
    _where_field.clear();
    _where_value = value();
    _has_where_equality = false;
}

bool Query::has_where_inequality() const
{
    return _has_where_inequality;
}

const std::string &Query::where_ne_field() const
{
    return _where_ne_field;
}

const value &Query::where_ne_value() const
{
    return _where_ne_value;
}

void Query::set_where_inequality(const std::string &field, const value &match)
{
    _where_ne_field = field;
    _where_ne_value = match;
    _has_where_inequality = true;
}

void Query::clear_where_inequality()
{
    _where_ne_field.clear();
    _where_ne_value = value();
    _has_where_inequality = false;
}

bool Query::has_where_in() const
{
    return _has_where_in;
}

const std::string &Query::where_in_field() const
{
    return _where_in_field;
}

const std::vector<value> &Query::where_in_values() const
{
    return _where_in_values;
}

void Query::set_where_in(const std::string &field, const std::vector<value> &matches)
{
    _where_in_field = field;
    _where_in_values = matches;
    _has_where_in = true;
}

void Query::clear_where_in()
{
    _where_in_field.clear();
    _where_in_values.clear();
    _has_where_in = false;
}

bool Query::has_where_not_in() const
{
    return _has_where_not_in;
}

const std::string &Query::where_not_in_field() const
{
    return _where_not_in_field;
}

const std::vector<value> &Query::where_not_in_values() const
{
    return _where_not_in_values;
}

void Query::set_where_not_in(const std::string &field, const std::vector<value> &matches)
{
    _where_not_in_field = field;
    _where_not_in_values = matches;
    _has_where_not_in = true;
}

void Query::clear_where_not_in()
{
    _where_not_in_field.clear();
    _where_not_in_values.clear();
    _has_where_not_in = false;
}

bool Query::has_where_like() const
{
    return _has_where_like;
}

const std::string &Query::where_like_field() const
{
    return _where_like_field;
}

const value &Query::where_like_value() const
{
    return _where_like_value;
}

void Query::set_where_like(const std::string &field, const value &match)
{
    _where_like_field = field;
    _where_like_value = match;
    _has_where_like = true;
}

void Query::clear_where_like()
{
    _where_like_field.clear();
    _where_like_value = value();
    _has_where_like = false;
}

bool Query::has_lower_bound() const
{
    return _has_lower_bound;
}

const std::string &Query::lower_bound_field() const
{
    return _lower_bound_field;
}

const value &Query::lower_bound_value() const
{
    return _lower_bound_value;
}

bool Query::lower_bound_inclusive() const
{
    return _lower_bound_inclusive;
}

void Query::set_lower_bound(const std::string &field,
			    const value &match,
			    bool inclusive)
{
    _lower_bound_field = field;
    _lower_bound_value = match;
    _has_lower_bound = true;
    _lower_bound_inclusive = inclusive;
}

void Query::clear_lower_bound()
{
    _lower_bound_field.clear();
    _lower_bound_value = value();
    _has_lower_bound = false;
    _lower_bound_inclusive = true;
}

bool Query::has_upper_bound() const
{
    return _has_upper_bound;
}

const std::string &Query::upper_bound_field() const
{
    return _upper_bound_field;
}

const value &Query::upper_bound_value() const
{
    return _upper_bound_value;
}

bool Query::upper_bound_inclusive() const
{
    return _upper_bound_inclusive;
}

void Query::set_upper_bound(const std::string &field,
			    const value &match,
			    bool inclusive)
{
    _upper_bound_field = field;
    _upper_bound_value = match;
    _has_upper_bound = true;
    _upper_bound_inclusive = inclusive;
}

void Query::clear_upper_bound()
{
    _upper_bound_field.clear();
    _upper_bound_value = value();
    _has_upper_bound = false;
    _upper_bound_inclusive = true;
}

bool Query::has_limit() const
{
    return _has_limit;
}

std::size_t Query::row_limit() const
{
    return _row_limit;
}

void Query::set_limit(std::size_t count)
{
    _row_limit = count;
    _has_limit = true;
}

void Query::clear_limit()
{
    _row_limit = 0;
    _has_limit = false;
}

struct QueryBuilder::impl
{
    std::string dataset_name;
    Query::match_mode match_mode = Query::match_mode::all;
    std::vector<std::string> selects;
    std::string where_field;
    value where_value;
    bool has_where = false;
    std::string where_ne_field;
    value where_ne_value;
    bool has_where_ne = false;
    std::string where_in_field;
    std::vector<value> where_in_values;
    bool has_where_in = false;
    std::string where_not_in_field;
    std::vector<value> where_not_in_values;
    bool has_where_not_in = false;
    std::string where_like_field;
    value where_like_value;
    bool has_where_like = false;
    std::string lower_bound_field;
    value lower_bound_value;
    bool has_lower_bound = false;
    bool lower_bound_inclusive = true;
    std::string upper_bound_field;
    value upper_bound_value;
    bool has_upper_bound = false;
    bool upper_bound_inclusive = true;
    std::size_t row_limit = 0;
};

QueryBuilder::QueryBuilder()
    : _(new impl())
{
}

QueryBuilder::~QueryBuilder()
{
}

QueryBuilder::QueryBuilder(QueryBuilder &&other) noexcept
    : _(std::move(other._))
{
}

QueryBuilder &QueryBuilder::operator=(QueryBuilder &&other) noexcept
{
    if ( this != &other )
	_ = std::move(other._);
    return *this;
}

QueryBuilder &QueryBuilder::from(const std::string &dataset_name)
{
    _->dataset_name = dataset_name;
    return *this;
}

QueryBuilder &QueryBuilder::match_all()
{
    _->match_mode = Query::match_mode::all;
    return *this;
}

QueryBuilder &QueryBuilder::match_any()
{
    _->match_mode = Query::match_mode::any;
    return *this;
}

QueryBuilder &QueryBuilder::where_eq(const std::string &field, const value &match)
{
    _->where_field = field;
    _->where_value = match;
    _->has_where = true;
    return *this;
}

QueryBuilder &QueryBuilder::where_ne(const std::string &field, const value &match)
{
    _->where_ne_field = field;
    _->where_ne_value = match;
    _->has_where_ne = true;
    return *this;
}

QueryBuilder &QueryBuilder::where_in(const std::string &field, const std::vector<value> &matches)
{
    _->where_in_field = field;
    _->where_in_values = matches;
    _->has_where_in = true;
    return *this;
}

QueryBuilder &QueryBuilder::where_not_in(const std::string &field, const std::vector<value> &matches)
{
    _->where_not_in_field = field;
    _->where_not_in_values = matches;
    _->has_where_not_in = true;
    return *this;
}

QueryBuilder &QueryBuilder::where_like(const std::string &field, const value &match)
{
    _->where_like_field = field;
    _->where_like_value = match;
    _->has_where_like = true;
    return *this;
}

QueryBuilder &QueryBuilder::where_gte(const std::string &field, const value &match)
{
    _->lower_bound_field = field;
    _->lower_bound_value = match;
    _->has_lower_bound = true;
    _->lower_bound_inclusive = true;
    return *this;
}

QueryBuilder &QueryBuilder::where_gt(const std::string &field, const value &match)
{
    _->lower_bound_field = field;
    _->lower_bound_value = match;
    _->has_lower_bound = true;
    _->lower_bound_inclusive = false;
    return *this;
}

QueryBuilder &QueryBuilder::where_lte(const std::string &field, const value &match)
{
    _->upper_bound_field = field;
    _->upper_bound_value = match;
    _->has_upper_bound = true;
    _->upper_bound_inclusive = true;
    return *this;
}

QueryBuilder &QueryBuilder::where_lt(const std::string &field, const value &match)
{
    _->upper_bound_field = field;
    _->upper_bound_value = match;
    _->has_upper_bound = true;
    _->upper_bound_inclusive = false;
    return *this;
}

QueryBuilder &QueryBuilder::select(const std::vector<std::string> &fields)
{
    _->selects = fields;
    return *this;
}

QueryBuilder &QueryBuilder::limit(std::size_t count)
{
    _->row_limit = count;
    return *this;
}

Query QueryBuilder::build() const
{
    Query q(Query::kind::builder);
    q.set_dataset_name(_->dataset_name);
    q.set_predicate_match_mode(_->match_mode);
    q.set_selected_fields(_->selects);
    if ( _->has_where )
	q.set_where_equality(_->where_field, _->where_value);
    if ( _->has_where_ne )
	q.set_where_inequality(_->where_ne_field, _->where_ne_value);
    if ( _->has_where_in )
	q.set_where_in(_->where_in_field, _->where_in_values);
    if ( _->has_where_not_in )
	q.set_where_not_in(_->where_not_in_field, _->where_not_in_values);
    if ( _->has_where_like )
	q.set_where_like(_->where_like_field, _->where_like_value);
    if ( _->has_lower_bound )
	q.set_lower_bound(_->lower_bound_field, _->lower_bound_value, _->lower_bound_inclusive);
    if ( _->has_upper_bound )
	q.set_upper_bound(_->upper_bound_field, _->upper_bound_value, _->upper_bound_inclusive);
    if ( _->row_limit )
	q.set_limit(_->row_limit);
    std::ostringstream os;
    os << "FROM " << _->dataset_name;
    if ( _->has_where )
	os << " WHERE " << _->where_field << " = ?";
    if ( _->has_where_ne )
	os << (_->has_where ? " AND " : " WHERE ")
	   << _->where_ne_field << " != ?";
    if ( _->has_where_in )
    {
	os << ((_->has_where || _->has_where_ne) ? " AND " : " WHERE ")
	   << _->where_in_field;
	if ( _->where_in_values.empty() )
	    os << " IN ()";
	else
	{
	    os << " IN (";
	    for ( std::size_t i = 0; i < _->where_in_values.size(); ++i )
	    {
		if ( i )
		    os << ", ";
		os << "?";
	    }
	    os << ")";
	}
    }
    if ( _->has_where_not_in )
    {
	os << ((_->has_where || _->has_where_ne || _->has_where_in) ? " AND " : " WHERE ")
	   << _->where_not_in_field;
	if ( _->where_not_in_values.empty() )
	    os << " NOT IN ()";
	else
	{
	    os << " NOT IN (";
	    for ( std::size_t i = 0; i < _->where_not_in_values.size(); ++i )
	    {
		if ( i )
		    os << ", ";
		os << "?";
	    }
	    os << ")";
	}
    }
    if ( _->has_where_like )
	os << ((_->has_where || _->has_where_ne || _->has_where_in || _->has_where_not_in) ? " AND " : " WHERE ")
	   << _->where_like_field << " LIKE ?";
    if ( _->has_lower_bound )
	os << ((_->has_where || _->has_where_ne || _->has_where_in || _->has_where_not_in || _->has_where_like) ? " AND " : " WHERE ")
	   << _->lower_bound_field
	   << (_->lower_bound_inclusive ? " >= ?" : " > ?");
    if ( _->has_upper_bound )
	os << ((_->has_where || _->has_where_ne || _->has_where_in || _->has_where_not_in || _->has_where_like || _->has_lower_bound) ? " AND " : " WHERE ")
	   << _->upper_bound_field
	   << (_->upper_bound_inclusive ? " <= ?" : " < ?");
    if ( _->row_limit )
	os << " LIMIT " << _->row_limit;
    q.set_text(os.str());
    return q;
}

QueryBuilder query()
{
    return QueryBuilder();
}

Query parse_sql(const std::string &text)
{
    Query q(Query::kind::sql);
    q.set_text(text);
    return q;
}

Query parse_gql(const std::string &text)
{
    Query q(Query::kind::gql);
    q.set_text(text);
    return q;
}

} // namespace madc
