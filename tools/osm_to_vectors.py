#!/usr/bin/env python3
"""osm_to_vectors.py — OpenStreetMap -> .mapvec (format v2, see docs/FORMAT.md).

Input is Overpass JSON with way geometry (`out geom`): a file you fetched yourself,
or let --fetch download it for the bbox (cached to the given path, so repeat runs
don't hit the API). Output is the compact fixed-point vector map that mapvec.h
parses on the device:

  * project lat/lon to a flat local frame about the bbox centre (cos(lat0) baked in
    here, so the device does no per-point trig), quantise to int16 METRES;
  * closed natural=water / leisure=park ways become filled POLYGONS, highways and
    waterways become classed POLYLINES (zoom-gated by class), schools/suburbs/pools
    become labelled places;
  * Douglas-Peucker simplify at --tol metres; polygon rings are capped at 128
    points (tolerance doubles until they fit) so renderers can fill on the stack.

    ./tools/osm_to_vectors.py area.json --fetch \
        --bbox=37.74,-122.46,37.81,-122.39 --tol 4 --out area.mapvec

(Use the --bbox=... form. A bare "--bbox <value>" breaks whenever the value starts
with a minus sign — any southern latitude — because argparse reads it as an option.)

Prints a size table across tolerances, then writes --out.

Map data (c) OpenStreetMap contributors, licensed ODbL. Keep that attribution with
any .mapvec you distribute: https://www.openstreetmap.org/copyright
"""
import argparse
import json
import math
import os
import struct
import urllib.request

MAGIC = 0x4D564543  # 'MVEC'
VERSION = 2  # v2: one pad byte after version, so geometry is 2-byte aligned
MLAT = 111132.0
RING_MAX_PTS = 128

# highway tag -> (line class, minimum zoom). class: 0 major, 1 minor, 2 path.
HIGHWAY = {
    "motorway": (0, 0), "trunk": (0, 0), "primary": (0, 0), "secondary": (0, 0),
    "motorway_link": (0, 1), "trunk_link": (0, 1), "primary_link": (0, 1),
    "secondary_link": (0, 1), "tertiary": (1, 1), "tertiary_link": (1, 1),
    "residential": (1, 2), "unclassified": (1, 2), "living_street": (1, 2),
    "service": (1, 3), "busway": (1, 2), "pedestrian": (2, 2),
    "footway": (2, 2), "path": (2, 2), "cycleway": (2, 2), "steps": (2, 3),
    "track": (2, 2), "bridleway": (2, 2), "corridor": (2, 3),
}

# Cycle infrastructure gets its own class (5) at minz 0, so a renderer can style the
# bike network distinctly and show it at EVERY zoom. Without this it lands in class 2
# alongside footways, steps and bridleways, indistinguishable after conversion — and
# class 2 only appears at the two closest zooms, so the network you would actually plan
# a ride around is invisible until you are already on top of it.
#
# ⚠️ "Is this a bike route" is not one tag. `highway=cycleway` is the easy case; a great
# deal of real shared-path network (all of Canberra's, for instance) is tagged
# `highway=path` or `footway` with `bicycle=designated`, and missing those loses most of
# the map's value. The rule below is the same predicate the author's separate cycling
# analysis settled on: dedicated or designated ways, not every road that merely tolerates
# a bike.
#
# Deliberately NOT included: on-road painted lanes (`cycleway=lane` on a road way). They
# are real infrastructure, but folding them in would recolour arterial roads as bike
# routes, and this map is drawn for a child. Roads stay roads.
# Class 4 stays RESERVED for rail (docs/FORMAT.md has said so since v1), so cycleways
# take 5. Adding a class is not a version bump: parsers skip records whose cls they
# don't know, which the length-prefixed layout makes free.
CYCLEWAY = (5, 0)  # (class, minz) — minz 0 = visible at every zoom


def is_cycleway(tags):
    """True if this highway way is dedicated/designated cycling infrastructure."""
    if tags.get("highway") == "cycleway":
        return True
    # A designated bike route that happens to be tagged as a path/footway.
    return tags.get("highway") in ("path", "footway", "track") and \
        tags.get("bicycle") == "designated"
# waterway tag -> (class 3, minimum zoom): rivers at overview, streams up close.
WATERWAY = {"river": (3, 0), "canal": (3, 1), "stream": (3, 2)}
PLACE_TYPE = {"suburb": 0, "school": 1, "pool": 2}

OVERPASS_URL = "https://overpass-api.de/api/interpreter"
OVERPASS_QL = """[out:json][timeout:90];
(
  way["highway"]({bbox});
  way["waterway"~"^(river|canal|stream)$"]({bbox});
  way["natural"="water"]({bbox});
  way["landuse"~"^(reservoir|basin|recreation_ground|grass|village_green|meadow)$"]({bbox});
  way["leisure"~"^(park|pitch|playground|golf_course|garden|swimming_pool)$"]({bbox});
  way["amenity"="school"]({bbox});
  node["place"="suburb"]({bbox});
  node["amenity"="school"]({bbox});
  node["leisure"="swimming_pool"]({bbox});
);
out geom;"""


