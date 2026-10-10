// Unit battery for madc::lsp::server — the LSP in madc's core (split arc
// 1C.12; design docs/plans/2026-10-10-lsp-in-core-design.md). Driven through
// handle(), one decoded message at a time, as testmadcide_lsp drives
// madcide's lsp_handle. ORACLE: LSP 3.17 for the lifecycle and the error
// codes (-32002 before initialize, -32600 after shutdown or for a
// notification with an id, -32601, -32602, -32803; exit 0 after shutdown,
// else 1); clangd 18 for the answers' shapes (DocumentSymbol's
// selectionRange is the name; a definition is the name's Location;
// didClose publishes an empty list); testmadcide_lsp.expect as the record
// of madcide's server (symbol kind 12, the seven-type legend, hover text).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>
#include <vector>

#include "madc_lsp.h"
#include "madc_documents.h"
#include "madcdis/hub.h"
#include "madcdis/world_text.h"	// the one value<->JSON bridge

using nlohmann::json;
using madc::hub::detail::wt_json_to_value;
using madc::hub::detail::wt_value_to_json;

namespace {

// One conversation with a server: send a message, read the reply and the
// notifications as JSON.
struct client
{
    madc::lsp::server srv;
    std::vector<json> notes;
    long next_id;
    client() : next_id(1) {}

    // A request; its reply.
    json request(const std::string &method, const json &params)
    {
	json m = { { "jsonrpc", "2.0" }, { "id", next_id++ }, { "method", method },
		   { "params", params } };
	return send(m);
    }
    // A notification; true when the server answered it (it should not).
    bool notify(const std::string &method, const json &params)
    {
	json m = { { "jsonrpc", "2.0" }, { "method", method }, { "params", params } };
	madc::value reply;
	std::vector<madc::value> ns;
	bool answered = srv.handle(wt_json_to_value(m), reply, ns);
	keep(ns);
	return answered;
    }
    json send(const json &m)
    {
	madc::value reply;
	std::vector<madc::value> ns;
	bool answered = srv.handle(wt_json_to_value(m), reply, ns);
	keep(ns);
	return answered ? wt_value_to_json(reply) : json();
    }
    void keep(const std::vector<madc::value> &ns)
    {
	notes.clear();
	for ( size_t i = 0; i < ns.size(); ++i )
	    notes.push_back(wt_value_to_json(ns[i]));
    }
    json initialize(bool utf8 = false)
    {
	json caps = json::object();
	if ( utf8 )
	    caps["general"] = { { "positionEncodings", { "utf-8", "utf-16" } } };
	json r = request("initialize", { { "processId", nullptr },
					 { "rootUri", "file:///work/space" },
					 { "capabilities", caps } });
	notify("initialized", json::object());
	return r;
    }
    void open(const std::string &uri, const std::string &text)
    {
	notify("textDocument/didOpen",
	       { { "textDocument", { { "uri", uri }, { "languageId", "c" },
				     { "version", 1 }, { "text", text } } } });
    }
};

json doc(const std::string &uri) { return { { "uri", uri } }; }

json at(const std::string &uri, int line, int character)
{
    return { { "textDocument", doc(uri) },
	     { "position", { { "line", line }, { "character", character } } } };
}

const char *URI = "file:///lsp-unit/calc.c";
const char *CALC =
    "long add(long a, long b)\n"		// 0
    "{\n"					// 1
    "    return a + b;\n"			// 2
    "}\n"					// 3
    "\n"					// 4
    "int main(void)\n"				// 5
    "{\n"					// 6
    "    return (int)add(2, 3);\n"		// 7
    "}\n";					// 8

} // namespace

TEST_CASE("lifecycle: initialize negotiates, shutdown then exit is 0")
{
    client c;
    json early = c.request("textDocument/hover", at(URI, 0, 0));
    CHECK(early["error"]["code"] == -32002);		// before initialize
    CHECK_FALSE(c.notify("textDocument/didOpen", { { "textDocument", doc(URI) } }));

    json init = c.initialize();
    json caps = init["result"]["capabilities"];
    CHECK(caps["positionEncoding"] == "utf-16");		// the default
    CHECK(caps["textDocumentSync"]["change"] == 2);
    CHECK(caps["hoverProvider"] == true);
    CHECK(caps["definitionProvider"] == true);
    CHECK(caps["referencesProvider"] == true);
    CHECK(caps["documentSymbolProvider"] == true);
    CHECK(caps["semanticTokensProvider"]["legend"]["tokenTypes"]
	  == json({ "keyword", "variable", "number", "string", "comment", "type",
		    "function" }));
    CHECK(init["result"]["serverInfo"]["name"] == "madc");
    CHECK(c.srv.root() == "/work/space");
    CHECK(c.request("initialize", json::object())["error"]["code"] == -32600);

    CHECK(c.request("shutdown", nullptr)["result"].is_null());
    CHECK(c.request("textDocument/hover", at(URI, 0, 0))["error"]["code"] == -32600);
    CHECK_FALSE(c.srv.exited());
    CHECK_FALSE(c.notify("exit", nullptr));
    CHECK(c.srv.exited());
    CHECK(c.srv.exit_code() == 0);

    client abrupt;
    abrupt.initialize();
    abrupt.notify("exit", nullptr);
    CHECK(abrupt.srv.exit_code() == 1);		// exit without shutdown
}

