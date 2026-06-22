#ifndef __MADC_PCH_H
#define __MADC_PCH_H 1

// madc Pre-Compiled Header format (.madh)
//
// Serializes a post-lexer token stream into a compact binary format,
// optionally compressed with zstd (preferred) or zlib (fallback).
// Designed to evolve: Phase 1 = token stream, Phase 2 = AST,
// Phase 3 = C++20-style modules (.madm).

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <deque>

class TokenBase;
class TokenStream;	// parser token stream (defined in madc.h); the
			// write side serializes it directly — see madc.cpp
			// PCH-write. The read side fills local std::deque
			// buffers that are then pushed into the live stream.

// .madh file header (28 bytes)
struct MadhHeader
{
    char     magic[4];          // "MADH"
    uint16_t version;           // format version (1 = token stream)
    uint16_t flags;             // bit 0: 0=zlib, 1=zstd
    uint64_t source_hash;       // hash of original header content
    uint64_t compiler_hash;     // hash of madc version/token format
    uint32_t uncompressed_size; // size of token data before compression
    uint32_t token_count;       // number of tokens
};

// Compression method
enum class PchCompression : uint8_t
{
    None  = 0,
    Zlib  = 1,
    Zstd  = 2
};

// Token value types in serialized form
enum class PchValueType : uint8_t
{
    None    = 0,   // no value (operators, punctuation, keywords)
    Int64   = 1,   // int64_t (TokenInt, TokenChar)
    Double  = 2,   // double (TokenReal)
    String  = 3,   // length-prefixed string (TokenIdent, TokenStr)
    IntStr  = 4,   // int64_t + string (TokenInt with source_text)
};

namespace madc_pch {

// Current format version
static const uint16_t FORMAT_VERSION = 1;

// Serialize a token stream to a binary buffer (uncompressed)
bool serialize_tokens(const TokenStream &tokens,
		      std::vector<uint8_t> &out);

// Deserialize a binary buffer back to a token stream
bool deserialize_tokens(const uint8_t *data, size_t len,
			uint32_t expected_count,
			std::deque<TokenBase *> &out);

// Compress a buffer (returns compressed data)
bool compress(const std::vector<uint8_t> &in,
	      std::vector<uint8_t> &out,
	      PchCompression method);

// Decompress a buffer
bool decompress(const uint8_t *in, size_t in_len,
		uint8_t *out, size_t out_len,
		PchCompression method);

// Write a complete .madh file
bool write_madh(const char *path,
		const TokenStream &tokens,
		uint64_t source_hash,
		PchCompression method = PchCompression::Zlib);

// Read a complete .madh file into a token stream
bool read_madh(const uint8_t *data, size_t len,
	       std::deque<TokenBase *> &tokens);

// Compute a hash of file content for invalidation
uint64_t hash_content(const char *data, size_t len);

// Compute the compiler version hash
uint64_t compiler_hash();

} // namespace madc_pch

#endif // __MADC_PCH_H
