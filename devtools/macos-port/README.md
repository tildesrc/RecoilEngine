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
