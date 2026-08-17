// Unit test for the AOT R4b .o-as-precompiled-cache lane: madc emits a
// relocatable object (madc_cir_emit_native, mnkObject) and this process
// loads it back through the fork's in-process ET_REL loader
// (MIR_object_load) — map, relocate, resolve, call. The whole product path
// runs in-repo with no external toolchain even at test time.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

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
#include "mir-debug.h"

#include <cstdio>
#include <cstring>
#include "madc_dl.h"
#include <elf.h>
#include <unistd.h>

// The one-shot CLI flag madc_cir_emit_native sets (defined weak in
// madc_globals.cpp); reset after each emission so later cases in this
// binary run in normal JIT mode.
extern thread_local bool madc_object_mode;
// -g equivalent: enables the DWARF builder during emission.
extern thread_local bool madc_debug_info;

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

// Emit src to a fresh .o via the production emit entry; returns the .o path.
std::string emit_object(const char *src)
{
    std::string src_path = write_temp("/tmp/madc_unit_objload_XXXXXX.mad", 4,
				      src);
    REQUIRE(!src_path.empty());
    std::string obj_path = write_temp("/tmp/madc_unit_objload_XXXXXX.o", 2,
				      "");
    REQUIRE(!obj_path.empty());

    MadcEngine engine;
    std::unique_ptr<Program> prog = engine.create_program();
    TokenProgram *tp = prog->tokenize(src_path.c_str());
    REQUIRE(tp != NULL);
    REQUIRE(prog->parse(tp));

    std::vector<std::string> user_libs;
    int rc = madc_cir_emit_native(prog.get(), src_path.c_str(), mnkObject,
				  obj_path.c_str(), user_libs);
    madc_object_mode = false;
    REQUIRE(rc == 0);
    std::remove(src_path.c_str());
    return obj_path;
}

// This binary embeds libmadc.a with -rdynamic, so dlsym(RTLD_DEFAULT)
// reaches libc, the madc runtime, and the mir builtin exports alike —
// the same reach the CLI resolver has.
void *dlsym_resolver(const char *name, void *)
{
    return madcdl_sym_default(name);
}

// Two import targets deliberately invisible to dlsym (static, not in the
// dynamic symbol table): at emit time madc's sentinel resolver hands BOTH
// the same &undef_sentinel value, so the address pool's (value, item) slot
// keying is what keeps their .mir.addrpool slots — and relocations — apart.
static int r6_ext_a_impl() { return 111; }
static int r6_ext_b_impl() { return 222; }

void *two_import_resolver(const char *name, void *)
{
    if (std::strcmp(name, "r6_test_ext_a") == 0)
	return (void *)&r6_ext_a_impl;
    if (std::strcmp(name, "r6_test_ext_b") == 0)
	return (void *)&r6_ext_b_impl;
    return madcdl_sym_default(name);
}

// ELF binding of a DEFINED symbol in an object file's SHT_SYMTAB
// (STB_GLOBAL/STB_WEAK), or -1 when the name is absent/undefined.
int sym_bind(const std::string &path, const char *name)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    std::string img((std::istreambuf_iterator<char>(f)),
		    std::istreambuf_iterator<char>());
    REQUIRE(img.size() > sizeof(Elf64_Ehdr));
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img.data();
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(img.data() + eh->e_shoff);
    for (int i = 1; i < eh->e_shnum; i++) {
	if (sh[i].sh_type != SHT_SYMTAB)
	    continue;
	const Elf64_Sym *syms =
	    (const Elf64_Sym *)(img.data() + sh[i].sh_offset);
	size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
	const char *str = img.data() + sh[sh[i].sh_link].sh_offset;
	for (size_t s = 1; s < n; s++)
	    if (syms[s].st_shndx != SHN_UNDEF && syms[s].st_name
		&& std::strcmp(str + syms[s].st_name, name) == 0)
		return ELF64_ST_BIND(syms[s].st_info);
    }
    return -1;
}

} // namespace

