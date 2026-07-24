// Unit tests for the AOT native-image lanes (madc_cir_emit_native):
// MIR assembles the artifacts directly — no external toolchain — and this
// process consumes them (dlopen/dlsym for the -shared ET_DYN; structural
// ELF probes for the executable flavors: PIE ET_DYN vs -no-pie ET_EXEC),
// so the whole product path is exercised in-repo with no host cc/ld even
// at test time.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <asmjit/x86.h>

#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <stack>
#include <string>
#include <vector>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "../src/madc_cir.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <unistd.h>

// The one-shot CLI flag madc_cir_emit_native sets (defined weak in
// madc_globals.cpp); reset after each emission so later cases in this
// binary run in normal JIT mode.
extern thread_local bool madc_object_mode;

namespace {

std::string write_temp(const char *suffix_tmpl, int suffix_len,
		       const char *contents)
{
    std::string tmpl = suffix_tmpl;
    std::vector<char> path(tmpl.begin(), tmpl.end());
    path.push_back('\0');
    int fd = mkstemps(path.data(), suffix_len);
    if (fd < 0)
	return std::string();
    if (contents && *contents) {
	ssize_t len = (ssize_t)std::strlen(contents);
	if (write(fd, contents, len) != len) {
	    close(fd);
	    return std::string();
	}
    }
    close(fd);
    return std::string(path.data());
}

// Emit `body` as a native executable of the given kind and return the raw
// image bytes (empty on failure). Shared by the DT_NEEDED and PIE cases.
std::string emit_native_image(const char *body, MadcNativeKind kind)
{
    std::string src_path =
	write_temp("/tmp/madc_unit_native_XXXXXX.mad", 4, body);
    REQUIRE(!src_path.empty());
    std::string out_path =
	write_temp("/tmp/madc_unit_native_XXXXXX.bin", 4, "");
    REQUIRE(!out_path.empty());
    MadcEngine engine;
    std::unique_ptr<Program> prog = engine.create_program();
    TokenProgram *tp = prog->tokenize(src_path.c_str());
    REQUIRE(tp != NULL);
    REQUIRE(prog->parse(tp));
    std::vector<std::string> user_libs;
    int rc = madc_cir_emit_native(prog.get(), src_path.c_str(), kind,
				  out_path.c_str(), user_libs);
    madc_object_mode = false;
    REQUIRE(rc == 0);
    std::ifstream f(out_path.c_str(), std::ios::binary);
    std::string img((std::istreambuf_iterator<char>(f)),
		    std::istreambuf_iterator<char>());
    std::remove(src_path.c_str());
    std::remove(out_path.c_str());
    return img;
}

// The emitter's identity file-offset<->vaddr mapping makes p_offset a
// direct file position, so the image can be probed without mapping it.
const Elf64_Phdr *find_phdr(const std::string &img, uint32_t type)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img.data();
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(img.data() + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++)
	if (ph[i].p_type == type)
	    return &ph[i];
    return NULL;
}

bool dyn_has_tag(const std::string &img, int64_t tag, uint64_t *val)
{
    const Elf64_Phdr *pd = find_phdr(img, PT_DYNAMIC);
    if (!pd)
	return false;
    const Elf64_Dyn *d = (const Elf64_Dyn *)(img.data() + pd->p_offset);
    for (; d->d_tag != DT_NULL; d++)
	if (d->d_tag == tag) {
	    if (val)
		*val = d->d_un.d_val;
	    return true;
	}
    return false;
}

} // namespace