TEST_CASE("the encoding: utf-8 when offered")
{
    client c;
    CHECK(c.initialize(true)["result"]["capabilities"]["positionEncoding"] == "utf-8");
    CHECK(c.srv.encoding() == madc::lsp::position_encoding::utf8);
}

TEST_CASE("didOpen publishes, and the reads answer from madc::lang")
{
    client c;
    c.initialize();
    c.open(URI, CALC);
    REQUIRE(c.notes.size() == 1u);
    CHECK(c.notes[0]["method"] == "textDocument/publishDiagnostics");
    CHECK(c.notes[0]["params"]["uri"] == URI);
    CHECK(c.notes[0]["params"]["diagnostics"].size() == 0u);

    json syms = c.request("textDocument/documentSymbol",
			  { { "textDocument", doc(URI) } })["result"];
    REQUIRE(syms.size() == 2u);
    CHECK(syms[0]["name"] == "add");
    CHECK(syms[0]["kind"] == 12);
    CHECK(syms[0]["range"]["start"]["line"] == 0);
    CHECK(syms[0]["range"]["end"]["line"] == 3);
    CHECK(syms[0]["selectionRange"]["start"]["character"] == 5);
    CHECK(syms[0]["selectionRange"]["end"]["character"] == 8);

    json defs = c.request("textDocument/definition", at(URI, 7, 17))["result"];
    REQUIRE(defs.size() == 1u);
    CHECK(defs[0]["uri"] == URI);
    CHECK(defs[0]["range"]["start"]["line"] == 0);
    CHECK(defs[0]["range"]["start"]["character"] == 5);	// the name
    CHECK(c.request("textDocument/definition", at(URI, 4, 0))["result"].size() == 0u);

    json refs = c.request("textDocument/references",
			  { { "textDocument", doc(URI) },
			    { "position", { { "line", 0 }, { "character", 6 } } },
			    { "context", { { "includeDeclaration", false } } } })["result"];
    REQUIRE(refs.size() == 1u);
    CHECK(refs[0]["range"]["start"]["line"] == 7);
    CHECK(refs[0]["range"]["start"]["character"] == 16);
    CHECK(refs[0]["range"]["end"]["character"] == 19);
    json both = c.request("textDocument/references",
			  { { "textDocument", doc(URI) },
			    { "position", { { "line", 0 }, { "character", 6 } } },
			    { "context", { { "includeDeclaration", true } } } })["result"];
    CHECK(both.size() == 2u);

    json h = c.request("textDocument/hover", at(URI, 7, 17))["result"];
    CHECK(h["contents"]["kind"] == "plaintext");
    CHECK(h["contents"]["value"] == "Function add \xE2\x80\x94 defined on line 1");
    CHECK(h["range"]["start"]["character"] == 16);
    CHECK(c.request("textDocument/hover", at(URI, 4, 0))["result"].is_null());

    json toks = c.request("textDocument/semanticTokens/full",
			  { { "textDocument", doc(URI) } })["result"]["data"];
    REQUIRE(toks.size() > 0u);
    CHECK(toks.size() % 5 == 0u);
    CHECK(toks[0] == 0);			// `long` on the first line
    CHECK(toks[1] == 0);
    CHECK(toks[2] == 4);
    CHECK(toks[3] == 5);			// type
}

