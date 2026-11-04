// The byte patches of legacy169 and the expected-bytes logic around them. Pure code: no Windows or D2RLoader headers, so
// tests/patch_table_test.cpp checks it without the game.
//
// D2R draws the classic (legacy) graphics into a fixed SD target, 640x480 (resolution mode 0) or 800x600 (mode 1), and
// composes it into the window with a 4:3 fit. legacy169 widens both sizes to 16:9 at the same height:
//   - mode 1: 800 -> 1066 in the client screen size (SetScreenResolution 0x7f170 and its inline copy at start-up 0x7e7b5)
//     and in the SD renderer's size switches (one per function, each `call GetResolutionMode; ...; mov reg, 800`);
//   - mode 0: 640 -> 853 in the same functions (the start-up copy only knows mode 1);
//   - the compose pass (0xedde70) divides by a 4:3 float at 0x1d65290 that other code shares, so its movss is pointed at
//     a plugin-owned float (1066/600) written into int3 padding after a function in .text.
// The bottom control panel keeps its own 800 / 640 (its widget, 0x14c9430, is not patched), so it stays 4:3 and centred.
// Around the wider screen:
//   - the tile cull rectangle (SetViewRectangleSize 0xf3c00) ends at ScreenSizeY - 47, which the 800-wide control panel
//     hid; at 1066 the bottom tile row was missing left and right of it. It now ends at ScreenSizeY + 40 (as SGD2FreeRes);
//   - an open side panel shifts the view by a quarter of the width, 266 at 1066: the draw window (0xb9c20, ViewSizeX / 4)
//     and the view offset helpers (0xb9ad0 and 0xb9b20, ScreenSizeX / 4; the world, units and the SD renderer use them).
//     The panels keep their 4:3 width, so the shift is put back to the stock 200 / 160: (width * 3 + 15) >> 4.
// The side panels themselves move through two hooks (ui_layout.h).
// All RVAs are for D2R 3.3 build 93847 under D2RLoader 1.3.1 (image stamp 0x6ab3782c).
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace legacy169 {

constexpr uint32_t ExpectedImage = 0x6ab3782cU;  // TimeDateStamp of D2R.exe 3.3 build 93847 as D2RLoader 1.3.1 runs it
constexpr const char* ExpectedBuild = "93847";

constexpr int ClassicHeight1 = 600;
constexpr int ClassicWidth1 = 1066;  // 600 * 16 / 9 = 1066.7, rounded down to an even width
constexpr int ClassicHeight0 = 480;
constexpr int ClassicWidth0 = 853;  // 480 * 16 / 9 = 853.3

// Game globals the plugin reads for its log (written by 0x7f170 / 0x7e7b5).
constexpr uint64_t RvaScreenSizeX = 0x22a0d68;
constexpr uint64_t RvaScreenSizeY = 0x22a0d6c;
constexpr uint64_t RvaViewSizeX = 0x22a0d70;  // view width, = screen width (the view height is screen height - 40)

// The width a set-up screen gets: D2R sets its classic screen up before plugins load, so the patched setters have not run
// yet. For the stock width of the current mode the 16:9 width, else 0 (leave the globals alone: not D2R's own value).
constexpr int WidenedWidth(int mode, int screenX, int screenY, int viewX) {
	if (mode == 1 && screenX == 800 && screenY == ClassicHeight1 && viewX == 800) {
		return ClassicWidth1;
	}
	if (mode == 0 && screenX == 640 && screenY == ClassicHeight0 && viewX == 640) {
		return ClassicWidth0;
	}
	return 0;
}
constexpr uint64_t RvaSetScreenResolution = 0x7f170;  // void (int mode); returns at once when mode is the current one
constexpr uint64_t RvaGetResolutionMode = 0x901750;   // int (void)

constexpr size_t MaxPatchBytes = 12;

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
constexpr uint32_t AspectBits = FloatBits(static_cast<float>(ClassicWidth1) / static_cast<float>(ClassicHeight1));  // 0x3fe369d0

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

// The table. Order does not matter for correctness (all of it goes in as one transaction, or none of it), but the float
// is listed before the instruction that reads it.
constexpr Patch Patches[] = {
    // mode 1, 800 -> 1066
    Imm32("SetScreenResolution mode 1 width", 0x7f18b, 800, ClassicWidth1),
    Imm32("start-up screen width", 0x7e7bb, 800, ClassicWidth1),
    Imm32("start-up view width", 0x7e7d5, 800, ClassicWidth1),
    Imm32("SD target set-up mode 1 width", 0x65b897, 800, ClassicWidth1),
    Imm32("SD renderer 0x65a803 mode 1 width", 0x65a81a, 800, ClassicWidth1),
    Imm32("SD renderer 0x6581c0 mode 1 width", 0x6581e8, 800, ClassicWidth1),
    Imm32("SD renderer 0x656497 mode 1 width", 0x6564a6, 800, ClassicWidth1),
    Imm32("SD renderer 0x654208 mode 1 width", 0x654217, 800, ClassicWidth1),
    Imm32("SD renderer 0x65b09d mode 1 width", 0x65b0ac, 800, ClassicWidth1),
    Imm32("SD renderer 0x65d4cf mode 1 width", 0x65d4de, 800, ClassicWidth1),
    // mode 0, 640 -> 853 (0x65baa4 is the second switch of the SD target set-up; its mode 1 reuses 0x65b897's register)
    Imm32("SetScreenResolution mode 0 width", 0x7f1ab, 640, ClassicWidth0),
    Imm32("SD target set-up mode 0 width", 0x65b8b7, 640, ClassicWidth0),
    Imm32("SD target set-up (2nd) mode 0 width", 0x65baa4, 640, ClassicWidth0),
    Imm32("SD renderer 0x65a803 mode 0 width", 0x65a82d, 640, ClassicWidth0),
    Imm32("SD renderer 0x6581c0 mode 0 width", 0x6581fd, 640, ClassicWidth0),
    Imm32("SD renderer 0x656497 mode 0 width", 0x6564b9, 640, ClassicWidth0),
    Imm32("SD renderer 0x654208 mode 0 width", 0x654232, 640, ClassicWidth0),
    Imm32("SD renderer 0x65b09d mode 0 width", 0x65b0c1, 640, ClassicWidth0),
    Imm32("SD renderer 0x65d4cf mode 0 width", 0x65d4f3, 640, ClassicWidth0),
    // compose: the float, then the instruction that reads it
    Imm32("compose aspect float (int3 padding)", RvaAspectCave, 0xccccccccU, AspectBits),
    ComposeMovss(),
    // tile cull bottom: `sub ecx, [ScreenBottomMargin 0x22a0d78]` -> `add ecx, 40; nop3` (ecx = ScreenSizeY at 0xf3c24)
    Patch{"tile cull bottom (0xf3c00)", 0xf3c30, 6, {0x2b, 0x0d, 0x42, 0xd1, 0x1a, 0x02}, {0x83, 0xc1, 0x28, 0x0f, 0x1f, 0x00}},
    // panel view shift (r9d = ViewSizeX): `mov eax, r9d; cdq` -> `lea r10d, [r9 + r9 * 2]`, the store of the panel state
    // at 0xb9c2f stays, then `and edx, 3; lea r10d, [rdx + rax]; sar r10d, 2` -> `add r10d, 15; sar r10d, 4; nop3`
    Patch{"panel view shift (0xb9c20) a", 0xb9c2b, 4, {0x41, 0x8b, 0xc1, 0x99}, {0x47, 0x8d, 0x14, 0x49}},
    Patch{"panel view shift (0xb9c20) b", 0xb9c35, 11, {0x83, 0xe2, 0x03, 0x44, 0x8d, 0x14, 0x02, 0x41, 0xc1, 0xfa, 0x02},
          {0x41, 0x83, 0xc2, 0x0f, 0x41, 0xc1, 0xfa, 0x04, 0x0f, 0x1f, 0x00}},
    // view offset helpers, eax = ScreenSizeX: `cdq; and edx, 3; add eax, edx; sar eax, 2` -> `lea eax, [rax + rax * 2];
    // add eax, 15; sar eax, 4` (0xb9ad0: +-shift by panel state, two branches)
    Patch{"view shift 0xb9ad0 a", 0xb9ae9, 9, {0x99, 0x83, 0xe2, 0x03, 0x03, 0xc2, 0xc1, 0xf8, 0x02}, {0x8d, 0x04, 0x40, 0x83, 0xc0, 0x0f, 0xc1, 0xf8, 0x04}},
    Patch{"view shift 0xb9ad0 b", 0xb9af9, 9, {0x99, 0x83, 0xe2, 0x03, 0x03, 0xc2, 0xc1, 0xf8, 0x02}, {0x8d, 0x04, 0x40, 0x83, 0xc0, 0x0f, 0xc1, 0xf8, 0x04}},
    // 0xb9b20 (view offset X = base -+ shift), two branches: `cdq; and edx, 3; lea ecx, [rdx + rax]` -> `lea ecx, [rax + rax * 2];
    // add ecx, 15; nop`, the base load after it stays, then `sar ecx, 2` -> `sar ecx, 4`
    Patch{"view shift 0xb9b20 a", 0xb9b41, 7, {0x99, 0x83, 0xe2, 0x03, 0x8d, 0x0c, 0x02}, {0x8d, 0x0c, 0x40, 0x83, 0xc1, 0x0f, 0x90}},
    Patch{"view shift 0xb9b20 a sar", 0xb9b4e, 3, {0xc1, 0xf9, 0x02}, {0xc1, 0xf9, 0x04}},
    Patch{"view shift 0xb9b20 b", 0xb9b5a, 7, {0x99, 0x83, 0xe2, 0x03, 0x8d, 0x0c, 0x02}, {0x8d, 0x0c, 0x40, 0x83, 0xc1, 0x0f, 0x90}},
    Patch{"view shift 0xb9b20 b sar", 0xb9b67, 3, {0xc1, 0xf9, 0x02}, {0xc1, 0xf9, 0x04}},
};
constexpr size_t PatchCount = sizeof(Patches) / sizeof(Patches[0]);

// The panel view shift of a wide width: the stock ViewSizeX / 4 of the 4:3 width at the same height.
constexpr int PanelViewShift(int viewX) {
	return (viewX * 3 + 15) >> 4;
}

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
	uint8_t found[MaxHookBytes];
	bool unreadable;
};

// Every entry's expected bytes (Patches or Hooks), before anything is written. The first mismatch stops the check.
template <typename Site>
inline CheckResult CheckSites(ReadFn read, void* user, const Site* table, size_t count) {
	CheckResult r{true, count, {}, false};
	for (size_t i = 0; i < count; ++i) {
		uint8_t buf[MaxHookBytes] = {};
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
inline CheckResult CheckAll(ReadFn read, void* user, const Patch* table = Patches, size_t count = PatchCount) {
	return CheckSites(read, user, table, count);
}
inline CheckResult CheckHooks(ReadFn read, void* user, const HookSite* table = Hooks, size_t count = HookCount) {
	return CheckSites(read, user, table, count);
}

// No two patches may touch the same byte (D2RLoader refuses overlapping operations in one transaction too).
inline bool NoOverlap(const Patch* table = Patches, size_t count = PatchCount) {
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

// The config file is small: [legacy169] enabled = true|false, reset_screen = true|false. A key missing (or a value
// that is neither true nor false) keeps the default. Only lines inside [legacy169] count; # starts a comment.
struct Config {
	bool enabled = true;
	bool resetScreen = true;  // when D2R set up its screen before the plugin loaded: set it up again with the new sizes
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
				if (value >= 0) {
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
