// madc_pe_icon.h — a Windows icon file (.ico) as the resources of a PE
// image: what windres makes of `32512 ICON "file.ico"` for the linker. Each
// image in the file becomes an RT_ICON (ids 1..n, the file's order) and one
// RT_GROUP_ICON directory names them by id — id 32512, IDI_APPLICATION: the
// icon Explorer shows for the file and the one the webview's window class
// loads from the executable (LoadImage(GetModuleHandle(NULL),
// IDI_APPLICATION, ...)), so the title bar and the taskbar carry it too.
// Language 0x409, windres's default. The PE writer (third_party/mir/
// mir-pe.c, MIR_object_exec_params::resources) lays the entries out as
// .rsrc; a project manifest's "icon" is the one route in.
//
// A group entry copies its .ico directory entry (width, height, colours,
// planes, bit count, size) with the image's offset replaced by its id.
//
// Thread contract: an object is used by one thread; distinct objects are
// independent.
#ifndef __MADC_PE_ICON_H
#define __MADC_PE_ICON_H 1

#include <cstdint>
#include <string>
#include <vector>
#include "mir-debug.h"	// MIR_object_resource

class PeIcon {
public:
	PeIcon() {}
	// The entries point into this object's bytes: no copies.
	PeIcon(const PeIcon &) = delete;
	PeIcon &operator=(const PeIcon &) = delete;

	// Take an icon file's bytes (the caller reads the file). False + err =
	// not an icon file; err names what is wrong.
	bool parse(std::vector<uint8_t> bytes, std::string &err);

	// RT_ICON entries in the file's order, then the RT_GROUP_ICON.
	const std::vector<MIR_object_resource> &resources() const { return res_; }

	static const uint32_t group_id = 32512;	// IDI_APPLICATION
	static const uint32_t language = 0x409;	// en-US
private:
	std::vector<uint8_t> ico_;	// the file; each RT_ICON is a run of it
	std::vector<uint8_t> group_;	// the RT_GROUP_ICON directory
	std::vector<MIR_object_resource> res_;
};

#endif // __MADC_PE_ICON_H
