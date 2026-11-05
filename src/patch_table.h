// The byte patches of legacy169 and the expected-bytes logic around them. Pure code: no Windows or D2RLoader headers, so
// tests/patch_table_test.cpp checks it without the game.
//
// D2R draws the classic (legacy) graphics into a fixed SD target, 640x480 (resolution mode 0) or 800x600 (mode 1), and
// composes it into the window with a 4:3 fit. The compose pass fits the picture to the window height and centres it: a
// wider window gets side bars, a narrower one loses the sides of the picture. legacy169 widens both sizes to the aspect of
// the screen (16:9 at most, 4:3 at least) at the same height: 1066x600 / 853x480 at 16:9, 960x600 / 768x480 at 16:10. The
// widths are chosen once at load (Layout below), so every site is built for them:
//   - mode 1: 800 -> width1 in the client screen size (SetScreenResolution 0x7f170 and its inline copy at start-up 0x7e7b5)
//     and in the SD renderer's size switches (one per function, each `call GetResolutionMode; ...; mov reg, 800`);
//   - mode 0: 640 -> width0 in the same functions (the start-up copy only knows mode 1);
//   - the compose pass (0xedde70) divides by a 4:3 float at 0x1d65290 that other code shares, so its movss is pointed at
//     a plugin-owned float (width1 / 600) written into int3 padding after a function in .text.
// The bottom control panel keeps its own 800 / 640 (its widget, 0x14c9430, is not patched), so it stays 4:3 and centred.
// Around the wider screen:
//   - the tile cull rectangle (SetViewRectangleSize 0xf3c00) ends at ScreenSizeY - 47, which the 800-wide control panel
//     hid; at 1066 the bottom tile row was missing left and right of it. It now ends at ScreenSizeY + 40 (as SGD2FreeRes);
//   - an open side panel shifts the view by a quarter of the width, 266 at 1066: the draw window (0xb9c20, ViewSizeX / 4)
//     and the view offset helpers (0xb9ad0 and 0xb9b20, ScreenSizeX / 4; the world, units and the SD renderer use them).
//     The panels keep their 4:3 width, so the shift is put back to the stock 200 / 160: (width * mul + add) >> sar, with
//     the three constants of the layout (at 16:9 (3 * width + 9) >> 4).
//   - the automap overlay follows the panel shift twice: its centre point in the world (0xb9a40, by ScreenSizeX / 2 and
//     / 4) and the screen point it is drawn at (AutoMapPanel::Draw 0x14e3480, by the width getter 0x7f510 / 4). Both get
//     the stock 4:3 relation back: 0xb9a40 in place, and the two width calls of the panel go to a small function in int3
//     padding that returns 4 * the stock shift in classic mode and the HD width as before.
// The side panels themselves move through two hooks (ui_layout.h).
// All RVAs are for D2R 3.3 build 93847 under D2RLoader 1.3.1 (image stamp 0x6ab3782c).
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace legacy169 {

constexpr uint32_t ExpectedImage = 0x6ab3782cU;  // TimeDateStamp of D2R.exe 3.3 build 93847 as D2RLoader 1.3.1 runs it
constexpr const char* ExpectedBuild = "93847";

constexpr int ClassicHeight1 = 600;
constexpr int ClassicWidth1 = 1066;  // 16:9: 600 * 16 / 9 = 1066.7, rounded down
constexpr int ClassicHeight0 = 480;
constexpr int ClassicWidth0 = 853;  // 16:9: 480 * 16 / 9 = 853.3

// Everything that depends on the widths. The panels keep their 4:3 size, so the stock numbers stay: a side panel shifts
// the view by 200 (mode 1) / 160 (mode 0), the left panel sits at 80 / 0 (beside the 80-pixel 800x600 frame in mode 1),
// the right one ends 80 / 0 before the right edge.
struct Layout {
	int width1, width0;
	int shiftMul, shiftAdd, shiftSar;  // stock shift = (width * mul + add) >> sar, exactly, at both widths
	int twiceMul, twiceSar;            // 2 * stock shift = (width * mul) >> sar, within 1 (the automap centre)
	double leftAnchor, rightAnchor;    // _profileSD.json panel anchor x (ui_layout.h)
};

// Where a panel lands: D2R computes the position in float; t = truncate, else round to nearest.
constexpr int PanelX(double anchor, int parentWidth, int rectX, bool truncate) {
	const float x = static_cast<float>(anchor) * static_cast<float>(parentWidth) + static_cast<float>(rectX);
	return truncate ? static_cast<int>(x) : static_cast<int>(x + 0.5f);
}

constexpr int Shift(const Layout& l, int width) {
	return (width * l.shiftMul + l.shiftAdd) >> l.shiftSar;
}
constexpr int TwiceShift(const Layout& l, int width) {
	return (width * l.twiceMul) >> l.twiceSar;
}

// One anchor that puts a panel at `x1` in width1 and at `x0` in width0 (the panel's rect x is rectX), whether D2R
// truncates or rounds. False when the two ranges do not meet.
constexpr bool FindAnchor(int w1, int x1, int w0, int x0, int rectX, double* out) {
	const double lo1 = static_cast<double>(x1 - rectX) / w1, hi1 = (x1 - rectX + 0.5) / w1;
	const double lo0 = static_cast<double>(x0 - rectX) / w0, hi0 = (x0 - rectX + 0.5) / w0;
	const double lo = lo1 > lo0 ? lo1 : lo0, hi = hi1 < hi0 ? hi1 : hi0;
	if (lo >= hi) {
		return false;
	}
	// the middle of the range, to 4 decimals (0.3755 / 0.625 at 16:9)
	const double a = static_cast<double>(static_cast<int64_t>((lo + hi) / 2 * 10000 + 0.5)) / 10000;
	for (bool t : {true, false}) {
		if (PanelX(a, w1, rectX, t) != x1 || PanelX(a, w0, rectX, t) != x0) {
			return false;
		}
	}
	*out = a;
	return true;
}

// The constants of a width pair, or false when the patch encodings cannot reach them (imul with an 8-bit factor, an
// 8-bit add, a shift count).
constexpr bool MakeLayout(int w1, int w0, Layout* out) {
	Layout l{w1, w0, 0, 0, 0, 0, 0, 0.0, 0.0};
	bool found = false;
	for (int sar = 2; sar <= 12 && !found; ++sar) {
		for (int mul = 1; mul <= 127 && !found; ++mul) {
			// add range for (w * mul + add) >> sar == shift at both widths
			const int lo1 = (200 << sar) - w1 * mul, hi1 = (201 << sar) - 1 - w1 * mul;
			const int lo0 = (160 << sar) - w0 * mul, hi0 = (161 << sar) - 1 - w0 * mul;
			int lo = lo1 > lo0 ? lo1 : lo0, hi = hi1 < hi0 ? hi1 : hi0;
			lo = lo < 0 ? 0 : lo;
			hi = hi > 127 ? 127 : hi;
			if (lo <= hi) {
				l.shiftMul = mul;
				l.shiftAdd = (lo + hi) / 2;
				l.shiftSar = sar;
				found = true;
			}
		}
	}
	if (!found) {
		return false;
	}
	int best = 2;
	for (int sar = 2; sar <= 12; ++sar) {
		for (int mul = 1; mul <= 127; ++mul) {
			const int e1 = ((w1 * mul) >> sar) - 400, e0 = ((w0 * mul) >> sar) - 320;
			const int e = (e1 < 0 ? -e1 : e1) > (e0 < 0 ? -e0 : e0) ? (e1 < 0 ? -e1 : e1) : (e0 < 0 ? -e0 : e0);
			if (e < best) {
				best = e;
				l.twiceMul = mul;
				l.twiceSar = sar;
			}
		}
	}
	if (best > 1) {
		return false;
	}
	if (!FindAnchor(w1, 80, w0, 0, -320, &l.leftAnchor) || !FindAnchor(w1, w1 - 400, w0, w0 - 320, 0, &l.rightAnchor)) {
		return false;
	}
	*out = l;
	return true;
}

// The layout for a screen of num:den (pixels or a ratio). Clamped to 4:3 .. 16:9: a wider screen keeps side bars, a
// narrower one gets the stock 800 / 640. width1 = 600 * num / den rounded down; width0 = 4 / 5 of it (rounded down, or up
// when that fits). When no constants fit a pair, the next narrower width is tried (a few pixels of side bar, never a cut).
constexpr Layout LayoutFor(int num, int den) {
	int w1 = ClassicWidth1;
	if (num > 0 && den > 0 && static_cast<int64_t>(num) * 9 < static_cast<int64_t>(den) * 16) {
		w1 = static_cast<int>(static_cast<int64_t>(ClassicHeight1) * num / den);
	}
	w1 = w1 < 800 ? 800 : w1;
	for (; w1 >= 800; --w1) {
		Layout l{};
		if (MakeLayout(w1, (w1 * 4 + 4) / 5, &l) || MakeLayout(w1, w1 * 4 / 5, &l)) {
			return l;
		}
	}
	return Layout{800, 640, 1, 0, 2, 1, 1, 0.5, 0.5};  // not reached: 800 / 640 is the stock layout
}

constexpr Layout Layout169 = LayoutFor(16, 9);
constexpr double LeftAnchorX = Layout169.leftAnchor;
constexpr double RightAnchorX = Layout169.rightAnchor;
constexpr double StockAnchorX = 0.5;

// config `aspect`: "auto" (0:0, the screen the game is on), or "W:H" / "WxH". False for anything else.
constexpr bool ParseAspect(const char* s, size_t len, int* num, int* den) {
	if (len == 4 && s[0] == 'a' && s[1] == 'u' && s[2] == 't' && s[3] == 'o') {
		*num = *den = 0;
		return true;
	}
	int v[2] = {0, 0}, k = 0;
	for (size_t i = 0; i < len; ++i) {
		if (s[i] >= '0' && s[i] <= '9') {
			if (v[k] > 100000) {
				return false;
			}
			v[k] = v[k] * 10 + (s[i] - '0');
		} else if ((s[i] == ':' || s[i] == 'x') && k == 0 && i > 0) {
			k = 1;
		} else {
			return false;
		}
	}
	if (k != 1 || v[0] == 0 || v[1] == 0) {
		return false;
	}
	*num = v[0];
	*den = v[1];
	return true;
}

// Game globals the plugin reads for its log (written by 0x7f170 / 0x7e7b5).
constexpr uint64_t RvaScreenSizeX = 0x22a0d68;
constexpr uint64_t RvaScreenSizeY = 0x22a0d6c;
constexpr uint64_t RvaViewSizeX = 0x22a0d70;  // view width, = screen width (the view height is screen height - 40)

// The width a set-up screen gets: D2R sets its classic screen up before plugins load, so the patched setters have not run
// yet. For the stock width of the current mode the layout's width, else 0 (leave the globals alone: not D2R's own value).
constexpr int WidenedWidth(int mode, int screenX, int screenY, int viewX, const Layout& l = Layout169) {
	if (mode == 1 && screenX == 800 && screenY == ClassicHeight1 && viewX == 800 && l.width1 != 800) {
		return l.width1;
	}
	if (mode == 0 && screenX == 640 && screenY == ClassicHeight0 && viewX == 640 && l.width0 != 640) {
		return l.width0;
	}
	return 0;
}
constexpr uint64_t RvaSetScreenResolution = 0x7f170;  // void (int mode); returns at once when mode is the current one
constexpr uint64_t RvaGetResolutionMode = 0x901750;   // int (void)

constexpr size_t MaxPatchBytes = 48;  // the automap function in padding; the rest are up to 12

struct Patch {
	const char* name;
	uint64_t rva;
	uint8_t size;
	uint8_t expected[MaxPatchBytes];
	uint8_t bytes[MaxPatchBytes];
};

constexpr uint32_t Le32(const uint8_t* p) {
	return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
	       static_cast<uint32_t>(p[3]) << 24;
}

// A 4-byte immediate (or other dword) `from` -> `to` at rva.
constexpr Patch Imm32(const char* name, uint64_t rva, uint32_t from, uint32_t to) {
	return Patch{name, rva, 4,
	             {static_cast<uint8_t>(from), static_cast<uint8_t>(from >> 8), static_cast<uint8_t>(from >> 16), static_cast<uint8_t>(from >> 24)},
	             {static_cast<uint8_t>(to), static_cast<uint8_t>(to >> 8), static_cast<uint8_t>(to >> 16), static_cast<uint8_t>(to >> 24)}};
}

// Compose constant. `movss xmm1, [rip+disp32]` at 0xeddec5 (f3 0f 10 0d, disp32 at +4, next instruction 0xeddecd) reads
// 4/3 at 0x1d65290. The float 1066/600 goes into int3 padding at 0xedef48 (0xedef42..0xedef4f is padding after a function),
// and the disp32 is pointed there. The whole instruction is the expected range, so a moved or changed instruction refuses.
constexpr uint64_t RvaComposeMovss = 0xeddec5;
constexpr uint64_t RvaComposeNext = 0xeddecd;
constexpr uint64_t RvaFourThirds = 0x1d65290;
constexpr uint64_t RvaAspectCave = 0xedef48;
constexpr uint32_t FloatBits(float f) {
	return std::bit_cast<uint32_t>(f);
}
constexpr uint32_t AspectBits(const Layout& l) {  // 0x3fe369d0 at 16:9
	return FloatBits(static_cast<float>(l.width1) / static_cast<float>(ClassicHeight1));
}

constexpr uint32_t Disp(uint64_t next, uint64_t target) {
	return static_cast<uint32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(next));
}

