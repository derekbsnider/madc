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

} // namespace

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
