///////////////////////////////////////////////////////////////////////////
//                                                                       //
// pch.cpp — madc Pre-Compiled Header serialization                     //
//                                                                       //
// Serializes/deserializes post-lexer token streams in .madh format.     //
// Compressed with zlib (fallback) or zstd (preferred, if available).    //
//                                                                       //
///////////////////////////////////////////////////////////////////////////

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <queue>
#include <stack>
#include <set>
#include <sstream>

#include <zlib.h>

extern thread_local bool madc_verbose;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_pch.h"

// Try zstd if available (detected by configure)
#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

namespace madc_pch {

// --- Binary buffer helpers ---

static void write_u8(std::vector<uint8_t> &buf, uint8_t v)
{
    buf.push_back(v);
}

static void write_u16(std::vector<uint8_t> &buf, uint16_t v)
{
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
}

static void write_u32(std::vector<uint8_t> &buf, uint32_t v)
{
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 24) & 0xFF);
}

static void write_i64(std::vector<uint8_t> &buf, int64_t v)
{
    for ( int i = 0; i < 8; i++ )
	buf.push_back((v >> (i * 8)) & 0xFF);
}

static void write_f64(std::vector<uint8_t> &buf, double v)
{
    int64_t bits;
    memcpy(&bits, &v, 8);
    write_i64(buf, bits);
}

static void write_str(std::vector<uint8_t> &buf, const std::string &s)
{
    write_u32(buf, (uint32_t)s.size());
    buf.insert(buf.end(), s.begin(), s.end());
}

// --- Read helpers ---

struct Reader
{
    const uint8_t *data;
    size_t len;
    size_t pos;

    Reader(const uint8_t *d, size_t l) : data(d), len(l), pos(0) {}

    bool has(size_t n) const { return pos + n <= len; }

    uint8_t read_u8()
    {
	if ( !has(1) ) return 0;
	return data[pos++];
    }
    uint16_t read_u16()
    {
	if ( !has(2) ) return 0;
	uint16_t v = data[pos] | ((uint16_t)data[pos+1] << 8);
	pos += 2;
	return v;
    }
    uint32_t read_u32()
    {
	if ( !has(4) ) return 0;
	uint32_t v = data[pos] | ((uint32_t)data[pos+1] << 8)
		   | ((uint32_t)data[pos+2] << 16) | ((uint32_t)data[pos+3] << 24);
	pos += 4;
	return v;
    }
    int64_t read_i64()
    {
	if ( !has(8) ) return 0;
	int64_t v = 0;
	for ( int i = 0; i < 8; i++ )
	    v |= ((int64_t)data[pos++]) << (i * 8);
	return v;
    }
    double read_f64()
    {
	int64_t bits = read_i64();
	double v;
	memcpy(&v, &bits, 8);
	return v;
    }
    std::string read_str()
    {
	uint32_t slen = read_u32();
	if ( !has(slen) ) return "";
	std::string s((const char *)data + pos, slen);
	pos += slen;
	return s;
    }
};