constexpr Patch ComposeMovss() {
	constexpr uint32_t from = Disp(RvaComposeNext, RvaFourThirds);
	constexpr uint32_t to = Disp(RvaComposeNext, RvaAspectCave);
	return Patch{"compose aspect movss", RvaComposeMovss, 8,
	             {0xf3, 0x0f, 0x10, 0x0d, static_cast<uint8_t>(from), static_cast<uint8_t>(from >> 8), static_cast<uint8_t>(from >> 16), static_cast<uint8_t>(from >> 24)},
	             {0xf3, 0x0f, 0x10, 0x0d, static_cast<uint8_t>(to), static_cast<uint8_t>(to >> 8), static_cast<uint8_t>(to >> 16), static_cast<uint8_t>(to >> 24)}};
}

// Automap. 0xb9a40 returns the world point the automap centres: ViewOffsetX + w / 2 - q, where w = ScreenSizeX
// (halved while a side panel is open) and q = +-ScreenSizeX / 4 by the panel state. Stock (4:3) the point is the player
// for one panel state and 2 * shift left of it for the other; wider, w / 2 must be ScreenSizeX / 2 - shift and q the shift:
//   0xb9a5a `mov eax, r9d; cdq; sub eax, edx; sar eax, 1; mov r9d, eax` (r9 = ScreenSizeX / 2)
//        -> `imul eax, r9d, twiceMul; sar eax, twiceSar; sub r9d, eax; nop` (r9 = X - 2 * shift, 666 at 1066, 533 at 853)
//   0xb9a79 / 0xb9a8a `cdq; and edx, 3; lea r10d, [rdx + rax]; sar r10d, 2` (eax = ScreenSizeX)
//        -> `imul r10d, eax, shiftMul; add r10d, shiftAdd; sar r10d, shiftSar` (the stock 200 / 160)
// AutoMapPanel::Draw adds -+ width / 4 to the screen point (width = 0x7f510: the HD width in HD, else ScreenSizeX). Its two
// `call 0x7f510` (0x14e36ce, 0x14e36de) call AutomapWidth in padding at 0xa16900 (0xa168e7..0xa16a2f is int3 after a jmp):
//   sub rsp, 0x28; call 0x846210 (HD?); add rsp, 0x28; test al, al; jz classic; jmp 0x7f510 (HD: as before)
//   classic: mov eax, [ScreenSizeX]; imul eax, eax, shiftMul; add eax, shiftAdd; sar eax, shiftSar; shl eax, 2; ret
//   (4 * the shift)
constexpr uint64_t RvaAutomapCentre = 0xb9a40;
constexpr uint64_t RvaAutomapWidth = 0xa16900;
constexpr uint64_t RvaWidthGetter = 0x7f510;
constexpr uint64_t RvaIsHd = 0x846210;
constexpr uint64_t RvaAutomapCall1 = 0x14e36ce;
constexpr uint64_t RvaAutomapCall2 = 0x14e36de;
constexpr size_t AutomapWidthSize = 41;

