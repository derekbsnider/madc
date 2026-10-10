//////////////////////////////////////////////////////////////////////////
//									//
// madc::discovery — how a client finds a running madc server, and the	//
// rule that a project has ONE owner (split arc 1C, plan			//
// 2026-10-09-madc-madcide-split-arc.md item 16; madcide's session		//
// discovery, tools/madcide/madcide_discover.inc, whose second consumer	//
// this is). An ADVERTISEMENT is a JSON record in a state directory —	//
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

// What a claim came to.
enum class claim_result : unsigned char
{
    owner,		// this process holds the key and advertises it
    held_elsewhere,	// a live process holds it: `holder` is its record
    unadvertised	// nowhere to publish: this process runs, unadvertised
};

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
    const std::string &path() const { return _path; }

private:
    std::unique_ptr<detail::held_file> _file;
    std::string _path;
    claim(const claim &);
    claim &operator=(const claim &);
};

// Every live advertisement — a held file carrying a record — each with its
// "file"; an unheld or empty file is no one's and is skipped.
std::vector<value> scan();

} // namespace discovery
} // namespace madc

#endif // __MADC_DISCOVERY_H
