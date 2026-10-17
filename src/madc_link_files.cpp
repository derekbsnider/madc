// madc_link_files — ld's view of a library file on an ELF target: the -l
// search over the -L and toolchain directories, and what a shared object, a
// linker script or an archive links. Contract in include/madc_modules.h.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "madc_modules.h"
#include "madc_posix_io.h"	// host_path_join / _type_of / _absolute
#include "madc_sys_includes.h"	// madc_toolchain_library_dirs

// Byte-level, like macho_dylib_id (madc_modules.cpp): the ELF classes madc links are 64-bit
// little-endian (x86-64, AArch64), and the reader runs on any host.

static uint64_t elf_le(const unsigned char *p, int n)
{
	uint64_t v = 0;
	for (int i = n - 1; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

enum class link_file_form : unsigned char { other, elf_shared, elf_foreign, archive_empty, archive, text };

// An ELF shared object's DT_SONAME ("" when it has none), through its
// PT_DYNAMIC segment and the PT_LOAD that holds DT_STRTAB.
static std::string elf_soname(FILE *f, const unsigned char *eh)
{
	uint64_t phoff = elf_le(eh + 32, 8);
	uint64_t phentsize = elf_le(eh + 54, 2), phnum = elf_le(eh + 56, 2);
	if (phentsize < 56 || phnum == 0 || phnum > 4096)
		return "";
	std::string ph(phentsize * phnum, '\0');
	if (fseek(f, (long)phoff, SEEK_SET) != 0 || fread(&ph[0], 1, ph.size(), f) != ph.size())
		return "";
	const unsigned char *p = (const unsigned char *)ph.data();
	uint64_t dyn_off = 0, dyn_size = 0;
	for (uint64_t i = 0; i < phnum; i++)
		if (elf_le(p + i * phentsize, 4) == 2) {	// PT_DYNAMIC
			dyn_off = elf_le(p + i * phentsize + 8, 8);
			dyn_size = elf_le(p + i * phentsize + 32, 8);
		}
	if (!dyn_size || dyn_size > (1u << 20))
		return "";
	std::string dyn(dyn_size, '\0');
	if (fseek(f, (long)dyn_off, SEEK_SET) != 0 || fread(&dyn[0], 1, dyn.size(), f) != dyn.size())
		return "";
	const unsigned char *d = (const unsigned char *)dyn.data();
	uint64_t strtab = 0, soname = 0;
	bool have_soname = false;
	for (uint64_t i = 0; i + 16 <= dyn_size; i += 16) {
		uint64_t tag = elf_le(d + i, 8), val = elf_le(d + i + 8, 8);
		if (tag == 0)	// DT_NULL
			break;
		if (tag == 5)	// DT_STRTAB
			strtab = val;
		else if (tag == 14) {	// DT_SONAME
			soname = val;
			have_soname = true;
		}
	}
	if (!have_soname)
		return "";
	for (uint64_t i = 0; i < phnum; i++) {
		const unsigned char *h = p + i * phentsize;
		uint64_t off = elf_le(h + 8, 8), vaddr = elf_le(h + 16, 8), filesz = elf_le(h + 32, 8);
		if (elf_le(h, 4) == 1 && strtab >= vaddr && strtab < vaddr + filesz) {	// PT_LOAD
			char name[4096];
			if (fseek(f, (long)(strtab - vaddr + off + soname), SEEK_SET) != 0)
				return "";
			size_t n = fread(name, 1, sizeof name - 1, f);
			name[n] = '\0';
			return std::string(name, strnlen(name, n));
		}
	}
	return "";
}

// The form of the file at `path`, as ld tells library inputs apart: an ELF
// shared object of the class madc links (`soname` read), an ELF of another
// class (ld passes it over: "skipping incompatible"), an archive (empty when
// it holds nothing past its magic), text (a linker script, `text` read), or
// something else.
static link_file_form link_file_form_of(const std::string &path, std::string &soname,
					std::string &text)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return link_file_form::other;
	link_file_form form = link_file_form::other;
	unsigned char head[512];
	size_t n = fread(head, 1, sizeof head, f);
	if (n >= 64 && memcmp(head, "\177ELF", 4) == 0) {
		bool ours = head[4] == 2 && head[5] == 1;	// ELFCLASS64, little-endian
		if (ours && elf_le(head + 16, 2) == 3) {	// ET_DYN
			form = link_file_form::elf_shared;
			soname = elf_soname(f, head);
		} else
			form = link_file_form::elf_foreign;
	} else if (n >= 8 && memcmp(head, "!<arch>\n", 8) == 0) {
		form = n == 8 && fgetc(f) == EOF ? link_file_form::archive_empty
						 : link_file_form::archive;
	} else if (n > 0 && memchr(head, '\0', n) == NULL) {
		text.assign((const char *)head, n);
		char more[4096];
		size_t m;
		while (text.size() < (1u << 16) && (m = fread(more, 1, sizeof more, f)) > 0)
			text.append(more, m);
		form = link_file_form::text;
	}
	fclose(f);
	return form;
}

// A linker script's library inputs: the words of its INPUT ( ... ) and
// GROUP ( ... ) lists, less an AS_NEEDED ( ... ) list's (ld records those
// only when they are used). False when the text holds no such list — not a
// linker script. The lists do not nest, so each read runs to its own ')'.
static bool linker_script_inputs(const std::string &text, std::vector<std::string> &inputs)
{
	std::vector<std::string> words;
	std::string w;
	for (size_t i = 0; i <= text.size(); i++) {
		char c = i < text.size() ? text[i] : ' ';
		if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
			size_t close = text.find("*/", i + 2);
			i = close == std::string::npos ? text.size() : close + 1;
			c = ' ';
		}
		if (c == '(' || c == ')' || c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			if (!w.empty())
				words.push_back(w);
			w.clear();
			if (c == '(' || c == ')')
				words.push_back(std::string(1, c));
		} else
			w += c;
	}
	bool script = false;
	for (size_t i = 0; i + 1 < words.size(); i++) {
		if ((words[i] != "INPUT" && words[i] != "GROUP") || words[i + 1] != "(")
			continue;
		script = true;
		for (i += 2; i < words.size() && words[i] != ")"; i++) {
			if (words[i] == "AS_NEEDED" && i + 1 < words.size() && words[i + 1] == "(")
				while (i < words.size() && words[i] != ")")
					i++;
			else
				inputs.push_back(words[i]);
		}
	}
	return script;
}

