#!/usr/bin/env python3
"""Build <write-dir>/games/bar-macos.sdd from the patches in bar-overrides/.

The overrides are stored as patches against stock BAR, not as whole files: BAR
arrives through rapid as a read-only archive, and the deployed copy lives in a
gitignored write dir that BAR itself rewrites. So the patches are the durable
copy and this script is what turns them back into a loadable archive.

  ./deploy-bar-overrides.py <write-dir>
  ./deploy-bar-overrides.py <write-dir> --check   # verify, change nothing

For each patch it reads the target path out of the diff header, pulls that file
out of the rapid pool, applies the patch, and writes the result into the .sdd at
the same path. The `depend` line in modinfo.lua is rewritten to whatever BAR
version is actually installed, since a mismatch there fails as "Dependent
archive ... not found" and reads like a missing archive rather than a stale
pin.
"""
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OVERRIDES = os.path.join(HERE, "bar-overrides")
EXTRACT = os.path.join(HERE, "bar-extract.py")


def extract(wdir, path):
    """Pull one file out of the installed BAR archive, as bytes."""
    p = subprocess.run([sys.executable, EXTRACT, wdir, path],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        raise SystemExit("could not extract %s from the BAR archive:\n  %s"
                         % (path, p.stderr.decode().strip()))
    return p.stdout


def bar_archive_name(wdir):
    """-> "Beyond All Reason test-31765-420384b", as the engine registers it.

    The gametype string an archive registers under is name .. " " .. version,
    not name, and `depend` has to match it exactly.
    """
    modinfo = extract(wdir, "modinfo.lua").decode("utf-8", "replace")
    name = re.search(r'name\s*=\s*"([^"]+)"', modinfo)
    version = re.search(r'version\s*=\s*"([^"]+)"', modinfo)
    if not name or not version:
        raise SystemExit("could not read name/version from BAR's modinfo.lua")
    return "%s %s" % (name.group(1), version.group(1))


def patch_target(patch_path):
    """The path a patch applies to, from its "--- a/<path>" header."""
    with open(patch_path) as fh:
        first = fh.readline()
    if not first.startswith("--- a/"):
        raise SystemExit("%s does not start with a '--- a/<path>' header"
                         % os.path.basename(patch_path))
    return first[len("--- a/"):].strip()


def render_modinfo(depend):
    """modinfo.lua with its depend list pointed at the installed BAR."""
    src = open(os.path.join(OVERRIDES, "modinfo.lua")).read()
    out, n = re.subn(r'(depend\s*=\s*\{\s*\n\s*)"[^"]*"',
                     lambda m: m.group(1) + '"%s"' % depend, src, count=1)
    if n != 1:
        raise SystemExit("could not rewrite the depend line in modinfo.lua")
    return out


def main():
    argv = sys.argv[1:]
    check = "--check" in argv
    argv = [a for a in argv if a != "--check"]
    if len(argv) != 1:
        sys.exit(__doc__)
    wdir = os.path.abspath(argv[0])

    if not os.path.isdir(os.path.join(wdir, "packages")):
        sys.exit("%s has no packages/ - BAR does not look installed there.\n"
                 "Fetch it first; see the README." % wdir)

    depend = bar_archive_name(wdir)
    print("installed BAR archive: %s" % depend)

    sdd = os.path.join(wdir, "games", "bar-macos.sdd")
    patches = sorted(p for p in os.listdir(OVERRIDES) if p.endswith(".patch"))
    if not patches:
        sys.exit("no patches in %s" % OVERRIDES)

    # Build into a scratch tree first: a half-written .sdd is worse than none,
    # because the engine happily loads it and the missing files fall back to
    # stock with no indication that a deploy failed.
    staging = sdd + ".new"
    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(staging)

    failed = []
    for name in patches:
        target = patch_target(os.path.join(OVERRIDES, name))
        dest = os.path.join(staging, target)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as fh:
            fh.write(extract(wdir, target))
        r = subprocess.run(["patch", "-p1", "--no-backup-if-mismatch",
                            "-i", os.path.join(OVERRIDES, name)],
                           cwd=staging, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
        ok = r.returncode == 0
        print("  %-34s %-48s %s" % (name, target, "ok" if ok else "FAILED"))
        if not ok:
            failed.append(name)
            print(r.stdout.decode().rstrip())

    with open(os.path.join(staging, "modinfo.lua"), "w") as fh:
        fh.write(render_modinfo(depend))

    if failed:
        shutil.rmtree(staging, ignore_errors=True)
        sys.exit("\n%d patch(es) did not apply: %s\n"
                 "BAR has almost certainly changed the file underneath them. "
                 "Re-extract the original with bar-extract.py, redo the edit by "
                 "hand, and regenerate the patch." % (len(failed), ", ".join(failed)))

    if check:
        shutil.rmtree(staging, ignore_errors=True)
        print("\nall %d patches apply to the installed BAR. Nothing written "
              "(--check)." % len(patches))
        return

    shutil.rmtree(sdd, ignore_errors=True)
    os.rename(staging, sdd)
    print("\ndeployed %d files into %s" % (len(patches), sdd))

    # The archive cache is keyed on mtime and can hide a fresh deploy, which
    # looks exactly like the overrides not working.
    cache = os.path.join(wdir, "cache")
    if os.path.isdir(cache):
        shutil.rmtree(cache, ignore_errors=True)
        print("cleared %s so the archive scan picks it up" % cache)

    print("\nNow set, in <write-dir>/springsettings.cfg:\n"
          "  ExtraMutators = BAR macOS overrides 1")


main()
