// The side panels of the wider classic screen. Pure code: no Windows or D2RLoader headers, so tests/patch_table_test.cpp
// checks it on fake game structures.
//
// D2R's classic panels are widgets placed at parentWidth * anchor.x + rect.x. Their anchors come from _profileSD.json
// (`"LeftPanelAnchor": { "x": 0.5, ... }`, `"RightPanelAnchor"`), their rects are -320 (left) and 0 (right), so on a wider
// screen they stay in the middle. A plugin cannot ship that file (it is not a mod), so the profile loader (0x8c80d0) is
// hooked and the two x values are rewritten in the parsed profile before any panel layout reads them:
//   left 0.3755: 1066 * 0.3755 - 320 = 80 (as at 800x600, beside the 80-pixel frame), 853 * 0.3755 - 320 = 0;
//   right 0.625: 1066 * 0.625 = 666 (666..986, then the frame), 853 * 0.625 = 533 (533..853).
// One pair fits both modes, whether D2R truncates or rounds the float position.
//
// In mode 1 the BordersPanel draws the 800x600 frame beside open panels at fixed 800-wide positions (panelborderspanel.json:
// right pieces at x 400..800 and a click catcher widget over them). Its Draw (0x2adb20) is hooked: the right pieces are
// drawn ScreenSizeX - 800 further right, and the right click catcher widget is moved by the same amount.
//
// Game structures (D2R 3.3 build 93847):
//   profile maps: *(0x34412d8) -> one 0x20-byte hash map per profile id (1 = _profileSD.json): {bucket count, buckets,
//     size, max load}; a bucket holds the first node, node: {next, key data, key size, key capacity, inline key[16],
//     value (16-byte rapidjson value) at +0x30}; keys are "$<name>";
//   rapidjson value: 16 bytes, flags (uint16) at +14; object: member count (uint32) at +0, 48-bit member pointer at +8,
//     members are {name value, value value}; double: the double at +0, flags 0x216;
//   BordersPanel: right positions (IntPoint x, y) inline at +0x1c0, their count (up to 5) at +0x1e8; the right click
//     catcher widget pointer at +0x208; a widget's rect x (int) at +0x70.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace legacy169 {

constexpr uint64_t RvaProfileMaps = 0x34412d8;
constexpr int ProfileSd = 1;
constexpr double LeftAnchorX = 0.3755;
constexpr double RightAnchorX = 0.625;
constexpr double StockAnchorX = 0.5;

// Where a panel lands: D2R computes the position in float; t = truncate, else round to nearest.
constexpr int PanelX(double anchor, int parentWidth, int rectX, bool truncate) {
	const float x = static_cast<float>(anchor) * static_cast<float>(parentWidth) + static_cast<float>(rectX);
	return truncate ? static_cast<int>(x) : static_cast<int>(x + 0.5f);
}

// rapidjson value flags (rapidjson/document.h)
constexpr uint16_t JsonObject = 3;
constexpr uint16_t JsonNumberType = 6;
constexpr uint16_t JsonNumberFlag = 0x10;
constexpr uint16_t JsonDouble = 0x216;  // kNumberDoubleFlag
constexpr uint16_t JsonStringFlag = 0x400;
constexpr uint16_t JsonInlineString = 0x1000;
constexpr uint64_t Ptr48 = 0xffffffffffffULL;

inline uint16_t JsonFlags(const uint8_t* v) {
	uint16_t f;
	std::memcpy(&f, v + 14, 2);
	return f;
}

inline const char* JsonString(const uint8_t* v) {
	const uint16_t f = JsonFlags(v);
	if ((f & JsonStringFlag) == 0) {
		return nullptr;
	}
	if (f & JsonInlineString) {
		return reinterpret_cast<const char*>(v);
	}
	uint64_t p;
	std::memcpy(&p, v + 8, 8);
	return reinterpret_cast<const char*>(p & Ptr48);
}

// Member `name` of a rapidjson object when it is a number, else nullptr.
inline uint8_t* JsonNumberMember(uint8_t* object, const char* name) {
	if (object == nullptr || JsonFlags(object) != JsonObject) {
		return nullptr;
	}
	uint32_t count;
	uint64_t members;
	std::memcpy(&count, object, 4);
	std::memcpy(&members, object + 8, 8);
	members &= Ptr48;
	if (members == 0 || count > 64) {
		return nullptr;
	}
	for (uint32_t i = 0; i < count; ++i) {
		uint8_t* m = reinterpret_cast<uint8_t*>(members) + i * 32;
		const char* key = JsonString(m);
		if (key == nullptr || std::strcmp(key, name) != 0) {
			continue;
		}
		const uint16_t f = JsonFlags(m + 16);
		return ((f & 7) == JsonNumberType && (f & JsonNumberFlag) != 0) ? m + 16 : nullptr;
	}
	return nullptr;
}

