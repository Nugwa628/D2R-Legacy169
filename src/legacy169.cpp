// legacy169: a D2RLoader plugin that shows D2R's legacy (classic 2D) graphics as wide as the screen (up to 16:9) instead
// of 4:3.
//
// At 16:9 800x600 becomes 1066x600 and 640x480 becomes 853x480, at 16:10 960x600 and 768x480 (the widths are chosen at
// load from the screen the game is on, or the config's aspect); the bottom control panel stays 4:3 and centred
// (patch_table.h has the sites). The compose pass fits the classic picture to the window height, so a picture wider than
// the window would lose its sides, and with them the HUD in the top corners (merc and summon portraits, the clock). The plugin only patches bytes: every patch is checked against the bytes it expects first, and all of them go
// in through one D2RLoader transaction, so on any other game image, or with bytes another plugin changed, nothing changes.
// Mouse mapping, culling and the cursor read the screen-size globals and follow the new width by themselves. Two hooks move
// the side panels to the window edges (ui_layout.h): the profile loader rewrites the panel anchors of _profileSD.json, and
// the 800x600 panel frame draws its right half at the right edge.
//
// Config: <scope>\d2rloader\config\legacy169.toml (D2RLoader writes the embedded default when it is missing). Log: the
// plugin's own D2RLoader log. Console command: `legacy169` prints the status.
#include <D2RLPlugin/api.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "patch_table.h"
#include "ui_layout.h"
#ifdef LEGACY169_TEST_HARNESS
#include "testharness.h"
#endif

namespace {

#if defined(LEGACY169_TEST_HARNESS) || defined(LEGACY169_TEST_BAD_BYTES)
constexpr const char* PluginVersion = "0.1.1-test";  // test build: not for release
#else
constexpr const char* PluginVersion = "0.1.1";
#endif

constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .abiVersion = D2RL_PLUGIN_ABI_VERSION,
    .id = "legacy169",
    .name = "Legacy 16:9",
    .version = PluginVersion,
    .author = "Nugwa628",
    .description = "Legacy (classic) graphics as wide as the screen (16:9: 1066x600 and 853x480); the control panel stays 4:3.",
    .flags = D2RL::PluginFlags::Client | D2RL::PluginFlags::NativeHooks,
};

const D2RL::PluginContext* g_context = nullptr;
char g_status[320] = "not loaded";
volatile LONG g_resets = 0;
char g_ui[160] = "panels: not moved";
volatile LONG g_profilePatches = 0;  // anchors rewritten (both of them count 2)
int g_catcherShift = 0;               // right click catcher x this plugin set last (UI thread)
legacy169::Layout g_layout = legacy169::Layout169;
legacy169::PatchTable g_table = legacy169::Patches;

using ProfileLoadFn = void(__fastcall*)(const char* path, int id);
using BordersDrawFn = void(__fastcall*)(uint8_t* panel, void* a2, void* a3, void* a4);
ProfileLoadFn g_profileLoad = nullptr;
BordersDrawFn g_bordersDraw = nullptr;

void Log(const char* line) {
	g_context->LogInfo(line);
}

void Fail(const char* line) {
	g_context->LogError(line);
	std::snprintf(g_status, sizeof(g_status), "off: %s", line);
}

auto ImageStamp(const D2RL::PluginContext* context) -> uint32_t {
	if (context->exeBase == 0) {
		return 0;
	}
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(context->exeBase);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(context->exeBase + dos->e_lfanew);
	return nt->FileHeader.TimeDateStamp;
}

auto ImageSize(const D2RL::PluginContext* context) -> uint64_t {
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(context->exeBase);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(context->exeBase + dos->e_lfanew);
	return nt->OptionalHeader.SizeOfImage;
}

// Reads the loaded image (patch_table.h ReadFn); only inside SizeOfImage.
bool ReadImage(void* user, uint64_t rva, uint8_t* out, size_t size) {
	const auto* context = static_cast<const D2RL::PluginContext*>(user);
	if (rva + size > ImageSize(context)) {
		return false;
	}
	std::memcpy(out, reinterpret_cast<const void*>(context->exeBase + rva), size);
	return true;
}

auto Global(uint64_t rva) -> int {
	return *reinterpret_cast<const volatile int*>(g_context->exeBase + rva);
}

auto ReadConfigText(char* out, uint32_t size) -> bool {
	out[0] = '\0';
	uint32_t need = 0;
	return g_context->ReadConfig(out, size, &need);
}