static TokenBase *token_from_id(TokenID ti)
{
    switch ( ti )
    {
    case TokenID::tkSpace: return new TokenSpace(1);
    case TokenID::tkTab: return new TokenTab(1);
    case TokenID::tkEOL: return new TokenEOL(1);
    case TokenID::tkREM: return new TokenREM("");
    case TokenID::tkHash: return new TokenHash();
    case TokenID::tkAssign: return new TokenAssign();
    case TokenID::tkEquals: return new TokenEquals();
    case TokenID::tk3Eq: return new Token3Eq();
    case TokenID::tk3NotEq: return new Token3NotEq();
    case TokenID::tkPlus: return new TokenAdd();
    case TokenID::tkInc: return new TokenInc();
    case TokenID::tkSub: return new TokenSub();
    case TokenID::tkDec: return new TokenDec();
    case TokenID::tkMul: return new TokenMul();
    case TokenID::tkSlash: return new TokenDiv();
    case TokenID::tkBslsh: return new TokenBslsh();
    case TokenID::tkOpBrc: return new TokenOpBrc();
    case TokenID::tkClBrc: return new TokenClBrc();
    case TokenID::tkOpBrk: return new TokenOpBrk();
    case TokenID::tkClBrk: return new TokenClBrk();
    case TokenID::tkOpSqr: return new TokenOpSqr();
    case TokenID::tkClSqr: return new TokenClSqr();
    case TokenID::tkNeg: return new TokenNeg();
    case TokenID::tkNot: return new TokenLnot();
    case TokenID::tkBand: return new TokenBand();
    case TokenID::tkLand: return new TokenLand();
    case TokenID::tkBor: return new TokenBor();
    case TokenID::tkLor: return new TokenLor();
    case TokenID::tkXor: return new TokenXor();
    case TokenID::tkMod: return new TokenMod();
    case TokenID::tkQmark: return new TokenTerQ();
    case TokenID::tkColon: return new TokenTerC();
    case TokenID::tkNS: return new TokenNS();
    case TokenID::tkSemi: return new TokenSemi();
    case TokenID::tkComma: return new TokenComma();
    case TokenID::tkDot: return new TokenDot();
    case TokenID::tkDeRef: return new TokenDeRef();
    case TokenID::tkQuote: return new TokenQuote();
    case TokenID::tkApost: return new TokenApost();
    case TokenID::tkGT: return new TokenGT();
    case TokenID::tkLT: return new TokenLT();
    case TokenID::tkBSL: return new TokenBSL();
    case TokenID::tkBSR: return new TokenBSR();
    case TokenID::tkAddEq: return new TokenAddEq();
    case TokenID::tkBSLEq: return new TokenBSLEq();
    case TokenID::tkBSREq: return new TokenBSREq();
    case TokenID::tkBandEq: return new TokenBandEq();
    case TokenID::tkBnot: return new TokenBnot();
    case TokenID::tkBorEq: return new TokenBorEq();
    case TokenID::tkDivEq: return new TokenDivEq();
    case TokenID::tkFuncOp: return new TokenFuncOp();
    case TokenID::tkGE: return new TokenGE();
    case TokenID::tkLE: return new TokenLE();
    case TokenID::tkLnot: return new TokenLnot();
    case TokenID::tkModEq: return new TokenModEq();
    case TokenID::tkMulEq: return new TokenMulEq();
    case TokenID::tk3Way: return new Token3Way();
    case TokenID::tkNotEq: return new TokenNotEq();
    case TokenID::tkSubEq: return new TokenSubEq();
    case TokenID::tkXorEq: return new TokenXorEq();
    case TokenID::tkColEq: return new TokenColEq();
    case TokenID::tkArrayOp: return new TokenArrayOp();
    case TokenID::tkFatArrow: return new TokenFatArrow();

    case TokenID::tkDO: return new TokenDO();
    case TokenID::tkIF: return new TokenIF();
    case TokenID::tkFOR: return new TokenFOR();
    case TokenID::tkELSE: return new TokenELSE();
    case TokenID::tkRETURN: return new TokenRETURN();
    case TokenID::tkGOTO: return new TokenGOTO();
    case TokenID::tkCASE: return new TokenCASE();
    case TokenID::tkBREAK: return new TokenBREAK();
    case TokenID::tkCONT: return new TokenCONT();
    case TokenID::tkTRY: return new TokenTRY();
    case TokenID::tkCATCH: return new TokenCATCH();
    case TokenID::tkTHROW: return new TokenTHROW();
    case TokenID::tkSWITCH: return new TokenSWITCH();
    case TokenID::tkWHILE: return new TokenWHILE();
    case TokenID::tkCLASS: return new TokenCLASS();
    case TokenID::tkSTRUCT: return new TokenSTRUCT();
    case TokenID::tkDEFAULT: return new TokenDEFAULT();
    case TokenID::tkTYPEDEF: return new TokenTYPEDEF();
    case TokenID::tkOPEROVER: return new TokenOPEROVER();
    case TokenID::tkREGISTER: return new TokenREGISTER();
    case TokenID::tkUSING: return new TokenUSING();
    case TokenID::tkNAMESPACE: return new TokenNAMESPACE();
    case TokenID::tkPREFER: return new TokenPREFER();
    case TokenID::tkDEFER: return new TokenDEFER();
    case TokenID::tkSTATIC: return new TokenSTATIC();
    case TokenID::tkCONST: return new TokenCONST();
    case TokenID::tkEXTERN: return new TokenEXTERN();
    case TokenID::tkENUM: return new TokenENUM();
    case TokenID::tkRESTRICT: return new TokenRESTRICT();
    case TokenID::tkVOLATILE: return new TokenVOLATILE();
    case TokenID::tkTEMPLATE: return new TokenTEMPLATE();
    case TokenID::tkMATCH: return new TokenMatch();
    case TokenID::tkUNION: return new TokenUNION();
    case TokenID::tkNEW: return new TokenNEW();
    case TokenID::tkDELETE: return new TokenDELETE();
    case TokenID::tkDynamicCast: return new TokenDynamicCast();
    case TokenID::tkTypeid: return new TokenTypeid();
    default: return NULL;
    }
}