TEST_SUITE("madc_cir_run_object") {

    TEST_CASE(".o loads in-process and its functions execute") {
	std::string obj_path = emit_object(
	    "int mul_base = 6;\n"
	    "int madd(int a, int b) { return a + b; }\n"
	    "int mmul(int a) { return a * mul_base; }\n"
	    "int main() { return 0; }\n");

	std::ifstream f(obj_path.c_str(), std::ios::binary);
	REQUIRE(f.good());
	std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
				std::istreambuf_iterator<char>());
	f.close();
	REQUIRE(!bytes.empty());

	char err[256] = "";
	MIR_object_loaded_t lo = MIR_object_load(bytes.data(), bytes.size(),
						 dlsym_resolver, NULL,
						 err, sizeof err);
	if (!lo)
	    MESSAGE("MIR_object_load: " << err);
	REQUIRE(lo != NULL);

	int (*madd)(int, int)
	    = (int (*)(int, int))MIR_object_loaded_sym(lo, "madd");
	int (*mmul)(int) = (int (*)(int))MIR_object_loaded_sym(lo, "mmul");
	REQUIRE(madd != NULL);
	REQUIRE(mmul != NULL);
	CHECK(madd(20, 22) == 42);
	CHECK(mmul(20) == 120);	// global data through an ABS64 .data reloc
	CHECK((MIR_object_loaded_sym(lo, "no_such_symbol") == NULL));

	// Safe here: nothing in the object registered atexit handlers.
	MIR_object_loaded_unload(lo);
	std::remove(obj_path.c_str());
    }

    TEST_CASE("madc_cir_run_object runs main and returns its exit status") {
	std::string obj_path = emit_object(
	    "int main() { return 42; }\n");

	char *oargv[] = { (char *)obj_path.c_str(), NULL };
	CHECK(madc_cir_run_object(obj_path.c_str(), 1, oargv) == 42);
	std::remove(obj_path.c_str());
    }

    TEST_CASE("multi-object link: cross-object calls, data, and dup detection") {
	// TU a: defines `scale` and calls b's `bonus` (undefined here);
	// TU b: defines `bonus` and reads a's `scale` (undefined here).
	// Circular cross-object references — resolvable only by merging.
	std::string a_path = emit_object(
	    "int scale = 6;\n"
	    "int bonus(int);\n"
	    "int amul(int x) { return bonus(x) * scale; }\n"
	    "int main(int argc, char **argv) { return amul(argc + 5); }\n");
	std::string b_path = emit_object(
	    "extern int scale;\n"
	    "int bonus(int x) { return x + scale; }\n");

	std::vector<std::string> paths;
	paths.push_back(a_path);
	paths.push_back(b_path);

	// Run lane (merge in memory, load, enter main):
	// main(1) = amul(6) = bonus(6) * 6 = (6 + 6) * 6 = 72.
	char *oargv[] = { (char *)a_path.c_str(), NULL };
	CHECK(madc_cir_run_objects(paths, 1, oargv) == 72);

	// ld -r lane: the merged relocatable loads and runs like any .o.
	std::string r_path =
	    write_temp("/tmp/madc_unit_objload_XXXXXX.o", 2, "");
	REQUIRE(!r_path.empty());
	std::vector<std::string> user_libs;
	REQUIRE(madc_cir_link_objects(paths, mnkRelocatable, r_path.c_str(),
				      user_libs, NULL) == 0);
	char *rargv[] = { (char *)r_path.c_str(), NULL };
	CHECK(madc_cir_run_object(r_path.c_str(), 1, rargv) == 72);

	// Executable lane: the merged capture through the PIE emitter.
	std::string x_path =
	    write_temp("/tmp/madc_unit_objload_XXXXXX.bin", 4, "");
	REQUIRE(!x_path.empty());
	REQUIRE(madc_cir_link_objects(paths, mnkPieExecutable,
				      x_path.c_str(), user_libs, NULL) == 0);
	{
	    std::ifstream f(x_path.c_str(), std::ios::binary);
	    std::string img((std::istreambuf_iterator<char>(f)),
			    std::istreambuf_iterator<char>());
	    REQUIRE(img.size() > 4);
	    CHECK(img.compare(0, 4, "\177ELF") == 0);
	}

	// Duplicate strong definition: linking a with itself must fail
	// loudly (stderr names the symbol; here we assert the rc).
	std::vector<std::string> dup;
	dup.push_back(a_path);
	dup.push_back(a_path);
	std::string d_path =
	    write_temp("/tmp/madc_unit_objload_XXXXXX.o", 2, "");
	REQUIRE(!d_path.empty());
	CHECK(madc_cir_link_objects(dup, mnkRelocatable, d_path.c_str(),
				    user_libs, NULL) != 0);

	std::remove(a_path.c_str());
	std::remove(b_path.c_str());
	std::remove(r_path.c_str());
	std::remove(x_path.c_str());
	std::remove(d_path.c_str());
    }

    TEST_CASE("init-array: two ctor TUs merge and BOTH inits run (S3)") {
	// Each TU's dynamic global initializer forces a per-TU static init
	// riding .init_array — the exact shape that collided as duplicate
	// __madc_global_init definitions before the init-array model.
	std::string a_path = emit_object(
	    "int aseed() { return 30; }\n"
	    "int abase = aseed();\n"
	    "extern int bbase;\n"
	    "int main(int argc, char **argv) { return abase + bbase; }\n");
	std::string b_path = emit_object(
	    "int bseed() { return 12; }\n"
	    "int bbase = bseed();\n");

	// Structural: each .o carries a 1-slot .init_array + its rela.
	auto initarr_size = [](const std::string &path) -> uint64_t {
	    std::ifstream f(path.c_str(), std::ios::binary);
	    std::string img((std::istreambuf_iterator<char>(f)),
			    std::istreambuf_iterator<char>());
	    REQUIRE(img.size() > sizeof(Elf64_Ehdr));
	    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img.data();
	    const Elf64_Shdr *sh = (const Elf64_Shdr *)(img.data()
							+ eh->e_shoff);
	    for (int i = 1; i < eh->e_shnum; i++)
		if (sh[i].sh_type == SHT_INIT_ARRAY)
		    return sh[i].sh_size;
	    return 0;
	};
	CHECK(initarr_size(a_path) == 8);
	CHECK(initarr_size(b_path) == 8);

	std::vector<std::string> paths;
	paths.push_back(a_path);
	paths.push_back(b_path);

	// Run lane: the loader walks the merged init array before main —
	// main sees BOTH TUs' dynamic inits done (30 + 12).
	char *oargv[] = { (char *)a_path.c_str(), NULL };
	CHECK(madc_cir_run_objects(paths, 1, oargv) == 42);

	// ld -r lane: the arrays concatenate (2 slots) and the re-emitted
	// merge still runs both inits.
	std::string r_path =
	    write_temp("/tmp/madc_unit_objload_XXXXXX.o", 2, "");
	REQUIRE(!r_path.empty());
	std::vector<std::string> user_libs;
	REQUIRE(madc_cir_link_objects(paths, mnkRelocatable, r_path.c_str(),
				      user_libs, NULL) == 0);
	CHECK(initarr_size(r_path) == 16);
	char *rargv[] = { (char *)r_path.c_str(), NULL };
	CHECK(madc_cir_run_object(r_path.c_str(), 1, rargv) == 42);

	std::remove(a_path.c_str());
	std::remove(b_path.c_str());
	std::remove(r_path.c_str());
    }

    TEST_CASE("linkonce: two TUs' identical in-class method merges weak (S4)") {
	// Both TUs define the same class with an in-class method body — the
	// C++ implicit-inline shape every TU emits. Pre-S4 this collided as
	// a duplicate strong 'Adder__add'; now both copies bind STB_WEAK and
	// the merge keeps the first.
	static const char *adder =
	    "class Adder {\n"
	    "public:\n"
	    "\tint add(int a, int b) { return a + b; }\n"
	    "};\n";
	std::string a_path = emit_object(
	    (std::string(adder)
	     + "int use_a() { Adder x; return x.add(30, 5); }\n").c_str());
	std::string b_path = emit_object(
	    (std::string(adder)
	     + "extern int use_a();\n"
	       "int main(int argc, char **argv)\n"
	       "{ Adder y; return y.add(4, 3) + use_a(); }\n").c_str());

	// Structural: the method is a WEAK defined symbol in each .o.
	CHECK(sym_bind(a_path, "Adder__add") == STB_WEAK);
	CHECK(sym_bind(b_path, "Adder__add") == STB_WEAK);

	std::vector<std::string> paths;
	paths.push_back(a_path);
	paths.push_back(b_path);

	// Run lane: merge dedupes the method (first weak wins) and the
	// program runs — main = 4+3 + 30+5 = 42.
	char *oargv[] = { (char *)a_path.c_str(), NULL };
	CHECK(madc_cir_run_objects(paths, 1, oargv) == 42);

	// ld -r lane: the merged relocatable keeps ONE weak survivor and
	// still runs.
	std::string r_path =
	    write_temp("/tmp/madc_unit_objload_XXXXXX.o", 2, "");
	REQUIRE(!r_path.empty());
	std::vector<std::string> user_libs;
	REQUIRE(madc_cir_link_objects(paths, mnkRelocatable, r_path.c_str(),
				      user_libs, NULL) == 0);
	CHECK(sym_bind(r_path, "Adder__add") == STB_WEAK);
	char *rargv[] = { (char *)r_path.c_str(), NULL };
	CHECK(madc_cir_run_object(r_path.c_str(), 1, rargv) == 42);

	std::remove(a_path.c_str());
	std::remove(b_path.c_str());
	std::remove(r_path.c_str());
    }

    TEST_CASE("inline un-erasure: user-header inline fn + variable merge weak, dynamic init runs once") {
	// The S4 boundary lifted: an explicitly-`inline` free function and
	// an `inline` variable shared by two TUs (the user-header shape)
	// bind STB_WEAK — function AND data — and merge instead of
	// duplicate-strong colliding. `once` has a DYNAMIC init that bumps
	// `counter`: the linkonce once-guard must run it exactly once in
	// the merged image (double-run makes counter 2 and the exit code
	// 142, not 42).
	static const char *hdr =
	    "inline int sumv(int a, int b) { return a + b; }\n"
	    "inline int tunable = 29;\n"
	    "inline int counter = 0;\n"
	    "inline int bump() { return ++counter; }\n"
	    "inline int once = bump();\n";
	std::string a_path = emit_object(
	    (std::string(hdr)
	     + "int use_a() { return sumv(tunable, 5); }\n").c_str());
	std::string b_path = emit_object(
	    (std::string(hdr)
	     + "extern int use_a();\n"
	       "int main(int argc, char **argv)\n"
	       "{ return sumv(4, 3) + use_a() + once + (counter - 1) * 100; }\n").c_str());

	CHECK(sym_bind(a_path, "sumv") == STB_WEAK);
	CHECK(sym_bind(b_path, "sumv") == STB_WEAK);
	CHECK(sym_bind(a_path, "tunable") == STB_WEAK);
	CHECK(sym_bind(b_path, "tunable") == STB_WEAK);
	CHECK(sym_bind(a_path, "once") == STB_WEAK);

	std::vector<std::string> paths;
	paths.push_back(a_path);
	paths.push_back(b_path);

	// Run lane: 4+3 + 29+5 + once(1) + 0 = 42.
	char *oargv[] = { (char *)a_path.c_str(), NULL };
	CHECK(madc_cir_run_objects(paths, 1, oargv) == 42);

	// ld -r lane: one weak survivor each, still 42.
	std::string r_path =
	    write_temp("/tmp/madc_unit_objload_XXXXXX.o", 2, "");
	REQUIRE(!r_path.empty());
	std::vector<std::string> user_libs;
	REQUIRE(madc_cir_link_objects(paths, mnkRelocatable, r_path.c_str(),
				      user_libs, NULL) == 0);
	CHECK(sym_bind(r_path, "sumv") == STB_WEAK);
	CHECK(sym_bind(r_path, "tunable") == STB_WEAK);
	char *rargv[] = { (char *)r_path.c_str(), NULL };
	CHECK(madc_cir_run_object(r_path.c_str(), 1, rargv) == 42);

	std::remove(a_path.c_str());
	std::remove(b_path.c_str());
	std::remove(r_path.c_str());
    }

    TEST_CASE("init-array: pre-S3 ctor cache is refused at merge") {
	// Synthesize a minimal old-model .o: it defines __madc_global_init,
	// the retired convention whose ctors nothing would run anymore.
	MIR_object_t obj = MIR_object_create();
	REQUIRE(obj != NULL);
	unsigned char ret_insn[16] = { 0xc3 };	// ret (padded to text align)
	MIR_object_text_append(obj, ret_insn, sizeof ret_insn);
	REQUIRE(MIR_object_add_symbol(obj, "__madc_global_init",
				      MIR_OBJ_SEC_TEXT, 0, 1, 1, 0, 0) >= 0);
	void *buf = NULL;
	size_t size = 0;
	REQUIRE(MIR_object_emit(obj, &buf, &size) == 0);
	MIR_object_destroy(obj);
	std::string o_path =
	    write_temp("/tmp/madc_unit_objload_XXXXXX.o", 2, "");
	REQUIRE(!o_path.empty());
	{
	    std::ofstream f(o_path.c_str(), std::ios::binary);
	    f.write((const char *)buf, (std::streamsize)size);
	}
	free(buf);

	std::vector<std::string> paths;
	paths.push_back(o_path);
	std::vector<std::string> user_libs;
	std::string out_path =
	    write_temp("/tmp/madc_unit_objload_XXXXXX.o", 2, "");
	REQUIRE(!out_path.empty());
	CHECK(madc_cir_link_objects(paths, mnkRelocatable, out_path.c_str(),
				    user_libs, NULL) != 0);
	std::remove(o_path.c_str());
	std::remove(out_path.c_str());
    }

    TEST_CASE("multi-object -g link: DWARF merges as multi-CU debug info") {
	madc_debug_info = true;
	std::string a_path = emit_object(
	    "int scale = 6;\n"
	    "int bonus(int);\n"
	    "int amul(int x) { return bonus(x) * scale; }\n"
	    "int main(int argc, char **argv) { return amul(argc + 5); }\n");
	std::string b_path = emit_object(
	    "extern int scale;\n"
	    "int bonus(int x) { return x + scale; }\n");
	madc_debug_info = false;

	std::vector<std::string> paths;
	paths.push_back(a_path);
	paths.push_back(b_path);

	// The merged relocatable must carry BOTH inputs' compile units and
	// re-emitted cross-debug-section relocations (2+ R_X86_64_32 per CU:
	// the CU's abbrev offset and stmt_list) — the pre-merge emitter
	// dropped debug sections entirely.
	std::string r_path =
	    write_temp("/tmp/madc_unit_objload_XXXXXX.o", 2, "");
	REQUIRE(!r_path.empty());
	std::vector<std::string> user_libs;
	REQUIRE(madc_cir_link_objects(paths, mnkRelocatable, r_path.c_str(),
				      user_libs, NULL) == 0);
	{
	    std::ifstream f(r_path.c_str(), std::ios::binary);
	    std::string img((std::istreambuf_iterator<char>(f)),
			    std::istreambuf_iterator<char>());
	    REQUIRE(img.size() > sizeof(Elf64_Ehdr));
	    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img.data();
	    const Elf64_Shdr *sh = (const Elf64_Shdr *)(img.data() + eh->e_shoff);
	    const char *shstr = img.data() + sh[eh->e_shstrndx].sh_offset;
	    const Elf64_Shdr *info = NULL, *rinfo = NULL;
	    for (int i = 1; i < eh->e_shnum; i++) {
		const char *nm = shstr + sh[i].sh_name;
		if (!strcmp(nm, ".debug_info"))
		    info = &sh[i];
		else if (!strcmp(nm, ".rela.debug_info"))
		    rinfo = &sh[i];
	    }
	    REQUIRE((info != NULL));
	    REQUIRE((rinfo != NULL));
	    // Walk the CU chain: unit_length u32 heads each CU.
	    size_t pos = 0, n_cus = 0;
	    while (pos + 4 <= info->sh_size) {
		uint32_t len;
		memcpy(&len, img.data() + info->sh_offset + pos, 4);
		if (len == 0)
		    break;
		n_cus++;
		pos += 4 + (size_t)len;
	    }
	    CHECK(n_cus == 2);
	    // Cross-debug relocs re-emitted: 2 per CU.
	    const Elf64_Rela *ra =
		(const Elf64_Rela *)(img.data() + rinfo->sh_offset);
	    size_t nr = rinfo->sh_size / sizeof(Elf64_Rela), n32 = 0;
	    for (size_t i = 0; i < nr; i++)
		if (ELF64_R_TYPE(ra[i].r_info) == R_X86_64_32)
		    n32++;
	    CHECK(n32 >= 4);
	}

	// The debug-bearing merge still runs (loader ignores debug) and the
	// merged relocatable still loads.
	char *oargv[] = { (char *)a_path.c_str(), NULL };
	CHECK(madc_cir_run_objects(paths, 1, oargv) == 72);
	char *rargv[] = { (char *)r_path.c_str(), NULL };
	CHECK(madc_cir_run_object(r_path.c_str(), 1, rargv) == 72);

	std::remove(a_path.c_str());
	std::remove(b_path.c_str());
	std::remove(r_path.c_str());
    }

    TEST_CASE("two same-sentinel imports keep distinct pool slots (R6)") {
	// Both prototypes are unresolvable at emit (static impls above are
	// not dlsym-visible), so the sentinel resolver hands them one shared
	// VALUE; the object must still carry one relocated slot per import.
	std::string obj_path = emit_object(
	    "int r6_test_ext_a();\n"
	    "int r6_test_ext_b();\n"
	    "int combine() { return r6_test_ext_a() * 1000 + r6_test_ext_b(); }\n"
	    "int main() { return 0; }\n");

	std::ifstream f(obj_path.c_str(), std::ios::binary);
	REQUIRE(f.good());
	std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
				std::istreambuf_iterator<char>());
	f.close();
	REQUIRE(!bytes.empty());

	char err[256] = "";
	MIR_object_loaded_t lo = MIR_object_load(bytes.data(), bytes.size(),
						 two_import_resolver, NULL,
						 err, sizeof err);
	if (!lo)
	    MESSAGE("MIR_object_load: " << err);
	REQUIRE(lo != NULL);

	int (*combine)() = (int (*)())MIR_object_loaded_sym(lo, "combine");
	REQUIRE(combine != NULL);
	CHECK(combine() == 111222); // a fused slot would give 111111 or 222222

	MIR_object_loaded_unload(lo);
	std::remove(obj_path.c_str());
    }
}