// The screen the game is on: the monitor of this process's game window, else the primary monitor. Fullscreen, borderless
// and a maximised window show the classic picture at (about) this shape; another window shape takes `aspect`.
BOOL CALLBACK FindGameWindow(HWND h, LPARAM out) {
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) {
		*reinterpret_cast<HWND*>(out) = h;
		return FALSE;
	}
	return TRUE;
}

auto ScreenSize(int* w, int* h) -> const char* {
	HWND game = nullptr;
	EnumWindows(&FindGameWindow, reinterpret_cast<LPARAM>(&game));
	const HMONITOR monitor = game != nullptr ? MonitorFromWindow(game, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO info{};
	info.cbSize = sizeof(info);
	if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) {
		*w = *h = 0;
		return "no monitor";
	}
	*w = info.rcMonitor.right - info.rcMonitor.left;
	*h = info.rcMonitor.bottom - info.rcMonitor.top;
	return game != nullptr ? "the game window's monitor" : "the primary monitor";
}

// The widths for this start (config `aspect`, else the screen) and the table built for them.
void ChooseLayout(const legacy169::Config& config) {
	int num = config.aspectNum, den = config.aspectDen;
	const char* from = "aspect in legacy169.toml";
	if (num == 0 || den == 0) {
		from = ScreenSize(&num, &den);
	}
	g_layout = legacy169::LayoutFor(num, den);
	g_table = legacy169::BuildPatches(g_layout);
#ifdef LEGACY169_TEST_BAD_BYTES
	// Test build only: the last patch expects one byte the game does not have, as on another build or next to a plugin that
	// patched the same site. The whole table must be refused and the game left unchanged.
	g_table[legacy169::PatchCount - 1].expected[0] ^= 0xff;
#endif
	char line[256];
	std::snprintf(line, sizeof(line), "legacy169: screen %d:%d (%s): classic %dx%d and %dx%d, view shift (x * %d + %d) >> %d, panel anchors %.4g / %.4g",
	              num, den, from, g_layout.width1, legacy169::ClassicHeight1, g_layout.width0, legacy169::ClassicHeight0, g_layout.shiftMul,
	              g_layout.shiftAdd, g_layout.shiftSar, g_layout.leftAnchor, g_layout.rightAnchor);
	Log(line);
}

auto Apply() -> bool {
	const legacy169::Patch* const Table = g_table.data();
	char line[512];
	const legacy169::CheckResult check = legacy169::CheckAll(&ReadImage, const_cast<D2RL::PluginContext*>(g_context), Table);
	if (!check.ok) {
		const legacy169::Patch& p = Table[check.failed];
		char want[160], got[160];
		legacy169::Hex(want, sizeof(want), p.expected, p.size);
		legacy169::Hex(got, sizeof(got), check.found, p.size);
		std::snprintf(line, sizeof(line), "legacy169: %s at 0x%llx: expected %s, found %s; nothing patched, the game is unchanged",
		              p.name, static_cast<unsigned long long>(p.rva), want, check.unreadable ? "(unreadable)" : got);
		Fail(line);
		return false;
	}
	const D2RL::MutationService* mutations = nullptr;
	if (g_context->QueryService(&mutations) != D2RL::ServiceQueryResult::Success ||
	    !D2RL::HasMutationServiceField(mutations, D2RL::MutationServiceRequiredSize)) {
		Fail("legacy169: D2RLoader has no patch transaction service (needs D2RLoader 1.3.1); nothing patched");
		return false;
	}
	D2RL::Mutations::TransactionHandle tx = 0;
	if (mutations->beginTransaction(g_context, &tx) != D2RL::Mutations::Result::Success) {
		Fail("legacy169: could not start a patch transaction; nothing patched");
		return false;
	}
	for (size_t i = 0; i < legacy169::PatchCount; ++i) {
		const legacy169::Patch& p = Table[i];
		const D2RL::Mutations::BytePatchRequest request{
		    .structSize = D2RL::Mutations::BytePatchRequestSize,
		    .flags = 0,
		    .rva = p.rva,
		    .expected = p.expected,
		    .expectedSize = p.size,
		    .reserved = 0,
		    .bytes = p.bytes,
		    .size = p.size,
		    .reserved2 = 0,
		};
		D2RL::Mutations::OperationHandle op = 0;
		const auto r = mutations->stageBytePatch(g_context, tx, &request, &op);
		if (r != D2RL::Mutations::Result::Success) {
			mutations->cancelTransaction(g_context, tx);
			std::snprintf(line, sizeof(line), "legacy169: %s at 0x%llx refused by D2RLoader (result %u, another plugin's patch?); nothing patched",
			              p.name, static_cast<unsigned long long>(p.rva), static_cast<unsigned>(r));
			Fail(line);
			return false;
		}
	}
	D2RL::Mutations::CommitResult result{.structSize = D2RL::Mutations::CommitResultSize};
	if (mutations->commit(g_context, tx, &result) != D2RL::Mutations::Result::Success) {
		const bool restored = D2RL::Mutations::HasFlag(result.flags, D2RL::Mutations::CommitFlags::OriginalStateRestored);
		mutations->cancelTransaction(g_context, tx);
		std::snprintf(line, sizeof(line), "legacy169: patch commit failed (result %u, reason %u)%s", static_cast<unsigned>(result.result),
		              static_cast<unsigned>(result.failureReason), restored ? "; the game is unchanged" : "; restore retried");
		Fail(line);
		return false;
	}
	return true;
}

