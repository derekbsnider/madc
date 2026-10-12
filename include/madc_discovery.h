//////////////////////////////////////////////////////////////////////////
//									//
// madc::discovery — how a client finds a running madc server, and the	//
// rule that a project has ONE owner (split arc 1C, plan			//
// 2026-10-09-madc-madcide-split-arc.md item 16; madcide's sessions	//
// advertise through it since 1D). An ADVERTISEMENT is a JSON record in	//
// a state directory —							//
// never the project root (madcide's R1 ruling: it would land in every	//
// project's git status). A record is live while its process HOLDS its	//
// file (detail::held_file): the OS drops the hold when the process ends,	//
// however it ends, so a crashed server leaves no live record — the	//
// question neither a pid (reused) nor a stale file can answer.		//
//									//
// Every failure to advertise is non-fatal (no state directory, no	//
// writable file): the server runs unadvertised.				//
//									//
// Thread contract (C++ standard library convention): a claim is used	//
// from one thread; distinct claims are independent; scan reads only.	//
//									//
//////////////////////////////////////////////////////////////////////////

#ifndef __MADC_DISCOVERY_H
#define __MADC_DISCOVERY_H 1

#include <memory>
#include <string>
#include <vector>

#include "libmadc/value.h"
#include "madc/bits/tool_enums"	// claim_result: the one enum text

namespace madc {
namespace detail { class held_file; }	// src/madc_posix_io.h: a record's hold
namespace discovery {

// The advertisement directory: $MADC_SESSION_DIR, else
// $XDG_STATE_HOME/madc/sessions, else ~/.local/state/madc/sessions (the
// user's home on every host); "" = nowhere to publish.
std::string session_dir();

// A file-name-safe rendering of a canonical path: every separator, a drive
// colon and a space become '_', a leading '_' dropped.
std::string slug(const std::string &canonical);

// What a claim came to (owner / held_elsewhere: `holder` is the live
// process's record / unadvertised); the one text is <bits/tool_enums>.
typedef ::madc::claim_result claim_result;

// One owner per KEY (a project's canonical path): the claim holds
// <dir>/<slug(key)>.json for its life and publishes `record` there,
// stamped {key, pid, started, version, host}. A released claim removes the
// file; an empty one is a claim still writing its record.
class claim
{
public:
    claim();
    ~claim();
    claim_result take(const std::string &key, value record, value &holder);
    // The owner's record replaced by `record` (its documents changed, say),
    // stamped as take stamped it: false when this claim holds nothing.
    bool update(value record);
    // Withdraw: the record and the hold go (the destructor's, early).
    void release();
    const std::string &path() const { return _path; }

private:
    std::unique_ptr<detail::held_file> _file;
    std::string _path;
    value _stamp;			// {key, pid, started, version, host}
    claim(const claim &);
    claim &operator=(const claim &);
};

// Every live advertisement — a held file carrying a record — each with its
// "file"; an unheld or empty file is no one's and is skipped.
std::vector<value> scan();

// The live advertisement serving `file`, best first: a record whose
// "documents" holds the file beats one whose "root" directory merely holds
// it; null when none does (and for "" — an untitled buffer is no one's).
value find(const std::string &file);

} // namespace discovery

// ---- the dialect face (<ns_madc>; split arc 1D) ------------------------
// Declaration-only there; the methods resolve mangled-direct here. LAYOUT
// CONTRACT with <ns_madc>: a single pointer member, append-only.
// Non-copyable. A claim's codes are <bits/tool_enums> claim_result's.
class session_claim
{
public:
    session_claim();
    ~session_claim();
    // discovery::claim::take: `holder` the live owner's record when held
    // elsewhere. Returns a claim_result code.
    int64_t take(value &holder, const char *key, value &record);
    bool update(value &record);
    void release();
    // The advertisement's file ("" until owned).
    value &path(value &out);

private:
    discovery::claim *_claim;
    session_claim(const session_claim &);
    session_claim &operator=(const session_claim &);
};

// discovery::session_dir / scan / find as values.
value &session_dir(value &out);
value &session_scan(value &out);
value &session_find(value &out, const char *file);

} // namespace madc

#endif // __MADC_DISCOVERY_H