def poly_class(tags):
    """Polygon class for a CLOSED way, or None. Order matters: a closed way with a
    highway tag is a roundabout, not a park, and is classified as a line first."""
    if tags.get("natural") == "water" or tags.get("landuse") in ("reservoir", "basin"):
        return (0, 0)  # water: visible from the overview
    if tags.get("leisure") in ("park", "pitch", "playground", "golf_course", "garden") or \
       tags.get("landuse") in ("recreation_ground", "grass", "village_green", "meadow"):
        return (1, 1)  # parks: from the mid zooms
    return None


def dp(pts, tol):
    if len(pts) < 3:
        return pts[:]
    ax, ay = pts[0]
    bx, by = pts[-1]
    dmax, idx = 0.0, 0
    seg2 = (bx - ax) ** 2 + (by - ay) ** 2
    for i in range(1, len(pts) - 1):
        px, py = pts[i]
        if seg2 == 0:
            d = math.hypot(px - ax, py - ay)
        else:
            t = max(0.0, min(1.0, ((px - ax) * (bx - ax) + (py - ay) * (by - ay)) / seg2))
            d = math.hypot(px - (ax + t * (bx - ax)), py - (ay + t * (by - ay)))
        if d > dmax:
            dmax, idx = d, i
    if dmax > tol:
        return dp(pts[:idx + 1], tol)[:-1] + dp(pts[idx:], tol)
    return [pts[0], pts[-1]]


def simplify_ring(pts, tol):
    """Simplify a ring (open point list) to <= RING_MAX_PTS, doubling the tolerance
    until it fits. Returns None if the ring degenerates below 3 points."""
    out = dp(pts, tol if tol > 0 else 0.0001)
    while len(out) > RING_MAX_PTS:
        tol = max(tol, 0.5) * 2
        out = dp(pts, tol)
    return out if len(out) >= 3 else None


def fetch(path, bbox):
    """Download Overpass JSON for bbox into `path` (skipped if it already exists)."""
    if os.path.exists(path):
        print(f"using cached {path} (delete it to re-fetch)")
        return
    minlat, minlon, maxlat, maxlon = bbox
    q = OVERPASS_QL.format(bbox=f"{minlat},{minlon},{maxlat},{maxlon}")
    print(f"fetching {OVERPASS_URL} for bbox {minlat},{minlon},{maxlat},{maxlon} ...")
    req = urllib.request.Request(OVERPASS_URL, data=q.encode(),
                                 headers={"User-Agent": "mapvec/1.0"})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
    open(path, "wb").write(data)
    print(f"wrote {path} ({len(data)/1024:.0f} KB)")


def first_latlon(e):
    g = e.get("geometry")
    lat = e.get("lat") or (g[0]["lat"] if g else None)
    lon = e.get("lon") or (g[0]["lon"] if g else None)
    return lat, lon


def load(path, minlat, minlon, maxlat, maxlon):
    d = json.load(open(path))
    lat0, lon0 = (minlat + maxlat) / 2, (minlon + maxlon) / 2
    mlon = 111320.0 * math.cos(math.radians(lat0))

    def proj(lat, lon):
        return ((lon - lon0) * mlon, (lat - lat0) * MLAT)  # metres, +y = north

    lines, polys, places = [], [], []
    skipped_open = 0
    for e in d["elements"]:
        t = e.get("tags", {})
        g = e.get("geometry")
        if e["type"] == "way" and "highway" in t:
            # Cycle infrastructure is checked FIRST: a designated bike path is also a
            # `highway=path`, and the HIGHWAY table would otherwise claim it for class 2.
            cz = CYCLEWAY if is_cycleway(t) else HIGHWAY.get(t["highway"])
            if not cz or not g or len(g) < 2:
                continue
            lines.append((cz[0], cz[1], [proj(p["lat"], p["lon"]) for p in g]))
        elif e["type"] == "way" and t.get("waterway") in WATERWAY:
            if not g or len(g) < 2:
                continue
            cz = WATERWAY[t["waterway"]]
            lines.append((cz[0], cz[1], [proj(p["lat"], p["lon"]) for p in g]))
        elif e["type"] == "way" and (cz := poly_class(t)) is not None \
                and t.get("leisure") != "swimming_pool" and t.get("amenity") != "school":
            if not isinstance(g, list) or len(g) < 4:
                continue
            closed = g[0]["lat"] == g[-1]["lat"] and g[0]["lon"] == g[-1]["lon"]
            if not closed:
                skipped_open += 1
                continue
            polys.append((cz[0], cz[1], [proj(p["lat"], p["lon"]) for p in g[:-1]]))
        elif t.get("place") == "suburb" and t.get("name"):
            lat, lon = first_latlon(e)
            if lat is not None:
                places.append((*proj(lat, lon), PLACE_TYPE["suburb"], t["name"]))
        elif t.get("amenity") == "school" and t.get("name"):
            lat, lon = first_latlon(e)
            if lat is not None:
                places.append((*proj(lat, lon), PLACE_TYPE["school"], t["name"]))
        elif t.get("leisure") == "swimming_pool" and t.get("name"):
            # The name requirement is load-bearing: unnamed swimming_pool ways are
            # BACKYARD pools, and a suburb has hundreds of them (found the hard way —
            # the first build of a residential extract came out with 430 places, most
            # of them private pools).
            lat, lon = first_latlon(e)
            if lat is not None:
                places.append((*proj(lat, lon), PLACE_TYPE["pool"], t["name"]))
    if skipped_open:
        print(f"note: skipped {skipped_open} unclosed area way(s) (multipolygon relations "
              f"are not supported in v1)")
    return lat0, lon0, polys, lines, places


