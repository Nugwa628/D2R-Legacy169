// Tests of the patch table and its expected-bytes logic (src/patch_table.h), without the game. With a D2R image dump
// given as the first argument (or env LEGACY169_IMAGE; file offset == RVA, as D2RLoader's dump writes it) the expected
// bytes are also checked against that image.
#include "../src/patch_table.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
	if (!ok) {
		++failures;
		std::printf("FAIL: %s\n", what);
	}
}

struct Image {
	std::vector<uint8_t> bytes;
	size_t reads = 0;
};

bool Read(void* user, uint64_t rva, uint8_t* out, size_t size) {
	auto* img = static_cast<Image*>(user);
	++img->reads;
	if (rva + size > img->bytes.size()) {
		return false;
	}
	std::memcpy(out, img->bytes.data() + rva, size);
	return true;
}

// A fake image holding every patch's expected bytes (the rest zero).
Image GoodImage() {
	Image img;
	img.bytes.assign(0x1e00000, 0);
	for (const auto& p : legacy169::Patches) {
		std::memcpy(img.bytes.data() + p.rva, p.expected, p.size);
	}
	return img;
}

int CountWidth(uint32_t from, uint32_t to) {
	int n = 0;
	for (const auto& p : legacy169::Patches) {
		if (p.size == 4 && legacy169::Le32(p.expected) == from && legacy169::Le32(p.bytes) == to) {
			++n;
		}
	}
	return n;
}

bool LoadFile(const char* path, Image& img) {
	FILE* f = std::fopen(path, "rb");
	if (f == nullptr) {
		return false;
	}
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	img.bytes.resize(static_cast<size_t>(size));
	const bool ok = std::fread(img.bytes.data(), 1, img.bytes.size(), f) == img.bytes.size();
	std::fclose(f);
	return ok;
}
}  // namespace

