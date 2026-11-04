// legacy169: a D2RLoader plugin that shows D2R's legacy (classic 2D) graphics in 16:9 instead of 4:3.
//
// 800x600 becomes 1066x600 and 640x480 becomes 853x480; the bottom control panel stays 4:3 and centred (patch_table.h has
// the sites). The plugin only patches bytes: every patch is checked against the bytes it expects first, and all of them go
// in through one D2RLoader transaction, so on any other game image, or with bytes another plugin changed, nothing changes.
// Mouse mapping, culling and the cursor read the screen-size globals and follow the new width by themselves.
//
// Config: <scope>\d2rloader\config\legacy169.toml (D2RLoader writes the embedded default when it is missing). Log: the
// plugin's own D2RLoader log. Console command: `legacy169` prints the status.
#include <D2RLPlugin/api.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "patch_table.h"

namespace {

constexpr const char* PluginVersion = "0.1.0";

constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .abiVersion = D2RL_PLUGIN_ABI_VERSION,
    .id = "legacy169",
    .name = "Legacy 16:9",
    .version = PluginVersion,
    .author = "Nugwa628",
    .description = "Legacy (classic) graphics in 16:9: 1066x600 and 853x480; the control panel stays 4:3.",
    .flags = D2RL::PluginFlags::Client,
};

const D2RL::PluginContext* g_context = nullptr;
char g_status[320] = "not loaded";
volatile LONG g_resets = 0;

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

auto Apply() -> bool {
	char line[256];
	const legacy169::CheckResult check = legacy169::CheckAll(&ReadImage, const_cast<D2RL::PluginContext*>(g_context));
	if (!check.ok) {
		const legacy169::Patch& p = legacy169::Patches[check.failed];
		char want[32], got[32];
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
	for (const legacy169::Patch& p : legacy169::Patches) {
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

auto __cdecl StatusCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept -> D2RL::ConsoleCommandResult {
	char line[480];
	std::snprintf(line, sizeof(line), "legacy169 %s: %s; classic screen %dx%d (mode %d), screen resets %ld", PluginVersion, g_status,
	              Global(legacy169::RvaScreenSizeX), Global(legacy169::RvaScreenSizeY),
	              reinterpret_cast<int(__fastcall*)()>(g_context->exeBase + legacy169::RvaGetResolutionMode)(), g_resets);
	command->plugin->WriteConsoleMessage(line);
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
	if (!legacy169::BuildMatches(stamp, context->buildName)) {
		Fail("legacy169: needs Diablo II: Resurrected 3.3 (build 93847) under D2RLoader 1.3.1; this game is another version, nothing patched");
		return true;
	}
	char text[4096];
	const legacy169::Config config = legacy169::ParseConfig(ReadConfigText(text, sizeof(text)) ? text : nullptr);
	if (!config.enabled) {
		Log("legacy169: enabled = false in legacy169.toml; nothing patched");
		std::snprintf(g_status, sizeof(g_status), "off (enabled = false)");
	} else if (Apply()) {
		const int w = Global(legacy169::RvaScreenSizeX), h = Global(legacy169::RvaScreenSizeY);
		std::snprintf(g_status, sizeof(g_status), "on: %zu patches, %dx%d and %dx%d, compose aspect %d/%d", legacy169::PatchCount,
		              legacy169::ClassicWidth1, legacy169::ClassicHeight1, legacy169::ClassicWidth0, legacy169::ClassicHeight0,
		              legacy169::ClassicWidth1, legacy169::ClassicHeight1);
		std::snprintf(line, sizeof(line), "legacy169: %s; classic screen at load %dx%d", g_status, w, h);
		Log(line);
		if (w != 0) {
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
