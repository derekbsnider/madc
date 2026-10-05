#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "doctest.h"
#include "madc_pe_icon.h"
#include <cstdio>
#include <cstring>

// An .ico as a PE image's icon resources (include/madc_pe_icon.h): the
// entries windres makes of `32512 ICON "file.ico"`.

namespace {
void put16(std::vector<uint8_t> &b, unsigned v) {
	b.push_back((uint8_t)v);
	b.push_back((uint8_t)(v >> 8));
}
void put32(std::vector<uint8_t> &b, unsigned v) {
	put16(b, v & 0xffff);
	put16(b, v >> 16);
}
// An icon file with two images whose bytes are `a` and `b`: 48x48 at 8 bits
// (16 colours) and 16x16 at 32 bits. The parser passes image bytes through,
// so they need not be real DIBs.
std::vector<uint8_t> two_image_ico(const char *a, const char *b) {
	std::vector<uint8_t> f;
	put16(f, 0); put16(f, 1); put16(f, 2);
	unsigned off = 6 + 2 * 16;
	f.push_back(48); f.push_back(48); f.push_back(16); f.push_back(0);
	put16(f, 1); put16(f, 8); put32(f, (unsigned)strlen(a)); put32(f, off);
	f.push_back(16); f.push_back(16); f.push_back(0); f.push_back(0);
	put16(f, 1); put16(f, 32); put32(f, (unsigned)strlen(b));
	put32(f, off + (unsigned)strlen(a));
	f.insert(f.end(), a, a + strlen(a));
	f.insert(f.end(), b, b + strlen(b));
	return f;
}
std::string bytes_of(const MIR_object_resource &r) {
	return std::string((const char *)r.data, r.size);
}
std::string hex_of(const MIR_object_resource &r) {
	static const char *d = "0123456789abcdef";
	std::string h;
	const uint8_t *p = (const uint8_t *)r.data;
	for (size_t i = 0; i < r.size; i++) {
		h += d[p[i] >> 4];
		h += d[p[i] & 15];
	}
	return h;
}
} // namespace

TEST_CASE("PeIcon: each image is an RT_ICON (ids 1..n), then the group as "
	  "RT_GROUP_ICON 32512, language 0x409") {
	PeIcon icon;
	std::string err;
	REQUIRE(icon.parse(two_image_ico("abc", "hello"), err));
	const std::vector<MIR_object_resource> &r = icon.resources();
	REQUIRE(r.size() == 3);
	CHECK(r[0].type == 3);
	CHECK(r[0].id == 1);
	CHECK(r[0].lang == 0x409);
	CHECK(bytes_of(r[0]) == "abc");
	CHECK(r[1].type == 3);
	CHECK(r[1].id == 2);
	CHECK(bytes_of(r[1]) == "hello");
	CHECK(r[2].type == 14);
	CHECK(r[2].id == 32512);
	CHECK(r[2].lang == 0x409);
	// GRPICONDIR {0, 1, 2}, then each directory entry's first 12 bytes
	// with a u16 id where the file had a u32 offset.
	CHECK(hex_of(r[2]) == "000001000200"
			      "30301000" "0100" "0800" "03000000" "0100"
			      "10100000" "0100" "2000" "05000000" "0200");
}

TEST_CASE("PeIcon: what is not an icon file refuses, naming the fault") {
	std::string err;
	PeIcon a;
	std::vector<uint8_t> cursor = two_image_ico("abc", "hello");
	cursor[2] = 2;	// type 2 = a cursor file
	CHECK_FALSE(a.parse(cursor, err));
	CHECK(err.find("not an icon file") != std::string::npos);

	PeIcon b;
	CHECK_FALSE(b.parse(std::vector<uint8_t>{0, 0, 1}, err));

	PeIcon c;
	std::vector<uint8_t> none;
	put16(none, 0); put16(none, 1); put16(none, 0);
	CHECK_FALSE(c.parse(none, err));
	CHECK(err.find("no images") != std::string::npos);

	PeIcon d;
	std::vector<uint8_t> shortdir = two_image_ico("abc", "hello");
	shortdir.resize(6 + 16 + 4);	// the second entry is cut off
	CHECK_FALSE(d.parse(shortdir, err));
	CHECK(err.find("directory") != std::string::npos);

	PeIcon e;
	std::vector<uint8_t> past = two_image_ico("abc", "hello");
	past.pop_back();	// the second image runs past the end
	CHECK_FALSE(e.parse(past, err));
	CHECK(err.find("image 2") != std::string::npos);

	PeIcon f;
	std::vector<uint8_t> intodir = two_image_ico("abc", "hello");
	intodir[6 + 12] = 4;	// the first image's offset points into the directory
	CHECK_FALSE(f.parse(intodir, err));
	CHECK(err.find("image 1") != std::string::npos);
}

TEST_CASE("PeIcon: an icon with a PNG image matches windres's resources for it") {
	// The oracle: GNU windres 2.41.90 on `32512 ICON "app.ico"` (webview's
	// example icon, 13 images, the sixth a PNG), linked by
	// x86_64-w64-mingw32-gcc; the image's .rsrc read back holds RT_ICON 1..13
	// at these sizes and this RT_GROUP_ICON, language 1033.
	std::vector<uint8_t> file;
	const char *paths[] = { "../third_party/webview/examples/resources/windows/webview.ico",
				"third_party/webview/examples/resources/windows/webview.ico" };
	for (const char *p : paths) {
		FILE *fp = fopen(p, "rb");
		if (!fp) continue;
		int ch;
		while ((ch = fgetc(fp)) != EOF) file.push_back((uint8_t)ch);
		fclose(fp);
		break;
	}
	REQUIRE(file.size() == 55011);
	PeIcon icon;
	std::string err;
	REQUIRE(icon.parse(file, err));
	const std::vector<MIR_object_resource> &r = icon.resources();
	REQUIRE(r.size() == 14);
	const size_t sizes[] = { 744, 296, 3752, 2216, 1384, 3517, 16936,
				 9640, 6760, 4264, 2440, 1720, 1128 };
	for (size_t i = 0; i < 13; i++) {
		CHECK(r[i].type == 3);
		CHECK(r[i].id == i + 1);
		CHECK(r[i].size == sizes[i]);
	}
	CHECK(hex_of(r[13]) ==
	      "000001000d00"
	      "2020100001000400e802000001001010100001000400280100000200"
	      "3030000001000800a80e000003002020000001000800a80800000400"
	      "10100000010008006805000005000000000001002000bd0d00000600"
	      "40400000010020002842000007003030000001002000a82500000800"
	      "2828000001002000681a000009002020000001002000a81000000a00"
	      "1818000001002000880900000b001414000001002000b80600000c00"
	      "1010000001002000680400000d00");
}
