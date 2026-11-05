#!/usr/bin/env python3
"""Extract one file from a rapid-installed BAR archive (a .sdp + the pool).

The macOS override archive in bar-overrides/ is stored as patches against stock
BAR content, so regenerating or re-applying a patch needs the pristine original.
BAR arrives through rapid, which does not unpack: the archive is an .sdp index
in packages/ and the file bodies are gzipped blobs in pool/, named by md5. This
reads the index and pulls one body out.

  ./bar-extract.py <write-dir> LuaUI/Shaders/foo.glsl > foo.glsl
  ./bar-extract.py <write-dir> --list | grep sensor

With no --sdp it picks the largest .sdp in <write-dir>/packages, which is BAR
itself (the other is Chobby).
"""
import gzip, os, struct, sys


def read_sdp(path):
    """-> {name: md5hex}. Entry: u8 namelen, name, 16B md5, 4B crc, 4B size."""
    out = {}
    with gzip.open(path, 'rb') as f:
        data = f.read()
    i = 0
    while i < len(data):
        n = data[i]; i += 1
        name = data[i:i + n].decode('utf-8', 'replace'); i += n
        md5 = data[i:i + 16].hex(); i += 16
        i += 8  # crc32 + size
        out[name] = md5
    return out


def main():
    argv = sys.argv[1:]
    sdp = None
    if '--sdp' in argv:
        k = argv.index('--sdp'); sdp = argv[k + 1]; del argv[k:k + 2]
    if len(argv) < 2:
        sys.exit(__doc__)
    wdir, want = argv[0], argv[1]

    if sdp is None:
        pkgs = [os.path.join(wdir, 'packages', p)
                for p in os.listdir(os.path.join(wdir, 'packages'))
                if p.endswith('.sdp')]
        if not pkgs:
            sys.exit('no .sdp in %s/packages' % wdir)
        sdp = max(pkgs, key=os.path.getsize)

    index = read_sdp(sdp)
    if want == '--list':
        for name in sorted(index):
            print(name)
        return

    # archive paths are case-insensitive; BAR's casing is not always what you type
    hit = index.get(want) or next(
        (v for k, v in index.items() if k.lower() == want.lower()), None)
    if hit is None:
        sys.exit('%s not in %s' % (want, os.path.basename(sdp)))

    blob = os.path.join(wdir, 'pool', hit[:2], hit[2:] + '.gz')
    with gzip.open(blob, 'rb') as f:
        sys.stdout.buffer.write(f.read())


main()
