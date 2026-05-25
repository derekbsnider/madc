# Pre-Compiled Embedded Headers (.madh format)

Plan created 2026-05-24. Phase 1 partially implemented.

## Context

madc currently embeds 58 hand-written header stubs (48KB) as raw C++ strings
in `embedded_headers.cpp`. These have hardcoded Linux-specific values, are
error-prone to maintain, and won't work on macOS/Windows.

**Goal:** During `make`, read the real system headers, pre-compile them into
madc's own binary format (compressed), and embed them into the `madc`
executable. At runtime, `#include <foo.h>` loads the pre-compiled form
instead of re-parsing text. The format should be forward-compatible with
C++20-style modules (`.cppm`/`.ixx`/`.mxx` equivalents).

**Licensing:** Build-time processing of system headers avoids embedding GPL
source in the repo. Same model as requiring gcc/g++ to build.

## Industry Research (2026-05-24)

- **GCC PCH:** raw heap dump, mmap'd, no compression — fragile, avoid
- **Clang PCH:** structured LLVM bitstream, lazy deserialization, on-disk
  hash tables — gold standard but complex
- **Clang PTH (deprecated):** token-level cache, simpler — closest to our
  Phase 1 approach
- **TCC:** no caching, just fast parsing — validates that small headers may
  not need it
- **V8:** bytecode caching + lazy deserialization — validates lazy approach
- **C++20 modules:** fine-grained DAGs with BMI files (.pcm/.gcm/.ifc) —
  our Phase 3 target
- **Compression:** zstd preferred (1 GB/s decompress), zlib fallback

## Three-Phase Design

### Phase 1: Post-Lexer Token Serialization (MVP) — PARTIALLY DONE

Serialize the **token stream** after lexing. Skips re-lexing at runtime.

**Status:** Core infrastructure implemented:
- [x] `.madh` format (magic/version/flags/hashes + zlib-compressed tokens)
- [x] Token serialization/deserialization (`src/pch.cpp`)
- [x] `madc --emit-pch` CLI mode (`src/madc.cpp`)
- [x] Build pipeline: `gcc -E -P` → `madc --emit-pch` → C arrays
- [x] 38 system headers successfully pre-compiled
- [x] Lexer integration (PCH lookup after text-embedded fallback)
- [ ] Transition: text-embedded stubs still take priority (parser not yet
      ready for full system header declarations)
- [ ] `configure` integration for zstd detection
- [ ] `make pch` target for rebuilding pre-compiled headers
- [ ] Validation: `make check-pch` comparing PCH vs text results

**Blocker for full transition:** madc's parser can't yet handle all the
type declarations in real system headers (complex structs, inline functions,
GNU attributes). The pre-compiled headers contain post-`gcc -E` output which
is clean C, but still has constructs the parser doesn't support.

### Phase 2: Post-Parser AST Serialization (Full PCH)

Serialize **AST + symbol tables** after parsing — skips both lexing AND
parsing at runtime.

**Additional format blocks:**
- Symbol table: serialized DataDef/Variable/FuncDef entries
- AST: serialized TokenBase tree (offset-based, not pointer-based)
- Lazy deserialization via on-disk hash tables (Clang-style)

### Phase 3: Module Support (`.madm`)

C++20-style modules — explicit export control, DAG dependencies, no macro
leakage. Format extends `.madh` with export tables and dependency graph.
Analogous to `.pcm` (Clang), `.gcm` (GCC), `.ifc` (MSVC).

## Build Pipeline

```
System header (/usr/include/stdio.h)
    │
    ▼ gcc -E -P (preprocess — handles GNU extensions)
    │
    ▼ madc --emit-pch -o stdio.madh (tokenize + compress)
    │
    ▼ xxd -i (convert to C array)
    │
    ▼ precompiled_headers.cpp (embedded in binary)
```

**Bootstrap:** First build uses current text-embedded headers. `make pch`
then rebuilds madc with pre-lexed embedded headers.

## Runtime Lookup Chain

```
#include <foo.h>:
  1. Text-embedded stubs (hand-written, parser-tailored)  ← current priority
  2. Pre-compiled .madh (from real system headers)        ← new, fallback
  3. Filesystem (-I paths, /usr/include)                  ← existing
```

Text-embedded stubs take priority until the parser can handle full system
header declarations. Eventually, text stubs are retired and pre-compiled
headers become primary.

## Compression

**zstd preferred, zlib fallback.** PCH header flags indicate which was used.
`configure` detects both. Currently only zlib is linked (`-lz`).

## Invalidation

PCH is invalid if source hash, compiler hash, or target arch changes.
Falls back to text-based include on mismatch.

## Files

| File | Status | Purpose |
|------|--------|---------|
| `include/madc_pch.h` | DONE | Format definitions, serialize/deserialize API |
| `src/pch.cpp` | DONE | Serialization, compression, hashing |
| `src/madc.cpp` | DONE | `--emit-pch` flag handling |
| `scripts/gen_precompiled_headers.sh` | DONE | Batch pre-compilation pipeline |
| `src/precompiled_headers.cpp` | GENERATED | 38 headers as C arrays |
| `src/lexer.cpp` | DONE | PCH lookup in #include handler |
| `src/Makefile` | DONE | pch.o, precompiled_headers.o, -lz |
| `configure.ac` | TODO | zstd detection, header list config |
| `src/embedded_headers.cpp` | KEEP | Text fallback until parser matures |