int main(int argc, char** argv) {
	using namespace legacy169;

	// The table itself.
	Check(PatchCount == 21, "21 patches: 10 mode-1 widths, 9 mode-0 widths, compose float and instruction");
	Check(CountWidth(800, 1066) == 10, "ten 800 -> 1066 immediates");
	Check(CountWidth(640, 853) == 9, "nine 640 -> 853 immediates");
	Check(NoOverlap(), "no two patches touch the same byte");
	for (const auto& p : Patches) {
		Check(p.size > 0 && p.size <= MaxPatchBytes, "patch size 1..8");
		Check(std::memcmp(p.expected, p.bytes, p.size) != 0, "every patch changes something");
	}
	Check(ClassicWidth1 * 9 <= ClassicHeight1 * 16 && (ClassicWidth1 + 1) * 9 > ClassicHeight1 * 16, "1066 = floor(600 * 16 / 9)");
	Check(ClassicWidth0 * 9 <= ClassicHeight0 * 16 && (ClassicWidth0 + 1) * 9 > ClassicHeight0 * 16, "853 = floor(480 * 16 / 9)");
	Check(AspectBits == 0x3fe369d0U, "compose aspect float is 1066/600 (0x3fe369d0)");

	// The compose instruction: old disp32 reads 4/3 at 0x1d65290, new one the cave.
	const Patch movss = ComposeMovss();
	Check(movss.rva == 0xeddec5 && movss.size == 8, "movss patch covers the whole 8-byte instruction");
	Check(RvaComposeNext + static_cast<int32_t>(Le32(movss.expected + 4)) == RvaFourThirds, "old disp32 points at the 4/3 float");
	Check(RvaComposeNext + static_cast<int32_t>(Le32(movss.bytes + 4)) == RvaAspectCave, "new disp32 points at the plugin's float");
	Check(Le32(movss.expected + 4) == 0x00e873c3U, "old disp32 is the dump's c3 73 e8 00");

	// Expected-bytes check on a fake image.
	{
		Image img = GoodImage();
		const CheckResult r = CheckAll(&Read, &img);
		Check(r.ok && r.failed == PatchCount, "good image passes");
		Check(img.reads == PatchCount, "every patch read once");
	}
	{
		Image img = GoodImage();
		img.bytes[0x65b897] = 0x21;  // another build or another plugin's patch at one site
		const CheckResult r = CheckAll(&Read, &img);
		Check(!r.ok, "one changed byte refuses everything");
		Check(r.failed < PatchCount && Patches[r.failed].rva == 0x65b897, "the mismatch names the site");
		Check(r.found[0] == 0x21 && !r.unreadable, "the found bytes are reported");
	}
	{
		Image img = GoodImage();
		std::memcpy(img.bytes.data() + 0x7f18b, "\x2a\x04\x00\x00", 4);  // already patched (a second copy of the plugin)
		Check(!CheckAll(&Read, &img).ok, "an already patched site refuses (no double patch)");
	}
	{
		Image img = GoodImage();
		img.bytes[RvaAspectCave + 2] = 0x90;  // the padding is used by something
		const CheckResult r = CheckAll(&Read, &img);
		Check(!r.ok && Patches[r.failed].rva == RvaAspectCave, "used padding at the float's place refuses");
	}
	{
		Image img = GoodImage();
		img.bytes.resize(0x7f000);  // image too small: unreadable
		const CheckResult r = CheckAll(&Read, &img);
		Check(!r.ok && r.unreadable, "unreadable range refuses");
	}
	{
		const Patch overlapping[] = {Imm32("a", 0x100, 1, 2), Imm32("b", 0x103, 1, 2)};
		Check(!NoOverlap(overlapping, 2), "overlap is detected");
		const Patch adjacent[] = {Imm32("a", 0x100, 1, 2), Imm32("b", 0x104, 1, 2)};
		Check(NoOverlap(adjacent, 2), "adjacent patches do not overlap");
	}
	{
		char hex[32];
		const uint8_t b[] = {0x20, 0x03, 0x00, 0xab};
		Hex(hex, sizeof(hex), b, 4);
		Check(std::strcmp(hex, "20 03 00 ab") == 0, "hex dump of found bytes");
	}

	// Width globals of a screen D2R set up before the plugin loaded.
	Check(WidenedWidth(1, 800, 600, 800) == 1066 && WidenedWidth(0, 640, 480, 640) == 853, "stock sizes get the 16:9 width");
	Check(WidenedWidth(1, 0, 0, 0) == 0, "not set up yet: left alone (the patched setter writes it)");
	Check(WidenedWidth(1, 1066, 600, 1066) == 0 && WidenedWidth(0, 800, 600, 800) == 0, "already wide or mode mismatch: left alone");
	Check(WidenedWidth(1, 800, 600, 1024) == 0 && WidenedWidth(2, 800, 600, 800) == 0, "foreign values or unknown mode: left alone");

	// Build check.
	Check(BuildMatches(0x6ab3782cU, "93847") && BuildMatches(0x6ab3782cU, nullptr) && BuildMatches(0x6ab3782cU, ""), "build 93847 matches");
	Check(!BuildMatches(0x6ab3782dU, "93847") && !BuildMatches(0x6ab3782cU, "94100") && !BuildMatches(0x6ab3782cU, "9384"), "other builds refused");

	// Config.
	{
		const Config d = ParseConfig(nullptr);
		Check(d.enabled && d.resetScreen, "no config: defaults on");
		const Config off = ParseConfig("# c\n[legacy169]\nenabled = false   # off\r\nreset_screen=false\n");
		Check(!off.enabled && !off.resetScreen, "both keys read, comments and CRLF ignored");
		const Config other = ParseConfig("[other]\nenabled = false\n[legacy169]\n");
		Check(other.enabled, "keys of another table are ignored");
		const Config bad = ParseConfig("[legacy169]\nenabled = 0\nreset_screen = \"no\"\n");
		Check(bad.enabled && bad.resetScreen, "values other than true/false keep the defaults");
		const Config later = ParseConfig("[legacy169]\nenabled = false\n[d2rl]\nenabled = true\n");
		Check(!later.enabled, "a later table does not change legacy169's keys");
	}

	// Optional: the real image.
	const char* path = argc > 1 ? argv[1] : std::getenv("LEGACY169_IMAGE");
	if (path != nullptr && path[0] != '\0') {
		Image img;
		if (!LoadFile(path, img)) {
			Check(false, "image file given but unreadable");
		} else {
			const CheckResult r = CheckAll(&Read, &img);
			if (!r.ok) {
				std::printf("image: mismatch at %s (0x%llx)\n", Patches[r.failed].name, static_cast<unsigned long long>(Patches[r.failed].rva));
			}
			Check(r.ok, "every expected range matches the D2R 93847 image");
			Check(std::memcmp(img.bytes.data() + RvaFourThirds, "\xab\xaa\xaa\x3f", 4) == 0, "the image's 4/3 float is where the movss read it");
			for (uint64_t a = RvaAspectCave - 6; a < RvaAspectCave + 8; ++a) {
				Check(img.bytes[a] == 0xcc, "the float sits inside int3 padding (0xedef42..0xedef4f)");
			}
			std::printf("image checked: %s\n", path);
		}
	}

	if (failures == 0) {
		std::printf("legacy169_tests: all passed\n");
	}
	return failures == 0 ? 0 : 1;
}