constexpr void Put32(uint8_t* p, uint32_t v) {
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
	p[2] = static_cast<uint8_t>(v >> 16);
	p[3] = static_cast<uint8_t>(v >> 24);
}

constexpr Patch AutomapWidthFn(const Layout& l = Layout169) {
	Patch p{"automap width (int3 padding)", RvaAutomapWidth, AutomapWidthSize, {}, {}};
	const uint8_t code[AutomapWidthSize] = {
	    0x48, 0x83, 0xec, 0x28,        // sub rsp, 0x28
	    0xe8, 0, 0, 0, 0,              // +4 call IsHd
	    0x48, 0x83, 0xc4, 0x28,        // +9 add rsp, 0x28
	    0x84, 0xc0,                    // +13 test al, al
	    0x74, 0x05,                    // +15 jz +22
	    0xe9, 0, 0, 0, 0,              // +17 jmp 0x7f510
	    0x8b, 0x05, 0, 0, 0, 0,        // +22 mov eax, [ScreenSizeX]
	    0x6b, 0xc0, 0,                 // +28 imul eax, eax, shiftMul
	    0x83, 0xc0, 0,                 // +31 add eax, shiftAdd
	    0xc1, 0xf8, 0,                 // +34 sar eax, shiftSar
	    0xc1, 0xe0, 0x02,              // +37 shl eax, 2
	    0xc3,                          // +40 ret
	};
	for (size_t i = 0; i < AutomapWidthSize; ++i) {
		p.expected[i] = 0xcc;
		p.bytes[i] = code[i];
	}
	Put32(p.bytes + 5, Disp(RvaAutomapWidth + 9, RvaIsHd));
	Put32(p.bytes + 18, Disp(RvaAutomapWidth + 22, RvaWidthGetter));
	Put32(p.bytes + 24, Disp(RvaAutomapWidth + 28, RvaScreenSizeX));
	p.bytes[30] = static_cast<uint8_t>(l.shiftMul);
	p.bytes[33] = static_cast<uint8_t>(l.shiftAdd);
	p.bytes[36] = static_cast<uint8_t>(l.shiftSar);
	return p;
}