TEST_CASE("didChange edits the document incrementally and republishes")
{
    client c;
    c.initialize();
    c.open(URI, CALC);
    // Two changes, applied in order: `add` -> `sum` on line 0, then `b` ->
    // `c` on line 2 — an undeclared identifier (gcc: 3:16, error).
    c.notify("textDocument/didChange",
	     { { "textDocument", { { "uri", URI }, { "version", 2 } } },
	       { "contentChanges",
		 { { { "range", { { "start", { { "line", 0 }, { "character", 5 } } },
				  { "end", { { "line", 0 }, { "character", 8 } } } } },
		     { "text", "sum" } },
		   { { "range", { { "start", { { "line", 2 }, { "character", 15 } } },
				  { "end", { { "line", 2 }, { "character", 16 } } } } },
		     { "text", "c" } } } } });
    REQUIRE(c.notes.size() == 1u);
    json diags = c.notes[0]["params"]["diagnostics"];
    REQUIRE(diags.size() >= 1u);
    CHECK(diags[0]["severity"] == 1);
    CHECK(diags[0]["source"] == "madc");
    CHECK(diags[0]["range"]["start"]["line"] == 2);
    CHECK(diags[0]["range"]["start"]["character"] == 15);
    CHECK(diags[0]["range"]["end"]["character"] == 16);
    madc::hub::entity_id e = madc::document_find(c.srv.world(), "/lsp-unit/calc.c");
    REQUIRE(e != 0);
    CHECK(c.srv.world().text_of(e)->text().compare(0, 44,
	  "long sum(long a, long b)\n{\n    return a + c;") == 0);

    c.notify("textDocument/didChange",				// a whole-text change
	     { { "textDocument", { { "uri", URI }, { "version", 3 } } },
	       { "contentChanges", { { { "text", CALC } } } } });
    CHECK(c.notes[0]["params"]["diagnostics"].size() == 0u);

    c.notify("textDocument/didClose", { { "textDocument", doc(URI) } });
    REQUIRE(c.notes.size() == 1u);
    CHECK(c.notes[0]["params"]["diagnostics"].size() == 0u);	// cleared
    CHECK(c.request("textDocument/documentSymbol",			// closed, not on disk
		    { { "textDocument", doc(URI) } })["error"]["code"] == -32602);
}

TEST_CASE("positions: UTF-16 columns against UTF-8 bytes on a non-ASCII line")
{
    const char *text =
	"int twice(int v) { return v * 2; }\n"
	"int main(void) { /* \xC3\xA9 */ return twice(1); }\n";	// é: 2 bytes, 1 unit
    // `twice` on line 1 starts at byte 33, UTF-16 column 32.
    client u16;
    u16.initialize();
    u16.open(URI, text);
    json d16 = u16.request("textDocument/references",
			   { { "textDocument", doc(URI) },
			     { "position", { { "line", 1 }, { "character", 33 } } },
			     { "context", { { "includeDeclaration", false } } } })["result"];
    REQUIRE(d16.size() == 1u);
    CHECK(d16[0]["range"]["start"]["character"] == 32);
    CHECK(d16[0]["range"]["end"]["character"] == 37);

    client u8;
    u8.initialize(true);
    u8.open(URI, text);
    json d8 = u8.request("textDocument/references",
			 { { "textDocument", doc(URI) },
			   { "position", { { "line", 1 }, { "character", 34 } } },
			   { "context", { { "includeDeclaration", false } } } })["result"];
    REQUIRE(d8.size() == 1u);
    CHECK(d8[0]["range"]["start"]["character"] == 33);
    CHECK(d8[0]["range"]["end"]["character"] == 38);

    json h = u16.request("textDocument/hover", at(URI, 1, 34))["result"];
    CHECK(h["range"]["start"]["character"] == 32);
}

TEST_CASE("refusals: unknown methods, misplaced ids, other schemes, text kinds")
{
    client c;
    c.initialize();
    CHECK(c.request("textDocument/completion", at(URI, 0, 0))["error"]["code"] == -32601);
    CHECK_FALSE(c.notify("$/somethingElse", json::object()));	// dropped
    json withid = c.send({ { "jsonrpc", "2.0" }, { "id", 99 },
			   { "method", "textDocument/didSave" },
			   { "params", { { "textDocument", doc(URI) } } } });
    CHECK(withid["error"]["code"] == -32600);
    CHECK(withid["id"] == 99);
    CHECK(c.request("textDocument/hover", at("untitled:Untitled-1", 0, 0))
	  ["error"]["code"] == -32602);

    const char *notes = "file:///lsp-unit/notes.txt";
    c.open(notes, "just words\n");
    CHECK(c.request("textDocument/documentSymbol", { { "textDocument", doc(notes) } })
	  ["error"]["code"] == -32803);

    json bad = c.send(json::array());
    CHECK(bad["error"]["code"] == -32600);
}

TEST_CASE("a file:// URI's path is percent-decoded")
{
    client c;
    c.initialize();
    const char *spaced = "file:///lsp-unit/my%20file.c";
    c.open(spaced, "int f(void) { return 0; }\n");
    json syms = c.request("textDocument/documentSymbol",
			  { { "textDocument", doc(spaced) } })["result"];
    REQUIRE(syms.size() == 1u);
    CHECK(syms[0]["name"] == "f");
    CHECK(madc::document_find(c.srv.world(), "/lsp-unit/my file.c") != 0);
}
