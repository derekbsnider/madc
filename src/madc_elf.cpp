// ELF x86-64 relocatable object writer for compiled madc programs.
// Reads from a finalized asmjit::CodeHolder and writes a .o file.

#include <algorithm>
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

std::string basename_of(const std::string &path)
{
	size_t slash = path.rfind('/');
	if ( slash == std::string::npos )
		return path;
	return path.substr(slash + 1);
}

bool is_madc_runtime_symbol(const std::string &sym_name)
{
	return sym_name.find("__madc_") != std::string::npos
	    || sym_name.find("print") != std::string::npos
	    || sym_name.find("string_") != std::string::npos
	    || sym_name.find("_Z") == 0;
}

std::string normalize_elf_symbol_name(const std::string &sym_name)
{
	if ( sym_name.find("__vdso_") == 0 )
		return sym_name.substr(7);
	return sym_name;
}

std::string canonical_library_for_symbol(const std::string &sym_name,
					 const std::string &raw_libname)
{
	std::string normalized_name = normalize_elf_symbol_name(sym_name);
	std::string libname = basename_of(raw_libname);

	if ( normalized_name == "__libc_start_main" )
		return "libc.so.6";
	if ( normalized_name == "crypt" || normalized_name == "crypt_r" )
		return "libcrypt.so.1";
	if ( is_madc_runtime_symbol(normalized_name) )
		return "libmadc.so";
	if ( sym_name.find("__vdso_") == 0 )
		return "libc.so.6";

	if ( libname.find("libmadc") == 0 )
		return "libmadc.so";
	if ( libname.find("libcrypt") == 0 )
		return "libcrypt.so.1";
	if ( libname.find("libc") == 0 )
		return "libc.so.6";
	if ( libname.find("libm") == 0 )
		return "libm.so.6";
	if ( libname.find("libdl") == 0 )
		return "libdl.so.2";
	if ( libname.find("libpthread") == 0 )
		return "libpthread.so.0";
	if ( libname.find("vdso") != std::string::npos )
		return "";
	if ( libname.find(".so") == std::string::npos && libname.find("lib") != 0 )
		return "libc.so.6";

	return libname;
}

const char *const *candidate_versions_for_library(const std::string &libname)
{
	static const char *glibc_versions[] = {
		"GLIBC_2.38", "GLIBC_2.37", "GLIBC_2.36", "GLIBC_2.35",
		"GLIBC_2.34", "GLIBC_2.33", "GLIBC_2.32", "GLIBC_2.31",
		"GLIBC_2.30", "GLIBC_2.29", "GLIBC_2.28", "GLIBC_2.27",
		"GLIBC_2.17", "GLIBC_2.14", "GLIBC_2.4", "GLIBC_2.3.4",
		"GLIBC_2.3", "GLIBC_2.2.5", NULL
	};
	static const char *xcrypt_versions[] = {
		"XCRYPT_4.4", "XCRYPT_4.3", "XCRYPT_4.2", "XCRYPT_4.1",
		"XCRYPT_2.0", NULL
	};

	if ( libname == "libcrypt.so.1" )
		return xcrypt_versions;
	if ( libname == "libc.so.6"
	  || libname == "libm.so.6"
	  || libname == "libdl.so.2"
	  || libname == "libpthread.so.0" )
		return glibc_versions;

	return NULL;
}

void *open_version_probe_handle(const std::string &libname, bool &opened_here)
{
	opened_here = false;
	void *handle = dlopen(libname.c_str(), RTLD_LAZY | RTLD_NOLOAD);
	if ( handle )
		return handle;
	handle = dlopen(libname.c_str(), RTLD_LAZY | RTLD_LOCAL);
	if ( handle )
		opened_here = true;
	return handle;
}

bool is_emit_data_mov_site(const std::vector<uint8_t> &buf, size_t label_off, uint8_t imm_offset)
{
	if ( label_off + 2 > buf.size() )
		return false;
	if ( imm_offset != 2 )
		return false;
	uint8_t rex = buf[label_off];
	uint8_t op = buf[label_off + 1];
	return (rex == 0x48 || rex == 0x49) && op >= 0xB8 && op <= 0xBF;
}

// _start stub matching gcc's CRT: calls __libc_start_main(main, argc, argv, ...)
// which initializes libc (stdio, malloc, atexit) then calls main, then exit.
// 31 bytes. Patch points:
//   offset 0x15: 4-byte main address (imm32 in mov edi)
//   offset 0x19: e8 opcode — rel32 at offset 0x1a for __libc_start_main
static const uint8_t start_stub[] = {
	0x31, 0xed,                               // xor %ebp, %ebp
	0x49, 0x89, 0xd1,                         // mov %rdx, %r9 (rtld_fini)
	0x5e,                                     // pop %rsi (argc)
	0x48, 0x89, 0xe2,                         // mov %rsp, %rdx (argv)
	0x48, 0x83, 0xe4, 0xf0,                   // and $-16, %rsp (align)
	0x50,                                     // push %rax (padding)
	0x54,                                     // push %rsp (stack_end)
	0x45, 0x31, 0xc0,                         // xor %r8d, %r8d (fini=0)
	0x31, 0xc9,                               // xor %ecx, %ecx (init=0)
	0xbf, 0x00, 0x00, 0x00, 0x00,             // mov $main, %edi (patched)
	0xe8, 0x00, 0x00, 0x00, 0x00,             // call __libc_start_main (patched)
	0xf4                                      // hlt
};
static const size_t START_STUB_SIZE = sizeof(start_stub);
static const size_t START_STUB_MAIN_IMM_OFFSET = 0x15;  // imm32 in mov edi
static const size_t START_STUB_CALL_REL_OFFSET = 0x1a;  // rel32 after e8

} // namespace

size_t Program::aot_variable_storage_size(const Variable *var) const
{
	if ( !var || !var->type )
		return 0;
	size_t sz = var->type->size;
	if ( var->is_fixed_array() )
		sz *= var->total_elements();
	return sz;
}

void Program::record_aot_variable_data(Variable *var)
{
	if ( !var || !var->data || !var->type )
		return;

	size_t sz = aot_variable_storage_size(var);
	if ( sz == 0 )
		return;

	record_aot_data("__aot_var_" + var->name,
	    var->data, sz, var->type, var->is_fixed_array() ? var->total_elements() : 1);

	if ( var->type->is_string() && sz == sizeof(std::string) )
	{
		std::string *str = static_cast<std::string *>(var->data);
		const char *cstr = str->c_str();
		uintptr_t obj_start = reinterpret_cast<uintptr_t>(var->data);
		uintptr_t cstr_addr = reinterpret_cast<uintptr_t>(cstr);
		bool is_internal = (cstr_addr >= obj_start
				 && cstr_addr < obj_start + sizeof(std::string));
		if ( cstr && !is_internal && !aot_discovered_data_index.count(cstr_addr) )
		{
			AotDiscoveredData centry;
			centry.name = "__aot_cstr_" + var->name;
			centry.address = (void *)cstr;
			centry.size = str->size() + 1;
			centry.type = NULL;
			centry.count = 0;
			aot_discovered_data_index[cstr_addr] = aot_discovered_data.size();
			aot_discovered_data.push_back(centry);
		}
	}
}

