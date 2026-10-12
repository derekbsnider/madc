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
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Program;
class ReplSession;

namespace madc {

class value;
class channel;
namespace hub { class world; typedef uint32_t entity_id; }

namespace lsp {

// How a position's `character` counts: UTF-16 code units (the protocol's
// default) or bytes.
enum class position_encoding : unsigned char { utf16, utf8 };

// The REPL session workspace/executeCommand runs commands in (design
// decision 5). Before each command the server hands the session its
// documents as the buffers and the client as the editor.
class command_host
{
public:
    virtual ~command_host() {}
    // The session, started again when the last one ended (a crash, %quit,
    // an exit in an entry); null, with `why`, when none starts.
    virtual ReplSession *session(std::string &why) = 0;
    // What the session's program printed and the diagnostics it rendered
    // since the last take; taking clears them.
    virtual void take(std::string &output, std::string &diagnostics) = 0;
};

// madc --lsp's: a crash-isolated backend session (madc -i's), its program's
// output piped — never the protocol's stdout — and its diagnostics plain.
// `options` are the backend's command line (Windows, which has no fork);
// `make_program` builds its configured Program (POSIX); `std_option` a
// --std= spelling, "" the Program's own.
class backend_commands : public command_host
{
public:
    typedef std::function<std::unique_ptr<Program>()> ProgramFactory;
    backend_commands(const std::vector<std::string> &options,
		     const ProgramFactory &make_program,
		     const std::string &std_option = std::string());
    ~backend_commands();
    ReplSession *session(std::string &why) override;
    void take(std::string &output, std::string &diagnostics) override;

private:
    struct state;
    std::unique_ptr<state> _s;
    backend_commands(const backend_commands &);
    backend_commands &operator=(const backend_commands &);
};

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

    // The framed loop over `io` (a channel in Content-Length message mode):
    // read one message, answer it, write the reply and then each note —
    // until `exit`, or until the client goes away. A body that is not JSON
    // answers -32700. Returns the process exit code the protocol names: 0
    // when `exit` followed `shutdown`, else 1.
    int serve(channel &io);

    // `path`'s document opened from disk before any client asks for it
    // (madc --lsp FILE); 0 when it cannot be read.
    hub::entity_id open_file(const std::string &path);

    // Where workspace/executeCommand runs (not owned); none: the server
    // advertises no commands. Set before initialize.
    void set_command_host(command_host *host);

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
    // workspace/executeCommand's answer.
    value run_command(const value &id, const value &params, std::vector<value> &notes);
    server(const server &);
    server &operator=(const server &);
};

} // namespace lsp
} // namespace madc

#endif // __MADC_LSP_H
