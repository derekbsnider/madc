//////////////////////////////////////////////////////////////////////////
//									//
// madc_pe_icon.cpp — an .ico file as a PE image's icon resources (the	//
// windres role; include/madc_pe_icon.h carries the contract).		//
//									//
// The file: ICONDIR (reserved 0, type 1, count) then `count` 16-byte	//
// ICONDIRENTRYs (width, height, colours, reserved, planes, bit count,	//
// bytes, offset), each naming a run of the file — a DIB or a PNG,	//
// passed through untouched. The group directory: GRPICONDIR (the same	//
// three u16s) then 14-byte GRPICONDIRENTRYs, the entry with its offset	//
// replaced by a u16 resource id.					//
//									//
// Thread contract: an object is used by one thread.			//
//									//
//////////////////////////////////////////////////////////////////////////

#include "madc_pe_icon.h"

#include <utility>

const uint32_t PeIcon::group_id;
const uint32_t PeIcon::language;

namespace {
uint32_t u16_at(const std::vector<uint8_t> &b, size_t at) {
	return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8);
}
uint32_t u32_at(const std::vector<uint8_t> &b, size_t at) {
	return u16_at(b, at) | (u16_at(b, at + 2) << 16);
}
void put_u16(std::vector<uint8_t> &b, uint32_t v) {
	b.push_back((uint8_t)v);
	b.push_back((uint8_t)(v >> 8));
}
} // namespace

bool PeIcon::parse(std::vector<uint8_t> bytes, std::string &err) {
	ico_ = std::move(bytes);
	group_.clear();
	res_.clear();
	const size_t size = ico_.size();
	if (size < 6 || u16_at(ico_, 0) != 0 || u16_at(ico_, 2) != 1) {
		err = "not an icon file (no ICONDIR of type 1)";
		return false;
	}
	const uint32_t count = u16_at(ico_, 4);
	const size_t dir_end = 6 + (size_t)count * 16;
	if (count == 0) {
		err = "the icon file holds no images";
		return false;
	}
	if (dir_end > size) {
		err = "the icon file's directory runs past its end";
		return false;
	}
	put_u16(group_, 0);
	put_u16(group_, 1);
	put_u16(group_, count);
	for (uint32_t i = 0; i < count; i++) {
		const size_t e = 6 + (size_t)i * 16;
		const uint32_t len = u32_at(ico_, e + 8);
		const uint32_t off = u32_at(ico_, e + 12);
		if (len == 0 || off < dir_end || off > size || len > size - off) {
			err = "image " + std::to_string(i + 1)
			      + " lies outside the icon file";
			return false;
		}
		// width, height, colours, reserved; planes, bit count; bytes
		group_.insert(group_.end(), ico_.begin() + e, ico_.begin() + e + 12);
		put_u16(group_, i + 1);
		MIR_object_resource r = { 3 /* RT_ICON */, i + 1, language,
					  ico_.data() + off, len };
		res_.push_back(r);
	}
	MIR_object_resource g = { 14 /* RT_GROUP_ICON */, group_id, language,
				  group_.data(), group_.size() };
	res_.push_back(g);
	return true;
}