void Program::record_aot_data(const std::string &name, void *address, size_t size,
	DataDef *type, uint32_t count)
{
	if ( !address || size == 0 )
		return;

	uintptr_t addr = reinterpret_cast<uintptr_t>(address);
	if ( aot_discovered_data_index.count(addr) )
		return;

	AotDiscoveredData entry;
	entry.name = name;
	entry.address = address;
	entry.size = size;
	entry.type = type;
	entry.count = count;
	aot_discovered_data_index[addr] = aot_discovered_data.size();
	aot_discovered_data.push_back(entry);
}

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
		if ( !var || !var->type )
			continue;
		if ( !var->is_global() )
			continue;
		Variable *storage = resolve_global_storage_variable(var);
		if ( storage && storage != var && !var->data )
			var->data = storage->data;
		if ( !var->data )
			continue;
		// Skip real function variables (their data is a Method*), but keep
		// function-pointer variables — DataDefFPTR is btFunct too, yet it owns
		// a real 8-byte global data slot that AOT needs to copy and patch.
		if ( var->type->basetype() == BaseType::btFunct
		  && dynamic_cast<DataDefFPTR *>(var->type) == NULL )
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

		// For string globals, also collect the c_str() buffer.
		if ( var->type->is_string() && sz == sizeof(std::string) )
		{
			std::string *str = static_cast<std::string *>(var->data);
			const char *cstr = str->c_str();
			// Only add c_str if it's NOT inside the parent object
			// (SSO strings store characters inline — adding them
			// would create overlapping data_offset_map entries).
			uintptr_t obj_start = reinterpret_cast<uintptr_t>(var->data);
			uintptr_t cstr_addr = reinterpret_cast<uintptr_t>(cstr);
			bool is_internal = (cstr_addr >= obj_start
					 && cstr_addr < obj_start + sizeof(std::string));
			if ( cstr && !is_internal && !seen.count((void *)cstr) )
			{
				seen.insert((void *)cstr);
				GlobalDataEntry ce;
				ce.name = "__cstr_" + var->name;
				ce.address = (void *)cstr;
				ce.size = str->size() + 1;
				entries.push_back(ce);
			}
		}
	}

	// Also collect static variables from function scopes.
	// Walk pending_funcs to find TokenFunc compounds with variables.
	for ( size_t fi = 0; fi < pending_funcs.size(); ++fi )
	{
		TokenFunc *tf = dynamic_cast<TokenFunc *>(pending_funcs[fi]);
		if ( !tf )
			continue;
		for ( size_t vi = 0; vi < tf->variables.size(); ++vi )
		{
			Variable *var = tf->variables[vi];
			if ( !var || !var->data || !var->type )
				continue;
			if ( (var->flags & vfPARAM) )
				continue;
			if ( (var->flags & vfSTACK) && !(var->flags & vfSTATIC) )
				continue;
			if ( var->type->basetype() == BaseType::btFunct
			  && dynamic_cast<DataDefFPTR *>(var->type) == NULL )
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
			e.name = "__static_" + var->name;
			e.address = var->data;
			e.size = sz;
			entries.push_back(e);

			if ( var->type->is_string() && sz == sizeof(std::string) )
			{
				std::string *str = static_cast<std::string *>(var->data);
				const char *cstr = str->c_str();
				uintptr_t so = reinterpret_cast<uintptr_t>(var->data);
				uintptr_t ca = reinterpret_cast<uintptr_t>(cstr);
				bool internal = (ca >= so && ca < so + sizeof(std::string));
				if ( cstr && !internal && !seen.count((void *)cstr) )
				{
					seen.insert((void *)cstr);
					GlobalDataEntry ce;
					ce.name = "__cstr_static_" + var->name;
					ce.address = (void *)cstr;
					ce.size = str->size() + 1;
					entries.push_back(ce);
				}
			}
		}
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
		uintptr_t lo = reinterpret_cast<uintptr_t>(var->data);
		uintptr_t lca = reinterpret_cast<uintptr_t>(cstr);
		bool lit_internal = (lca >= lo && lca < lo + sizeof(std::string));
		if ( cstr && !lit_internal && !seen.count((void *)cstr) )
		{
			seen.insert((void *)cstr);
			GlobalDataEntry e;
			e.name = "__cstr_" + it->first;
			e.address = (void *)cstr;
			e.size = str->size() + 1; // include NUL terminator
			entries.push_back(e);
		}
	}

	// Collect AOT string constants (raw character buffers allocated
	// during compilation when aot_tracking is on).
	for ( size_t i = 0; i < aot_string_constants.size(); ++i )
	{
		char *buf = aot_string_constants[i];
		if ( !buf || seen.count(buf) )
			continue;
		seen.insert(buf);
		size_t len = std::strlen(buf) + 1;
		GlobalDataEntry e;
		e.name = "__aot_str_" + std::to_string(i);
		e.address = buf;
		e.size = len;
		entries.push_back(e);
	}

	for ( size_t i = 0; i < aot_discovered_data.size(); ++i )
	{
		const AotDiscoveredData &entry = aot_discovered_data[i];
		if ( !entry.address || entry.size == 0 || seen.count(entry.address) )
			continue;
		seen.insert(entry.address);
		GlobalDataEntry e;
		e.name = entry.name;
		e.address = entry.address;
		e.size = entry.size;
		entries.push_back(e);
	}

	return entries;
}

void Program::prepare_aot_data_layout()
{
	std::vector<GlobalDataEntry> entries = collect_global_data();
	std::map<void *, size_t> offsets_by_addr;
	size_t offset = 0;
	aot_layout_offsets.clear();
	aot_layout_ranges.clear();
	const char *debug_var = ::getenv("MADC_DEBUG_AOT_VAR");

	for ( size_t i = 0; i < entries.size(); ++i )
	{
		size_t pad = (8 - (offset % 8)) % 8;
		offset += pad;
		offsets_by_addr[entries[i].address] = offset;
		aot_layout_offsets[reinterpret_cast<uintptr_t>(entries[i].address)] = offset;
		AotDataRange range;
		range.start = reinterpret_cast<uintptr_t>(entries[i].address);
		range.size = entries[i].size;
		range.data_offset = offset;
		aot_layout_ranges.push_back(range);
		offset += entries[i].size;
	}

	std::sort(aot_layout_ranges.begin(), aot_layout_ranges.end(),
		  [](const AotDataRange &a, const AotDataRange &b) { return a.start < b.start; });

	if ( !tkProgram )
		return;

	for ( size_t i = 0; i < tkProgram->variables.size(); ++i )
	{
		Variable *var = tkProgram->variables[i];
		if ( !var || !var->data || !var->type )
			continue;
		var->aot_data_offset = (size_t)-1;
		var->aot_cstr_offset = (size_t)-1;
		std::map<void *, size_t>::const_iterator it = offsets_by_addr.find(var->data);
		if ( it != offsets_by_addr.end() )
			var->aot_data_offset = it->second;
		if ( var->type->is_string() && var->aot_data_offset != (size_t)-1 )
		{
			std::string *str = static_cast<std::string *>(var->data);
			void *cstr = (void *)str->c_str();
			std::map<void *, size_t>::const_iterator cit = offsets_by_addr.find(cstr);
			if ( cit != offsets_by_addr.end() )
				var->aot_cstr_offset = cit->second;
		}
		if ( debug_var && var->name == debug_var )
		{
			std::fprintf(stderr,
			    "[aot] global-layout var=%s ptr=%p flags=%u count=%u fixed=%d off=%zu dims=%zu\n",
			    var->name.c_str(), var->data, (unsigned)var->flags, (unsigned)var->count,
			    var->is_fixed_array() ? 1 : 0, var->aot_data_offset, var->dims.size());
		}
	}

	for ( size_t fi = 0; fi < pending_funcs.size(); ++fi )
	{
		TokenFunc *tf = dynamic_cast<TokenFunc *>(pending_funcs[fi]);
		if ( !tf )
			continue;
		for ( size_t vi = 0; vi < tf->variables.size(); ++vi )
		{
			Variable *var = tf->variables[vi];
			if ( !var || !var->data || !var->type )
				continue;
			if ( (var->flags & vfPARAM) )
				continue;
			if ( (var->flags & vfSTACK) && !(var->flags & vfSTATIC) )
				continue;
			var->aot_data_offset = (size_t)-1;
			var->aot_cstr_offset = (size_t)-1;
			std::map<void *, size_t>::const_iterator it = offsets_by_addr.find(var->data);
			if ( it != offsets_by_addr.end() )
				var->aot_data_offset = it->second;
			if ( var->type->is_string() && var->aot_data_offset != (size_t)-1 )
			{
				std::string *str = static_cast<std::string *>(var->data);
				void *cstr = (void *)str->c_str();
				std::map<void *, size_t>::const_iterator cit = offsets_by_addr.find(cstr);
				if ( cit != offsets_by_addr.end() )
					var->aot_cstr_offset = cit->second;
			}
			if ( debug_var && var->name == debug_var )
			{
				std::fprintf(stderr,
				    "[aot] func-layout var=%s ptr=%p flags=%u count=%u fixed=%d off=%zu dims=%zu fn=%s\n",
				    var->name.c_str(), var->data, (unsigned)var->flags, (unsigned)var->count,
				    var->is_fixed_array() ? 1 : 0, var->aot_data_offset, var->dims.size(),
				    tf->var.name.c_str());
			}
		}
	}

	std::map<std::string, std::pair<size_t, size_t> > canonical_offsets;
	for ( size_t i = 0; i < tkProgram->variables.size(); ++i )
	{
		Variable *var = tkProgram->variables[i];
		if ( !var || !var->type || !var->is_global() )
			continue;
		if ( !var->has_aot_data() )
			continue;
		canonical_offsets[var->name] =
		    std::make_pair(var->aot_data_offset, var->aot_cstr_offset);
	}

	auto propagate_offsets = [&](Variable *var) {
		if ( !var || !var->type || !var->is_global() || var->has_aot_data() )
			return;
		Variable *storage = resolve_global_storage_variable(var);
		if ( storage && storage != var && storage->has_aot_data() )
		{
			var->aot_data_offset = storage->aot_data_offset;
			var->aot_cstr_offset = storage->aot_cstr_offset;
			return;
		}
		std::map<std::string, std::pair<size_t, size_t> >::const_iterator it =
		    canonical_offsets.find(var->name);
		if ( it == canonical_offsets.end() )
			return;
		var->aot_data_offset = it->second.first;
		var->aot_cstr_offset = it->second.second;
	};

	for ( size_t i = 0; i < tkProgram->variables.size(); ++i )
		propagate_offsets(tkProgram->variables[i]);

	for ( size_t fi = 0; fi < pending_funcs.size(); ++fi )
	{
		TokenFunc *tf = dynamic_cast<TokenFunc *>(pending_funcs[fi]);
		if ( !tf )
			continue;
		for ( size_t vi = 0; vi < tf->variables.size(); ++vi )
			propagate_offsets(tf->variables[vi]);
	}
}

