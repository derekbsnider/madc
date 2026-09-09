//////////////////////////////////////////////////////////////////////////
//									//
// file_kinds.cpp — the file-kind vocabulary meets TEXT here and nowhere	//
// else (include/madc_file_kinds.h carries the contract; the enum is	//
// include/madc/bits/file_kinds, shared with the dialect).		//
//									//
// Two tables. The NAME table below carries only the kinds that are not	//
// language standards — the family heads, madc's IR, the text, other-	//
// language and binary ranges; a standard's spelling comes from the ONE	//
// `--std=` table (Program::standard_canonical_name /			//
// standard_of_canonical_name, src/parser.cpp), so "c11" or "c++17" is	//
// spelled in exactly one place in the engine. The EXTENSION table is the	//
// input boundary a file OPEN converts at, once; the C / C++ rows answer	//
// the FAMILY (design doc §2.1: the standard within a family comes from	//
// the manifest or --std=, never from the extension).			//
//									//
// Thread contract: constant tables; pure functions.			//
//									//
//////////////////////////////////////////////////////////////////////////

#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"		// Program::standard_canonical_name / standard_of_canonical_name
#include "madc_file_kinds.h"

namespace {

struct KindNameRow {
	madc::file_kind kind;
	const char *name;
};

// Every non-standard kind, in enum order. A kind without a row here has no
// spelling (file_kind_name answers ""); every row spells back through
// file_kind_of.
const KindNameRow kKindNames[] = {
	{ madc::fkTEXT,       "text" },
	{ madc::fkMARKDOWN,   "markdown" },
	{ madc::fkJSON,       "json" },
	{ madc::fkHTML,       "html" },
	{ madc::fkCSS,        "css" },
	{ madc::fkJS,         "js" },
	{ madc::fkYAML,       "yaml" },
	{ madc::fkTOML,       "toml" },
	{ madc::fkXML,        "xml" },
	{ madc::fkSHELL,      "shell" },
	{ madc::fkMAKEFILE,   "makefile" },
	{ madc::fkINI,        "ini" },
	{ madc::fkCSV,        "csv" },
	{ madc::fkC,          "c" },		// the C family (a standard's row is --std='s)
	{ madc::fkCPP,        "c++" },		// the C++ family — the emitter's retained-source echo
	{ madc::fkMC11,       "mc11" },
	{ madc::fkOTHER,      "other" },
	{ madc::fkTYPESCRIPT, "typescript" },
	{ madc::fkPYTHON,     "python" },
	{ madc::fkRUST,       "rust" },
	{ madc::fkGO,         "go" },
	{ madc::fkJAVA,       "java" },
	{ madc::fkPHP,        "php" },
	{ madc::fkPERL,       "perl" },
	{ madc::fkRUBY,       "ruby" },
	{ madc::fkLUA,        "lua" },
	{ madc::fkSWIFT,      "swift" },
	{ madc::fkKOTLIN,     "kotlin" },
	{ madc::fkCSHARP,     "c#" },
	{ madc::fkASM,        "asm" },
	{ madc::fkBINARY,     "binary" },
	{ madc::fkOBJECT,     "object" },
	{ madc::fkEXECUTABLE, "executable" },
	{ madc::fkARCHIVE,    "archive" },
	{ madc::fkSHAREDLIB,  "sharedlib" },
	{ madc::fkIMAGE,      "image" },
	{ madc::fkPDF,        "pdf" },
	{ madc::fkAUDIO,      "audio" },
	{ madc::fkVIDEO,      "video" },
	{ madc::fkFONT,       "font" },
	{ madc::fkZIP,        "zip" },
};

struct ExtRow {
	const char *ext;	// lower-case, without the dot
	madc::file_kind kind;
};

// File extension -> kind. `.h` answers the C family: a header is C until a
// manifest or --std= says otherwise, and the C++ lens applies to both
// families. `.inc` / `.madv` are this repo's madc-dialect include and verb
// files.
const ExtRow kExtensions[] = {
	{ "mad", madc::fkMADC }, { "madv", madc::fkMADC }, { "inc", madc::fkMADC },
	{ "mc11", madc::fkMC11 },
	{ "c", madc::fkC }, { "h", madc::fkC },
	{ "cc", madc::fkCPP }, { "cpp", madc::fkCPP }, { "cxx", madc::fkCPP },
	{ "c++", madc::fkCPP }, { "hpp", madc::fkCPP }, { "hh", madc::fkCPP },
	{ "hxx", madc::fkCPP }, { "ipp", madc::fkCPP }, { "tpp", madc::fkCPP },
	{ "txt", madc::fkTEXT }, { "text", madc::fkTEXT },
	{ "md", madc::fkMARKDOWN }, { "markdown", madc::fkMARKDOWN },
	{ "json", madc::fkJSON },
	{ "html", madc::fkHTML }, { "htm", madc::fkHTML },
	{ "css", madc::fkCSS },
	{ "js", madc::fkJS }, { "mjs", madc::fkJS },
	{ "yaml", madc::fkYAML }, { "yml", madc::fkYAML },
	{ "toml", madc::fkTOML },
	{ "xml", madc::fkXML },
	{ "sh", madc::fkSHELL }, { "bash", madc::fkSHELL },
	{ "mk", madc::fkMAKEFILE },
	{ "ini", madc::fkINI }, { "cfg", madc::fkINI }, { "conf", madc::fkINI },
	{ "csv", madc::fkCSV },
	{ "ts", madc::fkTYPESCRIPT },
	{ "py", madc::fkPYTHON },
	{ "rs", madc::fkRUST },
	{ "go", madc::fkGO },
	{ "java", madc::fkJAVA },
	{ "php", madc::fkPHP },
	{ "pl", madc::fkPERL }, { "pm", madc::fkPERL },
	{ "rb", madc::fkRUBY },
	{ "lua", madc::fkLUA },
	{ "swift", madc::fkSWIFT },
	{ "kt", madc::fkKOTLIN },
	{ "cs", madc::fkCSHARP },
	{ "s", madc::fkASM }, { "asm", madc::fkASM },
	{ "o", madc::fkOBJECT }, { "obj", madc::fkOBJECT },
	{ "exe", madc::fkEXECUTABLE },
	{ "a", madc::fkARCHIVE }, { "lib", madc::fkARCHIVE },
	{ "so", madc::fkSHAREDLIB }, { "dylib", madc::fkSHAREDLIB }, { "dll", madc::fkSHAREDLIB },
	{ "png", madc::fkIMAGE }, { "jpg", madc::fkIMAGE }, { "jpeg", madc::fkIMAGE },
	{ "gif", madc::fkIMAGE }, { "bmp", madc::fkIMAGE }, { "svg", madc::fkIMAGE },
	{ "ico", madc::fkIMAGE }, { "webp", madc::fkIMAGE },
	{ "pdf", madc::fkPDF },
	{ "wav", madc::fkAUDIO }, { "mp3", madc::fkAUDIO }, { "ogg", madc::fkAUDIO },
	{ "flac", madc::fkAUDIO },
	{ "mp4", madc::fkVIDEO }, { "mkv", madc::fkVIDEO }, { "webm", madc::fkVIDEO },
	{ "ttf", madc::fkFONT }, { "otf", madc::fkFONT }, { "woff", madc::fkFONT },
	{ "woff2", madc::fkFONT },
	{ "zip", madc::fkZIP }, { "gz", madc::fkZIP }, { "tgz", madc::fkZIP },
	{ "xz", madc::fkZIP }, { "bz2", madc::fkZIP }, { "7z", madc::fkZIP },
};

// Extensionless basenames that ARE a kind (compared lower-case).
const ExtRow kBasenames[] = {
	{ "makefile", madc::fkMAKEFILE },
	{ "gnumakefile", madc::fkMAKEFILE },
};

std::string lower_of(const char *s, size_t n)
{
	std::string out(s, n);
	for ( size_t i = 0; i < out.size(); ++i )
		out[i] = (char)tolower((unsigned char)out[i]);
	return out;
}

}	// namespace

