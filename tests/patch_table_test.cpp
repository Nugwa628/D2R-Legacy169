// Tests of the patch table and its expected-bytes logic (src/patch_table.h), without the game. With a D2R image dump
// given as the first argument (or env LEGACY169_IMAGE; file offset == RVA, as D2RLoader's dump writes it) the expected
// bytes are also checked against that image.
#include "../src/patch_table.h"
#include "../src/ui_layout.h"

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
	for (const auto& h : legacy169::Hooks) {
		std::memcpy(img.bytes.data() + h.rva, h.expected, h.size);
	}
	return img;
}

int CountWidth(uint32_t from, uint32_t to, const legacy169::PatchTable& table = legacy169::Patches) {
	int n = 0;
	for (const auto& p : table) {
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

	// --dump [W:H]: every patch (of the 16:9 table, or that screen's) as "rva size expected bytes" (hex), for a disassembler.
	if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) {
		int num = 16, den = 9;
		if (argc > 2 && !ParseAspect(argv[2], std::strlen(argv[2]), &num, &den)) {
			return 2;
		}
		for (const auto& p : BuildPatches(LayoutFor(num, den))) {
			char e[160], b[160];
			Hex(e, sizeof(e), p.expected, p.size);
			Hex(b, sizeof(b), p.bytes, p.size);
			std::printf("%llx|%u|%s|%s|%s\n", static_cast<unsigned long long>(p.rva), p.size, e, b, p.name);
		}
		return 0;
	}

	// The table itself.
	Check(PatchCount == 36, "36 patches: 10 mode-1 widths, 9 mode-0 widths, compose float and instruction, tile cull, 8 view shift, 6 automap");
	Check(CountWidth(800, 1066) == 10, "ten 800 -> 1066 immediates");
	Check(CountWidth(640, 853) == 9, "nine 640 -> 853 immediates");
	Check(NoOverlap(), "no two patches touch the same byte");
	for (const auto& p : Patches) {
		Check(p.size > 0 && p.size <= MaxPatchBytes, "patch size 1..48");
		Check(std::memcmp(p.expected, p.bytes, p.size) != 0, "every patch changes something");
	}
	Check(ClassicWidth1 * 9 <= ClassicHeight1 * 16 && (ClassicWidth1 + 1) * 9 > ClassicHeight1 * 16, "1066 = floor(600 * 16 / 9)");
	Check(ClassicWidth0 * 9 <= ClassicHeight0 * 16 && (ClassicWidth0 + 1) * 9 > ClassicHeight0 * 16, "853 = floor(480 * 16 / 9)");
	Check(AspectBits(Layout169) == 0x3fe369d0U, "compose aspect float is 1066/600 (0x3fe369d0)");

	// The compose instruction: old disp32 reads 4/3 at 0x1d65290, new one the cave.
	const Patch movss = ComposeMovss();
	Check(movss.rva == 0xeddec5 && movss.size == 8, "movss patch covers the whole 8-byte instruction");
	Check(RvaComposeNext + static_cast<int32_t>(Le32(movss.expected + 4)) == RvaFourThirds, "old disp32 points at the 4/3 float");
	Check(RvaComposeNext + static_cast<int32_t>(Le32(movss.bytes + 4)) == RvaAspectCave, "new disp32 points at the plugin's float");
	Check(Le32(movss.expected + 4) == 0x00e873c3U, "old disp32 is the dump's c3 73 e8 00");

	// Hook sites: none overlaps a patch, each covers at least the 14 bytes of an absolute jump.
	for (const auto& h : Hooks) {
		Check(h.size >= 14 && h.size <= MaxHookBytes, "hook entry covers 14..24 bytes");
		for (const auto& p : Patches) {
			Check(!(h.rva < p.rva + p.size && p.rva < h.rva + h.size), "no patch inside a hooked entry");
		}
	}

	// Tile cull bottom: ScreenSizeY + 40 instead of - margin; the old instruction reads the margin global.
	{
		const Patch& cull = Patches[21];
		Check(cull.rva == 0xf3c30 && 0xf3c36 + static_cast<int32_t>(Le32(cull.expected + 2)) == 0x22a0d78, "cull patch replaces the margin read");
		Check(cull.bytes[0] == 0x83 && cull.bytes[1] == 0xc1 && cull.bytes[2] == 40, "add ecx, 40");
	}
	// Panel view shift: the stock 4:3 values at the 16:9 widths.
	Check(Shift(Layout169, ClassicWidth1) == 800 / 4 && Shift(Layout169, ClassicWidth0) == 640 / 4, "view shift 200 / 160 at 1066 / 853");
	Check(Layout169.width1 == ClassicWidth1 && Layout169.width0 == ClassicWidth0, "16:9 layout is 1066 / 853");

	// Automap: the width function in padding and the two calls that go to it.
	{
		const Patch fn = AutomapWidthFn();
		Check(fn.size == AutomapWidthSize && fn.size <= MaxPatchBytes, "automap width function fits a patch");
		for (size_t i = 0; i < fn.size; ++i) {
			Check(fn.expected[i] == 0xcc, "automap width function expects int3 padding");
		}
		Check(RvaAutomapWidth + 9 + static_cast<int32_t>(Le32(fn.bytes + 5)) == RvaIsHd, "it calls the HD check");
		Check(RvaAutomapWidth + 22 + static_cast<int32_t>(Le32(fn.bytes + 18)) == RvaWidthGetter, "HD: it jumps to the stock width getter");
		Check(RvaAutomapWidth + 28 + static_cast<int32_t>(Le32(fn.bytes + 24)) == RvaScreenSizeX, "classic: it reads ScreenSizeX");
		Check(fn.bytes[15] == 0x74 && fn.bytes[16] == 22 - 17 && fn.bytes[40] == 0xc3, "jz skips the HD jump; ends with ret");
		for (uint64_t site : {RvaAutomapCall1, RvaAutomapCall2}) {
			const Patch c = AutomapCall("c", site);
			Check(site + 5 + static_cast<int32_t>(Le32(c.expected + 1)) == RvaWidthGetter, "old call goes to 0x7f510");
			Check(site + 5 + static_cast<int32_t>(Le32(c.bytes + 1)) == RvaAutomapWidth, "new call goes to the automap width function");
		}
		// what the panel adds at 16:9: (4 * shift) / 4 = the stock shift
		Check(fn.bytes[28] == 0x6b && fn.bytes[29] == 0xc0 && fn.bytes[30] == Layout169.shiftMul && fn.bytes[33] == Layout169.shiftAdd &&
		          fn.bytes[36] == Layout169.shiftSar, "classic: imul / add / sar with the layout's constants");
		// 0xb9a40: w / 2 with a panel open is X - 2 * shift, halved: X / 2 - shift (within 1)
		for (int w : {ClassicWidth1, ClassicWidth0}) {
			const int half = (w - TwiceShift(Layout169, w)) / 2;
			Check(half - (w / 2 - Shift(Layout169, w)) >= -1 && half - (w / 2 - Shift(Layout169, w)) <= 1, "automap centre: X / 2 - shift");
		}
	}

	// Panel anchors: one pair for both modes, truncated or rounded.
	for (bool t : {true, false}) {
		Check(PanelX(LeftAnchorX, ClassicWidth1, -320, t) == 80, "left panel at 80 in 1066 (beside the frame)");
		Check(PanelX(LeftAnchorX, ClassicWidth0, -320, t) == 0, "left panel at 0 in 853");
		Check(PanelX(RightAnchorX, ClassicWidth1, 0, t) == ClassicWidth1 - 400, "right panel at 666 in 1066 (frame to the edge)");
		Check(PanelX(RightAnchorX, ClassicWidth0, 0, t) == ClassicWidth0 - 320, "right panel at 533 in 853 (to the edge)");
		Check(PanelX(StockAnchorX, 800, -320, t) == 80 && PanelX(StockAnchorX, 800, 0, t) == 400, "stock 0.5 at 800 for reference");
	}

	// Layouts of other screens: 16:10 is 960x600 / 768x480; every screen from 4:3 to 16:9 gets a layout whose shift is the
	// stock 200 / 160 at both widths and whose panels sit where they belong, a few pixels narrower at most.
	{
		const Layout l = LayoutFor(16, 10);
		Check(l.width1 == 960 && l.width0 == 768, "16:10: 960 / 768");
		Check(LayoutFor(1680, 1050).width1 == 960 && LayoutFor(2560, 1600).width1 == 960, "16:10 monitors in pixels");
		Check(LayoutFor(21, 9).width1 == 1066 && LayoutFor(32, 9).width1 == 1066, "wider than 16:9: the 16:9 layout");
		Check(LayoutFor(4, 3).width1 == 800 && LayoutFor(5, 4).width1 == 800 && LayoutFor(4, 3).width0 == 640, "4:3 or narrower: stock");
		Check(LayoutFor(0, 0).width1 == 1066 && LayoutFor(3, 0).width1 == 1066, "no screen size: 16:9");
		Check(AspectBits(l) == FloatBits(960.0f / 600.0f), "16:10 compose float 1.6");
		const PatchTable t = BuildPatches(l);
		Check(CountWidth(800, 960, t) == 10 && CountWidth(640, 768, t) == 9, "16:10: ten 960 and nine 768 immediates");
		Check(NoOverlap(t.data(), t.size()), "16:10 table: no overlap");
		for (size_t i = 0; i < PatchCount; ++i) {
			Check(t[i].rva == Patches[i].rva && t[i].size == Patches[i].size && std::memcmp(t[i].expected, Patches[i].expected, t[i].size) == 0,
			      "16:10 table: same sites and expected bytes as 16:9");
		}
		int bad = 0, narrowed = 0;
		for (int den : {900, 1000, 1080, 1200}) {
			for (int num = den * 4 / 3; num <= den * 16 / 9 + 1; ++num) {
				const Layout x = LayoutFor(num, den);
				int want = static_cast<int>(600LL * num / den);
				want = want > ClassicWidth1 ? ClassicWidth1 : want < 800 ? 800 : want;
				const bool ok = x.width1 <= want && x.width1 >= want - 4 && Shift(x, x.width1) == 200 && Shift(x, x.width0) == 160 &&
				                TwiceShift(x, x.width1) - 400 <= 1 && 400 - TwiceShift(x, x.width1) <= 1 && TwiceShift(x, x.width0) - 320 <= 1 &&
				                320 - TwiceShift(x, x.width0) <= 1 && x.shiftMul <= 127 && x.shiftAdd <= 127 && x.twiceMul <= 127 &&
				                (x.width0 == x.width1 * 4 / 5 || x.width0 == (x.width1 * 4 + 4) / 5);
				bool panels = true;
				for (bool tr : {true, false}) {
					panels = panels && PanelX(x.leftAnchor, x.width1, -320, tr) == 80 && PanelX(x.leftAnchor, x.width0, -320, tr) == 0 &&
					         PanelX(x.rightAnchor, x.width1, 0, tr) == x.width1 - 400 && PanelX(x.rightAnchor, x.width0, 0, tr) == x.width0 - 320;
				}
				bad += !(ok && panels);
				narrowed += x.width1 != want;
			}
		}
		Check(bad == 0, "every screen from 4:3 to 16:9: exact shift, panels in place, at most 4 pixels narrower");
		std::printf("layouts: %d of the swept screens a few pixels narrower than 600 * aspect\n", narrowed);
		Check(WidenedWidth(1, 800, 600, 800, l) == 960 && WidenedWidth(0, 640, 480, 640, l) == 768, "16:10: set-up screen gets 960 / 768");
		Check(WidenedWidth(1, 800, 600, 800, LayoutFor(4, 3)) == 0, "4:3 layout: nothing to widen");
	}
	{
		int n = -1, d = -1;
		Check(ParseAspect("auto", 4, &n, &d) && n == 0 && d == 0, "aspect auto");
		Check(ParseAspect("16:10", 5, &n, &d) && n == 16 && d == 10, "aspect 16:10");
		Check(ParseAspect("1920x1200", 9, &n, &d) && n == 1920 && d == 1200, "aspect 1920x1200");
		Check(!ParseAspect("16:", 3, &n, &d) && !ParseAspect(":9", 2, &n, &d) && !ParseAspect("16:9:1", 6, &n, &d) && !ParseAspect("wide", 4, &n, &d) &&
		          !ParseAspect("0:9", 3, &n, &d),
		      "bad aspects refused");
	}

	// Profile map and rapidjson values (fake structures shaped like D2R's).
	{
		auto flags = [](uint8_t* v, uint16_t f) { std::memcpy(v + 14, &f, 2); };
		auto inlineName = [&](uint8_t* v, const char* n) {
			std::memset(v, 0, 16);
			std::strcpy(reinterpret_cast<char*>(v), n);
			flags(v, JsonStringFlag | JsonInlineString | 5);
		};
		auto number = [&](uint8_t* v, double d) {
			std::memcpy(v, &d, 8);
			flags(v, JsonDouble);
		};
		auto object = [&](uint8_t* v, uint8_t* members, uint32_t n) {
			std::memset(v, 0, 16);
			std::memcpy(v, &n, 4);
			const uint64_t ptr = reinterpret_cast<uint64_t>(members);
			std::memcpy(v + 8, &ptr, 8);
			flags(v, JsonObject);
		};
		alignas(16) uint8_t membersL[64] = {}, membersR[64] = {};
		inlineName(membersL, "x");
		number(membersL + 16, 0.5);
		inlineName(membersL + 32, "y");
		number(membersL + 48, 0.5);
		inlineName(membersR, "y");
		number(membersR + 16, 0.5);
		inlineName(membersR + 32, "x");
		const int32_t zero = 0;  // an integer x: read, but only 0.5 is replaced
		std::memcpy(membersR + 48, &zero, 4);
		flags(membersR + 48, JsonNumberType | JsonNumberFlag | 0x20 | 0x40 | 0x80 | 0x100);
		ProfileNode left{}, right{}, other{};
		const char* kl = "$LeftPanelAnchor";
		const char* kr = "$RightPanelAnchor";
		const char* ko = "$FontColorWhite";
		left.key = kl;
		left.keySize = std::strlen(kl);
		right.key = kr;
		right.keySize = std::strlen(kr);
		other.key = ko;
		other.keySize = std::strlen(ko);
		object(left.value, membersL, 2);
		object(right.value, membersR, 2);
		object(other.value, membersL, 2);
		other.next = &left;  // a chain in one bucket
		uint8_t* slots[4] = {reinterpret_cast<uint8_t*>(&other), nullptr, nullptr, reinterpret_cast<uint8_t*>(&right)};
		ProfileMap maps[2] = {};
		maps[1] = ProfileMap{4, slots, 3, 1.0f};
		Check(FindProfileValue(&maps[1], "$LeftPanelAnchor") == left.value, "key found along a bucket chain");
		Check(FindProfileValue(&maps[1], "$Left") == nullptr && FindProfileValue(&maps[0], kl) == nullptr, "prefix / empty map: not found");

		AnchorResult r = PatchPanelAnchors(maps, ProfileSd);
		double x;
		std::memcpy(&x, membersL + 16, 8);
		Check(r.changed == 1 && r.oldLeft == 0.5 && x == LeftAnchorX, "left x 0.5 -> 0.3755");
		std::memcpy(&x, membersL + 48, 8);
		Check(x == 0.5, "y untouched");
		Check(r.oldRight == 0.0 && JsonFlags(membersR + 48) != JsonDouble, "a non-stock right x (0) is left alone");

		number(membersR + 48, 0.5);
		r = PatchPanelAnchors(maps, ProfileSd);
		std::memcpy(&x, membersR + 48, 8);
		Check(r.changed == 1 && r.already == 1 && x == RightAnchorX, "second run: right rewritten, left already set");
		r = PatchPanelAnchors(maps, ProfileSd);
		Check(r.changed == 0 && r.already == 2, "third run changes nothing");
		number(membersL + 16, 0.5);
		r = PatchPanelAnchors(maps, ProfileSd, 0.4169, 0.5836);
		std::memcpy(&x, membersL + 16, 8);
		Check(r.changed == 1 && r.already == 0 && x == 0.4169, "a layout's own anchors (16:10)");

		flags(right.value, 0);  // not an object any more
		Check(PatchPanelAnchors(maps, ProfileSd).changed == 0 && JsonNumberMember(right.value, "x") == nullptr, "non-object value refused");
		Check(PatchPanelAnchors(nullptr, ProfileSd).changed == 0, "no map array: nothing");
	}

	// BordersPanel right pieces and click catcher.
	{
		alignas(16) uint8_t panel[0x220] = {};
		alignas(16) uint8_t catcher[0x80] = {};
		const int xs[5] = {400, 544, 713, 544, 400};
		for (int i = 0; i < 5; ++i) {
			std::memcpy(panel + BordersRightPositions + 8 * i, &xs[i], 4);
		}
		int n = 5;
		std::memcpy(panel + BordersRightCount, &n, 4);
		uint8_t* cp = catcher;
		std::memcpy(panel + BordersRightCatcher, &cp, 8);
		Check(ShiftBordersRight(panel, 266), "shift 5 pieces");
		int x0, x2;
		std::memcpy(&x0, panel + BordersRightPositions, 4);
		std::memcpy(&x2, panel + BordersRightPositions + 16, 4);
		Check(x0 == 666 && x2 == 979, "pieces moved by 266");
		ShiftBordersRight(panel, -266);
		std::memcpy(&x0, panel + BordersRightPositions, 4);
		Check(x0 == 400, "and back");
		n = 6;
		std::memcpy(panel + BordersRightCount, &n, 4);
		Check(!ShiftBordersRight(panel, 266), "count above 5 refused");
		Check(PlaceRightCatcher(panel, 266, 0), "catcher from 0 to 266");
		int cx;
		std::memcpy(&cx, catcher + WidgetRectX, 4);
		Check(cx == 266 && PlaceRightCatcher(panel, 266, 266), "catcher at 266, again idempotent");
		const int own = 50;
		std::memcpy(catcher + WidgetRectX, &own, 4);
		Check(!PlaceRightCatcher(panel, 266, 266), "a catcher with its own rect is left alone");
	}

	// Expected-bytes check on a fake image.
	{
		Image img = GoodImage();
		const CheckResult r = CheckAll(&Read, &img);
		Check(r.ok && r.failed == PatchCount, "good image passes");
		Check(img.reads == PatchCount, "every patch read once");
		Check(CheckHooks(&Read, &img).ok, "good image: hook entries pass");
		img.bytes[RvaBordersDraw + 3] = 0xe9;  // another plugin hooked it
		const CheckResult h = CheckHooks(&Read, &img);
		Check(!h.ok && Hooks[h.failed].rva == RvaBordersDraw && h.found[3] == 0xe9, "a changed hook entry is refused and reported");
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
		Check(d.aspectNum == 0 && d.aspectDen == 0, "aspect default auto");
		const Config a = ParseConfig("[legacy169]\naspect = \"16:10\"  # monitor\n");
		Check(a.aspectNum == 16 && a.aspectDen == 10, "aspect = \"16:10\" read");
		const Config b = ParseConfig("[legacy169]\naspect = 1920x1200\n");
		Check(b.aspectNum == 1920 && b.aspectDen == 1200, "aspect without quotes read");
		const Config w = ParseConfig("[legacy169]\naspect = \"wide\"\n");
		Check(w.aspectNum == 0 && w.aspectDen == 0, "a bad aspect keeps auto");
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
			const CheckResult h = CheckHooks(&Read, &img);
			if (!h.ok) {
				std::printf("image: hook mismatch at %s\n", Hooks[h.failed].name);
			}
			Check(h.ok, "every hook entry matches the D2R 93847 image");
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
