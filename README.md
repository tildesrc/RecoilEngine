# Playing Beyond All Reason on macOS

This is a fork of the Recoil engine whose `macos` branch makes it build and run
natively on Apple Silicon macOS, far enough to play BAR single-player. For the
engine itself - what it is, releases, and building it on a supported platform -
see upstream:
[beyond-all-reason/RecoilEngine](https://github.com/beyond-all-reason/RecoilEngine).

It is an experiment, not a supported port:
Apple's OpenGL stops at a 4.1 core profile, BAR's modern render path needs 4.3,
and the gap is bridged by engine changes plus a set of patches against BAR's own
content.

**What you get:** terrain, units, models, shadows, team colour, the build menu,
the Chobby menu and lobby, health bars, the whole icon and selection-marker
family, sensor ranges, grass, reclaim fields, and jet/orb/paralyze effects.
Shader programs that fail to compile are down from 29 to 12, and 11 of those 12
are geometry-shader variants whose NoGS twins work and which BAR falls back to
on its own.

**What you don't:** `radarPreviewCoverage`, which writes to its buffer and so
needs image load/store (GL 4.2) rather than a read-only buffer texture. The
picture-in-picture minimap does not render either — use the engine minimap, see
[Widget config](#widget-config). Multiplayer is out of scope: the override
archive changes the set of loaded archives, and every client in a game has to
agree on that. Expect to be the only person on this engine.

For why any of it works the way it does, see
[devtools/macos-port/PORT-NOTES.md](devtools/macos-port/PORT-NOTES.md) and
[devtools/macos-port/SSBO-FALLBACK.md](devtools/macos-port/SSBO-FALLBACK.md). This
file is just the recipe.

---

## 1. Dependencies

```sh
brew install cmake devil libunwind-headers openal-soft libvorbis libogg expat sevenzip
```

## 2. Clone

```sh
git clone https://github.com/tildesrc/RecoilEngine.git
cd RecoilEngine
git submodule update --init --recursive
```

That is all — nothing to patch by hand. The `rts/lib/streflop` submodule points
at a fork, [tildesrc/streflop](https://github.com/tildesrc/streflop), which
already carries the one fix macOS needs, so `submodule update` is safe to re-run
and leaves nothing dirty.

What the fork changes: `streflop_cond.h` carried a `std::hypot` shim added for
macOS in 2013. It is obsolete — libc++ has had `std::hypot` for years — and it
no longer compiles, because it calls a `math::sqrtf` that does not exist in that
namespace. Upstream streflop has simply never built on macOS. Dropping the shim
is the whole change, kept as
`devtools/macos-port/streflop-drop-hypot-shim.patch` for anyone who wants to
apply it to upstream instead of using the fork.

## 3. Build

```sh
cmake -S . -B build-all -DCMAKE_BUILD_TYPE=RELWITHDEBINFO -DAI_TYPES=NONE \
  -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/openal-soft;/opt/homebrew/opt/expat;/opt/homebrew/opt/libunwind-headers;/opt/homebrew" \
  -DCUSTOM_COMPILER_DEFINITIONS="-I/opt/homebrew/opt/openal-soft/include/AL"
cmake --build build-all --target engine-legacy -j
```

Several of those formulae are keg-only, so CMake cannot find them without the
explicit prefixes. `CUSTOM_COMPILER_DEFINITIONS` is needed only for
`engine-legacy`, the graphics build: keg-only openal-soft puts `efx.h` under
`include/AL` while the engine includes it as `<efx.h>`. The target produces
`build-all/spring`.

Fonts are not copied by the build, and startup aborts with "Failed to load
FontFile" without them:

```sh
cp -R cont/fonts build-all/
```

From here on `build-all` is the **write dir** — the data directory the engine
reads content from and writes config into.

## 4. Fetch BAR

BAR is not in this repo and is not a git checkout; it ships through rapid. The
downloader comes from the same build:

```sh
cmake --build build-all --target pr-downloader_cli -j
```

It needs three overrides to reach BAR, because BAR runs its own rapid repo and
its server 404s the streaming protocol pr-downloader prefers:

```sh
cd build-all
export PRD_RAPID_REPO_MASTER="https://repos.beyondallreason.dev/repos.gz"
export PRD_RAPID_USE_STREAMER=false
export PRD_HTTP_SEARCH_URL="https://files-cdn.beyondallreason.dev/find"
PRD=./tools/pr-downloader/src/pr-downloader
$PRD --filesystem-writepath "$(pwd)" --download-game "byar:test"
$PRD --filesystem-writepath "$(pwd)" --download-game "byar-chobby:test"
$PRD --filesystem-writepath "$(pwd)" --download-map "DeltaSiege"
cd ..
```

**`PRD_HTTP_SEARCH_URL` matters when running the engine too, not just here.**
Games come through rapid, but maps are fetched one at a time through a
springfiles-style `find` endpoint, and the compiled-in default is the old Spring
one, which carries no BAR content. It answers `200 OK` with an empty array, so a
missing map surfaces only as `Download Failed ... errorID: 2` and "download
failed" in the menu, directly below a successful HTTP exchange. Export it in
whatever shell you launch from.

Check an endpoint directly before suspecting the engine — a `mirrors` array
means it works, `[]` means the service does not have that map:

```sh
curl -s "https://files-cdn.beyondallreason.dev/find?category=map&springname=DeltaSiege"
```

## 5. Deploy the content overrides

Some of BAR's widgets and shaders have to be patched to run on 4.1. Those
patches are in `devtools/macos-port/bar-overrides/`, stored as diffs against stock BAR rather than
as whole files, because BAR's content is a read-only rapid archive and the
deployed copy lives in a write dir that BAR itself rewrites.

```sh
./devtools/macos-port/deploy-bar-overrides.py build-all
```

This extracts each original from the rapid pool, applies its patch, and writes
`build-all/games/bar-macos.sdd/`, with its `depend` line pointed at whatever BAR
version you actually downloaded. `--check` verifies every patch applies and
writes nothing.

Nothing in BAR's archive is modified. Deleting the `.sdd` restores stock
behaviour.

## 6. Configure

```sh
cp devtools/macos-port/springsettings-macos.cfg build-all/springsettings.cfg
cp devtools/macos-port/chobby_config.json build-all/
```

Then edit `build-all/springsettings.cfg`:

- **`SpringData`** — absolute paths, `<build-all>:<repo>/cont`. The placeholder
  will not work.
- **`DefaultLuaMenu`** — the Chobby archive name exactly as the download
  reported it, e.g. `BYAR Chobby test-4668-2e85fd3`.

The mandatory settings are already in the template: `ForceCoreContext`,
`GLContextMajorVersion/MinorVersion` at 4.1 (without them the engine tries 4.4
through 4.6, fails every attempt and aborts with no context at all),
`ForceDisableGL4`, and `ExtraMutators = BAR macOS overrides 1`, which is what
loads the override archive.

The engine rewrites this file on exit, so edit it only while nothing is
running. `chobby_config.json` is **not** the `dist_cfg/config.json` inside the
Chobby archive — that one is a launcher template with no `server` key, and
deploying it is worse than having no file at all: `config.server` goes nil and
the whole lobby goes down.

## 7. Play

Keep the display awake. macOS will not composite a window on a sleeping
display, so the buffer swap blocks forever, the engine never leaves the splash
screen, and the watchdog logs a hang every 30s. No GL setting avoids this.

Through the menu:

```sh
cd build-all && ./spring --write-dir "$(pwd)"
```

Straight into a skirmish, no menu:

```sh
cp devtools/macos-port/bar-skirmish.txt build-all/
cd build-all && ./spring --write-dir "$(pwd)" bar-skirmish.txt
```

`GameType` in that file must match the installed BAR archive exactly, version
included — `deploy-bar-overrides.py` prints the right string. A mismatch fails
with "Dependent archive ... not found", which reads like a missing archive
rather than a stale version.

### Widget config

Two entries in `build-all/LuaUI/Config/BYAR.lua`, in the `order` table at the
end, are needed for a minimap (`order = 0` means disabled):

```lua
Minimap = 20,                          -- the engine minimap widget
["Picture-in-Picture Minimap"] = 0,    -- PiP, which otherwise takes over
```

`gui_pip.lua` replaces the engine minimap when it enters minimap mode, and its
own minimap does not render on this port — so with PiP enabled there is no
minimap at all. The disable is **persisted**, so one run where PiP took over
leaves `Minimap = 0` behind and later runs draw none either.

BAR rewrites this file, so re-apply after any write-dir reset.

### Camera

The template sets `CamMode = 3` (the Spring camera) deliberately. In
RotOverhead — which some content saves and restores over the engine's setting —
rotation comes only from dragging the middle mouse button, which an Apple
trackpad does not have, so that camera cannot be rotated at all. In the Spring
camera, **Option + arrow keys** rotates.

### Keys

**Never hand-edit `build-all/uikeys.txt`.** BAR adopts a hand-edited root
`uikeys.txt` as a complete keybind *profile*, names it `Custom (N)`, and rewrites
the file from it — leaving only the lines you added and silently discarding
every other binding. A healthy file is ~370 lines.

To add a binding, use the additive path instead:
`build-all/LuaUI/Widgets/uikeys.txt`, which BAR's "Lua UiKeys loader" widget
feeds through `Spring.SendCommands` on top of the profile. That widget ships
disabled, so also set `["Lua UiKeys loader"] = 1` in `LuaUI/Config/BYAR.lua`.

To recover from a clobbered file, delete both `uikeys.txt` and
`LuaUI/Config/keybind_profiles.json`; BAR rewrites them from its defaults.
`/keydebug` in the in-game console logs what the engine resolved for a key.

## When BAR updates

The patches in `devtools/macos-port/bar-overrides/` are written against specific BAR files, so a
BAR update can invalidate them. After downloading a new version:

```sh
./devtools/macos-port/deploy-bar-overrides.py build-all --check
```

A patch that no longer applies means BAR changed that file underneath it. Then
you are editing the port, not just running it: fix the deployed file in
`build-all/games/bar-macos.sdd/` until it works in-game, and run

```sh
./devtools/macos-port/regen-patches.sh
```

which rewrites every patch from the deployed tree — the opposite direction to
the deploy script — and dry-run re-applies them all.
`devtools/macos-port/bar-extract.py` pulls a
single pristine file out of the archive, and `--list` dumps every path in it,
which is how to find BAR's actual capitalisation.

The patches are the durable copy of this work; the deployed `.sdd` is gitignored
and disposable. `devtools/macos-port/SSBO-FALLBACK.md` is the guide to writing a
new conversion, and `devtools/macos-port/snippets/` holds the GLSL preambles they
share.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| `no member named 'sqrtf' in namespace 'math'` | `rts/lib/streflop` is on upstream, not the fork; re-run `git submodule sync && git submodule update --init` |
| "Failed to load FontFile" | `build-all/fonts` missing; copy `cont/fonts` |
| Aborts having created no GL context | the 4.1 GL settings are missing from `springsettings.cfg` |
| Never leaves the splash screen, watchdog hangs | the display is asleep |
| "Dependent archive ... not found" | `GameType` or the override's `depend` names the wrong BAR version; re-run the deploy script |
| Completely black menu | wrong `chobby_config.json` (see step 6) |
| "download failed", `errorID: 2` | `PRD_HTTP_SEARCH_URL` not exported for the engine process |
| No minimap | PiP took over; see [Widget config](#widget-config) |
| Camera will not rotate | RotOverhead; set `CamMode = 3` |
| Keys silently do nothing | a clobbered root `uikeys.txt` |
| Overrides seem not to load | stale `build-all/cache`; the deploy script clears it |
| `testUnitSync` fails | it is not isolated from installed content; `ctest -E testUnitSync` |

A run that behaves strangely after a content change usually wants
`rm -rf build-all/cache build-all/fontcache` first — a stale `ArchiveCache` hangs
VFS init.
