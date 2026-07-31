// mapvec.h — types + zero-copy parser for the .mapvec embedded vector-map format.
//
// See docs/FORMAT.md for the byte layout. The headline properties:
//   * ZERO-COPY: geometry pointers point BACK INTO the caller's file buffer, which
//     must stay alive as long as the MapData is used. Only the small index arrays
//     and the place-name blob are allocated, through a callback you provide, so the
//     identical parse runs with malloc on a host and ps_malloc on an ESP32.
//   * PURE: no Arduino, no filesystem, no I/O. The device reads the file into a
//     buffer and calls mapParse; the host tests read the same file with fopen and
//     call the same function. One implementation, provably shared.
//   * BOUNDS-CHECKED: every read is validated against the buffer end, so a corrupt
//     or truncated file fails cleanly with a reason instead of running off the end.
//
// Coordinates are int16 LOCAL METRES about the file's origin (x east, y north),
// which is what makes world->screen an integer scale+offset on the device. The
// projection lives in mapvec_render.h (mapProjectE7) and in the producer — they
// must agree, and the producer/parser cross-check test pins that.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MAPVEC_MAGIC 0x4D564543u  // 'MVEC' as a little-endian u32
// v2 added one pad byte after `version`, purely so geometry lands 2-byte aligned. See the
// alignment note in mapParse() for why that pad is load-bearing rather than cosmetic.
#define MAPVEC_VERSION 2

// Polyline classes. Renderers must SKIP classes they don't recognise (new classes
// are added without a version bump; the length-prefixed layout makes skipping free).
enum MapLineClass : uint8_t {
  MAPVEC_LINE_MAJOR = 0,
  MAPVEC_LINE_MINOR = 1,
  MAPVEC_LINE_PATH = 2,
  MAPVEC_LINE_WATERWAY = 3,
  // 4 reserved: rail
  MAPVEC_LINE_CYCLEWAY = 5,  // dedicated/designated cycling infrastructure
  MAPVEC_LINE_CLASSES = 6,   // size of a per-class table; index 4 is a reserved hole
};
enum MapPolyClass : uint8_t {
  MAPVEC_POLY_WATER = 0,
  MAPVEC_POLY_PARK = 1,
  // 2 reserved: building
};

// One road/path/stream. `xy` is interleaved int16 x,y (local metres), `n` points.
struct MapPolyline {
  uint8_t cls;
  uint8_t minz;  // lowest zoom level at which it appears
  uint16_t n;
  const int16_t *xy;
};

// One filled area (ring; first point != last, the ring closes implicitly).
struct MapPolygon {
  uint8_t cls;
  uint8_t minz;
  uint16_t n;
  const int16_t *xy;
};

struct MapPlace {
  int16_t x, y;  // local metres
  uint8_t type;  // 0 suburb / 1 school / 2 pool / 3+ app-defined
  const char *name;
};

struct MapData {
  int32_t lat0_e7, lon0_e7;  // projection origin (for unproject / marker placement)
  const MapPolygon *polys;
  uint16_t npolys;
  const MapPolyline *lines;
  uint16_t nlines;
  const MapPlace *places;
  uint16_t nplaces;
};

typedef void *(*MapAllocFn)(size_t);

// Result of a parse: index arrays + the name blob came from the allocator; geometry
// borrows into the caller's file buffer, MapPlace::name into `names`.
struct MapParsed {
  MapPolygon *polys = nullptr;
  MapPolyline *lines = nullptr;
  MapPlace *places = nullptr;
  char *names = nullptr;
  MapData data{};
};

static inline uint16_t mapRdU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline int16_t mapRdI16(const uint8_t *p) { return (int16_t)mapRdU16(p); }
static inline uint32_t mapRdU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t mapRdI32(const uint8_t *p) { return (int32_t)mapRdU32(p); }