// Rewrites the panel anchors of the loaded _profileSD.json. Game memory: a fault (another build's layout slipped past the
// checks) is caught and reported instead of crashing the game.
auto PatchProfile(legacy169::AnchorResult* out) -> bool {
	__try {
		auto* maps = *reinterpret_cast<legacy169::ProfileMap* const*>(g_context->exeBase + legacy169::RvaProfileMaps);
		*out = legacy169::PatchPanelAnchors(maps, legacy169::ProfileSd, g_layout.leftAnchor, g_layout.rightAnchor);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void LogProfile(const char* when) {
	legacy169::AnchorResult r{};
	char line[256];
	if (!PatchProfile(&r)) {
		std::snprintf(line, sizeof(line), "legacy169: %s: the profile map could not be read; panels stay centred", when);
		g_context->LogWarn(line);
		return;
	}
	InterlockedExchangeAdd(&g_profilePatches, r.changed);
	if (r.changed + r.already == 2) {
		std::snprintf(g_ui, sizeof(g_ui), "panels at the edges (anchor x %.4g / %.4g)", g_layout.leftAnchor, g_layout.rightAnchor);
	}
	std::snprintf(line, sizeof(line), "legacy169: %s: panel anchors left %.4g, right %.4g; %d rewritten, %d already set", when, r.oldLeft,
	              r.oldRight, r.changed, r.already);
	Log(line);
}

void __fastcall ProfileLoadHook(const char* path, int id) {
	g_profileLoad(path, id);
	if (id == legacy169::ProfileSd) {
		LogProfile("_profileSD.json loaded");
	}
}

// Mode 1 only (the frame is an 800x600 thing): the right pieces are drawn ScreenSizeX - 800 further right, for this draw
// only; the right click catcher stays moved (it is hit-tested outside Draw).
void __fastcall BordersDrawHook(uint8_t* panel, void* a2, void* a3, void* a4) {
	const int mode = reinterpret_cast<int(__fastcall*)()>(g_context->exeBase + legacy169::RvaGetResolutionMode)();
	const int dx = mode == 1 && Global(legacy169::RvaScreenSizeX) > 800 ? Global(legacy169::RvaScreenSizeX) - 800 : 0;
	if (panel == nullptr || dx == 0) {
		g_bordersDraw(panel, a2, a3, a4);
		return;
	}
	if (legacy169::PlaceRightCatcher(panel, dx, g_catcherShift)) {
		g_catcherShift = dx;
	}
	const bool shifted = legacy169::ShiftBordersRight(panel, dx);
	g_bordersDraw(panel, a2, a3, a4);
	if (shifted) {
		legacy169::ShiftBordersRight(panel, -dx);
	}
}

void __cdecl ProfileNow(const D2RL::PluginContext*, void*) noexcept {
	LogProfile("at load");
}

auto InstallHooks() -> bool {
	char line[256];
	const legacy169::CheckResult check = legacy169::CheckHooks(&ReadImage, const_cast<D2RL::PluginContext*>(g_context));
	if (!check.ok) {
		const legacy169::HookSite& h = legacy169::Hooks[check.failed];
		char want[80], got[80];
		legacy169::Hex(want, sizeof(want), h.expected, h.size);
		legacy169::Hex(got, sizeof(got), check.found, h.size);
		std::snprintf(line, sizeof(line), "legacy169: %s: expected %s, found %s; panels not moved", h.name, want, check.unreadable ? "(unreadable)" : got);
		g_context->LogWarn(line);
		std::snprintf(g_ui, sizeof(g_ui), "panels: not moved (%s changed)", h.name);
		return false;
	}
	const legacy169::HookSite& profile = legacy169::Hooks[0];
	const legacy169::HookSite& borders = legacy169::Hooks[1];
	if (!g_context->InstallInlineHook(profile.rva, profile.expected, profile.size, reinterpret_cast<void*>(&ProfileLoadHook),
	                                  reinterpret_cast<void**>(&g_profileLoad))) {
		g_context->LogWarn("legacy169: the profile load hook was refused; panels not moved");
		std::snprintf(g_ui, sizeof(g_ui), "panels: not moved (profile hook refused)");
		return false;
	}
	if (!g_context->InstallInlineHook(borders.rva, borders.expected, borders.size, reinterpret_cast<void*>(&BordersDrawHook),
	                                  reinterpret_cast<void**>(&g_bordersDraw))) {
		g_context->LogWarn("legacy169: the BordersPanel::Draw hook was refused; the 800x600 panel frame stays at 0..800");
	}
	std::snprintf(g_ui, sizeof(g_ui), "panels: hooks in, profile not loaded yet");
	return true;
}

// D2R set up its classic screen before the plugin loaded: SetScreenResolution returns at once for the current mode, so
// switch to the other mode and back; both now use the 16:9 widths. Runs on the UI thread (as the Options menu does).
void __cdecl ResetScreen(const D2RL::PluginContext*, void*) noexcept {
	using SetFn = void(__fastcall*)(int);
	using GetFn = int(__fastcall*)();
	const auto set = reinterpret_cast<SetFn>(g_context->exeBase + legacy169::RvaSetScreenResolution);
	const auto get = reinterpret_cast<GetFn>(g_context->exeBase + legacy169::RvaGetResolutionMode);
	const int mode = get();
	if (mode != 0 && mode != 1) {
		return;
	}
	set(1 - mode);
	set(mode);
	InterlockedIncrement(&g_resets);
	char line[160];
	std::snprintf(line, sizeof(line), "legacy169: classic screen set up again in mode %d: %dx%d", mode, Global(legacy169::RvaScreenSizeX),
	              Global(legacy169::RvaScreenSizeY));
	Log(line);
}

// `legacy169`: the status (console and log). `legacy169 mode 0|1`: switch the classic resolution mode with the game's own
// setter (0 = 640x480 class, here 853x480; 1 = 800x600 class, here 1066x600), for checks of the other mode. Console
// commands run on the UI thread.
auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	using SetFn = void(__fastcall*)(int);
	using GetFn = int(__fastcall*)();
	const auto get = reinterpret_cast<GetFn>(g_context->exeBase + legacy169::RvaGetResolutionMode);
	const char* args = command->args != nullptr ? command->args : "";
	while (*args == ' ') {
		++args;
	}
	if (std::strncmp(args, "mode ", 5) == 0) {
		const char m = args[5];
		if ((m != '0' && m != '1') || args[6] != '\0') {
			return D2RL::ConsoleCommandResult::InvalidArguments;
		}
		reinterpret_cast<SetFn>(g_context->exeBase + legacy169::RvaSetScreenResolution)(m - '0');
	} else if (*args != '\0') {
		return D2RL::ConsoleCommandResult::InvalidArguments;
	}
	char line[640];
	std::snprintf(line, sizeof(line), "legacy169 %s: %s; classic screen %dx%d (mode %d), screen resets %ld; %s, catcher x %d", PluginVersion,
	              g_status, Global(legacy169::RvaScreenSizeX), Global(legacy169::RvaScreenSizeY), get(), g_resets, g_ui, g_catcherShift);
	command->plugin->WriteConsoleMessage(line);
	Log(line);
	return D2RL::ConsoleCommandResult::Handled;
}

}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
	return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
	if (context == nullptr) {
		return false;
	}
	g_context = context;
	char line[320];
	const uint32_t stamp = ImageStamp(context);
	std::snprintf(line, sizeof(line), "legacy169 %s loaded (%s scope, mod %s, D2R %s, image 0x%08x)", PluginVersion,
	              context->loadScope == D2RL::LoadScope::Mod ? "mod" : "global", context->activeMod ? context->activeMod : "-",
	              context->buildVersion ? context->buildVersion : "?", stamp);
	Log(line);