static madc_link_kind link_file(const std::string &path, const std::string &named,
				const std::vector<std::string> &dirs,
				std::vector<madc_link_image> &images, int depth);

// The -l search for `spelling` over `dirs` (madc_module_link_images'
// contract): in each directory the spelling, then — for the generic shared
// spelling — its archive (lib<name>.a); a file that reads as no library of
// this target (an ELF of another class) is passed over, leaving `images` as
// it was.
static madc_link_kind search_library(const std::string &spelling,
				     const std::vector<std::string> &dirs,
				     std::vector<madc_link_image> &images,
				     std::string &file, int depth)
{
	const char *sfx = madc_target_dso_suffix(TargetOS::Posix);
	size_t n = strlen(sfx);
	bool generic = spelling.size() > n && spelling.compare(spelling.size() - n, n, sfx) == 0;
	std::string archive = generic ? spelling.substr(0, spelling.size() - n) + ".a" : "";
	for (const std::string &dir : dirs) {
		const std::string cands[2] = { madc::detail::host_path_join(dir, spelling),
					       archive.empty() ? std::string()
						: madc::detail::host_path_join(dir, archive) };
		for (const std::string &c : cands) {
			if (c.empty()
			    || madc::detail::host_path_type_of(c) != madc::detail::host_path_type::file)
				continue;
			size_t before = images.size();
			madc_link_kind k = link_file(c, spelling, dirs, images, depth + 1);
			if (k != madc_link_kind::unresolved) {
				file = c;
				return k;
			}
			images.resize(before);
		}
	}
	return madc_link_kind::unresolved;
}

