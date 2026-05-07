// ELF x86-64 relocatable object writer for compiled madc programs.
// Reads from a finalized asmjit::CodeHolder and writes a .o file.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

extern thread_local bool madc_verbose;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <asmjit/x86.h>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

#include <elf.h>

using namespace asmjit;

namespace {

// Append raw bytes to a buffer.
void emit(std::vector<uint8_t> &buf, const void *data, size_t len)
{
	const uint8_t *p = static_cast<const uint8_t *>(data);
	buf.insert(buf.end(), p, p + len);
}

template <typename T>
void emit_val(std::vector<uint8_t> &buf, T val)
{
	emit(buf, &val, sizeof(val));
}

void emit_zeros(std::vector<uint8_t> &buf, size_t count)
{
	buf.insert(buf.end(), count, 0);
}

size_t align_to(std::vector<uint8_t> &buf, size_t alignment)
{
	size_t pos = buf.size();
	size_t pad = (alignment - (pos % alignment)) % alignment;
	emit_zeros(buf, pad);
	return buf.size();
}

// String table builder.
struct strtab_builder
{
	std::vector<uint8_t> data;

	strtab_builder()
	{
		data.push_back(0); // index 0 is always empty string
	}

	uint32_t add(const std::string &s)
	{
		uint32_t offset = static_cast<uint32_t>(data.size());
		emit(data, s.c_str(), s.size() + 1);
		return offset;
	}
};

// ELF symbol entry for our table.
struct elf_sym
{
	std::string name;
	uint64_t value;      // offset within section
	uint64_t size;
	uint8_t binding;     // STB_LOCAL or STB_GLOBAL
	uint8_t type;        // STT_FUNC, STT_SECTION, STT_NOTYPE
	uint16_t section_index;
};

// Map asmjit RelocType to ELF x86-64 relocation type.
uint32_t madc_elf_reloc_type(RelocType rt)
{
	switch ( rt )
	{
		case RelocType::kAbsToAbs:       return R_X86_64_64;
		case RelocType::kRelToAbs:       return R_X86_64_PC32;
		case RelocType::kAbsToRel:       return R_X86_64_PC32;
		case RelocType::kX64AddressEntry: return R_X86_64_64;
		default:                          return R_X86_64_NONE;
	}
}

// SysV ELF hash function.
uint32_t elf_hash(const char *name)
{
	uint32_t h = 0, g;
	for ( ; *name; ++name )
	{
		h = (h << 4) + static_cast<uint8_t>(*name);
		g = h & 0xf0000000;
		if ( g )
			h ^= g >> 24;
		h &= ~g;
	}
	return h;
}

// _start stub with argc/argv: xor rbp; mov rdi,[rsp]; lea rsi,[rsp+8];
// call <disp32>; mov rdi,rax; mov rax,60; syscall
// Total: 29 bytes. The call displacement at offset 13 must be patched.
static const uint8_t start_stub[] = {
	0x48, 0x31, 0xed,                         // xor %rbp, %rbp
	0x48, 0x8b, 0x3c, 0x24,                   // mov (%rsp), %rdi  [argc]
	0x48, 0x8d, 0x74, 0x24, 0x08,             // lea 8(%rsp), %rsi [argv]
	0xe8, 0x00, 0x00, 0x00, 0x00,             // call <rel32> (patched)
	0x48, 0x89, 0xc7,                         // mov %rax, %rdi
	0x48, 0xc7, 0xc0, 0x3c, 0x00, 0x00, 0x00, // mov $60, %rax
	0x0f, 0x05                                 // syscall
};
static const size_t START_STUB_SIZE = sizeof(start_stub);
static const size_t START_STUB_CALL_OFFSET = 13; // offset of the rel32 in the call

} // namespace

std::vector<Program::GlobalDataEntry> Program::collect_global_data() const
{
	std::vector<GlobalDataEntry> entries;
	std::set<void *> seen;

	if ( !tkProgram )
		return entries;

	// Collect global variables from the top-level compound.
	for ( size_t i = 0; i < tkProgram->variables.size(); ++i )
	{
		Variable *var = tkProgram->variables[i];
		if ( !var || !var->data || !var->type )
			continue;
		if ( !var->is_global() )
			continue;
		// Skip function variables (their data is a Method*).
		if ( var->type->basetype() == BaseType::btFunct )
			continue;

		size_t sz = var->type->size;
		if ( var->is_fixed_array() )
			sz *= var->total_elements();
		if ( sz == 0 )
			continue;

		if ( seen.count(var->data) )
			continue;
		seen.insert(var->data);

		GlobalDataEntry e;
		e.name = var->name;
		e.address = var->data;
		e.size = sz;
		entries.push_back(e);
	}

	// Collect string literals from literal_map.
	// We need both the std::string object AND its character data,
	// because the compiler passes c_str() addresses to C functions.
	for ( variable_map_t::const_iterator it = literal_map.begin();
	      it != literal_map.end(); ++it )
	{
		Variable *var = it->second;
		if ( !var || !var->data )
			continue;

		// The std::string object itself.
		if ( !seen.count(var->data) )
		{
			seen.insert(var->data);
			GlobalDataEntry e;
			e.name = "__literal_" + it->first;
			e.address = var->data;
			e.size = sizeof(std::string);
			entries.push_back(e);
		}

		// The character buffer (c_str) — this is what gets loaded
		// via movabs for C function calls like puts/printf.
		std::string *str = static_cast<std::string *>(var->data);
		const char *cstr = str->c_str();
		if ( cstr && !seen.count((void *)cstr) )
		{
			seen.insert((void *)cstr);
			GlobalDataEntry e;
			e.name = "__cstr_" + it->first;
			e.address = (void *)cstr;
			e.size = str->size() + 1; // include NUL terminator
			entries.push_back(e);
		}
	}

	return entries;
}

