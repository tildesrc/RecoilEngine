#!/usr/bin/env python3
"""Compare two frame sets captured by capture-baseline.sh.

Exact comparison does not work here. Two runs of the identical scenario, shot at
the identical simulation frames, still differ on ~37% of pixels - but almost
every one of those differs by a single intensity level. Fog of war, the shadow
map and the info textures refresh on a render-frame cadence, and the render rate
varies with system load and window focus, so blends land a hair differently.

Measured baseline noise between two good runs: ~0.02-0.06% of pixels differ by
more than 8 levels. A real regression (missing shadows, wrong colours, text gone)
moves far more than that, so compare against a threshold instead.

  compare-baseline.py <dir-a> <dir-b> [--threshold N] [--tolerance PCT]
"""
import struct, subprocess, sys, tempfile, pathlib

def to_bmp(png, tmp):
    out = pathlib.Path(tmp) / (png.stem + ".bmp")
    subprocess.run(["sips", "-s", "format", "bmp", str(png), "--out", str(out)],
                   check=True, capture_output=True)
    return out

def load(p):
    d = p.read_bytes()
    off = struct.unpack_from("<I", d, 10)[0]
    w = struct.unpack_from("<i", d, 18)[0]
    h = abs(struct.unpack_from("<i", d, 22)[0])
    return d[off:], w, h

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = {a.split("=")[0]: a.split("=")[1] for a in sys.argv[1:] if "=" in a and a.startswith("--")}
    if len(args) != 2:
        print(__doc__); return 2

    threshold = int(opts.get("--threshold", 8))
    tolerance = float(opts.get("--tolerance", 0.5))
    a_dir, b_dir = pathlib.Path(args[0]), pathlib.Path(args[1])

    frames = sorted(p.name for p in a_dir.glob("frame-*.png"))
    if not frames:
        print(f"no frames in {a_dir}"); return 2

    worst = 0.0
    with tempfile.TemporaryDirectory() as tmp:
        for name in frames:
            pa, pb = a_dir / name, b_dir / name
            if not pb.exists():
                print(f"{name}: MISSING in {b_dir}"); return 1

            da, w, h = load(to_bmp(pa, tmp))
            db, _, _ = load(to_bmp(pb, tmp))
            n = min(len(da), len(db))

            over = 0
            for i in range(0, n, 3):
                if (abs(da[i] - db[i]) > threshold or
                    abs(da[i+1] - db[i+1]) > threshold or
                    abs(da[i+2] - db[i+2]) > threshold):
                    over += 1

            pct = 100.0 * over / (w * h)
            worst = max(worst, pct)
            print(f"{name}: {over} px differ by >{threshold} levels ({pct:.3f}%)")

    verdict = "PASS" if worst <= tolerance else "FAIL"
    print(f"{verdict}: worst frame {worst:.3f}% (tolerance {tolerance}%)")
    return 0 if verdict == "PASS" else 1

sys.exit(main())
