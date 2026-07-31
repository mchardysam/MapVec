#!/usr/bin/env python3
"""mapvec_preview.py — render a .mapvec to a PNG so you can eyeball an extract
before flashing anything. Requires Pillow (pip install pillow).

    ./tools/mapvec_preview.py area.mapvec --out area.png --width 1024 --minz 3

This is a second, independent reader of the format (struct-based, no C), which
makes it a soft cross-check as well as a viewer: if the preview looks right and
the device parser's tests pass, the file is almost certainly right.
"""
import argparse
import struct

from PIL import Image, ImageDraw

MAGIC = 0x4D564543
VERSION = 2

BG = (16, 20, 24)
POLY_COL = {0: (20, 50, 75), 1: (28, 46, 28)}          # water, park
LINE_COL = {0: (232, 232, 232), 1: (154, 160, 166), 2: (74, 80, 86), 3: (47, 107, 158)}
LINE_W = {0: 3, 1: 2, 2: 1, 3: 2}
PLACE_COL = (255, 196, 0)


def parse(path):
    b = open(path, "rb").read()
    magic, ver, lat0, lon0 = struct.unpack_from("<IBxii", b, 0)
    if magic != MAGIC:
        raise SystemExit("not a .mapvec (bad magic)")
    if ver != VERSION:
        raise SystemExit(f"unsupported format version {ver} (this tool reads v{VERSION})")
    off = 14   # 4 magic + 1 version + 1 pad + 4 lat0 + 4 lon0
    polys, lines, places = [], [], []

    (npolys,) = struct.unpack_from("<H", b, off); off += 2
    for _ in range(npolys):
        cls, minz, npts = struct.unpack_from("<BBH", b, off); off += 4
        xy = struct.unpack_from(f"<{2*npts}h", b, off); off += 4 * npts
        polys.append((cls, minz, list(zip(xy[0::2], xy[1::2]))))

    (nlines,) = struct.unpack_from("<H", b, off); off += 2
    for _ in range(nlines):
        cls, minz, npts = struct.unpack_from("<BBH", b, off); off += 4
        xy = struct.unpack_from(f"<{2*npts}h", b, off); off += 4 * npts
        lines.append((cls, minz, list(zip(xy[0::2], xy[1::2]))))

    (nplaces,) = struct.unpack_from("<H", b, off); off += 2
    for _ in range(nplaces):
        x, y, ptype, nlen = struct.unpack_from("<hhBB", b, off); off += 6
        name = b[off:off + nlen].decode(errors="replace"); off += nlen
        places.append((x, y, ptype, name))
    return polys, lines, places


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mapvec")
    ap.add_argument("--out", default=None, help="output PNG (default: <mapvec>.png)")
    ap.add_argument("--width", type=int, default=1024)
    ap.add_argument("--minz", type=int, default=3, help="LOD gate: draw minz <= this")
    ap.add_argument("--no-labels", action="store_true")
    args = ap.parse_args()

    polys, lines, places = parse(args.mapvec)
    pts = [p for _, _, seq in (polys + lines) for p in seq]
    if not pts:
        raise SystemExit("empty map")
    minx = min(p[0] for p in pts); maxx = max(p[0] for p in pts)
    miny = min(p[1] for p in pts); maxy = max(p[1] for p in pts)
    scale = (args.width - 20) / max(1, maxx - minx)
    H = int((maxy - miny) * scale) + 20

    def s(p):
        return (10 + (p[0] - minx) * scale, H - 10 - (p[1] - miny) * scale)  # +y north = up

    img = Image.new("RGB", (args.width, H), BG)
    dr = ImageDraw.Draw(img)
    for cls, minz, seq in polys:
        if minz > args.minz or cls not in POLY_COL:
            continue
        dr.polygon([s(p) for p in seq], fill=POLY_COL[cls])
    for cls, minz, seq in lines:
        if minz > args.minz or cls not in LINE_COL:
            continue
        dr.line([s(p) for p in seq], fill=LINE_COL[cls], width=LINE_W[cls])
    for x, y, ptype, name in places:
        sx, sy = s((x, y))
        dr.ellipse([sx - 3, sy - 3, sx + 3, sy + 3], fill=PLACE_COL)
        if name and not args.no_labels:
            dr.text((sx + 5, sy - 5), name, fill=PLACE_COL)

    out = args.out or (args.mapvec.rsplit(".", 1)[0] + ".png")
    img.save(out)
    print(f"wrote {out}  ({len(polys)} polygons, {len(lines)} lines, {len(places)} places)")
    print("map data (c) OpenStreetMap contributors, ODbL")


if __name__ == "__main__":
    main()