// A number value as a double (rapidjson keeps integers as int / uint / int64 / uint64).
inline double JsonNumber(const uint8_t* v) {
	const uint16_t f = JsonFlags(v);
	if (f & 0x200) {
		double d;
		std::memcpy(&d, v, 8);
		return d;
	}
	if (f & 0x20) {
		int32_t i;
		std::memcpy(&i, v, 4);
		return i;
	}
	if (f & 0x80) {
		int64_t i;
		std::memcpy(&i, v, 8);
		return static_cast<double>(i);
	}
	uint64_t u;
	std::memcpy(&u, v, 8);
	return static_cast<double>(u);
}

inline void SetJsonDouble(uint8_t* v, double value) {
	std::memcpy(v, &value, 8);
	const uint16_t f = JsonDouble;
	std::memcpy(v + 14, &f, 2);
}

struct ProfileMap {
	uint64_t buckets;
	uint8_t** slots;
	uint64_t size;
	float maxLoad;
};
static_assert(sizeof(ProfileMap) == 0x20);

struct ProfileNode {
	ProfileNode* next;
	const char* key;
	uint64_t keySize;
	uint64_t keyCapacity;
	uint8_t inlineKey[16];
	uint8_t value[16];
};
static_assert(offsetof(ProfileNode, value) == 0x30 && sizeof(ProfileNode) == 0x40);

// The value of `key` ("$LeftPanelAnchor") in a profile map, or nullptr.
inline uint8_t* FindProfileValue(const ProfileMap* map, const char* key) {
	if (map == nullptr || map->slots == nullptr || map->buckets == 0 || map->buckets > (1u << 20)) {
		return nullptr;
	}
	const size_t len = std::strlen(key);
	for (uint64_t b = 0; b < map->buckets; ++b) {
		int guard = 0;
		for (auto* n = reinterpret_cast<ProfileNode*>(map->slots[b]); n != nullptr && guard < 4096; n = n->next, ++guard) {
			if (n->keySize == len && n->key != nullptr && std::memcmp(n->key, key, len) == 0) {
				return n->value;
			}
		}
	}
	return nullptr;
}

struct AnchorResult {
	int changed;  // 0..2 anchors rewritten
	int already;  // anchors that already had the new value
	double oldLeft, oldRight;
};

// Rewrites the x of $LeftPanelAnchor / $RightPanelAnchor in profile map `id` of the map array. Only the stock 0.5 is
// changed (another value is a profile someone edited on purpose: left alone).
inline AnchorResult PatchPanelAnchors(ProfileMap* maps, int id) {
	AnchorResult r{0, 0, -1.0, -1.0};
	if (maps == nullptr) {
		return r;
	}
	const struct {
		const char* key;
		double to;
		double* old;
	} anchors[] = {{"$LeftPanelAnchor", LeftAnchorX, &r.oldLeft}, {"$RightPanelAnchor", RightAnchorX, &r.oldRight}};
	for (const auto& a : anchors) {
		uint8_t* v = JsonNumberMember(FindProfileValue(&maps[id], a.key), "x");
		if (v == nullptr) {
			continue;
		}
		const double cur = JsonNumber(v);
		*a.old = cur;
		if (cur == StockAnchorX) {
			SetJsonDouble(v, a.to);
			++r.changed;
		} else if (cur == a.to) {
			++r.already;
		}
	}
	return r;
}

// BordersPanel (see the top of the file).
constexpr size_t BordersRightPositions = 0x1c0;
constexpr size_t BordersRightCount = 0x1e8;
constexpr size_t BordersRightCatcher = 0x208;
constexpr size_t WidgetRectX = 0x70;
constexpr int BordersMaxPositions = 5;

// Adds dx to the x of every right frame piece. False (nothing changed) for a count outside 0..5.
inline bool ShiftBordersRight(uint8_t* panel, int dx) {
	int count;
	std::memcpy(&count, panel + BordersRightCount, 4);
	if (count < 0 || count > BordersMaxPositions) {
		return false;
	}
	for (int i = 0; i < count; ++i) {
		int x;
		std::memcpy(&x, panel + BordersRightPositions + 8 * i, 4);
		x += dx;
		std::memcpy(panel + BordersRightPositions + 8 * i, &x, 4);
	}
	return true;
}

// Moves the right click catcher widget to x = dx. Only from 0 (the stock layout has no rect) or from a shift this plugin
// set earlier (`known`), so a layout with its own rect is left alone.
inline bool PlaceRightCatcher(uint8_t* panel, int dx, int known) {
	uint8_t* widget;
	std::memcpy(&widget, panel + BordersRightCatcher, 8);
	if (widget == nullptr) {
		return false;
	}
	int x;
	std::memcpy(&x, widget + WidgetRectX, 4);
	if (x != 0 && x != known && x != dx) {
		return false;
	}
	std::memcpy(widget + WidgetRectX, &dx, 4);
	return true;
}

}  // namespace legacy169
