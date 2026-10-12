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
// registry's edit memory: graph.status / history / proposals / proposal /	//
// accept / reject / withdraw; nexus.record / records / create / update /	//
// close / link / unlink / context / explain / sources / sync (an	//
// external system's tasks, from its MCP server); test.discover / list /	//
// candidates / run / results. With the registry's version history	//
// (madcgit), a node's history begins with its commits.			//
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
    // Subscribes to `w`'s text changes; `w` outlives the memory. A document
    // already open has its text now as the baseline its first edit records.
    explicit project(hub::world &w);
    ~project();

    // Bind the stream to `path`: its records (if the file exists) restored
    // ahead of any this session logged — refused, with `why`, when this
    // session already logged records — then every new record appended to
    // it. The stream's FIRST file, when it does not exist yet, starts with
    // the session's records so far (a manifest made mid-session keeps what
    // was already logged); binding the file already bound is a no-op. A
    // refused bind leaves the stream bound to no file (unbind): another
    // project's file never takes this one's records. A restored record's
    // `doc` is the writing session's; the folds and replay match a record
    // by its path.
    bool bind(const std::string &path, std::string &why);
    // The stream writes to no file from here; its records stay in memory
    // (a project whose memory another session holds).
    void unbind();
    const std::string &bound_path() const { return _path; }
    // The project's manifest ("" = none): an external test command's
    // {project}.
    void set_manifest(const std::string &path);
    const std::string &manifest() const { return _manifest; }

    // One record appended: seq, ts, path and doc stamped (`doc` names the
    // document it belongs to; 0 = the project's own). Returns its seq.
    int64_t append(value rec, hub::entity_id doc);
    // An APPLICATION's record (an IDE's build run): appended as append
    // does when its kind is one the memory folds but never writes itself;
    // 0 = refused — no kind the memory knows, or one it writes (a splice,
    // a checkpoint: the memory's own record of a document's text).
    int64_t note(value rec, hub::entity_id doc);
    int64_t head() const { return _head; }
    const std::vector<value> &records() const { return _records; }
    // The text of `doc` at seq `upto` (<= 0: the latest): replayed from the
    // newest checkpoint at or below it; the live text when the stream holds
    // no record of the document.
    std::string replay(hub::entity_id doc, int64_t upto) const;

    // An external system nexus.sync pulls tasks from, by its manifest
    // <dir>/<file>.mcp.json: {name, transport?: "stdio", command: [words],
    // env?: {K: V} (POSIX), vocabulary?: {list_tasks | get_task |
    // create_task | update_task | comment: {tool, args?, map?}}}.
    struct source
    {
	std::string name, file;
	std::string command;		// the words joined by single spaces, env prefix first
	value ops;			// a vocabulary word -> its {tool, args?, map?}
    };
    // Load every <dir>/*.mcp.json (by file name), replacing the sources:
    // false, with `why` = "<file>: <why>", on the first refusal — nothing
    // replaced. A missing directory holds no sources.
    bool load_sources(const std::string &dir, std::string &why);
    // The sources: on first need, madc's user config directory's mcp/
    // unless load_sources ran.
    bool sources(const std::vector<source> *&out, std::string &why);

    // The tools over this memory, and this memory as the registry's edit
    // memory.
    void add_tools(tools::registry &r);

    // tools::edit_memory
    bool propose(const tools::context &c, const value &proposal, value &result) override;
    void applied(const tools::context &c, const value &nodeop) override;
    // hub::text_listener
    void on_text_change(hub::world &w, const hub::text_change &c) override;

    // Its world was closed under it (a tool_host over a ui:: world HANDLE):
    // destruction then unsubscribes from no world.
    void world_gone() { _world_gone = true; }

private:
    hub::world &_w;
    bool _world_gone = false;
    std::vector<value> _records;
    int64_t _head;
    std::string _path;			// "" = memory-only
    bool _was_bound;			// some file held the stream once
    std::string _manifest;
    // A document's text as loaded, kept until its first edit is recorded:
    // the checkpoint that edit's replay starts from.
    std::map<hub::entity_id, std::string> _baseline;
    std::set<std::string> _followed;	// the paths the stream holds records of
    std::vector<source> _sources;
    bool _sources_loaded;
    std::string path_of(hub::entity_id doc) const;
    project(const project &);
    project &operator=(const project &);
};

} // namespace memory
} // namespace madc

#endif // __MADC_MEMORY_H
