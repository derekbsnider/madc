#ifndef __LIBMADC_RELATION_H
#define __LIBMADC_RELATION_H 1

#include "libmadc/error.h"
#include "libmadc/value.h"

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

private:
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

#endif // __LIBMADC_RELATION_H
