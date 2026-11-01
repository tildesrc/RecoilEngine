# macOS core-profile port: rendering regression check

Tooling to catch rendering regressions while changing shared engine code on
`macos-headless-port`. Not a committed golden image set — capture *before* a
change and *after* it, then compare the two.

```sh
./capture-baseline.sh /tmp/before      # before touching shared code
# ... make the change, rebuild ...
./capture-baseline.sh /tmp/after
./compare-baseline.py /tmp/before /tmp/after
```

`FRAMES` and `SCRIPT` override the defaults, e.g. `FRAMES=600,900,1200`.

## Why the comparison is tolerant, not exact

Two runs of the identical scenario, captured at the identical simulation
frames, still differ on roughly a third of their pixels. Almost every one of
those differs by a **single intensity level**: fog of war, the shadow map and
the info textures refresh on a render-frame cadence, and the render rate varies
with system load and whether the window has focus, so blends land fractionally
differently.

Measured separation:

| Comparison | Pixels differing by >8 levels |
| --- | --- |
| Two good runs of the same scenario | 0.02% – 0.06% |
| Scenario staged vs. scenario missing | 87% – 89% |

So `compare-baseline.py` counts pixels differing by more than a threshold
(default 8 levels) and fails above a tolerance (default 0.5%).

## BAR widget config

Two entries in `<write-dir>/LuaUI/Config/BYAR.lua` have to be set for the
minimap to appear, under the `order` table at the end of the file (the file's
own header notes that `order = 0` means a disabled widget):

```lua
Minimap = 20,                          -- the engine minimap widget
["Picture-in-Picture Minimap"] = 0,    -- PiP, which otherwise takes over
```

`gui_pip.lua` replaces the engine minimap when it enters minimap mode: it slaves
and minimises the engine one and disables `gui_minimap`. Its own minimap does
not render on this port, so with PiP enabled there is no minimap at all - and
because the disable is **persisted**, a run where PiP took over leaves
`Minimap = 0` behind and neither draws one on later runs.

BAR rewrites this file, so the setting is lost whenever the write-dir is reset.
Re-apply it after any fresh setup.

## Gotchas

- **The scenario must actually stage.** BA registers a widget it has not seen
  before in its own config and only loads it from the *next* run onward, so the
  first run against a fresh write-dir silently captures an empty default scene.
  `capture-baseline.sh` greps the log for `BASELINE: staged` and fails if absent.
- **Capture fires at or past a target frame, not on an exact match.** The sim
  runs at a fixed 30Hz while the render rate varies, so the render loop often
  never observes a given frame number.
- **The scenario is deliberately static** — no combat. Weapon spread and
  particle effects draw on the unsynced RNG and do not reproduce frame-for-frame.
  It still covers terrain, models, team colour, features, shadows, fog of war,
  the build menu with icons, and UI text.
- **The display has to be awake.** macOS does not composite a window on a
  sleeping display, so `Cocoa_GL_SwapWindow` blocks forever and the engine never
  leaves the splash screen; the watchdog then logs a hang every 30s and not one
  frame is ever drawn. Nothing about the GL settings avoids this — turning vsync
  off does not help, because the block is in the compositor, not the swap
  interval. `capture-baseline.sh` now recognises the signature and says so
  instead of reporting a graphics failure. **Any GL measurement is therefore
  impossible while the machine is idle with the screen off** — only
  `spring-headless` runs unattended, and it has no GL at all.

## Probing a render-to-texture target

`SPRING_DBG_R2T_STATS=<substring>` makes `gl.RenderToTexture` sample its target
after the Lua function has run and log the fraction of non-zero colour and alpha
pixels, for every texture whose name contains the substring (empty matches all).
It fires on every 500th call per texture, since widgets issue thousands a frame.

This exists because counting `gl.RenderToTexture` calls proves only that the
widget ran. The PiP minimap issues thousands of them and still presents nothing,
and "the content is produced, the presentation is broken" was an assumption, not
a measurement. Reading the target back settles which half is at fault.

# Running Beyond All Reason

BAR is the goal content; BA is kept because old content exercises the
compatibility paths hardest.

## Fetching

pr-downloader needs three overrides. BAR ships from its own rapid repo, and its
server returns 404 for the `streamer.cgi` protocol pr-downloader prefers, so the
streaming path has to be turned off or every pool fetch fails:

```sh
cd <build>
export PRD_RAPID_REPO_MASTER="https://repos.beyondallreason.dev/repos.gz"
export PRD_RAPID_USE_STREAMER=false
export PRD_HTTP_SEARCH_URL="https://files-cdn.beyondallreason.dev/find"
./tools/pr-downloader/src/pr-downloader --filesystem-writepath "$(pwd)" --download-game "byar:test"
./tools/pr-downloader/src/pr-downloader --filesystem-writepath "$(pwd)" --download-game "byar-chobby:test"
```

**`PRD_HTTP_SEARCH_URL` is needed when running the engine too, not just when
pre-fetching** - maps are downloaded on demand from the menu, and games and
maps come from different places. Games come through rapid, which the first two
variables point at; maps are looked up one at a time through a springfiles-style
`find` endpoint, and the compiled-in default is the *old Spring* one
(`springfiles.springrts.com/json.php`), which carries no BAR content at all. It
answers `200 OK` with an empty JSON array, so the failure surfaces only as
`Download Failed ... errorID: 2` in the log and "download failed" in the menu,
with a successful HTTP exchange right above it. Nothing about it looks like a
missing setting.

