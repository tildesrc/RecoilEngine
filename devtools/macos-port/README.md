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

# Running Beyond All Reason

BAR is the goal content; BA is kept because old content exercises the
compatibility paths hardest.

## Fetching

pr-downloader needs two overrides. BAR ships from its own rapid repo, and its
server returns 404 for the `streamer.cgi` protocol pr-downloader prefers, so the
streaming path has to be turned off or every pool fetch fails:

```sh
cd <build>
export PRD_RAPID_REPO_MASTER="https://repos.beyondallreason.dev/repos.gz"
export PRD_RAPID_USE_STREAMER=false
./tools/pr-downloader/src/pr-downloader --filesystem-writepath "$(pwd)" --download-game "byar:test"
./tools/pr-downloader/src/pr-downloader --filesystem-writepath "$(pwd)" --download-game "byar-chobby:test"
```

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
