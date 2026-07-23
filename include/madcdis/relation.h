#ifndef __LIBMADCDIS_RELATION_H
#define __LIBMADCDIS_RELATION_H 1

#include "madcdis/dataset.h"
#include "libmadc/error.h"
#include "madcdis/query.h"
#include "libmadc/value.h"

#include <memory>
#include <string>
#include <vector>

namespace madc {

template <typename T>
class DataSet;

struct RelationKeyPair
{
    std::string from_field;
    std::string to_field;
};

enum class RelationKind
{
    key_match,
    positional,
    offset,
    graph_edge
};

template <typename A, typename B>
class Relation
{
public:
    Relation(DataSet<A> &from, DataSet<B> &to)
	: _from(&from), _to(&to), _kind(RelationKind::key_match)
    {}

    Relation &name(const std::string &relation_name)
    {
	_name = relation_name;
	return *this;
    }
    Relation &from(const std::string &field_name)
    {
	_from_field = field_name;
	return *this;
    }
    Relation &to(const std::string &field_name)
    {
	_to_field = field_name;
	return *this;
    }
    Relation &edge_label(const std::string &label)
    {
	_edge_label = label;
	return *this;
    }
    Relation &kind(RelationKind relation_kind)
    {
	_kind = relation_kind;
	return *this;
    }
    Relation &positional()
    {
	_kind = RelationKind::positional;
	_keys.clear();
	return *this;
    }
    Relation &offset(const std::string &from_field_name,
		     const std::string &to_field_name)
    {
	_kind = RelationKind::offset;
	_keys.clear();
	_keys.push_back(RelationKeyPair{from_field_name, to_field_name});
	return *this;
    }
    Relation &graph(const std::string &label = std::string())
    {
	_kind = RelationKind::graph_edge;
	if ( !label.empty() )
	    _edge_label = label;
	return *this;
    }
    Relation &key(const std::string &from_field_name,
		  const std::string &to_field_name)
    {
	_kind = RelationKind::key_match;
	_keys.push_back(RelationKeyPair{from_field_name, to_field_name});
	return *this;
    }

    const std::string &name() const { return _name; }
    const std::string &from_field() const { return _from_field; }
    const std::string &to_field() const { return _to_field; }
    const std::string &edge_label() const { return _edge_label; }
    RelationKind relation_kind() const { return _kind; }
    const std::vector<RelationKeyPair> &keys() const { return _keys; }

    bool resolve(const value &from_key, B &out, error *err = nullptr) const
    {
	if ( !_from || !_to )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation resolve failed: relation endpoints are not bound");
	    return false;
	}
	if ( _keys.empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation resolve failed: relation has no key mapping");
	    return false;
	}

	value intermediate;
	if ( !_from->get_field(from_key, _keys[0].from_field, intermediate, err) )
	    return false;
	bool found = false;
	if ( !resolve_intermediate(intermediate, out, found, err) )
	    return false;
	return found;
    }

    std::unique_ptr<Cursor<B>> query_related(const Query &from_query,
					     error *err = nullptr) const
    {
	Query to_query(Query::kind::builder);
	to_query.set_dataset_name(_to ? _to->name() : std::string());
	return query_related(from_query, to_query, err);
    }