// `call 0x7f510` at `site` -> `call AutomapWidth`.
constexpr Patch AutomapCall(const char* name, uint64_t site) {
	Patch p{name, site, 5, {0xe8}, {0xe8}};
	Put32(p.expected + 1, Disp(site + 5, RvaWidthGetter));
	Put32(p.bytes + 1, Disp(site + 5, RvaAutomapWidth));
	return p;
}

constexpr size_t PatchCount = 36;
using PatchTable = std::array<Patch, PatchCount>;

// The table for a layout. Order does not matter for correctness (all of it goes in as one transaction, or none of it),
// but the float is listed before the instruction that reads it.
constexpr PatchTable BuildPatches(const Layout& l) {
	const uint32_t w1 = static_cast<uint32_t>(l.width1), w0 = static_cast<uint32_t>(l.width0);
	const auto m = static_cast<uint8_t>(l.shiftMul), c = static_cast<uint8_t>(l.shiftAdd), s = static_cast<uint8_t>(l.shiftSar);
	const auto tm = static_cast<uint8_t>(l.twiceMul), ts = static_cast<uint8_t>(l.twiceSar);
	return PatchTable{{
	    // mode 1, 800 -> width1
	    Imm32("SetScreenResolution mode 1 width", 0x7f18b, 800, w1),
	    Imm32("start-up screen width", 0x7e7bb, 800, w1),
	    Imm32("start-up view width", 0x7e7d5, 800, w1),
	    Imm32("SD target set-up mode 1 width", 0x65b897, 800, w1),
	    Imm32("SD renderer 0x65a803 mode 1 width", 0x65a81a, 800, w1),
	    Imm32("SD renderer 0x6581c0 mode 1 width", 0x6581e8, 800, w1),
	    Imm32("SD renderer 0x656497 mode 1 width", 0x6564a6, 800, w1),
	    Imm32("SD renderer 0x654208 mode 1 width", 0x654217, 800, w1),
	    Imm32("SD renderer 0x65b09d mode 1 width", 0x65b0ac, 800, w1),
	    Imm32("SD renderer 0x65d4cf mode 1 width", 0x65d4de, 800, w1),
	    // mode 0, 640 -> width0 (0x65baa4 is the second switch of the SD target set-up; its mode 1 reuses 0x65b897's register)
	    Imm32("SetScreenResolution mode 0 width", 0x7f1ab, 640, w0),
	    Imm32("SD target set-up mode 0 width", 0x65b8b7, 640, w0),
	    Imm32("SD target set-up (2nd) mode 0 width", 0x65baa4, 640, w0),
	    Imm32("SD renderer 0x65a803 mode 0 width", 0x65a82d, 640, w0),
	    Imm32("SD renderer 0x6581c0 mode 0 width", 0x6581fd, 640, w0),
	    Imm32("SD renderer 0x656497 mode 0 width", 0x6564b9, 640, w0),
	    Imm32("SD renderer 0x654208 mode 0 width", 0x654232, 640, w0),
	    Imm32("SD renderer 0x65b09d mode 0 width", 0x65b0c1, 640, w0),
	    Imm32("SD renderer 0x65d4cf mode 0 width", 0x65d4f3, 640, w0),
	    // compose: the float, then the instruction that reads it
	    Imm32("compose aspect float (int3 padding)", RvaAspectCave, 0xccccccccU, AspectBits(l)),
	    ComposeMovss(),
	    // tile cull bottom: `sub ecx, [ScreenBottomMargin 0x22a0d78]` -> `add ecx, 40; nop3` (ecx = ScreenSizeY at 0xf3c24)
	    Patch{"tile cull bottom (0xf3c00)", 0xf3c30, 6, {0x2b, 0x0d, 0x42, 0xd1, 0x1a, 0x02}, {0x83, 0xc1, 0x28, 0x0f, 0x1f, 0x00}},
	    // panel view shift (r9d = ViewSizeX): `mov eax, r9d; cdq` -> `imul r10d, r9d, mul`, the store of the panel state at
	    // 0xb9c2f stays, then `and edx, 3; lea r10d, [rdx + rax]; sar r10d, 2` -> `add r10d, add; sar r10d, sar; nop3`
	    Patch{"panel view shift (0xb9c20) a", 0xb9c2b, 4, {0x41, 0x8b, 0xc1, 0x99}, {0x45, 0x6b, 0xd1, m}},
	    Patch{"panel view shift (0xb9c20) b", 0xb9c35, 11, {0x83, 0xe2, 0x03, 0x44, 0x8d, 0x14, 0x02, 0x41, 0xc1, 0xfa, 0x02},
	          {0x41, 0x83, 0xc2, c, 0x41, 0xc1, 0xfa, s, 0x0f, 0x1f, 0x00}},
	    // view offset helpers, eax = ScreenSizeX: `cdq; and edx, 3; add eax, edx; sar eax, 2` -> `imul eax, eax, mul;
	    // add eax, add; sar eax, sar` (0xb9ad0: +-shift by panel state, two branches)
	    Patch{"view shift 0xb9ad0 a", 0xb9ae9, 9, {0x99, 0x83, 0xe2, 0x03, 0x03, 0xc2, 0xc1, 0xf8, 0x02}, {0x6b, 0xc0, m, 0x83, 0xc0, c, 0xc1, 0xf8, s}},
	    Patch{"view shift 0xb9ad0 b", 0xb9af9, 9, {0x99, 0x83, 0xe2, 0x03, 0x03, 0xc2, 0xc1, 0xf8, 0x02}, {0x6b, 0xc0, m, 0x83, 0xc0, c, 0xc1, 0xf8, s}},
	    // 0xb9b20 (view offset X = base -+ shift), two branches: `cdq; and edx, 3; lea ecx, [rdx + rax]` -> `imul ecx, eax, mul;
	    // add ecx, add; nop`, the base load after it stays, then `sar ecx, 2` -> `sar ecx, sar`
	    Patch{"view shift 0xb9b20 a", 0xb9b41, 7, {0x99, 0x83, 0xe2, 0x03, 0x8d, 0x0c, 0x02}, {0x6b, 0xc8, m, 0x83, 0xc1, c, 0x90}},
	    Patch{"view shift 0xb9b20 a sar", 0xb9b4e, 3, {0xc1, 0xf9, 0x02}, {0xc1, 0xf9, s}},
	    Patch{"view shift 0xb9b20 b", 0xb9b5a, 7, {0x99, 0x83, 0xe2, 0x03, 0x8d, 0x0c, 0x02}, {0x6b, 0xc8, m, 0x83, 0xc1, c, 0x90}},
	    Patch{"view shift 0xb9b20 b sar", 0xb9b67, 3, {0xc1, 0xf9, 0x02}, {0xc1, 0xf9, s}},
	    // automap centre point (0xb9a40) and the screen point it is drawn at (AutoMapPanel::Draw), see AutomapWidthFn
	    Patch{"automap centre 0xb9a40 half width", 0xb9a5a, 11, {0x41, 0x8b, 0xc1, 0x99, 0x2b, 0xc2, 0xd1, 0xf8, 0x44, 0x8b, 0xc8},
	          {0x41, 0x6b, 0xc1, tm, 0xc1, 0xf8, ts, 0x44, 0x2b, 0xc8, 0x90}},
	    Patch{"automap centre 0xb9a40 shift a", 0xb9a79, 12, {0x99, 0x83, 0xe2, 0x03, 0x44, 0x8d, 0x14, 0x02, 0x41, 0xc1, 0xfa, 0x02},
	          {0x44, 0x6b, 0xd0, m, 0x41, 0x83, 0xc2, c, 0x41, 0xc1, 0xfa, s}},
	    Patch{"automap centre 0xb9a40 shift b", 0xb9a8a, 12, {0x99, 0x83, 0xe2, 0x03, 0x44, 0x8d, 0x14, 0x02, 0x41, 0xc1, 0xfa, 0x02},
	          {0x44, 0x6b, 0xd0, m, 0x41, 0x83, 0xc2, c, 0x41, 0xc1, 0xfa, s}},
	    AutomapWidthFn(l),
	    AutomapCall("AutoMapPanel::Draw width call a", RvaAutomapCall1),
	    AutomapCall("AutoMapPanel::Draw width call b", RvaAutomapCall2),
	}};
}

