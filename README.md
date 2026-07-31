# MapVec

Offline OpenStreetMap basemaps for microcontrollers. A suburb fits in about 150 KB of internal flash, parses zero-copy, and draws through four function pointers you provide.

This is a map LAYER for your own firmware, not a GPS navigator. If you want the ready-made device, [IceNav](https://github.com/jgauchia/IceNav-v3) and [ESP32_GPS](https://github.com/aresta/ESP32_GPS) are good projects and further along on navigator features; both are firmware you adopt wholesale. MapVec exists for the other case: you have a sketch already (a tracker, a messenger, a watch face) and want a map inside it. It came out of a family LoRa messenger where the map screen shows the walk to school on an ESP32-S3 with a 240x320 panel.

## The pieces

Each layer works without the one above it:

- `tools/osm_to_vectors.py` (Python, stdlib only): OpenStreetMap extract in, `.mapvec` out. Fetches from Overpass for a `--bbox` or reads a file you supply. Roads classed for zoom gating, water and parks as filled polygons, rivers and streams as lines, schools/suburbs/pools as labelled places. Douglas-Peucker simplification with a size table so you choose your flash budget.
- `tools/mapvec_preview.py` (Pillow): renders a `.mapvec` to PNG so you can look at your extract before flashing anything.
- `src/mapvec.h`: the parser. No dependencies at all (not even Arduino), bounds-checked against corrupt files, zero-copy: geometry points into your file buffer, and the only allocations go through a callback (malloc on host, `ps_malloc` on ESP32).
- `src/mapvec_render.h`: world-to-screen transform, per-shape viewport rejection, zoom-gated drawing, and even-odd scanline polygon fill, all against a pluggable draw interface: `line`, `hline`, and optionally `place`. The Arduino_GFX adapter in the example is five lines; Adafruit_GFX is the same shape; LVGL or e-paper implement the same two primitives their own way.

The format is one page: [docs/FORMAT.md](docs/FORMAT.md). Versioned, little-endian, int16 local metres, capped so a renderer can fill polygons on the stack.

## Quick start

```
./tools/osm_to_vectors.py area.json --fetch --bbox=37.74,-122.46,37.81,-122.39 --tol 4 --out area.mapvec
./tools/mapvec_preview.py area.mapvec --out area.png     # look at it first
```

Then on the device, read the file into RAM and:

```cpp
MapParsed m;
mapParse(buf, len, ps_malloc, m, &err);

MapDraw dr{gfx, gfxLine, gfxHline, gfxPlace};   // your 5-line adapter
mapDrawBase(&dr, &m.data, &view, &palette);
```

`examples/Basemap` is the complete version, self-contained (it assembles a demo map in RAM, so it runs on a bare board and panel).

## Limits, stated up front

- int16 metres from one origin caps an extract at roughly 32 km each way. For bigger areas, use a grid of adjacent `.mapvec` cells and load the one you're in; the format's version byte leaves room to fold tiling in properly if it earns it.
- Polygon rings only, no holes: a lake with an island draws as a full lake (v1 skips multipolygon relations, and the producer tells you how many it skipped).
- No routing, no navigation, no GPS handling. Those belong to your sketch.
- The renderer does no label layout. Collision-free label placement is opinionated app territory; the `place` callback hands you positions and names and you decide.

## Tests

`make -C test check` runs four layers, no hardware needed:

- the parser against hand-built byte buffers (every truncation point, degenerate records, forward-compat classes, allocator failure);
- the polygon fill against a pixel grid (concave notches, vertex-on-scanline, exact clipping, the outline fallback for over-long rings);
- deterministic fuzz sweeps over both (a map file comes off a filesystem: it will one day be corrupt), best run as `make SAN=1 clean check`;
- a round-trip: the Python producer writes a `.mapvec` from a committed fixture and the C parser verifies it, so the two implementations of the format can't drift apart silently.

## Licence and map data

Code is MIT. Map DATA is a different thing: a `.mapvec` built from OpenStreetMap is a derived database under ODbL, so keep the attribution ("(c) OpenStreetMap contributors") with any file you distribute, and read https://www.openstreetmap.org/copyright before shipping map data with a product.
