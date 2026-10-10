//////////////////////////////////////////////////////////////////////////
//									//
// madc::lsp — the Language Server Protocol in madc's core (split arc	//
// 1C.12, design docs/plans/2026-10-10-lsp-in-core-design.md).		//
//									//
// A server answers one decoded JSON-RPC message at a time (handle) and	//
// knows no transport. Its documents are madc's, in its own hub world:	//
// a URI names a document by its path, the client's text is the truth	//
// while the document is open, and no document is "active". Every	//
// answer is madc::lang's (include/madc_lang.h); the server converts	//
// positions between the negotiated encoding (UTF-16 by default, UTF-8	//
// when the client offers it) and lang's byte columns, and nothing	//
// else.								//
//									//
// Thread contract (C++ standard library convention): one server, used	//
// from one thread; its world and documents are its own; parse handles	//
// keep the runtime-eval confinement (the thread and Program that	//
// opened them).							//
//									//
//////////////////////////////////////////////////////////////////////////

#ifndef __MADC_LSP_H
#define __MADC_LSP_H 1

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace madc {

class value;
namespace hub { class world; typedef uint32_t entity_id; }

namespace lsp {

// How a position's `character` counts: UTF-16 code units (the protocol's
// default) or bytes.
enum class position_encoding : unsigned char { utf16, utf8 };

class server
{
public:
    server();
    ~server();

    // One decoded message. True when it is answered: `reply` is the
    // response (a request's result or error, or a refusal of a malformed
    // message). `notes` gains the server's notifications (diagnostics), to
    // be sent after the reply, in order.
    bool handle(const value &message, value &reply, std::vector<value> &notes);

    // `path`'s document opened from disk before any client asks for it
    // (madc --lsp FILE); 0 when it cannot be read.
    hub::entity_id open_file(const std::string &path);

    bool exited() const;
    int exit_code() const;
    position_encoding encoding() const;
    // The workspace's path, from initialize's workspaceFolders or rootUri;
    // empty when the client named none.
    const std::string &root() const;
    hub::world &world();

private:
    struct state;
    std::unique_ptr<state> _s;
    server(const server &);
    server &operator=(const server &);
};

} // namespace lsp
} // namespace madc

#endif // __MADC_LSP_H