// The 16:9 table (the tests check its sites; the plugin builds the one of its layout).
constexpr PatchTable Patches = BuildPatches(Layout169);

// Functions the plugin hooks (D2RLoader inline hooks, each with its own expected entry bytes). They go in after the
// patches; ui_layout.h has what they do.
constexpr size_t MaxHookBytes = 24;
struct HookSite {
	const char* name;
	uint64_t rva;
	uint8_t size;
	uint8_t expected[MaxHookBytes];
};
constexpr uint64_t RvaProfileLoad = 0x8c80d0;  // void (const char* path, int profileId): reads one _profile*.json
constexpr uint64_t RvaBordersDraw = 0x2adb20;  // void BordersPanel::Draw(BordersPanel*): the 800x600 frame beside panels
constexpr HookSite Hooks[] = {
    {"profile load (0x8c80d0)", RvaProfileLoad, 14, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8d, 0x6c, 0x24, 0xd1}},
    {"BordersPanel::Draw (0x2adb20)", RvaBordersDraw, 20,
     {0x4c, 0x8b, 0xdc, 0x55, 0x53, 0x56, 0x57, 0x41, 0x55, 0x49, 0x8d, 0x6b, 0xa1, 0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00}},
};
constexpr size_t HookCount = sizeof(Hooks) / sizeof(Hooks[0]);

