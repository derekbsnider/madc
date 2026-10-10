//////////////////////////////////////////////////////////////////////////
//									//
// madc::memory — a project's memory (split arc 1C, plan			//
// 2026-10-09-madc-madcide-split-arc.md item 15; madcide's Nexus L4c-e,	//
// docs/plans/2026-09-13-nexus-L4-design.md). ONE event stream every	//
// document of a hub world rides: text splices and checkpoints (the	//
// memory subscribes to the world's text changes), applied node-ops,	//
// proposals and their decisions, intent records and links, test runs.	//
// A record is a JSON object {seq, ts, kind, path, doc, actor, ...}; seq	//
// is the stream's monotonic counter, path the asset's durable identity.	//
// The folds derive state from the records (a proposal's status, a	//
// record's fields, a test's latest run); nothing derived is stored.	//
//									//
// The stream is memory-only until bound to a file (a project's		//
// <base>.events, beside its manifest): bound, every record is appended	//
// to it as one JSON line, and a restore reads one back (a torn line —	//
// a crash mid-append — is skipped, never trusted).			//
//									//
// The memory registers its tools into a registry and is that		//
// registry's edit memory: graph.proposals / proposal / accept / reject /	//
// withdraw; nexus.record / records / create / update / close / link /	//
// unlink / context / explain; test.discover / list / candidates / run /	//
// results.								//
//									//
// Thread contract (C++ standard library convention): a memory is used	//
// from its world's thread; its stream and folds are its own.		//
//									//
//////////////////////////////////////////////////////////////////////////

#ifndef __MADC_MEMORY_H
#define __MADC_MEMORY_H 1

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "madc_tools.h"
#include "madcdis/document.h"	// text_listener

namespace madc {
namespace memory {

class project : public tools::edit_memory, public hub::text_listener
{
public:
    // Subscribes to `w`'s text changes; `w` outlives the memory.
    explicit project(hub::world &w);
    ~project();

    // Bind the stream to `path`: its records (if the file exists) restored
    // ahead of any this session logged — refused, with `why`, when this
    // session already logged records — then every new record appended to
    // it. A restored record's `doc` is the writing session's; the folds
    // and replay match a record by its path.
    bool bind(const std::string &path, std::string &why);
    const std::string &bound_path() const { return _path; }
    // The project's manifest ("" = none): an external test command's
    // {project}.
    void set_manifest(const std::string &path);
    const std::string &manifest() const { return _manifest; }

    // One record appended: seq, ts, path and doc stamped (`doc` names the
    // document it belongs to; 0 = the project's own). Returns its seq.
    int64_t append(value rec, hub::entity_id doc);
    int64_t head() const { return _head; }
    const std::vector<value> &records() const { return _records; }
    // The text of `doc` at seq `upto` (<= 0: the latest): replayed from the
    // newest checkpoint at or below it; the live text when the stream holds
    // no record of the document.
    std::string replay(hub::entity_id doc, int64_t upto) const;

    // The tools over this memory, and this memory as the registry's edit
    // memory.
    void add_tools(tools::registry &r);

    // tools::edit_memory
    bool propose(const tools::context &c, const value &proposal, value &result) override;
    void applied(const tools::context &c, const value &nodeop) override;
    // hub::text_listener
    void on_text_change(hub::world &w, const hub::text_change &c) override;

private:
    hub::world &_w;
    std::vector<value> _records;
    int64_t _head;
    std::string _path;			// "" = memory-only
    std::string _manifest;
    // A document's text as loaded, kept until its first edit is recorded:
    // the checkpoint that edit's replay starts from.
    std::map<hub::entity_id, std::string> _baseline;
    std::set<std::string> _followed;	// the paths the stream holds records of
    std::string path_of(hub::entity_id doc) const;
    project(const project &);
    project &operator=(const project &);
};

} // namespace memory
} // namespace madc

#endif // __MADC_MEMORY_H