TEST_SUITE("madc_cir_emit_native") {

    TEST_CASE("-shared emits a dlopen-consumable ET_DYN shared object") {
	std::string src_path = write_temp("/tmp/madc_unit_shared_XXXXXX.mad", 4,
					  "int mul_base = 6;\n"
					  "int madd(int a, int b) { return a + b; }\n"
					  "int mmul(int a) { return a * mul_base; }\n");
	REQUIRE(!src_path.empty());
	std::string so_path = write_temp("/tmp/madc_unit_shared_XXXXXX.so", 3, "");
	REQUIRE(!so_path.empty());

	{
	    MadcEngine engine;
	    std::unique_ptr<Program> prog = engine.create_program();
	    TokenProgram *tp = prog->tokenize(src_path.c_str());
	    REQUIRE(tp != NULL);
	    REQUIRE(prog->parse(tp));

	    std::vector<std::string> user_libs;
	    int rc = madc_cir_emit_native(prog.get(), src_path.c_str(),
					  mnkShared, so_path.c_str(), user_libs);
	    madc_object_mode = false;
	    REQUIRE(rc == 0);
	}

	// RTLD_NOW forces eager application of every .rela.dyn entry —
	// R_X86_64_RELATIVE internals and imported-symbol slots alike.
	// RTLD_DEEPBIND: this binary statically embeds libmadc.a with
	// -rdynamic, so the .so's DT_NEEDED libmadc.so.0 would otherwise
	// bind its initializers to the executable's interposed symbols and
	// run them against mixed per-image state (split-brain hang in the
	// lexer intern table). Deep binding keeps the loaded subtree
	// self-consistent; a plain (non-madc-static) host needs no flag.
	void *h = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
	if (!h)
	    MESSAGE("dlopen: " << dlerror());
	REQUIRE(h != NULL);
	int (*madd)(int, int) = (int (*)(int, int))dlsym(h, "madd");
	int (*mmul)(int) = (int (*)(int))dlsym(h, "mmul");
	REQUIRE(madd != NULL);
	REQUIRE(mmul != NULL);
	CHECK(madd(20, 22) == 42);
	CHECK(mmul(20) == 120);	// module data through a RELATIVE reloc
	dlclose(h);

	std::remove(src_path.c_str());
	std::remove(so_path.c_str());
    }

    TEST_CASE("conditional DT_NEEDED: runtime-free exec drops libmadc.so.0") {
	// The needed-soname strings live in the image's .dynstr, so a raw
	// byte scan for "libmadc.so.0" is a faithful DT_NEEDED probe (the
	// RUNPATH holds directory paths only, never that spelling).

	// Every import resolves in libc: no runtime dependency.
	std::string free_img = emit_native_image(
	    "int main(int argc, char **argv) { return argc > 90 ? 1 : 0; }\n",
	    mnkExecutable);
	CHECK(free_img.find("libmadc.so.0") == std::string::npos);
	CHECK(free_img.find("libc.so.6") != std::string::npos);

	// A VLA local imports the runtime's __madc_vla_free cleanup:
	// the dependency must stay.
	std::string rt_img = emit_native_image(
	    "int main(int argc, char **argv) {\n"
	    "    int a[argc + 1];\n"
	    "    a[argc] = argc;\n"
	    "    return a[argc] > 90 ? 1 : 0;\n"
	    "}\n",
	    mnkExecutable);
	CHECK(rt_img.find("libmadc.so.0") != std::string::npos);
    }

    TEST_CASE("PIE flip: mnkPieExecutable emits an ET_DYN PIE, -no-pie stays ET_EXEC") {
	const char *body =
	    "int base = 7;\n"
	    "int main(int argc, char **argv) { return argc * base > 90 ? 1 : 0; }\n";

	std::string pie = emit_native_image(body, mnkPieExecutable);
	REQUIRE(pie.size() > sizeof(Elf64_Ehdr));
	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)pie.data();
	CHECK(eh->e_type == ET_DYN);
	CHECK(eh->e_entry != 0);		// _start survives the flip
	CHECK((find_phdr(pie, PT_INTERP) != NULL)); // still an executable
	uint64_t flags1 = 0;
	CHECK(dyn_has_tag(pie, DT_FLAGS_1, &flags1));
	CHECK((flags1 & DF_1_PIE) != 0);
	// base 0 layout: the entry point is a small link-time vaddr the
	// loader rebases, not an OBJX_BASE_ADDR-fixed one.
	CHECK(eh->e_entry < 0x400000);
	// R6 invariant carried over: a PIE must never need TEXTREL.
	CHECK(!dyn_has_tag(pie, DT_TEXTREL, NULL));
	// The internal `base` data reference must ride a RELATIVE reloc
	// (an ET_DYN image cannot resolve it at emit time).
	{
	    const Elf64_Phdr *pd = find_phdr(pie, PT_DYNAMIC);
	    REQUIRE((pd != NULL));
	    uint64_t rela = 0, relasz = 0;
	    REQUIRE(dyn_has_tag(pie, DT_RELA, &rela));
	    REQUIRE(dyn_has_tag(pie, DT_RELASZ, &relasz));
	    const Elf64_Rela *r = (const Elf64_Rela *)(pie.data() + rela);
	    size_t nrel = relasz / sizeof(Elf64_Rela), nrelative = 0;
	    for (size_t i = 0; i < nrel; i++)
		if (ELF64_R_TYPE(r[i].r_info) == R_X86_64_RELATIVE)
		    nrelative++;
	    CHECK(nrelative > 0);
	}

	std::string fixed = emit_native_image(body, mnkExecutable);
	REQUIRE(fixed.size() > sizeof(Elf64_Ehdr));
	const Elf64_Ehdr *fh = (const Elf64_Ehdr *)fixed.data();
	CHECK(fh->e_type == ET_EXEC);
	CHECK(fh->e_entry >= 0x400000);
	CHECK(!dyn_has_tag(fixed, DT_FLAGS_1, NULL));
    }
}