bool Program::lookup_aot_data_offset(uintptr_t address, size_t &out_offset) const
{
	std::map<uintptr_t, size_t>::const_iterator it = aot_layout_offsets.find(address);
	if ( it != aot_layout_offsets.end() )
	{
		out_offset = it->second;
		return true;
	}

	if ( aot_layout_ranges.empty() )
		return false;

	AotDataRange key;
	key.start = address;
	key.size = 0;
	key.data_offset = 0;
	std::vector<AotDataRange>::const_iterator rit =
	    std::upper_bound(aot_layout_ranges.begin(), aot_layout_ranges.end(), key,
		[](const AotDataRange &a, const AotDataRange &b) { return a.start < b.start; });
	if ( rit != aot_layout_ranges.begin() )
	{
		--rit;
		if ( address >= rit->start && address < rit->start + rit->size )
		{
			out_offset = rit->data_offset + static_cast<size_t>(address - rit->start);
			return true;
		}
	}

	return false;
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
	struct string_object_patch {
		size_t object_offset;
		size_t cstr_offset;
	};
	std::vector<string_object_patch> string_object_patches;
	struct stream_object_patch {
		size_t object_offset;
		std::string helper_name;
	};
	std::vector<stream_object_patch> stream_object_patches;

	// Also build range lookup: sorted by address for sub-offset matching.
	struct data_range { uintptr_t start; size_t size; size_t data_off; };
	std::vector<data_range> data_ranges;

	for ( size_t i = 0; i < globals.size(); ++i )
	{
		// Align each entry to 8 bytes.
		size_t pad = (8 - (data_section_buf.size() % 8)) % 8;
		data_section_buf.insert(data_section_buf.end(), pad, 0);

		size_t offset = data_section_buf.size();
		uintptr_t addr = reinterpret_cast<uintptr_t>(globals[i].address);
		data_offset_map[addr] = offset;

		data_range dr;
		dr.start = addr;
		dr.size = globals[i].size;
		dr.data_off = offset;
		data_ranges.push_back(dr);

		const uint8_t *src = static_cast<const uint8_t *>(globals[i].address);
		data_section_buf.insert(data_section_buf.end(), src, src + globals[i].size);

		if ( ::getenv("MADC_DEBUG_AOT_LAYOUT_NAMES") )
		{
			std::fprintf(stderr,
			    "[aot-layout] global[%zu] name=%s addr=%p off=%zu size=%zu\n",
			    i, globals[i].name.c_str(), globals[i].address, offset, globals[i].size);
		}
	}

	// Sort ranges by start address for binary search.
	std::sort(data_ranges.begin(), data_ranges.end(),
		  [](const data_range &a, const data_range &b) { return a.start < b.start; });

	// Helper: find the data offset for an address that may be WITHIN
	// a collected range (not just at the start).
	auto find_data_offset = [&](uintptr_t addr) -> std::pair<bool, size_t> {
		// First try exact match.
		std::map<uintptr_t, size_t>::const_iterator it = data_offset_map.find(addr);
		if ( it != data_offset_map.end() )
			return std::make_pair(true, it->second);
		// Range search: find the largest range.start <= addr.
		if ( data_ranges.empty() )
			return std::make_pair(false, (size_t)0);
		data_range key;
		key.start = addr;
		key.size = 0;
		key.data_off = 0;
		auto rit = std::upper_bound(data_ranges.begin(), data_ranges.end(), key,
			[](const data_range &a, const data_range &b) { return a.start < b.start; });
		if ( rit != data_ranges.begin() )
		{
			--rit;
			if ( addr >= rit->start && addr < rit->start + rit->size )
			{
				size_t inner_offset = static_cast<size_t>(addr - rit->start);
				return std::make_pair(true, rit->data_off + inner_offset);
			}
		}
		return std::make_pair(false, (size_t)0);
	};

	auto queue_string_patch = [&](std::string *str) {
		if ( !str )
			return;
		uintptr_t obj_addr = reinterpret_cast<uintptr_t>(str);
		std::map<uintptr_t, size_t>::const_iterator oit =
		    data_offset_map.find(obj_addr);
		if ( oit == data_offset_map.end() )
			return;

		const char *cstr = str->c_str();
		if ( !cstr )
			return;

		// Reconstruct std::string globals from a stable copied C-string
		// source, not from bytes inside the copied string object itself.
		// Some libstdc++ layouts use SSO and return c_str() pointers that
		// alias the object storage; placement-new construction from that
		// overlapping source is not reliable in native executables.
		size_t cstr_offset = data_section_buf.size();
		size_t cstr_size = std::strlen(cstr) + 1;
		data_section_buf.insert(data_section_buf.end(), cstr, cstr + cstr_size);

		// Global initializers can also bake direct c_str() pointers like
		// `&("X"[0])` into copied .data. Teach the generic pointer-slot
		// patcher that those host addresses map to the stable copied
		// character buffer we just appended.
		data_offset_map[reinterpret_cast<uintptr_t>(cstr)] = cstr_offset;
		data_range cstr_range;
		cstr_range.start = reinterpret_cast<uintptr_t>(cstr);
		cstr_range.size = cstr_size;
		cstr_range.data_off = cstr_offset;
		data_ranges.push_back(cstr_range);
		std::sort(data_ranges.begin(), data_ranges.end(),
			  [](const data_range &a, const data_range &b) { return a.start < b.start; });

		string_object_patch sop;
		sop.object_offset = oit->second;
		sop.cstr_offset = cstr_offset;
		string_object_patches.push_back(sop);
	};

	if ( tkProgram )
	{
		for ( size_t i = 0; i < tkProgram->variables.size(); ++i )
		{
			Variable *var = tkProgram->variables[i];
			if ( !var || !var->data || !var->type || !var->is_global() )
				continue;
			if ( var->type->is_string() && var->type->size == sizeof(std::string) )
				queue_string_patch(static_cast<std::string *>(var->data));
			if ( var->name == "cout" && var->type->type() == DataType::dtOSTREAM
			  && var->has_aot_data() )
			{
				stream_object_patch sop;
				sop.object_offset = var->aot_data_offset;
				sop.helper_name = "__madc_aot_init_cout";
				stream_object_patches.push_back(sop);
			}
			else if ( var->name == "cerr" && var->type->type() == DataType::dtOSTREAM
			       && var->has_aot_data() )
			{
				stream_object_patch sop;
				sop.object_offset = var->aot_data_offset;
				sop.helper_name = "__madc_aot_init_cerr";
				stream_object_patches.push_back(sop);
			}
			else if ( var->name == "cin" && var->type->type() == DataType::dtISTREAM
			       && var->has_aot_data() )
			{
				stream_object_patch sop;
				sop.object_offset = var->aot_data_offset;
				sop.helper_name = "__madc_aot_init_cin";
				stream_object_patches.push_back(sop);
			}
		}
	}

	for ( size_t fi = 0; fi < pending_funcs.size(); ++fi )
	{
		TokenFunc *tf = dynamic_cast<TokenFunc *>(pending_funcs[fi]);
		if ( !tf )
			continue;
		for ( size_t vi = 0; vi < tf->variables.size(); ++vi )
		{
			Variable *var = tf->variables[vi];
			if ( !var || !var->data || !var->type )
				continue;
			if ( (var->flags & vfPARAM) )
				continue;
			if ( (var->flags & vfSTACK) && !(var->flags & vfSTATIC) )
				continue;
			if ( var->type->is_string() && var->type->size == sizeof(std::string) )
				queue_string_patch(static_cast<std::string *>(var->data));
		}
	}

	for ( variable_map_t::const_iterator it = literal_map.begin();
	      it != literal_map.end(); ++it )
	{
		Variable *var = it->second;
		if ( !var || !var->data )
			continue;
		queue_string_patch(static_cast<std::string *>(var->data));
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
	bool main_takes_args = false;
	uint32_t libc_start_main_dynsym = 0;
	uint32_t aot_init_string_dynsym = 0;
	std::map<std::string, uint32_t> aot_stream_init_dynsyms;
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
		main_takes_args = func->parameters.size() >= 2;
		uint32_t label_id = func->funcnode->label().id();
		if ( code.isLabelBound(label_id) )
			main_offset = static_cast<int64_t>(code.labelOffset(label_id));
		break;
	}
	if ( main_offset < 0 )
		return false;

	// --- Collect external symbols from relocations ---

	struct ext_sym {
		std::string name;
		std::string needed_lib;
	};
	std::vector<ext_sym> extern_syms;
	std::map<uintptr_t, uint32_t> extern_indices; // addr -> dynsym index
	auto ensure_external_dynsym = [&](uintptr_t addr, const std::string &name) -> uint32_t {
		std::map<uintptr_t, uint32_t>::const_iterator eit =
		    extern_indices.find(addr);
		if ( eit != extern_indices.end() )
			return eit->second;

		uint32_t dynsym_idx = static_cast<uint32_t>(extern_syms.size() + 1);
		ext_sym es;
		es.name = normalize_elf_symbol_name(name);
		extern_syms.push_back(es);
		extern_indices[addr] = dynsym_idx;
		return dynsym_idx;
	};

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

			// kX64AddressEntry: code-side reference is handled by
			// relocateToBase. GOT slot patching is via addrtab_data_relas.
			// Still collect the symbol but don't emit a code-side rela.
			bool skip_code_rela = (re->relocType() == RelocType::kX64AddressEntry);

			uintptr_t payload = static_cast<uintptr_t>(re->payload());
			std::map<uintptr_t, std::string>::const_iterator it =
			    external_symbol_map.find(payload);

			if ( it != external_symbol_map.end() )
			{
				uint32_t dynsym_idx =
				    ensure_external_dynsym(payload, it->second);
				if ( !skip_code_rela )
				{
					rela_entry re_entry;
					re_entry.source_offset = re->sourceOffset();
					re_entry.dynsym_index = dynsym_idx;
					re_entry.elf_type = elf_type;
					re_entry.is_extern = true;
					re_entry.raw_addend = 0;
					rela_entries.push_back(re_entry);
				}
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

	// --- Add relocations for tracked movabs external address loads ---
	// emit_data_mov() marks these sites because asmjit won't surface a
	// reloc entry for mov reg, imm64. In AOT they must become
	// R_X86_64_64 references to the external symbol, not frozen host
	// addresses from the JIT process.
	for ( size_t i = 0; i < aot_data_refs.size(); ++i )
	{
		const AotDataRef &ref = aot_data_refs[i];
		if ( !code.isLabelBound(ref.label_id) )
			continue;
		std::map<uintptr_t, std::string>::const_iterator it =
		    external_symbol_map.find(ref.address);
		if ( it == external_symbol_map.end() )
			continue;

		rela_entry re_entry;
		re_entry.source_offset =
		    static_cast<uint64_t>(code.labelOffset(ref.label_id)) + ref.imm_offset;
		re_entry.dynsym_index = ensure_external_dynsym(ref.address, it->second);
		re_entry.elf_type = R_X86_64_64;
		re_entry.is_extern = true;
		re_entry.raw_addend = 0;
		rela_entries.push_back(re_entry);
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
		{
			// Try to resolve via dladdr as a fallback.
			Dl_info info;
			if ( dladdr(reinterpret_cast<void *>(func_addr), &info)
			  && info.dli_sname && info.dli_sname[0] )
			{
				external_symbol_map[func_addr] =
				    normalize_elf_symbol_name(info.dli_sname);
				it = external_symbol_map.find(func_addr);
				DBG(std::fprintf(stderr, "[aot] addrtab dladdr 0x%zx -> %s\n",
				    (size_t)func_addr, info.dli_sname));
			}
			else
				continue;
		}

		uint32_t dynsym_idx = ensure_external_dynsym(func_addr, it->second);

		data_rela dr;
		dr.data_offset = addrtab_relas[i].data_offset;
		dr.dynsym_index = dynsym_idx;
		addrtab_data_relas.push_back(dr);
	}

	// --- Add R_X86_64_COPY relocations for extern libc data symbols ---
	// stderr, stdout, stdin are extern data symbols that the dynamic
	// linker must copy from libc into our .data at load time.
	struct copy_rela { size_t data_offset; uint32_t dynsym_index; size_t sym_size; };
	std::vector<copy_rela> copy_relas;
	{
		static const char *extern_data_syms[] = {
			"stderr", "stdout", "stdin", NULL
		};
		if ( tkProgram )
		{
			for ( const char **sp = extern_data_syms; *sp; ++sp )
			{
				std::string sym_name(*sp);
				Variable *v = tkProgram->findVariable(sym_name);
				if ( !v || !v->has_aot_data() )
					continue;

				uint32_t dsym = static_cast<uint32_t>(extern_syms.size() + 1);
				ext_sym es;
				es.name = *sp;
				extern_syms.push_back(es);

				copy_rela cr;
				cr.data_offset = v->aot_data_offset;
				cr.dynsym_index = dsym;
				cr.sym_size = v->type ? v->type->size : 8;
				copy_relas.push_back(cr);
			}
		}
	}

	// Seed the exported text image from the finalized JIT buffer early so
	// direct external calls discovered here become part of dynsym/dynstr
	// before those tables are emitted.
	code.flatten();
	size_t flat_size = code.codeSize();
	std::vector<uint8_t> text_copy(flat_size, 0);
	std::memcpy(text_copy.data(), reinterpret_cast<const void *>(root_fn), flat_size);
	size_t total_code_size = flat_size;

	// Discover direct call rel32 sites that target external functions.
	// These calls don't always come with asmjit relocation entries, so
	// they must be surfaced before dynsym construction or later PLT GOT
	// relocs will refer to missing symbol indices.
	struct plt_entry { uintptr_t target_addr; uint32_t dynsym_index; size_t got_offset; };
	std::vector<plt_entry> plt_entries;
	std::map<uintptr_t, size_t> plt_index_map; // target → index into plt_entries
	struct call_patch { size_t code_offset; size_t plt_idx; };
	std::vector<call_patch> call_patches;
	{
		for ( size_t i = 0; i + 5 <= total_code_size; ++i )
		{
			size_t call_off = i;

			if ( text_copy[i] == 0xE8 )
				call_off = i;
			else if ( text_copy[i] >= 0x40 && text_copy[i] <= 0x4F
				&& i + 6 <= total_code_size && text_copy[i+1] == 0xE8 )
			{
				call_off = i + 1;
			}
			else
				continue;

			int32_t disp;
			std::memcpy(&disp, &text_copy[call_off + 1], 4);
			uintptr_t jit_base = reinterpret_cast<uintptr_t>(root_fn);
			int64_t rip = static_cast<int64_t>(jit_base + call_off + 5);
			int64_t target = rip + disp;

			int64_t jit_start = static_cast<int64_t>(jit_base);
			int64_t jit_end = jit_start + static_cast<int64_t>(total_code_size);
			if ( target >= jit_start && target < jit_end )
				continue;

			uintptr_t taddr = static_cast<uintptr_t>(target);
			std::map<uintptr_t, std::string>::const_iterator eit =
			    external_symbol_map.find(taddr);
			if ( eit == external_symbol_map.end() )
			{
				Dl_info info;
				if ( dladdr(reinterpret_cast<void *>(taddr), &info)
				  && info.dli_sname && info.dli_sname[0] )
				{
					external_symbol_map[taddr] =
					    normalize_elf_symbol_name(info.dli_sname);
					eit = external_symbol_map.find(taddr);
				}
				else
					continue;
			}

			size_t plt_idx;
			std::map<uintptr_t, size_t>::const_iterator pit =
			    plt_index_map.find(taddr);
			if ( pit != plt_index_map.end() )
			{
				plt_idx = pit->second;
			}
			else
			{
				uint32_t dsym =
				    ensure_external_dynsym(taddr, eit->second);

				plt_idx = plt_entries.size();
				plt_entry pe;
				pe.target_addr = taddr;
				pe.dynsym_index = dsym;
				pe.got_offset = 0;
				plt_entries.push_back(pe);
				plt_index_map[taddr] = plt_idx;
			}

			call_patch cp;
			cp.code_offset = call_off;
			cp.plt_idx = plt_idx;
			call_patches.push_back(cp);
		}
	}

	// --- Discover required shared libraries via dladdr ---

	struct needed_lib { std::string name; uint32_t dynstr_offset; };
	std::vector<needed_lib> needed_libs;
	std::set<std::string> needed_lib_set;

	for ( size_t i = 0; i < extern_syms.size(); ++i )
	{
		std::string raw_libname;
		for ( std::map<uintptr_t, uint32_t>::const_iterator eit = extern_indices.begin();
		      eit != extern_indices.end(); ++eit )
		{
			if ( eit->second != i + 1 )
				continue;
			Dl_info info;
			if ( dladdr(reinterpret_cast<void *>(eit->first), &info) && info.dli_fname )
			{
				raw_libname = info.dli_fname;
			}
			break;
		}

		extern_syms[i].needed_lib =
		    canonical_library_for_symbol(extern_syms[i].name, raw_libname);
		if ( extern_syms[i].needed_lib.empty() )
			continue;
		if ( needed_lib_set.find(extern_syms[i].needed_lib) == needed_lib_set.end() )
		{
			needed_lib_set.insert(extern_syms[i].needed_lib);
			needed_lib nl;
			nl.name = extern_syms[i].needed_lib;
			needed_libs.push_back(nl);
		}
	}

	// Always need libc (for __libc_start_main at minimum).
	if ( needed_lib_set.find("libc.so.6") == needed_lib_set.end() )
	{
		needed_lib nl;
		nl.name = "libc.so.6";
		needed_libs.push_back(nl);
	}

	// Always add __libc_start_main (needed by the _start stub).
	{
		ext_sym es;
		es.name = "__libc_start_main";
		es.needed_lib = "libc.so.6";
		extern_syms.push_back(es);
		libc_start_main_dynsym = static_cast<uint32_t>(extern_syms.size());
		// dynsym index = extern_syms.size() because dynsym[0] is NULL
	}

	if ( !string_object_patches.empty() || !stream_object_patches.empty() )
	{
		if ( needed_lib_set.find("libmadc.so") == needed_lib_set.end() )
		{
			needed_lib_set.insert("libmadc.so");
			needed_lib nl;
			nl.name = "libmadc.so";
			needed_libs.push_back(nl);
		}
		if ( !string_object_patches.empty() )
		{
			ext_sym es;
			es.name = "__madc_aot_init_string";
			es.needed_lib = "libmadc.so";
			extern_syms.push_back(es);
			aot_init_string_dynsym = static_cast<uint32_t>(extern_syms.size());
		}
		for ( size_t i = 0; i < stream_object_patches.size(); ++i )
		{
			if ( aot_stream_init_dynsyms.count(stream_object_patches[i].helper_name) )
				continue;
			ext_sym es;
			es.name = stream_object_patches[i].helper_name;
			es.needed_lib = "libmadc.so";
			extern_syms.push_back(es);
			aot_stream_init_dynsyms[stream_object_patches[i].helper_name] =
			    static_cast<uint32_t>(extern_syms.size());
		}
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

	// --- Build .gnu.version and .gnu.version_r ---
	// Detect the glibc version each symbol needs by trying dlvsym
	// with common version strings. Build the version tables dynamically.

	size_t dynsym_count = 1 + extern_syms.size();

	struct vernaux_entry {
		std::string name;
		uint32_t hash;
		uint32_t dynstr_off;
		uint16_t index;
	};
	struct verneed_lib {
		std::string name;
		uint32_t dynstr_off;
		std::vector<vernaux_entry> versions;
		std::map<std::string, uint16_t> version_index_map;
	};
	std::map<std::string, size_t> verneed_lib_map;
	std::vector<verneed_lib> verneed_libs;

	for ( size_t i = 0; i < needed_libs.size(); ++i )
	{
		if ( candidate_versions_for_library(needed_libs[i].name) == NULL )
			continue;
		verneed_lib_map[needed_libs[i].name] = verneed_libs.size();
		verneed_lib vl;
		vl.name = needed_libs[i].name;
		vl.dynstr_off = needed_libs[i].dynstr_offset;
		verneed_libs.push_back(vl);
	}

	std::vector<uint16_t> versym(dynsym_count, 0);
	versym[0] = 0; // NULL symbol = *local*
	uint16_t next_version_index = 2;

	for ( size_t i = 0; i < extern_syms.size(); ++i )
	{
		const ext_sym &sym = extern_syms[i];
		uint16_t ver_idx = 1; // default: *global* (unversioned)

		std::map<std::string, size_t>::iterator lit =
		    verneed_lib_map.find(sym.needed_lib);
		if ( lit != verneed_lib_map.end() )
		{
			verneed_lib &vl = verneed_libs[lit->second];
			const char *const *versions =
			    candidate_versions_for_library(vl.name);
			bool opened_here = false;
			void *handle = open_version_probe_handle(vl.name, opened_here);
			if ( handle && versions )
			{
				for ( const char *const *vp = versions; *vp; ++vp )
				{
					if ( dlvsym(handle, sym.name.c_str(), *vp) )
					{
						std::string ver_str = *vp;
						std::map<std::string, uint16_t>::iterator vit =
						    vl.version_index_map.find(ver_str);
						if ( vit != vl.version_index_map.end() )
						{
							ver_idx = vit->second;
						}
						else
						{
							ver_idx = next_version_index++;
							vernaux_entry ve;
							ve.name = ver_str;
							ve.hash = elf_hash(ver_str.c_str());
							ve.dynstr_off = dynstr.add(ver_str);
							ve.index = ver_idx;
							vl.versions.push_back(ve);
							vl.version_index_map[ver_str] = ver_idx;
						}
						break;
					}
				}
			}
			if ( handle && opened_here )
				dlclose(handle);
			if ( handle && !opened_here )
			{
				// RTLD_NOLOAD returns a real handle that should still be closed.
				dlclose(handle);
			}
		}
		versym[i + 1] = ver_idx;
	}

	// --- Build .dynsym ---

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
		// Check if this is a COPY relocation target (data symbol).
		bool is_copy = false;
		for ( size_t ci = 0; ci < copy_relas.size(); ++ci )
		{
			if ( copy_relas[ci].dynsym_index == i + 1 )
			{
				sym.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
				sym.st_size = copy_relas[ci].sym_size;
				is_copy = true;
				break;
			}
		}
		if ( !is_copy )
			sym.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
		sym.st_shndx = SHN_UNDEF;
		emit(out, &sym, sizeof(sym));
	}

	// .dynstr
	size_t dynstr_offset = out.size();
	size_t dynstr_size = dynstr.data.size();
	emit(out, dynstr.data.data(), dynstr_size);

	// .gnu.version (VERSYM): 2 bytes per dynsym entry.
	size_t versym_offset = align_to(out, 2);
	size_t versym_size = versym.size() * 2;
	for ( size_t i = 0; i < versym.size(); ++i )
		emit_val(out, versym[i]);

	// .gnu.version_r (VERNEED): one entry per versioned needed library.
	size_t verneed_offset = align_to(out, 8);
	size_t verneed_lib_count = 0;
	for ( size_t i = 0; i < verneed_libs.size(); ++i )
	{
		if ( verneed_libs[i].versions.empty() )
			continue;
		++verneed_lib_count;
		uint16_t vernaux_cnt =
		    static_cast<uint16_t>(verneed_libs[i].versions.size());
		uint32_t vn_next = 0;
		for ( size_t j = i + 1; j < verneed_libs.size(); ++j )
		{
			if ( !verneed_libs[j].versions.empty() )
			{
				vn_next = static_cast<uint32_t>(sizeof(Elf64_Verneed)
					+ vernaux_cnt * sizeof(Elf64_Vernaux));
				break;
			}
		}
		emit_val<uint16_t>(out, 1);
		emit_val<uint16_t>(out, vernaux_cnt);
		emit_val<uint32_t>(out, verneed_libs[i].dynstr_off);
		emit_val<uint32_t>(out, sizeof(Elf64_Verneed));
		emit_val<uint32_t>(out, vn_next);
		for ( size_t j = 0; j < verneed_libs[i].versions.size(); ++j )
		{
			emit_val<uint32_t>(out, verneed_libs[i].versions[j].hash);
			emit_val<uint16_t>(out, 0);
			emit_val<uint16_t>(out, verneed_libs[i].versions[j].index);
			emit_val<uint32_t>(out, verneed_libs[i].versions[j].dynstr_off);
			emit_val<uint32_t>(out,
			    (j + 1 < verneed_libs[i].versions.size())
				? sizeof(Elf64_Vernaux) : 0);
		}
	}
	size_t verneed_size = out.size() - verneed_offset;

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
	size_t total_rela_count = extern_rela_count + addrtab_data_relas.size() + copy_relas.size();
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
	// The flattened data includes .text + .addrtab contiguously.
	// Keep both so the RIP-relative addrtab references work.
	// total_code_size includes .text + any padding + .addrtab.
	if ( ::getenv("MADC_DEBUG_TEXTCOPY")
	  && total_code_size > 0x65da0 )
	{
		std::fprintf(stderr,
		    "[aot] text_copy raw @0x65d8c: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    text_copy[0x65d8c], text_copy[0x65d8d], text_copy[0x65d8e], text_copy[0x65d8f],
		    text_copy[0x65d90], text_copy[0x65d91], text_copy[0x65d92], text_copy[0x65d93],
		    text_copy[0x65d94], text_copy[0x65d95], text_copy[0x65d96], text_copy[0x65d97]);
	}

	// data_vaddr will be computed after PLT scan (which may grow total_code_size).
	uint64_t data_vaddr = 0;

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

			uint64_t label_off = code.labelOffset(ref.label_id);
			size_t imm_off = static_cast<size_t>(label_off) + ref.imm_offset;
			if ( imm_off + 8 > text_size )
				continue;
			if ( !is_emit_data_mov_site(text_copy, static_cast<size_t>(label_off), ref.imm_offset) )
				continue;

			size_t data_offset = ref.data_offset;
			std::pair<bool, size_t> found = find_data_offset(ref.address);
			if ( found.first )
				data_offset = found.second;
			else if ( data_offset == (size_t)-1 )
				continue;
			uint64_t new_addr = data_vaddr + data_offset;
			if ( ::getenv("MADC_DEBUG_AOT_REFS") )
			{
				std::fprintf(stderr,
				    "[aot-ref] prepass label=%u label_off=0x%llx imm_off=0x%zx addr=%p data_off=%zu new=0x%llx\n",
				    ref.label_id,
				    (unsigned long long)label_off,
				    imm_off,
				    (void *)ref.address,
				    data_offset,
				    (unsigned long long)new_addr);
			}
			if ( ::getenv("MADC_DEBUG_AOT_PATCHES")
			  && imm_off >= 0x65d80 && imm_off < 0x65da0 )
			{
				std::fprintf(stderr,
				    "[aot] pre-emit patch ref=%zu label=%u imm_off=0x%zx old_addr=0x%zx data_off=0x%zx new_addr=0x%llx\n",
				    i, ref.label_id, imm_off, (size_t)ref.address, data_offset,
				    (unsigned long long)new_addr);
			}
			std::memcpy(&text_copy[imm_off], &new_addr, 8);
			++data_patches;
		}
	}

	// Note: addrtab references are already patched by relocateToBase
	// above — they point to the correct (virtual) addrtab location.
	// The addrtab itself lives in .data and will be patched by the
	// dynamic linker via .rela.dyn entries.

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
		total_rela_count = extern_rela_count + addrtab_data_relas.size() + copy_relas.size();
		rela_dyn_size = total_rela_count * sizeof(Elf64_Rela);

		data_patches += call_patches.size();

		DBG(std::cout << "save_executable: built PLT with "
			      << plt_entries.size() << " entries, patched "
			      << call_patches.size() << " direct calls" << std::endl);
	}

	// Build a wrapper that mirrors Program::execute():
	// run the root program entry first for file-scope initialization,
	// then call the user-defined main.
	size_t entry_wrapper_offset = total_code_size;
	struct aot_string_init_ref {
		size_t wrapper_text_offset;
		size_t object_imm_offset;
		size_t cstr_imm_offset;
		size_t object_data_offset;
		size_t cstr_data_offset;
		size_t call_rel_offset;
	};
	std::vector<aot_string_init_ref> aot_string_init_refs;
	struct aot_stream_init_ref {
		size_t wrapper_text_offset;
		size_t object_imm_offset;
		size_t object_data_offset;
		size_t call_rel_offset;
		std::string helper_name;
	};
	std::vector<aot_stream_init_ref> aot_stream_init_refs;
	{
		std::vector<uint8_t> wrapper;
		uint64_t root_vaddr = code_vaddr;
		uint64_t main_vaddr = code_vaddr + static_cast<uint64_t>(main_offset);
		auto append_string_init = [&](size_t object_data_offset, size_t cstr_data_offset) {
			aot_string_init_ref ref;
			ref.wrapper_text_offset = entry_wrapper_offset;
			ref.object_imm_offset = wrapper.size() + 2;
			ref.cstr_imm_offset = wrapper.size() + 12;
			ref.object_data_offset = object_data_offset;
			ref.cstr_data_offset = cstr_data_offset;
			ref.call_rel_offset = wrapper.size() + 21;
			aot_string_init_refs.push_back(ref);

			wrapper.push_back(0x48); // movabs rdi, imm64
			wrapper.push_back(0xBF);
			for ( int i = 0; i < 8; ++i )
				wrapper.push_back(0x00);
			wrapper.push_back(0x48); // movabs rsi, imm64
			wrapper.push_back(0xBE);
			for ( int i = 0; i < 8; ++i )
				wrapper.push_back(0x00);
			wrapper.push_back(0xE8); // call helper
			for ( int i = 0; i < 4; ++i )
				wrapper.push_back(0x00);
		};
		auto append_stream_init = [&](size_t object_data_offset, const std::string &helper_name) {
			aot_stream_init_ref ref;
			ref.wrapper_text_offset = entry_wrapper_offset;
			ref.object_imm_offset = wrapper.size() + 2;
			ref.object_data_offset = object_data_offset;
			ref.call_rel_offset = wrapper.size() + 11;
			ref.helper_name = helper_name;
			aot_stream_init_refs.push_back(ref);

			wrapper.push_back(0x48); // movabs rdi, imm64
			wrapper.push_back(0xBF);
			for ( int i = 0; i < 8; ++i )
				wrapper.push_back(0x00);
			wrapper.push_back(0xE8); // call helper
			for ( int i = 0; i < 4; ++i )
				wrapper.push_back(0x00);
		};
		std::map<uint32_t, size_t> helper_plt_offsets;
		auto patch_helper_calls = [&]() {
			for ( size_t i = 0; i < aot_string_init_refs.size(); ++i )
			{
				if ( !aot_init_string_dynsym )
					continue;
				size_t helper_plt_off = helper_plt_offsets[aot_init_string_dynsym];
				int64_t call_rip = static_cast<int64_t>(code_vaddr
				    + aot_string_init_refs[i].wrapper_text_offset
				    + aot_string_init_refs[i].call_rel_offset + 4);
				int32_t disp = static_cast<int32_t>(
				    static_cast<int64_t>(code_vaddr + helper_plt_off) - call_rip);
				std::memcpy(&wrapper[aot_string_init_refs[i].call_rel_offset], &disp, 4);
			}
			for ( size_t i = 0; i < aot_stream_init_refs.size(); ++i )
			{
				uint32_t dynsym = aot_stream_init_dynsyms[aot_stream_init_refs[i].helper_name];
				size_t helper_plt_off = helper_plt_offsets[dynsym];
				int64_t call_rip = static_cast<int64_t>(code_vaddr
				    + aot_stream_init_refs[i].wrapper_text_offset
				    + aot_stream_init_refs[i].call_rel_offset + 4);
				int32_t disp = static_cast<int32_t>(
				    static_cast<int64_t>(code_vaddr + helper_plt_off) - call_rip);
				std::memcpy(&wrapper[aot_stream_init_refs[i].call_rel_offset], &disp, 4);
			}
		};
		if ( main_takes_args )
		{
			static const uint8_t prefix[] = {
				0x48, 0x83, 0xEC, 0x18,       // sub rsp, 24
				0x48, 0x89, 0x3C, 0x24,       // mov [rsp], rdi
				0x48, 0x89, 0x74, 0x24, 0x08, // mov [rsp+8], rsi
				0xE8, 0x00, 0x00, 0x00, 0x00, // call root_fn (patched later)
				0x48, 0x8B, 0x3C, 0x24,       // mov rdi, [rsp]
				0x48, 0x8B, 0x74, 0x24, 0x08, // mov rsi, [rsp+8]
				0xE8, 0x00, 0x00, 0x00, 0x00, // call main (patched later)
				0x48, 0x83, 0xC4, 0x18,       // add rsp, 24
				0xC3                                // ret
			};
			wrapper.insert(wrapper.end(), prefix, prefix + 13);
			for ( size_t i = 0; i < string_object_patches.size(); ++i )
				append_string_init(string_object_patches[i].object_offset,
						   string_object_patches[i].cstr_offset);
			for ( size_t i = 0; i < stream_object_patches.size(); ++i )
				append_stream_init(stream_object_patches[i].object_offset,
						   stream_object_patches[i].helper_name);
			size_t root_call_imm = wrapper.size() + 1;
			wrapper.insert(wrapper.end(), prefix + 13, prefix + sizeof(prefix));
			int64_t root_rip = static_cast<int64_t>(code_vaddr + entry_wrapper_offset
				+ root_call_imm + 4);
			int32_t root_disp = static_cast<int32_t>(
			    static_cast<int64_t>(root_vaddr) - root_rip);
			std::memcpy(&wrapper[root_call_imm], &root_disp, 4);
			size_t main_call_imm = root_call_imm + 14;
			int64_t main_rip = static_cast<int64_t>(code_vaddr + entry_wrapper_offset
				+ main_call_imm + 4);
			int32_t main_disp = static_cast<int32_t>(
			    static_cast<int64_t>(main_vaddr) - main_rip);
			std::memcpy(&wrapper[main_call_imm], &main_disp, 4);
		}
		else
		{
			static const uint8_t prefix[] = {
				0x48, 0x83, 0xEC, 0x08,       // sub rsp, 8
				0xE8, 0x00, 0x00, 0x00, 0x00, // call root_fn (patched later)
				0xE8, 0x00, 0x00, 0x00, 0x00, // call main (patched later)
				0x48, 0x83, 0xC4, 0x08,       // add rsp, 8
				0xC3                                // ret
			};
			wrapper.insert(wrapper.end(), prefix, prefix + 4);
			for ( size_t i = 0; i < string_object_patches.size(); ++i )
				append_string_init(string_object_patches[i].object_offset,
						   string_object_patches[i].cstr_offset);
			for ( size_t i = 0; i < stream_object_patches.size(); ++i )
				append_stream_init(stream_object_patches[i].object_offset,
						   stream_object_patches[i].helper_name);
			size_t root_call_imm = wrapper.size() + 1;
			wrapper.insert(wrapper.end(), prefix + 4, prefix + sizeof(prefix));
			int64_t root_rip = static_cast<int64_t>(code_vaddr + entry_wrapper_offset
				+ root_call_imm + 4);
			int32_t root_disp = static_cast<int32_t>(
			    static_cast<int64_t>(root_vaddr) - root_rip);
			std::memcpy(&wrapper[root_call_imm], &root_disp, 4);
			size_t main_call_imm = root_call_imm + 5;
			int64_t main_rip = static_cast<int64_t>(code_vaddr + entry_wrapper_offset
				+ main_call_imm + 4);
			int32_t main_disp = static_cast<int32_t>(
			    static_cast<int64_t>(main_vaddr) - main_rip);
			std::memcpy(&wrapper[main_call_imm], &main_disp, 4);
		}
		std::vector<uint32_t> helper_dynsyms;
		if ( aot_init_string_dynsym && !string_object_patches.empty() )
			helper_dynsyms.push_back(aot_init_string_dynsym);
		for ( std::map<std::string, uint32_t>::const_iterator it = aot_stream_init_dynsyms.begin();
		      it != aot_stream_init_dynsyms.end(); ++it )
			helper_dynsyms.push_back(it->second);
		for ( size_t i = 0; i < helper_dynsyms.size(); ++i )
		{
			helper_plt_offsets[helper_dynsyms[i]] = entry_wrapper_offset + wrapper.size();
			static const uint8_t plt_stub[] = {
				0xFF, 0x25, 0x00, 0x00, 0x00, 0x00
			};
			wrapper.insert(wrapper.end(), plt_stub, plt_stub + sizeof(plt_stub));
			for ( int j = 0; j < 8; ++j )
				wrapper.push_back(0x00);
		}
		patch_helper_calls();
		text_copy.insert(text_copy.end(), wrapper.begin(), wrapper.end());
		total_code_size += wrapper.size();
		for ( size_t i = 0; i < helper_dynsyms.size(); ++i )
		{
			rela_entry re;
			re.source_offset = helper_plt_offsets[helper_dynsyms[i]] + 6;
			re.dynsym_index = helper_dynsyms[i];
			re.elf_type = R_X86_64_64;
			re.is_extern = true;
			re.raw_addend = 0;
			rela_entries.push_back(re);
		}
	}

	// Recalculate data_vaddr now that total_code_size is final
	// (includes text + addrtab + PLT).
	{
		size_t data_file_offset_est = text_file_offset + START_STUB_SIZE + total_code_size;
		data_file_offset_est = (data_file_offset_est + 15) & ~(size_t)15;
		data_vaddr = BASE_ADDR + data_file_offset_est;
	}

	// Re-run the label-based data patching with the correct data_vaddr.
	if ( !data_offset_map.empty() && !aot_data_refs.empty() )
	{
		for ( size_t i = 0; i < aot_data_refs.size(); ++i )
		{
			const AotDataRef &ref = aot_data_refs[i];
			if ( !code.isLabelBound(ref.label_id) )
				continue;
			uint64_t label_off = code.labelOffset(ref.label_id);
			size_t imm_off = static_cast<size_t>(label_off) + ref.imm_offset;
			if ( imm_off + 8 > total_code_size )
				continue;
			if ( !is_emit_data_mov_site(text_copy, static_cast<size_t>(label_off), ref.imm_offset) )
				continue;
			size_t data_offset = ref.data_offset;
			std::pair<bool, size_t> found = find_data_offset(ref.address);
			if ( found.first )
				data_offset = found.second;
			else if ( data_offset == (size_t)-1 )
				continue;
			uint64_t new_addr = data_vaddr + data_offset;
			if ( ::getenv("MADC_DEBUG_AOT_REFS") )
			{
				std::fprintf(stderr,
				    "[aot-ref] finalpass label=%u label_off=0x%llx imm_off=0x%zx addr=%p data_off=%zu new=0x%llx\n",
				    ref.label_id,
				    (unsigned long long)label_off,
				    imm_off,
				    (void *)ref.address,
				    data_offset,
				    (unsigned long long)new_addr);
			}
			std::memcpy(&text_copy[imm_off], &new_addr, 8);
		}
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

			bool is_moffs64 = (b0 == 0x48 && (b1 == 0xA1 || b1 == 0xA3));
			bool is_moffs8  = (b0 == 0xA0 || b0 == 0xA2);
			bool is_movabs  = ((b0 == 0x48 || b0 == 0x49) && b1 >= 0xB8 && b1 <= 0xBF);

			if ( !is_moffs64 && !is_moffs8 && !is_movabs )
				continue;

			// Address starts at offset +1 for A0/A2, +2 for REX variants.
			size_t addr_off = is_moffs8 ? 1 : 2;
			if ( i + addr_off + 8 > total_code_size )
				continue;

			uint64_t imm_val;
			std::memcpy(&imm_val, &text_copy[i + addr_off], 8);
			uintptr_t addr = static_cast<uintptr_t>(imm_val);

			std::map<uintptr_t, size_t>::const_iterator dit2 =
			    data_offset_map.find(addr);
			if ( dit2 != data_offset_map.end() )
			{
				uint64_t new_addr = data_vaddr + dit2->second;
				std::memcpy(&text_copy[i + addr_off], &new_addr, 8);
				++data_patches;
			}
		}
	}

	DBG(std::cout << "save_executable: patched " << data_patches
		      << " global data references (" << globals.size()
		      << " globals, " << data_section_buf.size()
		      << " bytes in .data)" << std::endl);

	auto debug_dump_out_text = [&](const char *tag) {
		if ( !::getenv("MADC_DEBUG_TEXTCOPY") )
			return;
		size_t base = code_file_offset + 0x65d8c;
		if ( base + 12 > out.size() )
			return;
		std::fprintf(stderr,
		    "[aot] %s @0x65d8c: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    tag,
		    out[base + 0], out[base + 1], out[base + 2], out[base + 3],
		    out[base + 4], out[base + 5], out[base + 6], out[base + 7],
		    out[base + 8], out[base + 9], out[base + 10], out[base + 11]);
	};

	emit(out, text_copy.data(), total_code_size);
	size_t total_text_size = START_STUB_SIZE + total_code_size;
	debug_dump_out_text("after emit text_copy");

	// Patch _start stub: main address and __libc_start_main PLT.
	{
		uint64_t main_vaddr = code_vaddr + static_cast<uint64_t>(entry_wrapper_offset);

		// Patch mov $main, %edi (imm32).
		uint32_t main32 = static_cast<uint32_t>(main_vaddr);
		std::memcpy(&out[start_file_offset + START_STUB_MAIN_IMM_OFFSET],
			    &main32, 4);

		// Build PLT stub + GOT slot for __libc_start_main.
		// Append directly to `out` since text_copy was already emitted.
		size_t lsm_plt_off = total_code_size;
		uint8_t jmp_stub[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
		emit(out, jmp_stub, 6);
		total_code_size += 6;
		total_text_size += 6;
		size_t lsm_got_off = total_code_size;
		emit_zeros(out, 8);
		total_code_size += 8;
		total_text_size += 8;

		// Patch call rel32 to PLT stub.
		uint64_t plt_vaddr = code_vaddr + lsm_plt_off;
		uint64_t call_rip = start_vaddr + START_STUB_CALL_REL_OFFSET + 4;
		int32_t disp = static_cast<int32_t>(
		    static_cast<int64_t>(plt_vaddr) - static_cast<int64_t>(call_rip));
		std::memcpy(&out[start_file_offset + START_STUB_CALL_REL_OFFSET], &disp, 4);

		// Emit rela for GOT slot.
		rela_entry re;
		re.source_offset = lsm_got_off;
		re.dynsym_index = libc_start_main_dynsym;
		re.elf_type = R_X86_64_64;
		re.is_extern = true;
		re.raw_addend = 0;
		rela_entries.push_back(re);

		// Recount.
		extern_rela_count = 0;
		for ( size_t i = 0; i < rela_entries.size(); ++i )
			if ( rela_entries[i].is_extern )
				++extern_rela_count;
		total_rela_count = extern_rela_count + addrtab_data_relas.size() + copy_relas.size();
		rela_dyn_size = total_rela_count * sizeof(Elf64_Rela);
	}

	// Recalculate data_vaddr after __libc_start_main PLT.
	{
		size_t est = text_file_offset + total_text_size;
		est = (est + 15) & ~(size_t)15;
		data_vaddr = BASE_ADDR + est;
	}

	// Re-patch data references with final data_vaddr (emit_data_mov labels).
	if ( !data_offset_map.empty() && !aot_data_refs.empty() )
	{
		for ( size_t i = 0; i < aot_data_refs.size(); ++i )
		{
			const AotDataRef &ref = aot_data_refs[i];
			if ( !code.isLabelBound(ref.label_id) )
				continue;
			uint64_t label_off = code.labelOffset(ref.label_id);
			size_t imm_off = static_cast<size_t>(label_off) + ref.imm_offset;
			if ( imm_off + 8 > total_code_size )
				continue;
			if ( !is_emit_data_mov_site(text_copy, static_cast<size_t>(label_off), ref.imm_offset) )
				continue;
			size_t data_offset = ref.data_offset;
			std::pair<bool, size_t> found = find_data_offset(ref.address);
			if ( found.first )
				data_offset = found.second;
			else if ( data_offset == (size_t)-1 )
				continue;
			uint64_t new_addr = data_vaddr + data_offset;
			// Patch in `out` (text was already emitted).
			size_t file_pos = code_file_offset + imm_off;
			if ( ::getenv("MADC_DEBUG_AOT_PATCHES")
			  && imm_off >= 0x65d80 && imm_off < 0x65da0 )
			{
				std::fprintf(stderr,
				    "[aot] final patch ref=%zu label=%u imm_off=0x%zx file_pos=0x%zx old_addr=0x%zx data_off=0x%zx new_addr=0x%llx\n",
				    i, ref.label_id, imm_off, file_pos, (size_t)ref.address, data_offset,
				    (unsigned long long)new_addr);
			}
			if ( file_pos + 8 <= out.size() )
				std::memcpy(&out[file_pos], &new_addr, 8);
		}
	}

	for ( size_t i = 0; i < aot_string_init_refs.size(); ++i )
	{
		uint64_t obj_vaddr = data_vaddr + aot_string_init_refs[i].object_data_offset;
		uint64_t cstr_vaddr = data_vaddr + aot_string_init_refs[i].cstr_data_offset;
		std::memcpy(&out[code_file_offset
				 + aot_string_init_refs[i].wrapper_text_offset
				 + aot_string_init_refs[i].object_imm_offset],
			    &obj_vaddr, 8);
		std::memcpy(&out[code_file_offset
				 + aot_string_init_refs[i].wrapper_text_offset
				 + aot_string_init_refs[i].cstr_imm_offset],
			    &cstr_vaddr, 8);
	}
	for ( size_t i = 0; i < aot_stream_init_refs.size(); ++i )
	{
		uint64_t obj_vaddr = data_vaddr + aot_stream_init_refs[i].object_data_offset;
		std::memcpy(&out[code_file_offset
				 + aot_stream_init_refs[i].wrapper_text_offset
				 + aot_stream_init_refs[i].object_imm_offset],
			    &obj_vaddr, 8);
	}
	debug_dump_out_text("after final aot patch");

	// Re-run moffs scanner with final data_vaddr.
	if ( !data_offset_map.empty() )
	{
		for ( size_t i = 0; i + 9 <= total_code_size; ++i )
		{
			size_t file_pos = code_file_offset + i;
			if ( file_pos + 10 > out.size() )
				break;
			uint8_t b0 = out[file_pos];
			uint8_t b1 = out[file_pos + 1];
			bool is_moffs64 = (b0 == 0x48 && (b1 == 0xA1 || b1 == 0xA3));
			bool is_moffs8  = (b0 == 0xA0 || b0 == 0xA2);
			if ( !is_moffs64 && !is_moffs8 )
				continue;
			size_t addr_off = is_moffs8 ? 1 : 2;
			if ( file_pos + addr_off + 8 > out.size() )
				continue;
			uint64_t imm_val;
			std::memcpy(&imm_val, &out[file_pos + addr_off], 8);
			uintptr_t addr = static_cast<uintptr_t>(imm_val);
			std::map<uintptr_t, size_t>::const_iterator dit2 =
			    data_offset_map.find(addr);
			if ( dit2 != data_offset_map.end() )
			{
				uint64_t new_addr = data_vaddr + dit2->second;
				std::memcpy(&out[file_pos + addr_off], &new_addr, 8);
			}
		}
	}
	debug_dump_out_text("after moffs patch");

		size_t copy_rela_start = 0;
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
		// where the addrtab lives (contiguous in the flattened code).
		// Use the addrtab section's offset from code.flatten().
		Section *ats = code.addressTableSection();
		uint64_t ats_offset = ats ? ats->offset() : text_size;
		uint64_t addrtab_vaddr_in_text = code_vaddr + ats_offset;
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
		copy_rela_start = rela_pos;
		for ( size_t i = 0; i < copy_relas.size(); ++i )
		{
			Elf64_Rela rela;
			rela.r_offset = 0; // placeholder — patched after data_vaddr known
			rela.r_info = ELF64_R_INFO(copy_relas[i].dynsym_index, R_X86_64_COPY);
			rela.r_addend = 0;
			std::memcpy(&out[rela_pos], &rela, sizeof(rela));
			rela_pos += sizeof(rela);
		}
	}
	debug_dump_out_text("after rela patch");

	// .data (global variables and string literals)
	size_t data_file_offset = 0;
	size_t data_section_size = data_section_buf.size();
	if ( data_section_size > 0 )
	{
		data_file_offset = align_to(out, 16);
		emit(out, data_section_buf.data(), data_section_size);
		// Recalculate actual data vaddr for section header.
		data_vaddr = BASE_ADDR + data_file_offset;

		auto patch_data_pointer_slot = [&](size_t file_pos) {
			if ( file_pos + sizeof(uint64_t) > out.size() )
				return;
			uint64_t raw = 0;
			std::memcpy(&raw, &out[file_pos], sizeof(raw));
			std::pair<bool, size_t> found =
			    find_data_offset(static_cast<uintptr_t>(raw));
			if ( !found.first )
				return;
			uint64_t new_addr = data_vaddr + found.second;
			std::memcpy(&out[file_pos], &new_addr, sizeof(new_addr));
		};

		std::function<void(size_t, DataDef *, uint32_t)> patch_data_region;
		patch_data_region = [&](size_t file_pos, DataDef *dd, uint32_t count) {
			if ( !dd || count == 0 )
				return;
			if ( dd->is_pointer() )
			{
				size_t step = dd->size ? dd->size : sizeof(uint64_t);
				if ( step < sizeof(uint64_t) )
					step = sizeof(uint64_t);
				for ( uint32_t i = 0; i < count; ++i )
				    patch_data_pointer_slot(file_pos + i * step);
				return;
			}
			if ( dd->basetype() != BaseType::btStruct )
				return;

			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd);
			if ( !sdd || sdd->union_layout )
				return;

			size_t struct_size = sdd->size ? sdd->size : dd->size;
			for ( uint32_t ci = 0; ci < count; ++ci )
			{
				size_t base = file_pos + ci * struct_size;
				for ( size_t mi = 0; mi < sdd->members.size(); ++mi )
				{
				    DataDef *member_dd = sdd->members[mi].second;
				    if ( !member_dd )
					continue;
				    size_t member_count =
					(mi < sdd->member_counts.size()) ? sdd->member_counts[mi] : 1;
				    size_t member_ofs =
					(mi < sdd->member_offsets.size()) ? sdd->member_offsets[mi] : 0;
				    patch_data_region(base + member_ofs, member_dd,
					(uint32_t)member_count);
				}
			}
		};

		auto patch_variable_region = [&](Variable *var) {
			if ( !var || !var->data || !var->type )
				return;
			size_t offset = 0;
			if ( var->has_aot_data() )
				offset = var->aot_data_offset;
			else
			{
				std::pair<bool, size_t> found =
				    find_data_offset(reinterpret_cast<uintptr_t>(var->data));
				if ( !found.first )
					return;
				offset = found.second;
			}
			uint32_t count = var->is_fixed_array() ? var->total_elements() : 1;
			patch_data_region(data_file_offset + offset, var->type, count);
		};

		if ( tkProgram )
		{
			for ( size_t i = 0; i < tkProgram->variables.size(); ++i )
			{
				Variable *var = tkProgram->variables[i];
				if ( !var || !var->is_global() )
					continue;
				patch_variable_region(var);
			}
		}

		for ( size_t fi = 0; fi < pending_funcs.size(); ++fi )
		{
			TokenFunc *tf = dynamic_cast<TokenFunc *>(pending_funcs[fi]);
			if ( !tf )
				continue;
			for ( size_t vi = 0; vi < tf->variables.size(); ++vi )
			{
				Variable *var = tf->variables[vi];
				if ( !var || (var->flags & vfPARAM) )
					continue;
				if ( (var->flags & vfSTACK) && !(var->flags & vfSTATIC) )
					continue;
				patch_variable_region(var);
			}
		}

		for ( size_t i = 0; i < aot_discovered_data.size(); ++i )
		{
			const AotDiscoveredData &entry = aot_discovered_data[i];
			if ( !entry.address || !entry.type || entry.count == 0 )
				continue;
			std::pair<bool, size_t> found =
			    find_data_offset(reinterpret_cast<uintptr_t>(entry.address));
			if ( !found.first )
				continue;
			patch_data_region(data_file_offset + found.second,
			    entry.type, entry.count);
		}

		for ( size_t i = 0; i < string_object_patches.size(); ++i )
		{
			uint64_t cstr_vaddr = data_vaddr + string_object_patches[i].cstr_offset;
			size_t ptr_pos = data_file_offset + string_object_patches[i].object_offset;
			if ( ptr_pos + sizeof(uint64_t) <= out.size() )
				std::memcpy(&out[ptr_pos], &cstr_vaddr, sizeof(cstr_vaddr));
		}

		// Patch COPY rela offsets now that data_vaddr is known.
		{
			size_t rp = copy_rela_start;
			for ( size_t i = 0; i < copy_relas.size(); ++i )
			{
				uint64_t offset = data_vaddr + copy_relas[i].data_offset;
				std::memcpy(&out[rp], &offset, 8);
				rp += sizeof(Elf64_Rela);
			}
		}
	}
	debug_dump_out_text("after data emit");

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
	emit_dyn(0x6ffffff0, BASE_ADDR + versym_offset);  // DT_VERSYM
	emit_dyn(0x6ffffffe, BASE_ADDR + verneed_offset); // DT_VERNEED
	emit_dyn(0x6fffffff, verneed_lib_count);         // DT_VERNEEDNUM
	emit_dyn(DT_NULL, 0);

	size_t dynamic_size = out.size() - dynamic_offset;

	// Section headers (optional but helpful for readelf/objdump).
	strtab_builder shstrtab;
	uint32_t shname_interp   = shstrtab.add(".interp");
	uint32_t shname_hash     = shstrtab.add(".hash");
	uint32_t shname_dynsym   = shstrtab.add(".dynsym");
	uint32_t shname_dynstr   = shstrtab.add(".dynstr");
	uint32_t shname_versym   = shstrtab.add(".gnu.version");
	uint32_t shname_verneed  = shstrtab.add(".gnu.version_r");
	uint32_t shname_rela     = shstrtab.add(".rela.dyn");
	uint32_t shname_text     = shstrtab.add(".text");
	uint32_t shname_data     = shstrtab.add(".data");
	uint32_t shname_dynamic  = shstrtab.add(".dynamic");
	uint32_t shname_symtab   = shstrtab.add(".symtab");
	uint32_t shname_strtab   = shstrtab.add(".strtab");
	uint32_t shname_shstrtab = shstrtab.add(".shstrtab");

	// Build .symtab + .strtab with user-defined function symbols.
	strtab_builder sym_strtab;
	struct sym_entry { std::string name; uint64_t value; };
	std::vector<sym_entry> func_syms;

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
		sym_entry se;
		se.name = tf->var.name;
		se.value = code.labelOffset(label_id);
		func_syms.push_back(se);
	}

	// Emit .symtab data: NULL + section sym + func syms.
	size_t symtab_offset = align_to(out, 8);
	size_t first_global_sym = 2; // after NULL + section
	{
		// [0] NULL
		Elf64_Sym null_sym;
		std::memset(&null_sym, 0, sizeof(null_sym));
		emit(out, &null_sym, sizeof(null_sym));
		// [1] .text section symbol
		Elf64_Sym sec_sym;
		std::memset(&sec_sym, 0, sizeof(sec_sym));
		sec_sym.st_info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
		sec_sym.st_shndx = 8; // .text section index
		emit(out, &sec_sym, sizeof(sec_sym));
		// [2+] function symbols
		for ( size_t i = 0; i < func_syms.size(); ++i )
		{
			Elf64_Sym sym;
			std::memset(&sym, 0, sizeof(sym));
			sym.st_name = sym_strtab.add(func_syms[i].name);
			sym.st_value = text_vaddr + START_STUB_SIZE + func_syms[i].value;
			sym.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
			sym.st_shndx = 8; // .text section index
			emit(out, &sym, sizeof(sym));
		}
	}
	size_t symtab_size = out.size() - symtab_offset;

	// Emit .strtab data.
	size_t strtab_offset = out.size();
	size_t strtab_size = sym_strtab.data.size();
	emit(out, sym_strtab.data.data(), strtab_size);

	// Section indices computed in the section header block below.
	size_t shstrtab_offset = out.size();
	size_t shstrtab_size = shstrtab.data.size();
	emit(out, shstrtab.data.data(), shstrtab_size);

	size_t shdr_offset = align_to(out, 8);
	// Sections: NULL, .interp, .hash, .dynsym, .dynstr, .gnu.version,
	//   .gnu.version_r, .rela.dyn, .text, [.data], .dynamic,
	//   .symtab, .strtab, .shstrtab
	int num_sections = (data_section_size > 0 ? 12 : 11) + 2;
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
	const int SEC_VERSYM   = 5;
	const int SEC_VERNEED  = 6;
	const int SEC_RELA     = 7;
	const int SEC_TEXT     = 8;
	const int SEC_DATA     = 9;
	const int SEC_DYNAMIC  = data_section_size > 0 ? 10 : 9;
	const int SEC_SYMTAB2  = SEC_DYNAMIC + 1;
	const int SEC_STRTAB2  = SEC_DYNAMIC + 2;
	const int SEC_SHSTRTAB_IDX = num_sections - 1;
	(void)SEC_NULL; (void)SEC_INTERP; (void)SEC_HASH;
	(void)SEC_VERSYM; (void)SEC_VERNEED;
	(void)SEC_RELA; (void)SEC_TEXT;
	(void)SEC_DATA; (void)SEC_DYNAMIC;
	(void)SEC_SYMTAB2; (void)SEC_STRTAB2; (void)SEC_SHSTRTAB_IDX;

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

	// [5] .gnu.version
	emit_shdr(shname_versym, 0x6fffffff /*SHT_GNU_versym*/, SHF_ALLOC,
		  BASE_ADDR + versym_offset, versym_offset, versym_size,
		  SEC_DYNSYM, 0, 2, 2);

	// [6] .gnu.version_r
	emit_shdr(shname_verneed, 0x6ffffffe /*SHT_GNU_verneed*/, SHF_ALLOC,
		  BASE_ADDR + verneed_offset, verneed_offset, verneed_size,
		  SEC_DYNSTR, 1, 8, 0);

	// [7] .rela.dyn
	emit_shdr(shname_rela, SHT_RELA, SHF_ALLOC,
		  BASE_ADDR + rela_dyn_offset, rela_dyn_offset, rela_dyn_size,
		  SEC_DYNSYM, 0, 8, sizeof(Elf64_Rela));

	// [8] .text
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

	// .symtab
	emit_shdr(shname_symtab, SHT_SYMTAB, 0,
		  0, symtab_offset, symtab_size,
		  SEC_STRTAB2, static_cast<uint32_t>(first_global_sym),
		  8, sizeof(Elf64_Sym));

	// .strtab
	emit_shdr(shname_strtab, SHT_STRTAB, 0,
		  0, strtab_offset, strtab_size,
		  0, 0, 1, 0);

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