    std::unique_ptr<Cursor<B>> query_related(const Query &from_query,
					     const Query &to_query,
					     error *err = nullptr) const
    {
	if ( !_from || !_to )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation query_related failed: relation endpoints are not bound");
	    return std::unique_ptr<Cursor<B>>();
	}
	if ( _keys.empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation query_related failed: relation has no key mapping");
	    return std::unique_ptr<Cursor<B>>();
	}
	if ( from_query.query_kind() != Query::kind::builder )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation query_related failed: source query must be a builder query");
	    return std::unique_ptr<Cursor<B>>();
	}
	if ( to_query.query_kind() != Query::kind::builder )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation query_related failed: target query must be a builder query");
	    return std::unique_ptr<Cursor<B>>();
	}
	if ( !to_query.selected_fields().empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation query_related failed: typed target query does not support projection");
	    return std::unique_ptr<Cursor<B>>();
	}
	if ( !to_query.dataset_name().empty() && to_query.dataset_name() != _to->name() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation query_related failed: target query targets dataset `" + to_query.dataset_name()
			     + "` but relation target is `" + _to->name() + "`");
	    return std::unique_ptr<Cursor<B>>();
	}

	std::unique_ptr<Cursor<A> > source_rows = _from->query(from_query, err);
	if ( !source_rows.get() )
	    return std::unique_ptr<Cursor<B>>();

	std::vector<B> resolved_rows;
	A from_row;
	while ( source_rows->next(from_row) )
	{
	    if ( to_query.has_limit() && resolved_rows.size() >= to_query.row_limit() )
		break;

	    value intermediate;
	    if ( !_from->get_field_from_row(from_row, _keys[0].from_field, intermediate, err) )
	    {
		source_rows->close();
		return std::unique_ptr<Cursor<B>>();
	    }

	    B related_row;
	    bool found = false;
	    if ( !resolve_intermediate(intermediate, related_row, found, err) )
	    {
		source_rows->close();
		return std::unique_ptr<Cursor<B>>();
	    }
	    if ( !found )
		continue;

	    if ( !_to->row_matches_query(related_row, to_query, err) )
		continue;

	    resolved_rows.push_back(related_row);
	}

	source_rows->close();
	return std::unique_ptr<Cursor<B> >(new detail::VectorCursor<B>(std::move(resolved_rows)));
    }

    std::unique_ptr<Cursor<value>> query_related_raw(const Query &from_query,
						     const Query &to_query,
						     error *err = nullptr) const
    {
	if ( !_from || !_to )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation query_related_raw failed: relation endpoints are not bound");
	    return std::unique_ptr<Cursor<value>>();
	}
	if ( _keys.empty() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation query_related_raw failed: relation has no key mapping");
	    return std::unique_ptr<Cursor<value>>();
	}
	if ( from_query.query_kind() != Query::kind::builder )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation query_related_raw failed: source query must be a builder query");
	    return std::unique_ptr<Cursor<value>>();
	}
	if ( to_query.query_kind() != Query::kind::builder )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation query_related_raw failed: target query must be a builder query");
	    return std::unique_ptr<Cursor<value>>();
	}
	if ( !to_query.dataset_name().empty() && to_query.dataset_name() != _to->name() )
	{
	    if ( err )
		*err = error(error::severity::error,
			     error::phase::runtime,
			     "Relation query_related_raw failed: target query targets dataset `" + to_query.dataset_name()
			     + "` but relation target is `" + _to->name() + "`");
	    return std::unique_ptr<Cursor<value>>();
	}

	std::unique_ptr<Cursor<A> > source_rows = _from->query(from_query, err);
	if ( !source_rows.get() )
	    return std::unique_ptr<Cursor<value>>();

	std::vector<value> projected_rows;
	A from_row;
	while ( source_rows->next(from_row) )
	{
	    if ( to_query.has_limit() && projected_rows.size() >= to_query.row_limit() )
		break;

	    value intermediate;
	    if ( !_from->get_field_from_row(from_row, _keys[0].from_field, intermediate, err) )
	    {
		source_rows->close();
		return std::unique_ptr<Cursor<value>>();
	    }

	    B related_row;
	    bool found = false;
	    if ( !resolve_intermediate(intermediate, related_row, found, err) )
	    {
		source_rows->close();
		return std::unique_ptr<Cursor<value>>();
	    }
	    if ( !found )
		continue;

	    if ( !_to->row_matches_query(related_row, to_query, err) )
		continue;

	    value projected;
	    if ( !_to->project_row(related_row, to_query, projected, err) )
	    {
		source_rows->close();
		return std::unique_ptr<Cursor<value>>();
	    }
	    projected_rows.push_back(projected);
	}

	source_rows->close();
	return std::unique_ptr<Cursor<value> >(new detail::VectorCursor<value>(std::move(projected_rows)));
    }

private:
    bool resolve_intermediate(const value &intermediate,
			      B &out,
			      bool &found,
			      error *err) const
    {
	found = false;

	switch ( _kind )
	{
	    case RelationKind::key_match:
	    {
		error lookup_err;
		if ( !_to->get(intermediate, out, &lookup_err) )
		{
		    if ( !lookup_err.message.empty() )
		    {
			if ( err )
			    *err = lookup_err;
			return false;
		    }
		    return true;
		}
		found = true;
		return true;
	    }

	    case RelationKind::offset:
	    {
		if ( !intermediate.is_integer() )
		{
		    if ( err )
			*err = error(error::severity::error,
				     error::phase::runtime,
				     "Relation resolve failed: offset field `" + _keys[0].from_field + "` is not an integer");
		    return false;
		}
		RecordLocator locator =
		    RecordLocator::at_byte_offset(static_cast<uint64_t>(intermediate.as_integer()));
		error lookup_err;
		if ( !_to->get_by_locator(locator, out, &lookup_err) )
		{
		    if ( !lookup_err.message.empty() )
		    {
			if ( err )
			    *err = lookup_err;
			return false;
		    }
		    return true;
		}
		found = true;
		return true;
	    }

	    case RelationKind::positional:
	    case RelationKind::graph_edge:
	    default:
		if ( err )
		    *err = error(error::severity::error,
				 error::phase::runtime,
				 "Relation resolve failed: relation kind is not directly resolvable yet");
		return false;
	}
    }

    DataSet<A> *_from;
    DataSet<B> *_to;
    std::string _name;
    std::string _from_field;
    std::string _to_field;
    std::string _edge_label;
    RelationKind _kind;
    std::vector<RelationKeyPair> _keys;
};

template <typename A, typename B>
Relation<A, B> relate(DataSet<A> &from, DataSet<B> &to);

template <typename A, typename B>
Relation<A, B> relate(DataSet<A> &from, DataSet<B> &to)
{
    return Relation<A, B>(from, to);
}

} // namespace madc

#endif // __LIBMADCDIS_RELATION_H