// Read-only view of the game image for the check: returns false when the range cannot be read.
using ReadFn = bool (*)(void* user, uint64_t rva, uint8_t* out, size_t size);

struct CheckResult {
	bool ok;
	size_t failed;  // index into the table of the first mismatch (its count when ok)
	uint8_t found[MaxPatchBytes > MaxHookBytes ? MaxPatchBytes : MaxHookBytes];
	bool unreadable;
};

// Every entry's expected bytes (Patches or Hooks), before anything is written. The first mismatch stops the check.
template <typename Site>
inline CheckResult CheckSites(ReadFn read, void* user, const Site* table, size_t count) {
	CheckResult r{true, count, {}, false};
	for (size_t i = 0; i < count; ++i) {
		uint8_t buf[MaxPatchBytes > MaxHookBytes ? MaxPatchBytes : MaxHookBytes] = {};
		if (!read(user, table[i].rva, buf, table[i].size)) {
			r = CheckResult{false, i, {}, true};
			return r;
		}
		if (std::memcmp(buf, table[i].expected, table[i].size) != 0) {
			r.ok = false;
			r.failed = i;
			std::memcpy(r.found, buf, table[i].size);
			return r;
		}
	}
	return r;
}
inline CheckResult CheckAll(ReadFn read, void* user, const Patch* table = Patches.data(), size_t count = PatchCount) {
	return CheckSites(read, user, table, count);
}
inline CheckResult CheckHooks(ReadFn read, void* user, const HookSite* table = Hooks, size_t count = HookCount) {
	return CheckSites(read, user, table, count);
}