def q16(v):
    return max(-32768, min(32767, int(round(v))))


def pack(lat0, lon0, polys, lines, places, tol):
    buf = bytearray()
    # "x" is the v2 pad byte. It exists so the reader can hand out geometry zero-copy as
    # int16* without unaligned loads -- see the alignment note in src/mapvec.h.
    buf += struct.pack("<IBxii", MAGIC, VERSION, int(lat0 * 1e7), int(lon0 * 1e7))

    srings = []
    for cls, minz, pts in polys:
        r = simplify_ring(pts, tol)
        if r:
            srings.append((cls, minz, r))
    buf += struct.pack("<H", len(srings))
    for cls, minz, pts in srings:
        buf += struct.pack("<BBH", cls, minz, len(pts))
        for x, y in pts:
            buf += struct.pack("<hh", q16(x), q16(y))

    slines = []
    for cls, minz, pts in lines:
        s = dp(pts, tol if tol > 0 else 0.0001)
        if len(s) >= 2:
            slines.append((cls, minz, s))
    buf += struct.pack("<H", len(slines))
    for cls, minz, pts in slines:
        buf += struct.pack("<BBH", cls, minz, len(pts))
        for x, y in pts:
            buf += struct.pack("<hh", q16(x), q16(y))

    buf += struct.pack("<H", len(places))
    for x, y, ptype, name in places:
        nm = name.encode()[:31]
        buf += struct.pack("<hhBB", q16(x), q16(y), ptype, len(nm)) + nm

    pts_total = sum(len(p) for _, _, p in srings) + sum(len(p) for _, _, p in slines)
    return bytes(buf), pts_total


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("json", help="Overpass JSON with `out geom` (cache path when using --fetch)")
    ap.add_argument("--bbox", required=True, help="minlat,minlon,maxlat,maxlon")
    ap.add_argument("--fetch", action="store_true",
                    help="download the Overpass extract for --bbox into the json path")
    ap.add_argument("--tol", type=float, default=4.0, help="Douglas-Peucker metres for --out")
    ap.add_argument("--out", help="write the packed .mapvec here")
    args = ap.parse_args()

    bbox = tuple(float(v) for v in args.bbox.split(","))
    if args.fetch:
        fetch(args.json, bbox)
    lat0, lon0, polys, lines, places = load(args.json, *bbox)
    raw_pts = sum(len(p) for _, _, p in lines) + sum(len(p) for _, _, p in polys)
    print(f"origin {lat0:.5f},{lon0:.5f}   {len(polys)} polygons, {len(lines)} polylines, "
          f"{raw_pts} raw points, {len(places)} places")

    print(f"{'tol(m)':>7}  {'points':>8}  {'kept%':>6}  {'packed KB':>10}")
    for tol in (0, 2, 4, 8, 16):
        b, pts = pack(lat0, lon0, polys, lines, places, tol)
        pct = 100.0 * pts / raw_pts if raw_pts else 0
        print(f"{(tol or 'raw'):>7}  {pts:>8}  {pct:>5.0f}%  {len(b)/1024:>10.1f}")

    if args.out:
        b, pts = pack(lat0, lon0, polys, lines, places, args.tol)
        open(args.out, "wb").write(b)
        print(f"\nwrote {args.out}  ({len(b)/1024:.1f} KB, tol {args.tol} m, {pts} points)")
        print("map data (c) OpenStreetMap contributors, ODbL — keep this attribution "
              "with the file: https://www.openstreetmap.org/copyright")


if __name__ == "__main__":
    main()
