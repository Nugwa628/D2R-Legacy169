# D2R Legacy 16:9

A D2RLoader plugin that shows the classic (legacy) graphics of Diablo II: Resurrected in 16:9 instead of 4:3.
The bottom control panel stays 4:3 and centred. Work in progress.

- 800x600 shows as 1066x600, 640x480 as 853x480 (16:9 at the same height). Only 16:9; a wider window keeps side bars.
- Needs Diablo II: Resurrected 3.3 (build 93847) under D2RLoader 1.3.1. On any other game version the plugin loads,
  logs why and changes nothing. Works without a mod and next to other plugins that do not patch the same bytes.
- Offline / single player (D2RLoader).

## Install

Put `legacy169.dll` into `<game>\d2rloader\plugins\` (all mods) or `<game>\mods\<mod>\d2rloader\plugins\` (one mod).
D2RLoader writes the default `legacy169.toml` into the matching `d2rloader\config\` folder on the first start:

```toml
[legacy169]
enabled = true       # false: load but patch nothing
reset_screen = true  # set the classic screen up again when D2R did it before the plugin loaded
```

The plugin writes its own D2RLoader log (`legacy169.log`). The console command `legacy169` prints its status.

## How it works

D2R renders the classic engine into a fixed SD target (640x480 or 800x600) and composes it into the window with a 4:3
fit. The plugin changes the width immediates of the two screen-size setters and of the SD renderer's size switches, and
points the compose pass's 4:3 constant at its own 1066/600 float. Mouse mapping, culling and the cursor read the
screen-size globals, so they follow. Every site is listed in [src/patch_table.h](src/patch_table.h) with the bytes it
expects; all patches go in as one D2RLoader transaction or not at all.

- The tile cull reaches past the bottom panel (as SGD2FreeRes does), so the bottom corners beside the 4:3 panel show
  the world instead of black wedges.
- Side panels sit at the window edges: the profile loader is hooked and the panel anchors of `_profileSD.json` are
  rewritten after D2R parses it (only the stock 0.5 values). At 1066x600 the 800x600 frame stays beside them (the
  BordersPanel draw is hooked and moves the right frame and its click catcher to the right edge).
- With a panel open the view shifts by the stock 200 / 160 pixels instead of a quarter of the wider screen.
- The hooks check their entry bytes like the patches and are not installed on another build.

Known gap: with a side panel open the automap overlay is off by about the extra width (its centre helper 0xb9a40 is not
patched yet).

## Build

Windows x64, CMake 3.29+, Visual Studio 2026 (MSVC), and a checkout of the D2RLoader PluginSDK:

```powershell
pwsh tools/build.ps1 -Sdk <PluginSDK folder> [-Image <D2R image dump>]
```

Output: `build\Release\legacy169.dll`. The script also runs the unit tests (`build\tests\Release\legacy169_tests.exe`);
with `-Image` they check every expected byte range against a dump of the game image.