// No two patches may touch the same byte (D2RLoader refuses overlapping operations in one transaction too).
inline bool NoOverlap(const Patch* table = Patches.data(), size_t count = PatchCount) {
	for (size_t i = 0; i < count; ++i) {
		for (size_t j = i + 1; j < count; ++j) {
			const uint64_t a0 = table[i].rva, a1 = a0 + table[i].size, b0 = table[j].rva, b1 = b0 + table[j].size;
			if (a0 < b1 && b0 < a1) {
				return false;
			}
		}
	}
	return true;
}

inline void Hex(char* out, size_t outSize, const uint8_t* bytes, size_t size) {
	static const char digits[] = "0123456789abcdef";
	size_t o = 0;
	for (size_t i = 0; i < size && o + 3 < outSize; ++i) {
		if (i) {
			out[o++] = ' ';
		}
		out[o++] = digits[bytes[i] >> 4];
		out[o++] = digits[bytes[i] & 15];
	}
	if (outSize) {
		out[o < outSize ? o : outSize - 1] = '\0';
	}
}

// Build check as mxl2012-core does it: the image stamp must match; a build name, when D2RLoader gives one, too.
constexpr bool BuildMatches(uint32_t imageStamp, const char* buildName) {
	if (imageStamp != ExpectedImage) {
		return false;
	}
	if (buildName == nullptr || buildName[0] == '\0') {
		return true;
	}
	const char* want = ExpectedBuild;
	for (; *buildName != '\0' && *want != '\0'; ++buildName, ++want) {
		if (*buildName != *want) {
			return false;
		}
	}
	return *buildName == '\0' && *want == '\0';
}