namespace madc {

const char *file_kind_name(int64_t kind)
{
	if ( kind <= 0 || kind > 0xFFFF )
		return "";
	const char *std_name =
		Program::standard_canonical_name((Program::LanguageStd)kind);
	if ( std_name && *std_name )
		return std_name;
	for ( size_t i = 0; i < sizeof(kKindNames) / sizeof(kKindNames[0]); ++i )
		if ( (int64_t)kKindNames[i].kind == kind )
			return kKindNames[i].name;
	return "";
}

int64_t file_kind_of(const char *name)
{
	if ( !name || !*name )
		return fkUNKNOWN;
	Program::LanguageStd std;
	if ( Program::standard_of_canonical_name(name, std) )
		return (int64_t)std;
	for ( size_t i = 0; i < sizeof(kKindNames) / sizeof(kKindNames[0]); ++i )
		if ( strcmp(kKindNames[i].name, name) == 0 )
			return kKindNames[i].kind;
	return fkUNKNOWN;
}

int64_t file_kind_of_path(const char *path)
{
	if ( !path || !*path )
		return fkUNKNOWN;
	// The basename: after the last separator (both spellings — a Windows
	// launch hands the IDE backslashed paths).
	const char *base = path;
	for ( const char *p = path; *p; ++p )
		if ( *p == '/' || *p == '\\' )
			base = p + 1;
	if ( !*base )
		return fkUNKNOWN;
	// The extension: after the LAST dot, and never a leading dot (".hidden"
	// has no extension).
	const char *dot = strrchr(base, '.');
	if ( dot == NULL || dot == base )
	{
		std::string b = lower_of(base, strlen(base));
		for ( size_t i = 0; i < sizeof(kBasenames) / sizeof(kBasenames[0]); ++i )
			if ( b == kBasenames[i].ext )
				return kBasenames[i].kind;
		return fkUNKNOWN;
	}
	std::string ext = lower_of(dot + 1, strlen(dot + 1));
	for ( size_t i = 0; i < sizeof(kExtensions) / sizeof(kExtensions[0]); ++i )
		if ( ext == kExtensions[i].ext )
			return kExtensions[i].kind;
	return fkUNKNOWN;
}

}	// namespace madc