static DataDef *builtin_datadef_from_spelling(const std::string &s)
{
    if ( s == "void" ) return &ddVOID;
    if ( s == "bool" ) return &ddBOOL;
    if ( s == "_Bool" ) return &ddBOOL;
    if ( s == "char" ) return &ddCHAR;
    if ( s == "signed char" ) return &ddINT8;
    if ( s == "unsigned char" ) return &ddUINT8;
    if ( s == "short" || s == "short int" || s == "signed short"
      || s == "signed short int" ) return &ddINT16;
    if ( s == "unsigned short" || s == "unsigned short int" ) return &ddUINT16;
    if ( s == "int" || s == "signed" || s == "signed int" ) return &ddINT32;
    if ( s == "unsigned" || s == "unsigned int" ) return &ddUINT32;
    if ( s == "long" || s == "long int" || s == "signed long"
      || s == "signed long int" || s == "long long" || s == "long long int"
      || s == "signed long long" || s == "signed long long int" ) return &ddINT64;
    if ( s == "unsigned long" || s == "unsigned long int"
      || s == "unsigned long long" || s == "unsigned long long int" ) return &ddUINT64;
    if ( s == "__int128" || s == "signed __int128" ) return &ddINT128;
    if ( s == "unsigned __int128" ) return &ddUINT128;
    if ( s == "float" || s == "_Float32" ) return &ddFLOAT;
    if ( s == "double" || s == "long double"
      || s == "_Float64" || s == "_Float128"
      || s == "_Float32x" || s == "_Float64x" ) return &ddDOUBLE;
    if ( s == "int8_t" ) return &ddINT8;
    if ( s == "int16_t" ) return &ddINT16;
    if ( s == "int32_t" ) return &ddINT32;
    if ( s == "int64_t" ) return &ddINT64;
    if ( s == "uint8_t" ) return &ddUINT8;
    if ( s == "uint16_t" ) return &ddUINT16;
    if ( s == "uint32_t" ) return &ddUINT32;
    if ( s == "uint64_t" ) return &ddUINT64;
    if ( s == "size_t" ) return &ddUINT64;
    if ( s == "ptrdiff_t" ) return &ddINT64;
    if ( s == "wchar_t" ) return &ddINT32;
    if ( s == "char16_t" ) return &ddUINT16;
    if ( s == "char32_t" ) return &ddUINT32;
    if ( s == "max_align_t" ) return &ddMAX_ALIGN_T;
    if ( s == "LPSTR" ) return &ddLPSTR;
    if ( s == "array" ) return &ddARRAY;
    if ( s == "auto" ) return &ddAUTO;
    return NULL;
}

// --- Serialization ---