Sanity-check the endpoint directly before suspecting the engine:

```sh
curl -s "https://files-cdn.beyondallreason.dev/find?category=map&springname=Shallow%20Straits%20v1.0.1"
```

A JSON object with a `mirrors` array is a working endpoint; `[]` means the
service does not have that map.

## The menu

Set `DefaultLuaMenu` in `springsettings.cfg` to the Chobby archive name (as the
download reports it, e.g. `BYAR Chobby test-4668-2e85fd3`), then launch with no
start script.

Chobby also needs `<write-dir>/chobby_config.json`. **It is not the
`dist_cfg/config.json` inside the Chobby archive** — that is a launcher
template with a `setups` list and no `server` key, and deploying it is worse
than having no file at all: with the file absent Chobby falls back to a working
default, while the wrong file makes `config.server` nil and takes the whole
lobby down. The file Chobby actually wants is:

```json
{
	"server": {
		"address": "road-flag.bnr.la",
		"port": 8200,
		"serverName": "BAR",
		"protocol": "spring"
	},
	"game": "byar"
}
```

`game` matters: the fallback uses `"generic"`, whose game config has no
`ShortenNameString`, and that one missing function kills the Chili lobby widget,
leaves `WG.Chobby` unset and renders a completely black menu.

## BAR widget config

Two entries in `<write-dir>/LuaUI/Config/BYAR.lua` have to be set for the
minimap to appear, under the `order` table at the end of the file (the file's
own header notes that `order = 0` means a disabled widget):

```lua
Minimap = 20,                          -- the engine minimap widget
["Picture-in-Picture Minimap"] = 0,    -- PiP, which otherwise takes over
```

`gui_pip.lua` replaces the engine minimap when it enters minimap mode: it slaves
and minimises the engine one and disables `gui_minimap`. Its own minimap does
not render on this port, so with PiP enabled there is no minimap at all - and
because the disable is **persisted**, a run where PiP took over leaves
`Minimap = 0` behind and neither draws one on later runs.

BAR rewrites this file, so the setting is lost whenever the write-dir is reset.
Re-apply it after any fresh setup.

## Gotchas

- Spring rewrites `springsettings.cfg` on exit, so edits made while an instance
  is running are silently lost.
- `<build>/fonts` must exist (copy `cont/fonts`), or startup aborts with
  "Failed to load FontFile".
- **A baseline is only valid for the config it was captured under.** Editing
  `springsettings.cfg` (adding `DefaultLuaMenu`, changing `Water`) changes what
  is drawn and invalidates an older capture - a comparison then reports a huge
  difference that is not a regression. Recapture the "before" side after any
  config change, or compare two fresh runs to check self-consistency.
- Adding BAR content to the data directory makes `testUnitSync` fail; it is not
  isolated from what is installed. Run `ctest -E testUnitSync`, or move
  `pool/` and `packages/` aside.

## The Zink-over-Metal spike (measured 2026-11-01)

Result: **not viable today.** Zink runs, on the GPU, and reports **OpenGL 2.1 /
GLSL 1.20** - far below the OpenGL 4.1 / GLSL 4.10 that Apple's own driver
gives. Every attempt to create a 3.3-or-newer core context fails with
`EGL_BAD_MATCH`. The stack is a large regression, not a way past the 4.1
ceiling.

What does work: `brew install mesa molten-vk vulkan-loader` gives a working
Vulkan 1.4 on Metal through Mesa's own driver (KosmicKrisp) - `vkprobe` sees
`Apple M3`. Zink then loads on it and identifies itself as
`zink Vulkan 1.4(Apple M3 (MESA_KOSMICKRISP))`. So the plumbing is fine; the
feature coverage is not. Zink says so on startup:

```
WARNING: ... the selected Vulkan device (Apple M3) doesn't support base Zink
requirements: have_EXT_custom_border_color
```

Missing base requirements cap what Zink will advertise, and it drops to 2.1.

To reproduce (`glprobe.c` walks 4.6 down to 3.2 and reports the first core
profile granted, then checks the extensions BAR needs):

```sh
export DYLD_LIBRARY_PATH="$(brew --prefix mesa)/lib:/opt/homebrew/lib"
export VK_ICD_FILENAMES=/opt/homebrew/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json
export LIBGL_DRIVERS_PATH="$(brew --prefix mesa)/lib/dri"
export EGL_PLATFORM=surfaceless GALLIUM_DRIVER=zink
./glprobe
```

Two traps worth knowing. `MESA_LOADER_DRIVER_OVERRIDE=zink` is silently
ignored here and you get `llvmpipe`, Mesa's software rasteriser, which reports
a full GL 4.6 with every extension present - a very convincing false positive.
`GALLIUM_DRIVER=zink` is the variable that works. And `EGL_PLATFORM=surfaceless`
is required or `eglInitialize` fails outright.

That llvmpipe run is a useful control, though: Mesa itself is complete, so the
gap is specifically the Vulkan-on-Metal driver's feature coverage, not Zink.

Untested: **MoltenVK**, the more mature Vulkan-on-Metal driver. Homebrew ships
only a static `libMoltenVK.a`, so its ICD points at a `libMoltenVK.dylib` that
does not exist and the loader cannot use it. Getting a shared build (from the
Vulkan SDK, or built from source) would be needed to tell whether it satisfies
more of Zink's requirements than KosmicKrisp does. Until that is measured, the
spike has only ruled out one of the two drivers.