#ifdef LEGACY169_TEST_HARNESS
	char harness[256];
	InstallTestHarness(harness, sizeof(harness));
	context->LogWarn(harness);
#endif
	if (!legacy169::BuildMatches(stamp, context->buildName)) {
		Fail("legacy169: needs Diablo II: Resurrected 3.3 or 3.4 under D2RLoader 1.3.1; this game is another version, nothing patched");
		return true;
	}
	char text[4096];
	const legacy169::Config config = legacy169::ParseConfig(ReadConfigText(text, sizeof(text)) ? text : nullptr);
	if (config.enabled) {
		ChooseLayout(config);
	}
	if (!config.enabled) {
		Log("legacy169: enabled = false in legacy169.toml; nothing patched");
		std::snprintf(g_status, sizeof(g_status), "off (enabled = false)");
	} else if (g_layout.width1 == 800) {
		Log("legacy169: the screen is 4:3 or narrower; the stock classic screen fits it, nothing patched");
		std::snprintf(g_status, sizeof(g_status), "off (4:3 screen)");
	} else if (Apply()) {
		const int w = Global(legacy169::RvaScreenSizeX), h = Global(legacy169::RvaScreenSizeY);
		std::snprintf(g_status, sizeof(g_status), "on: %zu patches, %dx%d and %dx%d, compose aspect %d/%d", legacy169::PatchCount,
		              g_layout.width1, legacy169::ClassicHeight1, g_layout.width0, legacy169::ClassicHeight0, g_layout.width1,
		              legacy169::ClassicHeight1);
		std::snprintf(line, sizeof(line), "legacy169: %s; classic screen at load %dx%d", g_status, w, h);
		Log(line);
		const D2RL::ThreadService* uiThreads = nullptr;
		if (InstallHooks()) {
			// the profile may have been read before the plugin loaded: rewrite it now (on the UI thread, which reads it)
			if (context->QueryService(&uiThreads) == D2RL::ServiceQueryResult::Success &&
			    D2RL::HasThreadServiceField(uiThreads, D2RL::ThreadServiceRequiredSize) && uiThreads->runOnUiThread != nullptr &&
			    uiThreads->runOnUiThread(context, &ProfileNow, nullptr) == D2RL::Threads::Result::Success) {
				Log("legacy169: hooks in; the loaded profile is checked on the UI thread");
			} else {
				Log("legacy169: hooks in; no UI thread service, the profile is patched when D2R loads it");
			}
		}
		// D2R set its classic screen up before plugins load (seen with D2RLoader 1.3.1: 800x600 at load, no thread service
		// yet). The setters' code is patched now; the two width globals get the wide width the setter would have written.
		const int mode = reinterpret_cast<int(__fastcall*)()>(context->exeBase + legacy169::RvaGetResolutionMode)();
		const int wide = legacy169::WidenedWidth(mode, w, h, Global(legacy169::RvaViewSizeX), g_layout);
		if (wide != 0) {
			*reinterpret_cast<volatile int*>(context->exeBase + legacy169::RvaScreenSizeX) = wide;
			*reinterpret_cast<volatile int*>(context->exeBase + legacy169::RvaViewSizeX) = wide;
			std::snprintf(line, sizeof(line), "legacy169: classic screen widths set to %d (mode %d)", wide, mode);
			Log(line);
		} else if (w != 0) {
			const D2RL::ThreadService* threads = nullptr;
			if (!config.resetScreen) {
				Log("legacy169: reset_screen = false: the new widths apply from the next resolution change");
			} else if (context->QueryService(&threads) == D2RL::ServiceQueryResult::Success &&
			           D2RL::HasThreadServiceField(threads, D2RL::ThreadServiceRequiredSize) && threads->runOnUiThread != nullptr &&
			           threads->runOnUiThread(context, &ResetScreen, nullptr) == D2RL::Threads::Result::Success) {
				Log("legacy169: the classic screen was set up before the plugin loaded; it is set up again on the UI thread");
			} else {
				context->LogWarn("legacy169: no UI thread service; the new widths apply from the next resolution change");
			}
		}
	}
	if (!context->RegisterConsoleCommand("legacy169", &StatusCommand, "Legacy 16:9 plugin status.")) {
		context->LogWarn("legacy169: console command not registered");
	}
	return true;
}