bool Program::save_object(const std::string &path) const
{
	if ( !root_fn )
		return false;

	// --- Gather sections from CodeHolder ---

	Section *text_section = code.textSection();
	if ( !text_section )
		return false;

	const uint8_t *text_data = text_section->data();
	size_t text_size = text_section->buffer().size();
	if ( !text_data || text_size == 0 )
		return false;

	// --- Build symbol table ---

	std::vector<elf_sym> symbols;
	strtab_builder strtab;
	strtab_builder shstrtab;

	// Section names.
	uint32_t shname_text    = shstrtab.add(".text");
	uint32_t shname_symtab  = shstrtab.add(".symtab");
	uint32_t shname_strtab  = shstrtab.add(".strtab");
	uint32_t shname_shstrtab = shstrtab.add(".shstrtab");
	uint32_t shname_rela    = shstrtab.add(".rela.text");

	// Null symbol (required at index 0).
	{
		elf_sym null_sym;
		null_sym.name = "";
		null_sym.value = 0;
		null_sym.size = 0;
		null_sym.binding = STB_LOCAL;
		null_sym.type = STT_NOTYPE;
		null_sym.section_index = SHN_UNDEF;
		symbols.push_back(null_sym);
	}

	// .text section symbol.
	{
		elf_sym sect_sym;
		sect_sym.name = "";
		sect_sym.value = 0;
		sect_sym.size = 0;
		sect_sym.binding = STB_LOCAL;
		sect_sym.type = STT_SECTION;
		sect_sym.section_index = 1; // .text is section 1
		symbols.push_back(sect_sym);
	}

	// Count local symbols (null + section symbols come first).
	size_t first_global = symbols.size();

	// Add function symbols from pending_funcs (user-defined functions).
	for ( size_t i = 0; i < pending_funcs.size(); ++i )
	{
		TokenFunc *tf = dynamic_cast<TokenFunc *>(pending_funcs[i]);
		if ( !tf || !tf->var.data || tf->is_overridden )
			continue;

		Method *method = static_cast<Method *>(tf->var.data);
		if ( !method )
			continue;

		FuncDef *func = dynamic_cast<FuncDef *>(method->returns.type);
		if ( !func || !func->funcnode )
			continue;

		uint32_t label_id = func->funcnode->label().id();
		if ( !code.isLabelBound(label_id) )
			continue;

		elf_sym sym;
		sym.name = tf->var.name;
		sym.value = code.labelOffset(label_id);
		sym.size = 0; // unknown; could compute from next label
		sym.binding = STB_GLOBAL;
		sym.type = STT_FUNC;
		sym.section_index = 1; // .text
		symbols.push_back(sym);
	}

	// --- Build external (UNDEF) symbols and relocation entries ---

	struct elf_rela
	{
		uint64_t offset;
		uint32_t sym_index;
		uint32_t type;
		int64_t addend;
	};

	std::vector<elf_rela> rela_entries;

	// Map from external address to symbol index (for dedup).
	std::map<uintptr_t, uint32_t> extern_sym_indices;

	if ( code.hasRelocEntries() )
	{
		const ZoneVector<RelocEntry *> &relocs = code.relocEntries();
		for ( uint32_t i = 0; i < relocs.size(); ++i )
		{
			const RelocEntry *re = relocs[i];
			if ( !re || re->relocType() == RelocType::kNone )
				continue;

			uint32_t elf_type = madc_elf_reloc_type(re->relocType());
			if ( elf_type == R_X86_64_NONE )
				continue;

			uintptr_t payload = static_cast<uintptr_t>(re->payload());

			// Try to resolve the payload address to a named
			// external symbol via the map built during compilation.
			std::map<uintptr_t, std::string>::const_iterator it =
			    external_symbol_map.find(payload);

			if ( it != external_symbol_map.end() )
			{
				// Known external symbol — emit as UNDEF + named reloc.
				uint32_t sym_idx;
				std::map<uintptr_t, uint32_t>::const_iterator eit =
				    extern_sym_indices.find(payload);
				if ( eit != extern_sym_indices.end() )
				{
					sym_idx = eit->second;
				}
				else
				{
					sym_idx = static_cast<uint32_t>(symbols.size());
					elf_sym ext;
					ext.name = it->second;
					ext.value = 0;
					ext.size = 0;
					ext.binding = STB_GLOBAL;
					ext.type = STT_NOTYPE;
					ext.section_index = SHN_UNDEF;
					symbols.push_back(ext);
					extern_sym_indices[payload] = sym_idx;
				}

				elf_rela entry;
				entry.offset = re->sourceOffset();
				entry.sym_index = sym_idx;
				entry.type = elf_type;
				entry.addend = 0; // resolved at link/load time
				rela_entries.push_back(entry);
			}
			else
			{
				// Unknown address — emit with raw addend (internal reloc).
				elf_rela entry;
				entry.offset = re->sourceOffset();
				entry.sym_index = 1; // .text section symbol
				entry.type = elf_type;
				entry.addend = static_cast<int64_t>(re->payload());
				rela_entries.push_back(entry);
			}
		}
	}

	// --- Assign strtab offsets to symbols ---

	std::vector<uint32_t> sym_name_offsets;
	for ( size_t i = 0; i < symbols.size(); ++i )
	{
		if ( symbols[i].name.empty() )
			sym_name_offsets.push_back(0);
		else
			sym_name_offsets.push_back(strtab.add(symbols[i].name));
	}

	// --- Layout sections ---
	// Section order: [0]=NULL, [1]=.text, [2]=.rela.text, [3]=.symtab,
	//                [4]=.strtab, [5]=.shstrtab

	const int SEC_NULL     = 0;
	const int SEC_TEXT     = 1;
	const int SEC_RELA     = 2;
	const int SEC_SYMTAB   = 3;
	const int SEC_STRTAB   = 4;
	const int SEC_SHSTRTAB = 5;
	const int NUM_SECTIONS = 6;

	(void)SEC_NULL;
	(void)SEC_RELA;

	// --- Write the file ---

	std::vector<uint8_t> out;

	// ELF header (64 bytes).
	Elf64_Ehdr ehdr;
	std::memset(&ehdr, 0, sizeof(ehdr));
	ehdr.e_ident[EI_MAG0] = ELFMAG0;
	ehdr.e_ident[EI_MAG1] = ELFMAG1;
	ehdr.e_ident[EI_MAG2] = ELFMAG2;
	ehdr.e_ident[EI_MAG3] = ELFMAG3;
	ehdr.e_ident[EI_CLASS] = ELFCLASS64;
	ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
	ehdr.e_ident[EI_VERSION] = EV_CURRENT;
	ehdr.e_ident[EI_OSABI] = ELFOSABI_NONE;
	ehdr.e_type = ET_REL;
	ehdr.e_machine = EM_X86_64;
	ehdr.e_version = EV_CURRENT;
	ehdr.e_ehsize = sizeof(Elf64_Ehdr);
	ehdr.e_shentsize = sizeof(Elf64_Shdr);
	ehdr.e_shnum = NUM_SECTIONS;
	ehdr.e_shstrndx = SEC_SHSTRTAB;

	emit(out, &ehdr, sizeof(ehdr));

	// .text section data.
	size_t text_offset = align_to(out, 16);
	emit(out, text_data, text_size);

	// .rela.text section data.
	size_t rela_offset = align_to(out, 8);
	size_t rela_size = rela_entries.size() * sizeof(Elf64_Rela);
	for ( size_t i = 0; i < rela_entries.size(); ++i )
	{
		Elf64_Rela rela;
		rela.r_offset = rela_entries[i].offset;
		rela.r_info = ELF64_R_INFO(rela_entries[i].sym_index, rela_entries[i].type);
		rela.r_addend = rela_entries[i].addend;
		emit(out, &rela, sizeof(rela));
	}

	// .symtab section data.
	size_t symtab_offset = align_to(out, 8);
	size_t symtab_size = symbols.size() * sizeof(Elf64_Sym);
	for ( size_t i = 0; i < symbols.size(); ++i )
	{
		Elf64_Sym sym;
		std::memset(&sym, 0, sizeof(sym));
		sym.st_name = sym_name_offsets[i];
		sym.st_value = symbols[i].value;
		sym.st_size = symbols[i].size;
		sym.st_info = ELF64_ST_INFO(symbols[i].binding, symbols[i].type);
		sym.st_shndx = symbols[i].section_index;
		emit(out, &sym, sizeof(sym));
	}

	// .strtab section data.
	size_t strtab_offset = out.size();
	size_t strtab_size = strtab.data.size();
	emit(out, strtab.data.data(), strtab_size);

	// .shstrtab section data.
	size_t shstrtab_offset = out.size();
	size_t shstrtab_size = shstrtab.data.size();
	emit(out, shstrtab.data.data(), shstrtab_size);

	// Section header table (must be aligned).
	size_t shdr_offset = align_to(out, 8);

	// Patch e_shoff in ELF header.
	{
		uint64_t shoff = shdr_offset;
		std::memcpy(&out[offsetof(Elf64_Ehdr, e_shoff)], &shoff, sizeof(shoff));
	}

	// [0] NULL section header.
	{
		Elf64_Shdr shdr;
		std::memset(&shdr, 0, sizeof(shdr));
		emit(out, &shdr, sizeof(shdr));
	}

	// [1] .text
	{
		Elf64_Shdr shdr;
		std::memset(&shdr, 0, sizeof(shdr));
		shdr.sh_name = shname_text;
		shdr.sh_type = SHT_PROGBITS;
		shdr.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
		shdr.sh_offset = text_offset;
		shdr.sh_size = text_size;
		shdr.sh_addralign = 16;
		emit(out, &shdr, sizeof(shdr));
	}

	// [2] .rela.text
	{
		Elf64_Shdr shdr;
		std::memset(&shdr, 0, sizeof(shdr));
		shdr.sh_name = shname_rela;
		shdr.sh_type = SHT_RELA;
		shdr.sh_flags = SHF_INFO_LINK;
		shdr.sh_offset = rela_offset;
		shdr.sh_size = rela_size;
		shdr.sh_link = SEC_SYMTAB;
		shdr.sh_info = SEC_TEXT;
		shdr.sh_addralign = 8;
		shdr.sh_entsize = sizeof(Elf64_Rela);
		emit(out, &shdr, sizeof(shdr));
	}

	// [3] .symtab
	{
		Elf64_Shdr shdr;
		std::memset(&shdr, 0, sizeof(shdr));
		shdr.sh_name = shname_symtab;
		shdr.sh_type = SHT_SYMTAB;
		shdr.sh_offset = symtab_offset;
		shdr.sh_size = symtab_size;
		shdr.sh_link = SEC_STRTAB;
		shdr.sh_info = static_cast<uint32_t>(first_global);
		shdr.sh_addralign = 8;
		shdr.sh_entsize = sizeof(Elf64_Sym);
		emit(out, &shdr, sizeof(shdr));
	}

	// [4] .strtab
	{
		Elf64_Shdr shdr;
		std::memset(&shdr, 0, sizeof(shdr));
		shdr.sh_name = shname_strtab;
		shdr.sh_type = SHT_STRTAB;
		shdr.sh_offset = strtab_offset;
		shdr.sh_size = strtab_size;
		shdr.sh_addralign = 1;
		emit(out, &shdr, sizeof(shdr));
	}

	// [5] .shstrtab
	{
		Elf64_Shdr shdr;
		std::memset(&shdr, 0, sizeof(shdr));
		shdr.sh_name = shname_shstrtab;
		shdr.sh_type = SHT_STRTAB;
		shdr.sh_offset = shstrtab_offset;
		shdr.sh_size = shstrtab_size;
		shdr.sh_addralign = 1;
		emit(out, &shdr, sizeof(shdr));
	}

	// --- Write to file ---

	FILE *fp = std::fopen(path.c_str(), "wb");
	if ( !fp )
		return false;
	size_t written = std::fwrite(out.data(), 1, out.size(), fp);
	std::fclose(fp);

	DBG(std::cout << "save_object: wrote " << written << " bytes to "
		      << path << " (" << symbols.size() << " symbols, "
		      << rela_entries.size() << " relocations)" << std::endl);

	return written == out.size();
}

