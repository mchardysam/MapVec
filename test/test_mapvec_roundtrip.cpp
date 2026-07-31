// test_mapvec_roundtrip.cpp — the producer and the parser agree, proven end to end.
//
// The Makefile runs tools/osm_to_vectors.py over test/fixture_overpass.json (a tiny
// hand-written Overpass extract) and hands the resulting .mapvec to this binary.
// Python wrote it, C reads it: same idea as testing crypto against an independent
// implementation — a shared misunderstanding of the format can't pass both this and
// test_mapvec_parse's hand-built buffers.
//
//   ./test_mapvec_roundtrip fixture.mapvec
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <mapvec.h>

static int failures = 0;
static void check(bool ok, const char *what) {
  printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) failures++;
}

// A bump arena, as in test_mapvec_fuzz.cpp and test_mapvec_parse.cpp. mapParse never frees (it is
// built for an arena or a ps_malloc region), so malloc here would be a guaranteed LeakSanitizer
// failure. It also holds the FILE BUFFER, which matters: geometry is handed out zero-copy as
// int16_t*, so the buffer itself must be at least 2-byte aligned or every point is misaligned
// however well the format is laid out. 8-byte blocks here; malloc would also have been fine.
static uint8_t arena[1 << 20];
static size_t arenaAt = 0;
static void *arenaAlloc(size_t n) {
  n = (n + 7) & ~(size_t)7;
  if (arenaAt + n > sizeof arena) return nullptr;
  void *p = arena + arenaAt;
  arenaAt += n;
  return p;
}

int main(int argc, char **argv) {
  printf("mapvec_roundtrip:\n");
  if (argc < 2) {
    printf("usage: test_mapvec_roundtrip <file.mapvec>\n");
    return 1;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) {
    printf("  cannot open %s\n", argv[1]);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = (uint8_t *)arenaAlloc((size_t)sz);
  if (!buf) {
    printf("  fixture larger than the test arena\n");
    return 1;
  }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    printf("  short read\n");
    return 1;
  }
  fclose(f);

  MapParsed m;
  const char *err = nullptr;
  // Arena, not malloc: mapParse never frees (see the note in test_mapvec_parse.cpp), so malloc
  // here means a guaranteed LeakSanitizer failure on Linux.
  check(mapParse(buf, (size_t)sz, arenaAlloc, m, &err), "producer output parses");
  if (err) printf("    err: %s\n", err);
  if (failures) return 1;

  // The fixture: origin at the bbox centre (-35.000, 149.000).
  check(m.data.lat0_e7 == -350000000 && m.data.lon0_e7 == 1490000000,
        "origin is the bbox centre, e7-exact");

  // One water polygon + one park polygon, in that order of discovery.
  check(m.data.npolys == 2, "both closed ways became polygons");
  int water = -1, park = -1;
  for (uint16_t i = 0; i < m.data.npolys; i++) {
    if (m.polys[i].cls == MAPVEC_POLY_WATER) water = i;
    if (m.polys[i].cls == MAPVEC_POLY_PARK) park = i;
  }
  check(water >= 0, "natural=water closed way became a WATER polygon");
  check(park >= 0, "leisure=park closed way became a PARK polygon");
  if (water >= 0) {
    check(m.polys[water].n >= 3, "water ring kept >= 3 points after simplify");
    bool inRange = true;
    for (uint16_t i = 0; i < 2 * m.polys[water].n; i++)
      if (m.polys[water].xy[i] < -3000 || m.polys[water].xy[i] > 3000) inRange = false;
    check(inRange, "water ring coordinates are plausible local metres");
    // The ring is stored OPEN (first != last): the producer must drop OSM's
    // duplicated closing point.
    const int16_t *xy = m.polys[water].xy;
    const uint16_t n = m.polys[water].n;
    check(xy[0] != xy[2 * (n - 1)] || xy[1] != xy[2 * (n - 1) + 1],
          "ring stored open (duplicate closing point dropped)");
  }

  // One road + one waterway + three cycling-related ways (see below).
  check(m.data.nlines == 5, "highways + waterway all became lines");
  int road = -1, stream = -1, foot = -1;
  int cycleways = 0;
  for (uint16_t i = 0; i < m.data.nlines; i++) {
    if (m.lines[i].cls == MAPVEC_LINE_MINOR) road = i;
    if (m.lines[i].cls == MAPVEC_LINE_WATERWAY) stream = i;
    if (m.lines[i].cls == MAPVEC_LINE_PATH) foot = i;
    if (m.lines[i].cls == MAPVEC_LINE_CYCLEWAY) cycleways++;
  }
  check(road >= 0, "residential way classified as MINOR");
  check(stream >= 0, "waterway=stream classified as WATERWAY");
  if (stream >= 0) check(m.lines[stream].n >= 2, "stream survived simplification");

  // Cycle infrastructure is its own class, and the membership rule is the interesting
  // part: `highway=cycleway` is easy, but a `highway=footway` carrying
  // `bicycle=designated` is a shared path and belongs with it — that tagging is most of
  // a real network, and a highway-tag-only lookup loses all of it to the generic path
  // class. Equally, a footway with NO bicycle tag must NOT be swept in, or the map
  // starts routing a child down pedestrian-only footpaths as if they were bike routes.
  check(cycleways == 2, "highway=cycleway AND bicycle=designated footway -> CYCLEWAY");
  check(foot >= 0, "a plain footway stays PATH, not CYCLEWAY");
  for (uint16_t i = 0; i < m.data.nlines; i++)
    if (m.lines[i].cls == MAPVEC_LINE_CYCLEWAY)
      check(m.lines[i].minz == 0, "cycleways are minz 0 (visible at every zoom)");

  // Places.
  check(m.data.nplaces == 2, "school + suburb became places");
  bool school = false, suburb = false;
  for (uint16_t i = 0; i < m.data.nplaces; i++) {
    if (m.places[i].type == 1 && strcmp(m.places[i].name, "Test Primary") == 0) school = true;
    if (m.places[i].type == 0 && strcmp(m.places[i].name, "Testville") == 0) suburb = true;
  }
  check(school, "school kept its name through the pipeline");
  check(suburb, "suburb kept its name through the pipeline");

  printf("%s (%d failure%s)\n", failures ? "FAILURES" : "all passed", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