// Parse a whole .mapvec resident in `buf` (keep it alive — geometry borrows into it).
// Returns true and fills `out`; on malformed input returns false and points *err at
// a short static reason (never allocates or prints on the failure path after the
// first alloc; a returned false may leave allocations in `out` for the caller's
// allocator to reclaim — an arena or ps_malloc region both make that free).
static inline bool mapParse(const uint8_t *buf, size_t n, MapAllocFn alloc, MapParsed &out,
                            const char **err) {
  static const char *sink;
  if (!err) err = &sink;
  const uint8_t *p = buf, *end = buf + n;

  if (n < 20 || mapRdU32(p) != MAPVEC_MAGIC) { *err = "bad magic / too short"; return false; }
  if (p[4] != MAPVEC_VERSION) { *err = "unsupported version"; return false; }
  // ⚠️ p[5] IS A PAD BYTE, AND IT IS LOAD-BEARING. Everything else in this parser is read
  // byte-wise and does not care about alignment, but polygon and polyline geometry is handed out
  // ZERO-COPY as a `const int16_t *` pointing straight into the caller's buffer. In v1 the fixed
  // header was 13 bytes, so the first xy array began at offset 19 and EVERY point in EVERY file
  // sat on an odd address: undefined behaviour by the standard (gcc's UBSan flags it; clang on
  // x86 happens not to, which is how it shipped), and on ESP32 an unaligned 16-bit load is
  // trap-and-fixup — costing precisely the speed the zero-copy design exists to buy.
  //
  // With the pad the fixed header is 14 bytes, so the first polygon header ends at 20, and since
  // every shape header is 4 bytes and every point is 4 bytes, all geometry stays 2-byte aligned
  // for the rest of the file. Places are read byte-wise (mapRdI16), so their variable-length names
  // cannot disturb it. Do not remove the pad, and do not put an odd-sized field ahead of geometry.
  p += 6;
  out.data.lat0_e7 = mapRdI32(p); p += 4;
  out.data.lon0_e7 = mapRdI32(p); p += 4;

  // ---- polygons (drawn first, so stored first) --------------------------------
  uint16_t npolys = mapRdU16(p); p += 2;
  out.polys = (MapPolygon *)alloc(sizeof(MapPolygon) * (npolys ? npolys : 1));
  if (!out.polys) { *err = "alloc polys"; return false; }
  for (uint16_t i = 0; i < npolys; i++) {
    if (end - p < 4) { *err = "truncated polygon header"; return false; }
    uint8_t cls = p[0], minz = p[1];
    uint16_t npts = mapRdU16(p + 2);
    p += 4;
    if (npts < 3) { *err = "polygon with < 3 points"; return false; }
    if ((size_t)(end - p) < (size_t)npts * 4u) { *err = "truncated polygon points"; return false; }
    out.polys[i].cls = cls;
    out.polys[i].minz = minz;
    out.polys[i].n = npts;
    out.polys[i].xy = (const int16_t *)p;  // zero-copy: contiguous LE int16 x,y pairs
    p += (size_t)npts * 4u;
  }

  // ---- polylines --------------------------------------------------------------
  if (end - p < 2) { *err = "missing line count"; return false; }
  uint16_t nlines = mapRdU16(p); p += 2;
  out.lines = (MapPolyline *)alloc(sizeof(MapPolyline) * (nlines ? nlines : 1));
  if (!out.lines) { *err = "alloc lines"; return false; }
  for (uint16_t i = 0; i < nlines; i++) {
    if (end - p < 4) { *err = "truncated polyline header"; return false; }
    uint8_t cls = p[0], minz = p[1];
    uint16_t npts = mapRdU16(p + 2);
    p += 4;
    if (npts < 2) { *err = "polyline with < 2 points"; return false; }
    if ((size_t)(end - p) < (size_t)npts * 4u) { *err = "truncated polyline points"; return false; }
    out.lines[i].cls = cls;
    out.lines[i].minz = minz;
    out.lines[i].n = npts;
    out.lines[i].xy = (const int16_t *)p;
    p += (size_t)npts * 4u;
  }

  // ---- places -----------------------------------------------------------------
  if (end - p < 2) { *err = "missing places count"; return false; }
  uint16_t nplaces = mapRdU16(p); p += 2;

  // First pass sizes + validates the name blob so it's one contiguous alloc.
  const uint8_t *scan = p;
  size_t nameBytes = 0;
  for (uint16_t i = 0; i < nplaces; i++) {
    if (end - scan < 6) { *err = "truncated place header"; return false; }
    uint8_t len = scan[5];
    scan += 6;
    if ((size_t)(end - scan) < len) { *err = "truncated place name"; return false; }
    scan += len;
    nameBytes += (size_t)len + 1;
  }
  out.places = (MapPlace *)alloc(sizeof(MapPlace) * (nplaces ? nplaces : 1));
  out.names = (char *)alloc(nameBytes ? nameBytes : 1);
  if (!out.places || !out.names) { *err = "alloc places/names"; return false; }

  char *np = out.names;
  for (uint16_t i = 0; i < nplaces; i++) {
    out.places[i].x = mapRdI16(p);
    out.places[i].y = mapRdI16(p + 2);
    out.places[i].type = p[4];
    uint8_t len = p[5];
    p += 6;
    memcpy(np, p, len);
    np[len] = '\0';
    out.places[i].name = np;
    np += (size_t)len + 1;
    p += len;
  }

  out.data.polys = out.polys;
  out.data.npolys = npolys;
  out.data.lines = out.lines;
  out.data.nlines = nlines;
  out.data.places = out.places;
  out.data.nplaces = nplaces;
  *err = nullptr;
  return true;
}