bool serialize_tokens(const TokenStream &tokens,
		      std::vector<uint8_t> &out)
{
    out.clear();
    out.reserve(tokens.size() * 16); // rough estimate

    for ( TokenBase *tb : tokens )
    {
	if ( !tb ) continue;

	TokenType tt = tb->type();
	TokenID   ti = tb->id();

	write_u8(out, (uint8_t)tt);
	write_u16(out, (uint16_t)ti);
	write_u32(out, (uint32_t)tb->line);
	write_u16(out, (uint16_t)tb->column);

	// Determine value type and write value
	switch ( tt )
	{
	case TokenType::ttInteger:
	{
	    TokenInt *tok = dynamic_cast<TokenInt *>(tb);
	    if ( tok && !tok->source_text.empty() )
	    {
		write_u8(out, (uint8_t)PchValueType::IntStr);
		write_i64(out, tok->ival());
		write_str(out, tok->source_text);
	    }
	    else
	    {
		write_u8(out, (uint8_t)PchValueType::Int64);
		write_i64(out, tb->ival());
	    }
	    break;
	}
	case TokenType::ttChar:
	    write_u8(out, (uint8_t)PchValueType::Int64);
	    write_i64(out, tb->ival());
	    break;

	case TokenType::ttReal:
	    write_u8(out, (uint8_t)PchValueType::Double);
	    write_f64(out, tb->dval());
	    break;

	case TokenType::ttString:
	{
	    TokenStr *ts = dynamic_cast<TokenStr *>(tb);
	    write_u8(out, (uint8_t)PchValueType::String);
	    write_str(out, ts ? ts->str : "");
	    // Store wide flag in high bit of string length (already encoded in write_str)
	    write_u8(out, ts ? (ts->wide ? 1 : 0) : 0);
	    break;
	}
	case TokenType::ttIdentifier:
	{
	    TokenIdent *ti_tok = dynamic_cast<TokenIdent *>(tb);
	    write_u8(out, (uint8_t)PchValueType::String);
	    write_str(out, ti_tok ? ti_tok->str : "");
	    break;
	}
	case TokenType::ttKeyword:
	case TokenType::ttDataType:
	    // Keywords and datatypes are identified by their TokenID;
	    // no additional value needed.  But some have string content
	    // (e.g., typedef'd names).
	    if ( TokenIdent *ki = dynamic_cast<TokenIdent *>(tb) )
	    {
		write_u8(out, (uint8_t)PchValueType::String);
		write_str(out, ki->str);
	    }
	    else
	    {
		write_u8(out, (uint8_t)PchValueType::None);
	    }
	    break;

	default:
	    // Operators, symbols, punctuation — identified by TokenID alone
	    write_u8(out, (uint8_t)PchValueType::None);
	    break;
	}
    }

    return true;
}

// --- Deserialization ---

bool deserialize_tokens(const uint8_t *data, size_t len,
			uint32_t expected_count,
			std::deque<TokenBase *> &out)
{
    Reader r(data, len);
    out.clear();

    for ( uint32_t i = 0; i < expected_count && r.has(1); i++ )
    {
	TokenType tt = (TokenType)r.read_u8();
	TokenID   ti = (TokenID)r.read_u16();
	uint32_t line = r.read_u32();
	uint16_t column = r.read_u16();
	PchValueType vt = (PchValueType)r.read_u8();

	TokenBase *tb = NULL;

	switch ( vt )
	{
	case PchValueType::Int64:
	{
	    int64_t val = r.read_i64();
	    if ( tt == TokenType::ttChar )
		tb = new TokenChar((int)val);
	    else
		tb = new TokenInt(val);
	    break;
	}
	case PchValueType::IntStr:
	{
	    int64_t val = r.read_i64();
	    std::string src = r.read_str();
	    tb = new TokenInt(val, src);
	    break;
	}
	case PchValueType::Double:
	{
	    double val = r.read_f64();
	    tb = new TokenReal(val);
	    break;
	}
	case PchValueType::String:
	{
	    std::string s = r.read_str();
	    if ( tt == TokenType::ttString )
	    {
		uint8_t wide = r.read_u8();
		tb = new TokenStr(s, wide != 0);
	    }
	    else if ( tt == TokenType::ttIdentifier )
		tb = new TokenIdent(s);
	    else if ( tt == TokenType::ttKeyword || tt == TokenType::ttDataType )
	    {
		if ( tt == TokenType::ttDataType )
		{
		    if ( DataDef *dd = builtin_datadef_from_spelling(s) )
			tb = new TokenDataType(s.c_str(), *dd);
		}
		if ( !tb )
		    tb = token_from_id(ti);
		if ( !tb )
		    tb = new TokenIdent(s);
	    }
	    else
		tb = new TokenIdent(s);
	    break;
	}
	case PchValueType::None:
	default:
	    // Reconstruct from TokenID — operators, punctuation, keywords
	    tb = token_from_id(ti);
	    if ( !tb )
		tb = new TokenBase((int64_t)ti);
	    break;
	}

	if ( tb )
	{
	    tb->line = line;
	    tb->column = column;
	    out.push_back(tb);
	}
    }

    return true;
}

// --- Compression ---

