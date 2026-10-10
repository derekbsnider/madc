//////////////////////////////////////////////////////////////////////////
//									//
// madc_jsonrpc.cpp — JSON-RPC 2.0's envelopes and a message's members	//
// (contract in include/madc_jsonrpc.h).				//
//									//
//////////////////////////////////////////////////////////////////////////

#include <map>
#include <string>

#include "madc_jsonrpc.h"
#include "libmadc/value.h"
#include "madcdis/world_text.h"	// the one value<->JSON bridge

namespace madc {
namespace jsonrpc {

namespace {
typedef std::map<std::string, value> fields;
}

value response(const value &id, const value &result)
{
    fields f;
    f["jsonrpc"] = value("2.0");
    f["id"] = id;
    f["result"] = result;
    return value::make_object(f);
}

value error_response(const value &id, error_code code, const std::string &message)
{
    fields e;
    e["code"] = value((int64_t)code);
    e["message"] = value(message);
    fields f;
    f["jsonrpc"] = value("2.0");
    f["id"] = id;
    f["error"] = value::make_object(e);
    return value::make_object(f);
}

value notification(const char *method, const value &params)
{
    fields f;
    f["jsonrpc"] = value("2.0");
    f["method"] = value(method);
    f["params"] = params;
    return value::make_object(f);
}

value request(int64_t id, const char *method, const value &params)
{
    value r = notification(method, params);
    r.object()["id"] = value(id);
    return r;
}

const value *field(const value &v, const char *name)
{
    if ( !v.is_object() )
	return 0;
    const fields &f = v.as_object();
    fields::const_iterator i = f.find(name);
    return i == f.end() ? 0 : &i->second;
}

std::string text_field(const value &v, const char *name)
{
    const value *f = field(v, name);
    return f && f->is_string() ? f->as_string() : std::string();
}

int64_t int_field(const value &v, const char *name, int64_t dflt)
{
    const value *f = field(v, name);
    return f && f->is_integer() ? f->as_integer() : dflt;
}

const value &object_field(const value &v, const char *name)
{
    static const value none;
    const value *f = field(v, name);
    return f ? *f : none;
}

bool decode(const std::string &text, value &out)
{
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if ( j.is_discarded() )
	return false;
    out = hub::detail::wt_json_to_value(j);
    return true;
}

std::string encode(const value &v)
{
    return hub::detail::wt_value_to_json(v).dump(
	-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace jsonrpc
} // namespace madc