// ---------------------------------------------------------------------------
// ELF .o loader
// ---------------------------------------------------------------------------

bool Program::load_object(const std::string &path)
{
	unload_object();

	// --- Read the file ---

	FILE *fp = std::fopen(path.c_str(), "rb");
	if ( !fp )
		return false;
	std::fseek(fp, 0, SEEK_END);
	long file_size = std::ftell(fp);
	std::fseek(fp, 0, SEEK_SET);
	if ( file_size < (long)sizeof(Elf64_Ehdr) )
	{
		std::fclose(fp);
		return false;
	}
	std::vector<uint8_t> buf(file_size);
	if ( std::fread(buf.data(), 1, file_size, fp) != (size_t)file_size )
	{
		std::fclose(fp);
		return false;
	}
	std::fclose(fp);

	// --- Validate ELF header ---

	const Elf64_Ehdr *ehdr = reinterpret_cast<const Elf64_Ehdr *>(buf.data());
	if ( ehdr->e_ident[EI_MAG0] != ELFMAG0
	  || ehdr->e_ident[EI_MAG1] != ELFMAG1
	  || ehdr->e_ident[EI_MAG2] != ELFMAG2
	  || ehdr->e_ident[EI_MAG3] != ELFMAG3 )
		return false;
	if ( ehdr->e_ident[EI_CLASS] != ELFCLASS64 )
		return false;
	if ( ehdr->e_type != ET_REL )
		return false;
	if ( ehdr->e_machine != EM_X86_64 )
		return false;

	// --- Parse section headers ---

	if ( ehdr->e_shoff == 0 || ehdr->e_shnum == 0 )
		return false;

	const Elf64_Shdr *shdrs = reinterpret_cast<const Elf64_Shdr *>(
	    buf.data() + ehdr->e_shoff);

	// Find .text, .symtab, .strtab, .rela.text sections.
	const Elf64_Shdr *text_shdr = NULL;
	const Elf64_Shdr *symtab_shdr = NULL;
	const Elf64_Shdr *strtab_shdr = NULL;
	const Elf64_Shdr *rela_shdr = NULL;

	// Get section name string table.
	const char *shstrtab = NULL;
	if ( ehdr->e_shstrndx < ehdr->e_shnum )
		shstrtab = reinterpret_cast<const char *>(
		    buf.data() + shdrs[ehdr->e_shstrndx].sh_offset);

	for ( uint16_t i = 0; i < ehdr->e_shnum; ++i )
	{
		const Elf64_Shdr *sh = &shdrs[i];
		const char *name = shstrtab ? shstrtab + sh->sh_name : "";

		if ( sh->sh_type == SHT_PROGBITS
		  && (sh->sh_flags & SHF_EXECINSTR)
		  && std::strcmp(name, ".text") == 0 )
			text_shdr = sh;
		else if ( sh->sh_type == SHT_SYMTAB )
			symtab_shdr = sh;
		else if ( sh->sh_type == SHT_STRTAB
			&& std::strcmp(name, ".strtab") == 0 )
			strtab_shdr = sh;
		else if ( sh->sh_type == SHT_RELA
			&& std::strcmp(name, ".rela.text") == 0 )
			rela_shdr = sh;
	}

	if ( !text_shdr || text_shdr->sh_size == 0 )
		return false;

	// --- Allocate executable memory and copy .text ---

	size_t code_size = text_shdr->sh_size;
	void *code_mem = mmap(NULL, code_size,
			      PROT_READ | PROT_WRITE | PROT_EXEC,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if ( code_mem == MAP_FAILED )
		return false;

	std::memcpy(code_mem, buf.data() + text_shdr->sh_offset, code_size);

	// --- Parse symbol table for relocation resolution ---

	const Elf64_Sym *elf_syms = NULL;
	size_t elf_sym_count = 0;
	const char *elf_strtab = NULL;

	if ( symtab_shdr && strtab_shdr )
	{
		elf_syms = reinterpret_cast<const Elf64_Sym *>(
		    buf.data() + symtab_shdr->sh_offset);
		elf_sym_count = symtab_shdr->sh_size / sizeof(Elf64_Sym);
		elf_strtab = reinterpret_cast<const char *>(
		    buf.data() + strtab_shdr->sh_offset);
	}

	// --- Apply relocations ---

	if ( rela_shdr && rela_shdr->sh_size > 0 )
	{
		size_t rela_count = rela_shdr->sh_size / sizeof(Elf64_Rela);
		const Elf64_Rela *relas = reinterpret_cast<const Elf64_Rela *>(
		    buf.data() + rela_shdr->sh_offset);

		for ( size_t i = 0; i < rela_count; ++i )
		{
			const Elf64_Rela *r = &relas[i];
			uint32_t r_type = ELF64_R_TYPE(r->r_info);
			uint32_t r_sym = ELF64_R_SYM(r->r_info);
			uint8_t *patch_addr = static_cast<uint8_t *>(code_mem) + r->r_offset;

			if ( r->r_offset >= code_size )
				continue;

			// Resolve the target address for this relocation.
			uint64_t target = 0;
			bool resolved = false;

			if ( r_sym > 0 && r_sym < elf_sym_count && elf_syms && elf_strtab )
			{
				const Elf64_Sym *sym = &elf_syms[r_sym];
				if ( sym->st_shndx == SHN_UNDEF && sym->st_name > 0 )
				{
					// External symbol — resolve via dlsym.
					const char *sym_name = elf_strtab + sym->st_name;
					void *resolved_addr = dlsym(RTLD_DEFAULT, sym_name);
					if ( resolved_addr )
					{
						target = reinterpret_cast<uint64_t>(resolved_addr)
							 + static_cast<uint64_t>(r->r_addend);
						resolved = true;
						DBG(std::cout << "load_object: resolved '"
							      << sym_name << "' -> "
							      << resolved_addr << std::endl);
					}
					else
					{
						DBG(std::cout << "load_object: WARNING: unresolved symbol '"
							      << sym_name << "'" << std::endl);
					}
				}
				else if ( sym->st_shndx != SHN_UNDEF )
				{
					// Defined symbol — offset within .text.
					target = reinterpret_cast<uint64_t>(code_mem)
						 + sym->st_value
						 + static_cast<uint64_t>(r->r_addend);
					resolved = true;
				}
			}

			if ( !resolved )
			{
				// Fallback: use raw addend as absolute address.
				target = static_cast<uint64_t>(r->r_addend);
			}

			if ( r_type == R_X86_64_64 )
			{
				std::memcpy(patch_addr, &target, 8);
			}
			else if ( r_type == R_X86_64_PC32 )
			{
				int64_t pc = reinterpret_cast<int64_t>(patch_addr) + 4;
				int32_t rel = static_cast<int32_t>(
				    static_cast<int64_t>(target) - pc);
				std::memcpy(patch_addr, &rel, 4);
			}
		}
	}

	// --- Resolve function symbols ---

	loaded_object.code_base = code_mem;
	loaded_object.code_size = code_size;

	if ( symtab_shdr && strtab_shdr )
	{
		const char *str_data = reinterpret_cast<const char *>(
		    buf.data() + strtab_shdr->sh_offset);
		size_t sym_count = symtab_shdr->sh_size / sizeof(Elf64_Sym);
		const Elf64_Sym *syms = reinterpret_cast<const Elf64_Sym *>(
		    buf.data() + symtab_shdr->sh_offset);

		for ( size_t i = 0; i < sym_count; ++i )
		{
			const Elf64_Sym *sym = &syms[i];
			if ( ELF64_ST_TYPE(sym->st_info) != STT_FUNC )
				continue;
			if ( ELF64_ST_BIND(sym->st_info) != STB_GLOBAL )
				continue;
			if ( sym->st_shndx == SHN_UNDEF )
				continue;

			const char *name = str_data + sym->st_name;
			if ( !name[0] )
				continue;

			void *entry = static_cast<uint8_t *>(code_mem) + sym->st_value;
			loaded_object.functions[name] = entry;
		}
	}

	DBG(std::cout << "load_object: loaded " << code_size << " bytes from "
		      << path << " (" << loaded_object.functions.size()
		      << " functions)" << std::endl);

	return !loaded_object.functions.empty();
}

bool Program::has_loaded_function(const std::string &name) const
{
	return loaded_object.functions.count(name) > 0;
}

void *Program::loaded_function_ptr(const std::string &name) const
{
	std::map<std::string, void *>::const_iterator it =
	    loaded_object.functions.find(name);
	if ( it == loaded_object.functions.end() )
		return NULL;
	return it->second;
}

void Program::unload_object()
{
	if ( loaded_object.code_base )
	{
		munmap(loaded_object.code_base, loaded_object.code_size);
		loaded_object.code_base = NULL;
		loaded_object.code_size = 0;
	}
	loaded_object.functions.clear();
}

// ---------------------------------------------------------------------------
// ELF executable writer
// ---------------------------------------------------------------------------

bool Program::save_executable(const std::string &path)
{
	if ( !root_fn )
		return false;

	Section *text_section = code.textSection();
	if ( !text_section )
		return false;

	const uint8_t *text_data = text_section->data();
	size_t text_size = text_section->buffer().size();
	if ( !text_data || text_size == 0 )
		return false;

	// --- Collect .addrtab section (asmjit's 64-bit address table) ---
	const uint8_t *addrtab_data = NULL;
	size_t addrtab_size = 0;
	{
		const ZoneVector<Section *> &secs = code.sections();
		for ( uint32_t i = 0; i < secs.size(); ++i )
		{
			if ( std::strcmp(secs[i]->name(), ".addrtab") == 0 )
			{
				addrtab_data = secs[i]->data();
				addrtab_size = secs[i]->buffer().size();
				break;
			}
		}
	}

	// --- Build addrtab → symbol mapping ---
	struct addrtab_slot_info { size_t slot_index; uintptr_t address; };
	std::vector<addrtab_slot_info> addrtab_slots;
	uintptr_t old_addrtab_base = 0;

	if ( addrtab_data && addrtab_size > 0 )
	{
		Section *addrtab_section = code.addressTableSection();
		if ( addrtab_section )
			old_addrtab_base = reinterpret_cast<uintptr_t>(root_fn)
			    + addrtab_section->offset();

		size_t slot_count = addrtab_size / 8;
		for ( size_t i = 0; i < slot_count; ++i )
		{
			uint64_t addr;
			std::memcpy(&addr, addrtab_data + i * 8, 8);
			addrtab_slot_info info;
			info.slot_index = i;
			info.address = static_cast<uintptr_t>(addr);
			addrtab_slots.push_back(info);
		}
	}

	// --- Collect global data for .data section ---

	std::vector<GlobalDataEntry> globals = collect_global_data();
	std::map<uintptr_t, size_t> data_offset_map; // old addr → offset in .data
	std::vector<uint8_t> data_section_buf;

	for ( size_t i = 0; i < globals.size(); ++i )
	{
		// Align each entry to 8 bytes.
		size_t pad = (8 - (data_section_buf.size() % 8)) % 8;
		data_section_buf.insert(data_section_buf.end(), pad, 0);

		size_t offset = data_section_buf.size();
		data_offset_map[reinterpret_cast<uintptr_t>(globals[i].address)] = offset;

		const uint8_t *src = static_cast<const uint8_t *>(globals[i].address);
		data_section_buf.insert(data_section_buf.end(), src, src + globals[i].size);
	}

	// Append .addrtab slots to .data section so the movabs scanner
	// can patch .text references to the addrtab. Record which data
	// offsets are addrtab slots needing dynamic symbol resolution.
	struct addrtab_rela { size_t data_offset; uintptr_t func_addr; };
	std::vector<addrtab_rela> addrtab_relas;

	if ( old_addrtab_base && !addrtab_slots.empty() )
	{
		// Align to 8 bytes.
		size_t pad = (8 - (data_section_buf.size() % 8)) % 8;
		data_section_buf.insert(data_section_buf.end(), pad, 0);

		size_t addrtab_data_start = data_section_buf.size();

		// Map old addrtab slot addresses → data section offsets.
		for ( size_t i = 0; i < addrtab_slots.size(); ++i )
		{
			uintptr_t old_slot_addr = old_addrtab_base + i * 8;
			size_t slot_data_offset = addrtab_data_start + i * 8;
			data_offset_map[old_slot_addr] = slot_data_offset;

			addrtab_rela ar;
			ar.data_offset = slot_data_offset;
			ar.func_addr = addrtab_slots[i].address;
			addrtab_relas.push_back(ar);
		}

		// Copy the raw addrtab data (will be overwritten by dynamic linker).
		data_section_buf.insert(data_section_buf.end(),
		    addrtab_data, addrtab_data + addrtab_size);
	}

	// --- Find main() entry point offset ---

	int64_t main_offset = -1;
	for ( size_t i = 0; i < pending_funcs.size(); ++i )
	{
		TokenFunc *tf = dynamic_cast<TokenFunc *>(pending_funcs[i]);
		if ( !tf || !tf->var.data || tf->is_overridden )
			continue;
		if ( tf->var.name != "main" )
			continue;
		Method *method = static_cast<Method *>(tf->var.data);
		if ( !method )
			continue;
		FuncDef *func = dynamic_cast<FuncDef *>(method->returns.type);
		if ( !func || !func->funcnode )
			continue;
		uint32_t label_id = func->funcnode->label().id();
		if ( code.isLabelBound(label_id) )
			main_offset = static_cast<int64_t>(code.labelOffset(label_id));
		break;
	}
	if ( main_offset < 0 )
		return false;

	// --- Collect external symbols from relocations ---

	struct ext_sym { std::string name; };
	std::vector<ext_sym> extern_syms;
	std::map<uintptr_t, uint32_t> extern_indices; // addr -> dynsym index

	struct rela_entry
	{
		uint64_t source_offset; // in .text (relative to text start)
		uint32_t dynsym_index;
		uint32_t elf_type;
		bool is_extern;
		int64_t raw_addend;
	};
	std::vector<rela_entry> rela_entries;

	if ( code.hasRelocEntries() )
	{
		const ZoneVector<RelocEntry *> &relocs = code.relocEntries();
		for ( uint32_t i = 0; i < relocs.size(); ++i )
		{
			const RelocEntry *re = relocs[i];
			if ( !re || re->relocType() == RelocType::kNone )
				continue;
			uint32_t elf_type = madc_elf_reloc_type(re->relocType());
			if ( elf_type == R_X86_64_NONE )
				continue;

			uintptr_t payload = static_cast<uintptr_t>(re->payload());
			std::map<uintptr_t, std::string>::const_iterator it =
			    external_symbol_map.find(payload);

			if ( it != external_symbol_map.end() )
			{
				uint32_t dynsym_idx;
				std::map<uintptr_t, uint32_t>::const_iterator eit =
				    extern_indices.find(payload);
				if ( eit != extern_indices.end() )
				{
					dynsym_idx = eit->second;
				}
				else
				{
					// dynsym[0] is NULL, so first real symbol is index 1
					dynsym_idx = static_cast<uint32_t>(extern_syms.size() + 1);
					ext_sym es;
					es.name = it->second;
					extern_syms.push_back(es);
					extern_indices[payload] = dynsym_idx;
				}
				rela_entry re_entry;
				re_entry.source_offset = re->sourceOffset();
				re_entry.dynsym_index = dynsym_idx;
				re_entry.elf_type = elf_type;
				re_entry.is_extern = true;
				re_entry.raw_addend = 0;
				rela_entries.push_back(re_entry);
			}
			else
			{
				// Internal relocation — patch with raw addend.
				rela_entry re_entry;
				re_entry.source_offset = re->sourceOffset();
				re_entry.dynsym_index = 0;
				re_entry.elf_type = elf_type;
				re_entry.is_extern = false;
				re_entry.raw_addend = static_cast<int64_t>(re->payload());
				rela_entries.push_back(re_entry);
			}
		}
	}

	// --- Add addrtab slot relocations ---
	// Each addrtab slot is a function pointer that needs to be resolved
	// by the dynamic linker. Emit as UNDEF symbol + R_X86_64_64 on
	// the .data section offset where the slot lives.
	struct data_rela { size_t data_offset; uint32_t dynsym_index; };
	std::vector<data_rela> addrtab_data_relas;

	for ( size_t i = 0; i < addrtab_relas.size(); ++i )
	{
		uintptr_t func_addr = addrtab_relas[i].func_addr;
		std::map<uintptr_t, std::string>::const_iterator it =
		    external_symbol_map.find(func_addr);
		if ( it == external_symbol_map.end() )
			continue;

		uint32_t dynsym_idx;
		std::map<uintptr_t, uint32_t>::const_iterator eit =
		    extern_indices.find(func_addr);
		if ( eit != extern_indices.end() )
		{
			dynsym_idx = eit->second;
		}
		else
		{
			dynsym_idx = static_cast<uint32_t>(extern_syms.size() + 1);
			ext_sym es;
			es.name = it->second;
			extern_syms.push_back(es);
			extern_indices[func_addr] = dynsym_idx;
		}

		data_rela dr;
		dr.data_offset = addrtab_relas[i].data_offset;
		dr.dynsym_index = dynsym_idx;
		addrtab_data_relas.push_back(dr);
	}

	// --- Discover required shared libraries via dladdr ---

	struct needed_lib { std::string name; uint32_t dynstr_offset; };
	std::vector<needed_lib> needed_libs;
	std::set<std::string> needed_lib_set;

	for ( std::map<uintptr_t, uint32_t>::const_iterator eit = extern_indices.begin();
	      eit != extern_indices.end(); ++eit )
	{
		Dl_info info;
		if ( dladdr(reinterpret_cast<void *>(eit->first), &info) && info.dli_fname )
		{
			// Extract basename from the library path.
			std::string libpath = info.dli_fname;
			std::string libname = libpath;
			size_t slash = libpath.rfind('/');
			if ( slash != std::string::npos )
				libname = libpath.substr(slash + 1);
			// Map known library names to their SONAME form.
			// When dladdr reports the main executable, the
			// symbol could be a madc runtime helper or a
			// libc-family function that was statically resolved
			// during link. Check the symbol name to classify.
			std::map<uintptr_t, std::string>::const_iterator sym_it =
			    external_symbol_map.find(eit->first);
			std::string sym_name = (sym_it != external_symbol_map.end())
			    ? sym_it->second : "";

			if ( libname.find(".so") == std::string::npos
			  && libname.find("lib") != 0 )
			{
				// From the main executable. Classify by name.
				if ( sym_name == "crypt" || sym_name == "crypt_r" )
					libname = "libcrypt.so.1";
				else if ( sym_name.find("__madc_") == 0
				       || sym_name == "printstring"
				       || sym_name == "printstarred"
				       || sym_name == "printstream"
				       || sym_name == "printinteger"
				       || sym_name == "printuinteger"
				       || sym_name == "printdouble"
				       || sym_name == "string_construct"
				       || sym_name == "string_destruct"
				       || sym_name == "string_assign"
				       || sym_name == "string_cstr" )
					libname = "libmadc.so";
				else
					libname = "libc.so.6";
			}
			else if ( libname.find("libmadc") == 0 )
				libname = "libmadc.so";
			else if ( libname.find("libc") == 0 )
				libname = "libc.so.6";
			else if ( libname.find("libm") == 0 )
				libname = "libm.so.6";
			else if ( libname.find("libdl") == 0 )
				libname = "libdl.so.2";
			else if ( libname.find("libpthread") == 0 )
				libname = "libpthread.so.0";
			else if ( libname.find("libcrypt") == 0 )
				libname = "libcrypt.so.1";
			// Skip kernel virtual DSOs.
			if ( libname.find("vdso") != std::string::npos )
				continue;
			if ( needed_lib_set.find(libname) == needed_lib_set.end() )
			{
				needed_lib_set.insert(libname);
				needed_lib nl;
				nl.name = libname;
				needed_libs.push_back(nl);
			}
		}
	}

	// Always need libc if there are any external symbols.
	if ( !extern_syms.empty() && needed_lib_set.find("libc.so.6") == needed_lib_set.end() )
	{
		needed_lib nl;
		nl.name = "libc.so.6";
		needed_libs.push_back(nl);
	}

	// --- Build .dynstr ---

	strtab_builder dynstr;
	std::vector<uint32_t> dynsym_name_offsets;
	dynsym_name_offsets.push_back(0); // NULL symbol

	// DT_NEEDED strings.
	for ( size_t i = 0; i < needed_libs.size(); ++i )
		needed_libs[i].dynstr_offset = dynstr.add(needed_libs[i].name);

	for ( size_t i = 0; i < extern_syms.size(); ++i )
		dynsym_name_offsets.push_back(dynstr.add(extern_syms[i].name));

	// --- Build .dynsym ---

	size_t dynsym_count = 1 + extern_syms.size(); // NULL + externals
	size_t dynsym_size = dynsym_count * sizeof(Elf64_Sym);

	// --- Build .hash (SysV) ---

	uint32_t nbucket = dynsym_count > 1 ? static_cast<uint32_t>(dynsym_count) : 1;
	uint32_t nchain = static_cast<uint32_t>(dynsym_count);
	size_t hash_size = (2 + nbucket + nchain) * sizeof(uint32_t);

	std::vector<uint32_t> hash_buckets(nbucket, 0);
	std::vector<uint32_t> hash_chains(nchain, 0);

	for ( size_t i = 1; i < dynsym_count; ++i )
	{
		const char *name = extern_syms[i - 1].name.c_str();
		uint32_t h = elf_hash(name) % nbucket;
		hash_chains[i] = hash_buckets[h];
		hash_buckets[h] = static_cast<uint32_t>(i);
	}

	// --- Build .rela.dyn (only external relocations) ---

	std::vector<Elf64_Rela> elf_relas;
	// We'll also need to handle internal relocs by pre-patching the code.

	// --- Layout ---
	// Everything in one PT_LOAD segment for simplicity.

	const uint64_t BASE_ADDR = 0x400000;
	const char *interp_str = "/lib64/ld-linux-x86-64.so.2";
	size_t interp_size = std::strlen(interp_str) + 1;

	// We need 3 program headers: PT_LOAD, PT_INTERP, PT_DYNAMIC.
	const int NUM_PHDRS = 3;

	std::vector<uint8_t> out;

	// --- Pass 1: write everything, record offsets ---

	// ELF header placeholder.
	Elf64_Ehdr ehdr;
	std::memset(&ehdr, 0, sizeof(ehdr));
	emit(out, &ehdr, sizeof(ehdr));

	// Program headers placeholder.
	size_t phdr_offset = out.size();
	Elf64_Phdr phdr_placeholder;
	std::memset(&phdr_placeholder, 0, sizeof(phdr_placeholder));
	for ( int i = 0; i < NUM_PHDRS; ++i )
		emit(out, &phdr_placeholder, sizeof(phdr_placeholder));

	// .interp
	size_t interp_offset = out.size();
	emit(out, interp_str, interp_size);

	// .hash
	size_t hash_offset = align_to(out, 4);
	emit_val(out, nbucket);
	emit_val(out, nchain);
	for ( uint32_t i = 0; i < nbucket; ++i )
		emit_val(out, hash_buckets[i]);
	for ( uint32_t i = 0; i < nchain; ++i )
		emit_val(out, hash_chains[i]);

	// .dynsym
	size_t dynsym_offset = align_to(out, 8);
	// NULL symbol.
	{
		Elf64_Sym sym;
		std::memset(&sym, 0, sizeof(sym));
		emit(out, &sym, sizeof(sym));
	}
	for ( size_t i = 0; i < extern_syms.size(); ++i )
	{
		Elf64_Sym sym;
		std::memset(&sym, 0, sizeof(sym));
		sym.st_name = dynsym_name_offsets[i + 1];
		sym.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
		sym.st_shndx = SHN_UNDEF;
		emit(out, &sym, sizeof(sym));
	}

	// .dynstr
	size_t dynstr_offset = out.size();
	size_t dynstr_size = dynstr.data.size();
	emit(out, dynstr.data.data(), dynstr_size);

	// .rela.dyn — build and emit external relocations.
	// Text will start at a known offset; compute it now.
	(void)0; // rela follows
	// Reserve space: we'll compute text_file_offset after rela.
	// External relas need the text virtual address to compute offsets.
	// Two-pass: first compute sizes, then lay out.

	// Compute rela size (text relocs + addrtab relocs).
	size_t extern_rela_count = 0;
	for ( size_t i = 0; i < rela_entries.size(); ++i )
		if ( rela_entries[i].is_extern )
			++extern_rela_count;
	size_t total_rela_count = extern_rela_count + addrtab_data_relas.size();
	// Reserve extra space for PLT GOT relas that may be added later.
	size_t max_plt_relas = extern_syms.size() + external_symbol_map.size();
	size_t rela_dyn_size = (total_rela_count + max_plt_relas) * sizeof(Elf64_Rela);

	size_t rela_dyn_offset = align_to(out, 8);
	// Emit placeholder relas — we'll patch them later.
	size_t rela_dyn_data_start = out.size();
	emit_zeros(out, rela_dyn_size);

	// .text (stub + compiled code)
	size_t text_file_offset = align_to(out, 16);
	uint64_t text_vaddr = BASE_ADDR + text_file_offset;

	// _start stub
	size_t start_file_offset = out.size();
	uint64_t start_vaddr = BASE_ADDR + start_file_offset;
	emit(out, start_stub, START_STUB_SIZE);

	// Compiled code
	size_t code_file_offset = out.size();
	uint64_t code_vaddr = BASE_ADDR + code_file_offset;
	// Use asmjit's own relocation engine to produce properly
	// relocated code at our target virtual address. This handles
	// all internal relocations (kX64AddressEntry, kAbsToAbs, etc.)
	// correctly, including RIP-relative displacements.
	// The addrtab function addresses will still point to the
	// compiling process's libc — we fix those via .rela.dyn.
	code.flatten();
	code.relocateToBase(code_vaddr);

	size_t flat_size = code.codeSize();
	std::vector<uint8_t> text_copy(flat_size, 0);
	code.copyFlattenedData(text_copy.data(), flat_size);
	// The flattened data includes .text + .addrtab contiguously.
	// Keep both so the RIP-relative addrtab references work.
	// total_code_size includes .text + any padding + .addrtab.
	size_t total_code_size = flat_size;

	// Compute .data virtual address (follows .text+addrtab, aligned).
	size_t data_file_offset_est = text_file_offset + START_STUB_SIZE + total_code_size;
	data_file_offset_est = (data_file_offset_est + 15) & ~(size_t)15; // align 16
	uint64_t data_vaddr = BASE_ADDR + data_file_offset_est;

	// Patch data references using compile-time tracked labels.
	// Each aot_data_ref records the label bound BEFORE the movabs
	// instruction and the original data address. The movabs is
	// 10 bytes: REX(1) + opcode(1) + imm64(8). The immediate
	// starts at label_offset + 2.
	size_t data_patches = 0;
	if ( !data_offset_map.empty() && !aot_data_refs.empty() )
	{
		for ( size_t i = 0; i < aot_data_refs.size(); ++i )
		{
			const AotDataRef &ref = aot_data_refs[i];
			if ( !code.isLabelBound(ref.label_id) )
				continue;

			std::map<uintptr_t, size_t>::const_iterator dit =
			    data_offset_map.find(ref.address);
			if ( dit == data_offset_map.end() )
				continue;

			uint64_t label_off = code.labelOffset(ref.label_id);
			size_t imm_off = static_cast<size_t>(label_off) + 2;
			if ( imm_off + 8 > text_size )
				continue;

			uint64_t new_addr = data_vaddr + dit->second;
			std::memcpy(&text_copy[imm_off], &new_addr, 8);
			++data_patches;
		}
	}

	// Note: addrtab references are already patched by relocateToBase
	// above — they point to the correct (virtual) addrtab location.
	// The addrtab itself lives in .data and will be patched by the
	// dynamic linker via .rela.dyn entries.

	// Scan for direct call rel32 to external functions that asmjit
	// encoded without the address table. Build a mini-PLT: for each
	// unique external target, create a trampoline stub (jmp *[rip+N])
	// that jumps through a GOT slot. Rewrite the call rel32 to target
	// the trampoline. The dynamic linker fills the GOT slots.
	struct plt_entry { uintptr_t target_addr; uint32_t dynsym_index; size_t got_offset; };
	std::vector<plt_entry> plt_entries;
	std::map<uintptr_t, size_t> plt_index_map; // target → index into plt_entries

	// First pass: find all call rel32 to external targets.
	struct call_patch { size_t code_offset; size_t plt_idx; };
	std::vector<call_patch> call_patches;
	{
		for ( size_t i = 0; i + 5 <= total_code_size; ++i )
		{
			bool has_rex = false;
			size_t call_off = i;

			if ( text_copy[i] == 0xE8 )
				call_off = i;
			else if ( text_copy[i] >= 0x40 && text_copy[i] <= 0x4F
				&& i + 6 <= total_code_size && text_copy[i+1] == 0xE8 )
			{
				has_rex = true;
				call_off = i + 1;
			}
			else
				continue;

			int32_t disp;
			std::memcpy(&disp, &text_copy[call_off + 1], 4);
			int64_t rip = static_cast<int64_t>(code_vaddr + call_off + 5);
			int64_t target = rip + disp;

			if ( target >= (int64_t)code_vaddr
			  && target < (int64_t)(code_vaddr + total_code_size) )
				continue; // internal

			uintptr_t taddr = static_cast<uintptr_t>(target);
			std::map<uintptr_t, std::string>::const_iterator eit =
			    external_symbol_map.find(taddr);
			if ( eit == external_symbol_map.end() )
				continue;

			size_t plt_idx;
			std::map<uintptr_t, size_t>::const_iterator pit =
			    plt_index_map.find(taddr);
			if ( pit != plt_index_map.end() )
			{
				plt_idx = pit->second;
			}
			else
			{
				// Find or create dynsym entry.
				uint32_t dsym;
				std::map<uintptr_t, uint32_t>::const_iterator dit =
				    extern_indices.find(taddr);
				if ( dit != extern_indices.end() )
				{
					dsym = dit->second;
				}
				else
				{
					dsym = static_cast<uint32_t>(extern_syms.size() + 1);
					ext_sym es;
					es.name = eit->second;
					extern_syms.push_back(es);
					extern_indices[taddr] = dsym;
				}

				plt_idx = plt_entries.size();
				plt_entry pe;
				pe.target_addr = taddr;
				pe.dynsym_index = dsym;
				pe.got_offset = 0; // computed later
				plt_entries.push_back(pe);
				plt_index_map[taddr] = plt_idx;
			}

			call_patch cp;
			cp.code_offset = call_off;
			cp.plt_idx = plt_idx;
			call_patches.push_back(cp);
		}
	}

	// Build PLT stub + GOT data.
	// Layout: [stubs: 6 bytes each] [GOT slots: 8 bytes each]
	// jmp *[rip+N] = FF 25 <disp32>
	size_t plt_stub_size = plt_entries.size() * 6;
	size_t plt_got_offset_base = plt_stub_size; // GOT starts after stubs
	std::vector<uint8_t> plt_data;

	if ( !plt_entries.empty() )
	{
		// Emit stubs.
		for ( size_t i = 0; i < plt_entries.size(); ++i )
		{
			size_t stub_off = plt_data.size();
			size_t got_off = plt_got_offset_base + i * 8;
			// RIP after this instruction = stub_off + 6
			int32_t got_disp = static_cast<int32_t>(got_off - (stub_off + 6));
			uint8_t stub[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
			std::memcpy(&stub[2], &got_disp, 4);
			plt_data.insert(plt_data.end(), stub, stub + 6);
		}
		// Emit GOT slots (zeroed — dynamic linker fills them).
		for ( size_t i = 0; i < plt_entries.size(); ++i )
		{
			plt_entries[i].got_offset = plt_data.size();
			uint64_t zero = 0;
			const uint8_t *p = reinterpret_cast<const uint8_t *>(&zero);
			plt_data.insert(plt_data.end(), p, p + 8);
		}

		// Append PLT to text_copy.
		size_t plt_file_start = total_code_size;
		text_copy.insert(text_copy.end(), plt_data.begin(), plt_data.end());
		total_code_size += plt_data.size();

		// Rewrite call rel32 instructions to target PLT stubs.
		for ( size_t i = 0; i < call_patches.size(); ++i )
		{
			size_t co = call_patches[i].code_offset;
			size_t pi = call_patches[i].plt_idx;
			size_t plt_stub_addr = code_vaddr + plt_file_start + pi * 6;
			int64_t rip = static_cast<int64_t>(code_vaddr + co + 5);
			int32_t new_disp = static_cast<int32_t>(
			    static_cast<int64_t>(plt_stub_addr) - rip);
			std::memcpy(&text_copy[co + 1], &new_disp, 4);
		}

		// Add R_X86_64_64 relocations for GOT slots.
		for ( size_t i = 0; i < plt_entries.size(); ++i )
		{
			addrtab_rela ar;
			ar.func_addr = plt_entries[i].target_addr;
			// Create a data_rela for the GOT slot in the PLT area.
			data_rela dr;
			dr.data_offset = 0; // not in .data — handled separately
			dr.dynsym_index = plt_entries[i].dynsym_index;

			// We'll emit these relas directly; store info for later.
			rela_entry re;
			re.source_offset = plt_file_start + plt_entries[i].got_offset;
			re.dynsym_index = plt_entries[i].dynsym_index;
			re.elf_type = R_X86_64_64;
			re.is_extern = true;
			re.raw_addend = 0;
			rela_entries.push_back(re);
		}

		// Recount extern relas.
		extern_rela_count = 0;
		for ( size_t i = 0; i < rela_entries.size(); ++i )
			if ( rela_entries[i].is_extern )
				++extern_rela_count;
		total_rela_count = extern_rela_count + addrtab_data_relas.size();
		rela_dyn_size = total_rela_count * sizeof(Elf64_Rela);

		data_patches += call_patches.size();

		DBG(std::cout << "save_executable: built PLT with "
			      << plt_entries.size() << " entries, patched "
			      << call_patches.size() << " direct calls" << std::endl);
	}

	// Scan for absolute address patterns in the relocated code.
	// After relocateToBase, addrtab references are correct, but
	// global variable loads/stores (0xA1/0xA3 moffs) and string
	// constant loads (0xB8-0xBF movabs) still have old addresses.
	if ( !data_offset_map.empty() )
	{
		for ( size_t i = 0; i + 10 <= total_code_size; ++i )
		{
			uint8_t b0 = text_copy[i];
			uint8_t b1 = text_copy[i + 1];

			bool is_moffs = (b0 == 0x48 && (b1 == 0xA1 || b1 == 0xA3));
			bool is_movabs = ((b0 == 0x48 || b0 == 0x49) && b1 >= 0xB8 && b1 <= 0xBF);

			if ( !is_moffs && !is_movabs )
				continue;

			uint64_t imm_val;
			std::memcpy(&imm_val, &text_copy[i + 2], 8);
			uintptr_t addr = static_cast<uintptr_t>(imm_val);

			std::map<uintptr_t, size_t>::const_iterator dit =
			    data_offset_map.find(addr);
			if ( dit != data_offset_map.end() )
			{
				uint64_t new_addr = data_vaddr + dit->second;
				std::memcpy(&text_copy[i + 2], &new_addr, 8);
				++data_patches;
			}
		}
	}

	DBG(std::cout << "save_executable: patched " << data_patches
		      << " global data references (" << globals.size()
		      << " globals, " << data_section_buf.size()
		      << " bytes in .data)" << std::endl);

	emit(out, text_copy.data(), total_code_size);
	size_t total_text_size = START_STUB_SIZE + total_code_size;

	// Patch _start stub call displacement to main.
	{
		uint64_t main_vaddr = code_vaddr + static_cast<uint64_t>(main_offset);
		uint64_t call_pc = start_vaddr + START_STUB_CALL_OFFSET + 4;
		int32_t disp = static_cast<int32_t>(
		    static_cast<int64_t>(main_vaddr) - static_cast<int64_t>(call_pc));
		size_t patch_pos = start_file_offset + START_STUB_CALL_OFFSET;
		std::memcpy(&out[patch_pos], &disp, 4);
	}

	size_t addrtab_rela_start = 0;
	// Now patch .rela.dyn entries with correct virtual addresses.
	// For each relocation, inspect the instruction encoding to
	// determine the correct ELF relocation type:
	// - call rel32 (E8 xx xx xx xx): R_X86_64_PC32, addend = -4
	// - movabs (48 B8+r / 48 A1/A3): R_X86_64_64, addend = 0
	{
		size_t rela_pos = rela_dyn_data_start;
		for ( size_t i = 0; i < rela_entries.size(); ++i )
		{
			if ( !rela_entries[i].is_extern )
				continue;

			size_t src_off = rela_entries[i].source_offset;
			uint32_t elf_type = rela_entries[i].elf_type;
			int64_t addend = 0;

			// Check instruction byte before the patch offset.
			// asmjit's sourceOffset points to the value being
			// patched. For call rel32, that's the byte after E8.
			// For movabs, it's the byte after REX+opcode.
			if ( src_off > 0 && src_off < text_size )
			{
				uint8_t prev = text_copy[src_off - 1];
				uint8_t prev2 = (src_off > 1) ? text_copy[src_off - 2] : 0;

				// E8 = call rel32 (no REX prefix)
				// 40-4F E8 = REX call rel32
				if ( prev == 0xE8
				  || (prev == 0xE8 && prev2 >= 0x40 && prev2 <= 0x4F) )
				{
					elf_type = R_X86_64_PC32;
					addend = -4;
				}
			}

			Elf64_Rela rela;
			rela.r_offset = code_vaddr + src_off;
			rela.r_info = ELF64_R_INFO(rela_entries[i].dynsym_index, elf_type);
			rela.r_addend = addend;
			std::memcpy(&out[rela_pos], &rela, sizeof(rela));
			rela_pos += sizeof(rela);
		}
		// Addrtab slot relas: R_X86_64_64 on .text offsets
		// where the addrtab lives (contiguous after the code).
		// The addrtab starts at code_vaddr + text_size.
		uint64_t addrtab_vaddr_in_text = code_vaddr + text_size;
		for ( size_t i = 0; i < addrtab_data_relas.size(); ++i )
		{
			// Find the slot index from the data_offset.
			// Each slot is 8 bytes from the start of addrtab.
			size_t slot_idx = 0;
			for ( size_t j = 0; j < addrtab_slots.size(); ++j )
			{
				if ( addrtab_slots[j].address == addrtab_relas[i].func_addr )
				{
					slot_idx = j;
					break;
				}
			}

			Elf64_Rela rela;
			rela.r_offset = addrtab_vaddr_in_text + slot_idx * 8;
			rela.r_info = ELF64_R_INFO(addrtab_data_relas[i].dynsym_index,
						    R_X86_64_64);
			rela.r_addend = 0;
			std::memcpy(&out[rela_pos], &rela, sizeof(rela));
			rela_pos += sizeof(rela);
		}
	}

	// .data (global variables and string literals)
	size_t data_file_offset = 0;
	size_t data_section_size = data_section_buf.size();
	if ( data_section_size > 0 )
	{
		data_file_offset = align_to(out, 16);
		emit(out, data_section_buf.data(), data_section_size);
		// Recalculate actual data vaddr for section header.
		data_vaddr = BASE_ADDR + data_file_offset;
	}

	// .dynamic
	size_t dynamic_offset = align_to(out, 8);
	uint64_t dynamic_vaddr = BASE_ADDR + dynamic_offset;

	auto emit_dyn = [&](int64_t tag, uint64_t val) {
		Elf64_Dyn d;
		d.d_tag = tag;
		d.d_un.d_val = val;
		emit(out, &d, sizeof(d));
	};

	for ( size_t i = 0; i < needed_libs.size(); ++i )
		emit_dyn(DT_NEEDED, needed_libs[i].dynstr_offset);
	emit_dyn(DT_STRTAB, BASE_ADDR + dynstr_offset);
	emit_dyn(DT_STRSZ, dynstr_size);
	emit_dyn(DT_SYMTAB, BASE_ADDR + dynsym_offset);
	emit_dyn(DT_SYMENT, sizeof(Elf64_Sym));
	emit_dyn(DT_HASH, BASE_ADDR + hash_offset);
	if ( total_rela_count > 0 )
	{
		emit_dyn(DT_RELA, BASE_ADDR + rela_dyn_offset);
		emit_dyn(DT_RELASZ, rela_dyn_size);
		emit_dyn(DT_RELAENT, sizeof(Elf64_Rela));
		// TEXTREL needed if any relocs target .text (not just .data).
		if ( extern_rela_count > 0 )
			emit_dyn(DT_TEXTREL, 0);
	}
	emit_dyn(DT_NULL, 0);

	size_t dynamic_size = out.size() - dynamic_offset;

	// Section headers (optional but helpful for readelf/objdump).
	strtab_builder shstrtab;
	uint32_t shname_interp   = shstrtab.add(".interp");
	uint32_t shname_hash     = shstrtab.add(".hash");
	uint32_t shname_dynsym   = shstrtab.add(".dynsym");
	uint32_t shname_dynstr   = shstrtab.add(".dynstr");
	uint32_t shname_rela     = shstrtab.add(".rela.dyn");
	uint32_t shname_text     = shstrtab.add(".text");
	uint32_t shname_data     = shstrtab.add(".data");
	uint32_t shname_dynamic  = shstrtab.add(".dynamic");
	uint32_t shname_shstrtab = shstrtab.add(".shstrtab");

	size_t shstrtab_offset = out.size();
	size_t shstrtab_size = shstrtab.data.size();
	emit(out, shstrtab.data.data(), shstrtab_size);

	size_t shdr_offset = align_to(out, 8);
	int num_sections = data_section_size > 0 ? 10 : 9;
	size_t total_file_size = out.size() + num_sections * sizeof(Elf64_Shdr);

	// Total loadable size for PT_LOAD.
	size_t load_filesz = total_file_size;

	// --- Fill in ELF header ---

	std::memset(&ehdr, 0, sizeof(ehdr));
	ehdr.e_ident[EI_MAG0] = ELFMAG0;
	ehdr.e_ident[EI_MAG1] = ELFMAG1;
	ehdr.e_ident[EI_MAG2] = ELFMAG2;
	ehdr.e_ident[EI_MAG3] = ELFMAG3;
	ehdr.e_ident[EI_CLASS] = ELFCLASS64;
	ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
	ehdr.e_ident[EI_VERSION] = EV_CURRENT;
	ehdr.e_ident[EI_OSABI] = ELFOSABI_NONE;
	ehdr.e_type = ET_EXEC;
	ehdr.e_machine = EM_X86_64;
	ehdr.e_version = EV_CURRENT;
	ehdr.e_entry = start_vaddr;
	ehdr.e_phoff = phdr_offset;
	ehdr.e_shoff = shdr_offset;
	ehdr.e_ehsize = sizeof(Elf64_Ehdr);
	ehdr.e_phentsize = sizeof(Elf64_Phdr);
	ehdr.e_phnum = NUM_PHDRS;
	ehdr.e_shentsize = sizeof(Elf64_Shdr);
	ehdr.e_shnum = num_sections;
	ehdr.e_shstrndx = num_sections - 1; // .shstrtab is last
	std::memcpy(&out[0], &ehdr, sizeof(ehdr));

	// --- Fill in program headers ---

	// PT_LOAD: map entire file RWX at BASE_ADDR.
	{
		Elf64_Phdr ph;
		std::memset(&ph, 0, sizeof(ph));
		ph.p_type = PT_LOAD;
		ph.p_flags = PF_R | PF_W | PF_X;
		ph.p_offset = 0;
		ph.p_vaddr = BASE_ADDR;
		ph.p_paddr = BASE_ADDR;
		ph.p_filesz = load_filesz;
		ph.p_memsz = load_filesz;
		ph.p_align = 0x200000;
		std::memcpy(&out[phdr_offset], &ph, sizeof(ph));
	}

	// PT_INTERP
	{
		Elf64_Phdr ph;
		std::memset(&ph, 0, sizeof(ph));
		ph.p_type = PT_INTERP;
		ph.p_flags = PF_R;
		ph.p_offset = interp_offset;
		ph.p_vaddr = BASE_ADDR + interp_offset;
		ph.p_paddr = BASE_ADDR + interp_offset;
		ph.p_filesz = interp_size;
		ph.p_memsz = interp_size;
		ph.p_align = 1;
		std::memcpy(&out[phdr_offset + sizeof(Elf64_Phdr)], &ph, sizeof(ph));
	}

	// PT_DYNAMIC
	{
		Elf64_Phdr ph;
		std::memset(&ph, 0, sizeof(ph));
		ph.p_type = PT_DYNAMIC;
		ph.p_flags = PF_R | PF_W;
		ph.p_offset = dynamic_offset;
		ph.p_vaddr = dynamic_vaddr;
		ph.p_paddr = dynamic_vaddr;
		ph.p_filesz = dynamic_size;
		ph.p_memsz = dynamic_size;
		ph.p_align = 8;
		std::memcpy(&out[phdr_offset + 2 * sizeof(Elf64_Phdr)], &ph, sizeof(ph));
	}

	// --- Section headers ---

	const int SEC_NULL     = 0;
	const int SEC_INTERP   = 1;
	const int SEC_HASH     = 2;
	const int SEC_DYNSYM   = 3;
	const int SEC_DYNSTR   = 4;
	const int SEC_RELA     = 5;
	const int SEC_TEXT     = 6;
	const int SEC_DATA     = 7;
	const int SEC_DYNAMIC  = data_section_size > 0 ? 8 : 7;
	const int SEC_SHSTRTAB_IDX = num_sections - 1;
	(void)SEC_NULL; (void)SEC_INTERP; (void)SEC_HASH;
	(void)SEC_RELA; (void)SEC_TEXT;
	(void)SEC_DATA; (void)SEC_DYNAMIC; (void)SEC_SHSTRTAB_IDX;

	auto emit_shdr = [&](uint32_t name, uint32_t type, uint64_t flags,
			     uint64_t addr, uint64_t offset, uint64_t size,
			     uint32_t link, uint32_t info,
			     uint64_t addralign, uint64_t entsize) {
		Elf64_Shdr sh;
		std::memset(&sh, 0, sizeof(sh));
		sh.sh_name = name;
		sh.sh_type = type;
		sh.sh_flags = flags;
		sh.sh_addr = addr;
		sh.sh_offset = offset;
		sh.sh_size = size;
		sh.sh_link = link;
		sh.sh_info = info;
		sh.sh_addralign = addralign;
		sh.sh_entsize = entsize;
		emit(out, &sh, sizeof(sh));
	};

	// [0] NULL
	emit_shdr(0, SHT_NULL, 0, 0, 0, 0, 0, 0, 0, 0);

	// [1] .interp
	emit_shdr(shname_interp, SHT_PROGBITS, SHF_ALLOC,
		  BASE_ADDR + interp_offset, interp_offset, interp_size,
		  0, 0, 1, 0);

	// [2] .hash
	emit_shdr(shname_hash, SHT_HASH, SHF_ALLOC,
		  BASE_ADDR + hash_offset, hash_offset, hash_size,
		  SEC_DYNSYM, 0, 4, 4);

	// [3] .dynsym
	emit_shdr(shname_dynsym, SHT_DYNSYM, SHF_ALLOC,
		  BASE_ADDR + dynsym_offset, dynsym_offset, dynsym_size,
		  SEC_DYNSTR, 1, 8, sizeof(Elf64_Sym));

	// [4] .dynstr
	emit_shdr(shname_dynstr, SHT_STRTAB, SHF_ALLOC,
		  BASE_ADDR + dynstr_offset, dynstr_offset, dynstr_size,
		  0, 0, 1, 0);

	// [5] .rela.dyn
	emit_shdr(shname_rela, SHT_RELA, SHF_ALLOC,
		  BASE_ADDR + rela_dyn_offset, rela_dyn_offset, rela_dyn_size,
		  SEC_DYNSYM, 0, 8, sizeof(Elf64_Rela));

	// [6] .text
	emit_shdr(shname_text, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
		  text_vaddr, text_file_offset, total_text_size,
		  0, 0, 16, 0);

	// [7] .data (if present)
	if ( data_section_size > 0 )
	{
		emit_shdr(shname_data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,
			  data_vaddr, data_file_offset, data_section_size,
			  0, 0, 16, 0);
	}

	// .dynamic
	emit_shdr(shname_dynamic, SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE,
		  dynamic_vaddr, dynamic_offset, dynamic_size,
		  SEC_DYNSTR, 0, 8, sizeof(Elf64_Dyn));

	// .shstrtab
	emit_shdr(shname_shstrtab, SHT_STRTAB, 0,
		  0, shstrtab_offset, shstrtab_size,
		  0, 0, 1, 0);

	// --- Write to file ---

	FILE *fp = std::fopen(path.c_str(), "wb");
	if ( !fp )
		return false;
	size_t written = std::fwrite(out.data(), 1, out.size(), fp);
	std::fclose(fp);

	// Make executable.
	chmod(path.c_str(), 0755);

	DBG(std::cout << "save_executable: wrote " << written << " bytes to "
		      << path << " (entry=" << std::hex << start_vaddr
		      << ", main=" << code_vaddr + main_offset
		      << ", " << extern_syms.size() << " external symbols)"
		      << std::dec << std::endl);

	return written == out.size();
}