// A linker script's member, found as ld finds it: `-l<name>` by the -l
// search (its spelling madc_module_library_spelling's); a path (a leading
// '=' is the sysroot) as it stands; a bare file name in the working
// directory, then in each of `dirs` — recorded, with no DT_SONAME, as the
// path ld opened (`INPUT(libx.so)` beside -Llib records lib/libx.so; a -l
// member records its -l name).
static madc_link_kind link_script_member(const std::string &member,
					 const std::vector<std::string> &dirs,
					 std::vector<madc_link_image> &images, int depth)
{
	if (member.compare(0, 2, "-l") == 0) {
		std::string file;
		return search_library(madc_module_library_spelling(member.substr(2), TargetOS::Posix),
				      dirs, images, file, depth);
	}
	std::string name = member[0] == '=' ? member.substr(1) : member;
	if (madc::detail::host_path_absolute(name)
	    || madc::detail::host_path_type_of(name) == madc::detail::host_path_type::file)
		return link_file(name, name, dirs, images, depth + 1);
	for (const std::string &dir : dirs) {
		std::string path = madc::detail::host_path_join(dir, name);
		if (madc::detail::host_path_type_of(path) == madc::detail::host_path_type::file)
			return link_file(path, path, dirs, images, depth + 1);
	}
	return madc_link_kind::unresolved;
}

static madc_link_kind link_file(const std::string &path, const std::string &named,
				const std::vector<std::string> &dirs,
				std::vector<madc_link_image> &images, int depth)
{
	std::string soname, text;
	switch (link_file_form_of(path, soname, text)) {
	case link_file_form::elf_shared:
		images.push_back({ path, soname.empty() ? named : soname });
		return madc_link_kind::images;
	case link_file_form::archive_empty:
		return madc_link_kind::none;
	case link_file_form::archive:
		return madc_link_kind::archive;
	case link_file_form::text: {
		std::vector<std::string> inputs;
		if (depth > 8 || !linker_script_inputs(text, inputs))
			return madc_link_kind::unresolved;
		// A script's archive members (glibc's libc_nonshared.a) are not
		// recorded: the shared members carry the library. A script of
		// archives alone links as an archive.
		size_t before = images.size();
		bool archive = false;
		for (const std::string &m : inputs)
			archive |= link_script_member(m, dirs, images, depth) == madc_link_kind::archive;
		return images.size() > before ? madc_link_kind::images
		       : archive ? madc_link_kind::archive : madc_link_kind::none;
	}
	case link_file_form::elf_foreign:
	case link_file_form::other:
		break;
	}
	return madc_link_kind::unresolved;
}

madc_link_kind madc_module_link_file(const std::string &path, const std::string &named,
				     const std::vector<std::string> &dirs,
				     std::vector<madc_link_image> &images)
{
	size_t before = images.size();
	madc_link_kind k = link_file(path, named, dirs, images, 0);
	if (k != madc_link_kind::images)
		images.resize(before);
	return k;
}

// The -l rule's generic ELF spelling, lib<name>.so — the one a search reads
// (a module row's real soname, a path or a versioned name stands as it is).
static bool generic_elf_spelling(const std::string &spelling)
{
	const char *sfx = madc_target_dso_suffix(TargetOS::Posix);
	size_t n = strlen(sfx);
	return !madc_spelling_is_path(spelling) && spelling.size() > n + 3
	       && spelling.compare(0, 3, "lib") == 0
	       && spelling.compare(spelling.size() - n, n, sfx) == 0;
}

madc_link_kind madc_module_link_images(const std::string &spelling,
				       const std::vector<std::string> &dirs,
				       std::vector<madc_link_image> &images,
				       std::string &file)
{
	if (madc_spelling_is_path(spelling)) {
		file = spelling;
		return madc_module_link_file(spelling, spelling, dirs, images);
	}
	if (!generic_elf_spelling(spelling))
		return madc_link_kind::unresolved;
	size_t before = images.size();
	madc_link_kind k = search_library(spelling, dirs, images, file, -1);
	if (k != madc_link_kind::images)
		images.resize(before);
	return k;
}

std::vector<std::string> madc_link_search_path(const std::vector<std::string> &cli_dirs)
{
	std::vector<std::string> dirs(cli_dirs);
	for (int i = 0; madc_toolchain_library_dirs[i]; i++)
		dirs.push_back(madc_toolchain_library_dirs[i]);
	return dirs;
}

std::string madc_module_locate(const std::string &spelling,
			       const std::vector<std::string> &search_dirs)
{
	if (madc_spelling_is_path(spelling))
		return spelling;
	std::vector<madc_link_image> images;
	std::string file;
	if (madc_module_link_images(spelling, search_dirs, images, file) != madc_link_kind::unresolved)
		return file;
	for (const std::string &dir : search_dirs) {
		std::string path = madc::detail::host_path_join(dir, spelling);
		if (madc::detail::host_path_type_of(path) == madc::detail::host_path_type::file)
			return path;
	}
	return spelling;
}