// The config file is small: [legacy169] enabled = true|false, reset_screen = true|false, aspect = "auto"|"W:H". A key
// missing (or a value it cannot use) keeps the default. Only lines inside [legacy169] count; # starts a comment.
struct Config {
	bool enabled = true;
	bool resetScreen = true;  // when D2R set up its screen before the plugin loaded: set it up again with the new sizes
	int aspectNum = 0, aspectDen = 0;  // 0:0 = auto (the screen the game is on)
};

inline Config ParseConfig(const char* text) {
	Config c;
	if (text == nullptr) {
		return c;
	}
	bool inSection = false;
	const char* p = text;
	while (*p != '\0') {
		const char* end = p;
		while (*end != '\0' && *end != '\n') {
			++end;
		}
		const char* s = p;
		while (s < end && (*s == ' ' || *s == '\t')) {
			++s;
		}
		const char* e = s;
		while (e < end && *e != '#' && *e != '\r') {
			++e;
		}
		while (e > s && (e[-1] == ' ' || e[-1] == '\t')) {
			--e;
		}
		const size_t len = static_cast<size_t>(e - s);
		if (len > 0 && *s == '[') {
			inSection = len == 11 && std::strncmp(s, "[legacy169]", 11) == 0;
		} else if (inSection && len > 0) {
			const char* eq = s;
			while (eq < e && *eq != '=') {
				++eq;
			}
			if (eq < e) {
				const char* k1 = eq;
				while (k1 > s && (k1[-1] == ' ' || k1[-1] == '\t')) {
					--k1;
				}
				const char* v = eq + 1;
				while (v < e && (*v == ' ' || *v == '\t')) {
					++v;
				}
				const size_t klen = static_cast<size_t>(k1 - s), vlen = static_cast<size_t>(e - v);
				const int value = (vlen == 4 && std::strncmp(v, "true", 4) == 0) ? 1 : (vlen == 5 && std::strncmp(v, "false", 5) == 0) ? 0 : -1;
				if (klen == 6 && std::strncmp(s, "aspect", 6) == 0) {
					const bool quoted = vlen >= 2 && v[0] == '"' && v[vlen - 1] == '"';
					int num = 0, den = 0;
					if (ParseAspect(quoted ? v + 1 : v, quoted ? vlen - 2 : vlen, &num, &den)) {
						c.aspectNum = num;
						c.aspectDen = den;
					}
				} else if (value >= 0) {
					if (klen == 7 && std::strncmp(s, "enabled", 7) == 0) {
						c.enabled = value == 1;
					} else if (klen == 12 && std::strncmp(s, "reset_screen", 12) == 0) {
						c.resetScreen = value == 1;
					}
				}
			}
		}
		p = *end == '\n' ? end + 1 : end;
	}
	return c;
}

}  // namespace legacy169