bool compress(const std::vector<uint8_t> &in,
	      std::vector<uint8_t> &out,
	      PchCompression method)
{
    if ( method == PchCompression::None )
    {
	out = in;
	return true;
    }

#ifdef HAVE_ZSTD
    if ( method == PchCompression::Zstd )
    {
	size_t bound = ZSTD_compressBound(in.size());
	out.resize(bound);
	size_t result = ZSTD_compress(out.data(), bound, in.data(), in.size(), 3);
	if ( ZSTD_isError(result) )
	    return false;
	out.resize(result);
	return true;
    }
#endif

    if ( method == PchCompression::Zlib )
    {
	uLongf bound = compressBound((uLong)in.size());
	out.resize(bound);
	int rc = compress2(out.data(), &bound, in.data(), (uLong)in.size(), Z_DEFAULT_COMPRESSION);
	if ( rc != Z_OK )
	    return false;
	out.resize(bound);
	return true;
    }

    return false;
}

bool decompress(const uint8_t *in, size_t in_len,
		uint8_t *out, size_t out_len,
		PchCompression method)
{
    if ( method == PchCompression::None )
    {
	if ( in_len != out_len ) return false;
	memcpy(out, in, in_len);
	return true;
    }

#ifdef HAVE_ZSTD
    if ( method == PchCompression::Zstd )
    {
	size_t result = ZSTD_decompress(out, out_len, in, in_len);
	return !ZSTD_isError(result) && result == out_len;
    }
#endif

    if ( method == PchCompression::Zlib )
    {
	uLongf dest_len = (uLongf)out_len;
	int rc = ::uncompress(out, &dest_len, in, (uLong)in_len);
	return rc == Z_OK && dest_len == (uLongf)out_len;
    }

    return false;
}

// --- File I/O ---

bool write_madh(const char *path,
		const TokenStream &tokens,
		uint64_t source_hash,
		PchCompression method)
{
    // Serialize tokens
    std::vector<uint8_t> raw;
    if ( !serialize_tokens(tokens, raw) )
	return false;

    // Compress
    std::vector<uint8_t> compressed;
    if ( !compress(raw, compressed, method) )
	return false;

    // Build header
    MadhHeader hdr;
    memcpy(hdr.magic, "MADH", 4);
    hdr.version = FORMAT_VERSION;
    hdr.flags = (uint16_t)method;
    hdr.source_hash = source_hash;
    hdr.compiler_hash = compiler_hash();
    hdr.uncompressed_size = (uint32_t)raw.size();
    hdr.token_count = (uint32_t)tokens.size();

    // Write file
    std::ofstream f(path, std::ios::binary);
    if ( !f ) return false;
    f.write((const char *)&hdr, sizeof(hdr));
    f.write((const char *)compressed.data(), compressed.size());
    return f.good();
}

bool read_madh(const uint8_t *data, size_t len,
	       std::deque<TokenBase *> &tokens)
{
    if ( len < sizeof(MadhHeader) )
	return false;

    const MadhHeader *hdr = (const MadhHeader *)data;

    // Validate magic
    if ( memcmp(hdr->magic, "MADH", 4) != 0 )
	return false;

    // Check version
    if ( hdr->version > FORMAT_VERSION )
	return false;

    // Check compiler hash
    if ( hdr->compiler_hash != compiler_hash() )
	return false;

    // Decompress
    PchCompression method = (PchCompression)(hdr->flags & 0x03);
    const uint8_t *compressed = data + sizeof(MadhHeader);
    size_t compressed_len = len - sizeof(MadhHeader);

    std::vector<uint8_t> raw(hdr->uncompressed_size);
    if ( !decompress(compressed, compressed_len,
		     raw.data(), raw.size(), method) )
	return false;

    // Deserialize
    return deserialize_tokens(raw.data(), raw.size(),
			      hdr->token_count, tokens);
}

// --- Hashing ---

uint64_t hash_content(const char *data, size_t len)
{
    // FNV-1a 64-bit
    uint64_t h = 14695981039346656037ULL;
    for ( size_t i = 0; i < len; i++ )
    {
	h ^= (uint8_t)data[i];
	h *= 1099511628211ULL;
    }
    return h;
}

uint64_t compiler_hash()
{
    // Hash the token format version + key enum sizes so that
    // PCH files are invalidated when the token format changes.
    static uint64_t cached = 0;
    if ( !cached )
    {
	const char *sig = "madh-v1-tt" // token type enum
			  "-ti"        // token id enum
			  "-2026b";    // format generation
	cached = hash_content(sig, strlen(sig));
    }
    return cached;
}

} // namespace madc_pch
