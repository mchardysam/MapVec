// test_mapvec_parse.cpp — the parser against hand-built byte buffers.
//
// Every buffer here is constructed byte by byte in the test, independently of the
// producer, so a mutual misunderstanding of the format can't hide (the producer
// cross-check in test_mapvec_roundtrip covers the other direction).
//
//   c++ -std=c++17 -O1 -I ../src -o test_mapvec_parse test_mapvec_parse.cpp && ./test_mapvec_parse
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <mapvec.h>

static int failures = 0;
static void check(bool ok, const char *what) {
  printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) failures++;
}

// ---- byte-buffer builder -----------------------------------------------------
struct Buf {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
  void i16(int16_t v) { u16((uint16_t)v); }
  void i32(int32_t v) {
    for (int i = 0; i < 4; i++) b.push_back((uint8_t)(((uint32_t)v >> (8 * i)) & 0xFF));
  }
  void str(const char *s) { while (*s) b.push_back((uint8_t)*s++); }
  void header(uint8_t ver = MAPVEC_VERSION, int32_t lat0 = -353300000, int32_t lon0 = 1490800000) {
    i32((int32_t)MAPVEC_MAGIC);
    u8(ver);
    u8(0);   // v2 pad byte: keeps geometry 2-byte aligned
    i32(lat0);
    i32(lon0);
  }
};

// A bump arena, the same one test_mapvec_fuzz.cpp uses, and NOT malloc. mapParse never frees: it
// documents that a failed parse may leave allocations in `out` for the caller's allocator to
// reclaim, because the intended callers are an arena or a ps_malloc region where that is free. With
// malloc here, every rejection test leaks by design and LeakSanitizer fails the build on Linux
// (macOS doesn't run LSan, which is how this got past a local run).
static uint8_t arena[1 << 20];
static size_t arenaAt = 0;
static void *arenaAlloc(size_t n) {
  n = (n + 7) & ~(size_t)7;
  if (arenaAt + n > sizeof arena) return nullptr;
  void *p = arena + arenaAt;
  arenaAt += n;
  return p;
}
// Only safe where the MapParsed from earlier parses is dead. Used by the truncation sweep.
static void arenaReset() { arenaAt = 0; }

static bool parse(const Buf &buf, MapParsed &out, const char **err = nullptr) {
  return mapParse(buf.b.data(), buf.b.size(), arenaAlloc, out, err);
}

// A complete little map: 2 polygons, 2 lines, 2 places.
static Buf sample() {
  Buf f;
  f.header();
  f.u16(2);  // polys
  f.u8(MAPVEC_POLY_WATER); f.u8(0); f.u16(4);            // a 4-point lake
  f.i16(0); f.i16(0); f.i16(100); f.i16(0); f.i16(100); f.i16(80); f.i16(0); f.i16(80);
  f.u8(MAPVEC_POLY_PARK); f.u8(1); f.u16(3);             // a 3-point park
  f.i16(-50); f.i16(-50); f.i16(-10); f.i16(-50); f.i16(-30); f.i16(-10);
  f.u16(2);  // lines
  f.u8(MAPVEC_LINE_MAJOR); f.u8(0); f.u16(2);
  f.i16(-200); f.i16(0); f.i16(200); f.i16(0);
  f.u8(MAPVEC_LINE_WATERWAY); f.u8(1); f.u16(3);
  f.i16(0); f.i16(-200); f.i16(10); f.i16(0); f.i16(0); f.i16(200);
  f.u16(2);  // places
  f.i16(5); f.i16(6); f.u8(1); f.u8(6); f.str("School");
  f.i16(-5); f.i16(-6); f.u8(0); f.u8(0);                // empty name is legal
  return f;
}

int main() {
  printf("mapvec_parse:\n");

  // ---- the happy path, field by field ----------------------------------------
  {
    Buf f = sample();
    MapParsed m;
    const char *err = "unset";
    check(parse(f, m, &err) && err == nullptr, "sample map parses");
    check(m.data.lat0_e7 == -353300000 && m.data.lon0_e7 == 1490800000, "origin read back");
    check(m.data.npolys == 2 && m.data.nlines == 2 && m.data.nplaces == 2, "section counts");
    check(m.polys[0].cls == MAPVEC_POLY_WATER && m.polys[0].n == 4, "water polygon header");
    check(m.polys[1].cls == MAPVEC_POLY_PARK && m.polys[1].minz == 1, "park polygon header");
    check(m.polys[0].xy[0] == 0 && m.polys[0].xy[4] == 100 && m.polys[0].xy[5] == 80,
          "polygon points read as LE int16 pairs");
    check(m.lines[1].cls == MAPVEC_LINE_WATERWAY && m.lines[1].n == 3, "waterway line header");
    check(strcmp(m.places[0].name, "School") == 0 && m.places[0].type == 1, "place name NUL-terminated");
    check(m.places[1].name[0] == '\0', "empty place name is an empty string, not garbage");

    // Zero-copy: geometry points INTO the file buffer, names do not.
    const uint8_t *lo = f.b.data(), *hi = f.b.data() + f.b.size();
    check((const uint8_t *)m.polys[0].xy >= lo && (const uint8_t *)m.polys[0].xy < hi,
          "polygon geometry borrows into the file buffer (zero-copy)");
    check((const uint8_t *)m.lines[0].xy >= lo && (const uint8_t *)m.lines[0].xy < hi,
          "line geometry borrows into the file buffer");
    check((const uint8_t *)m.places[0].name < lo || (const uint8_t *)m.places[0].name >= hi,
          "names are copied out (the file's names aren't NUL-terminated)");

    // ⚠️ THE REGRESSION GUARD FOR THE v1 BUG. Zero-copy geometry is only legal if it is 2-byte
    // aligned: `xy` is dereferenced as int16_t, so an odd offset is undefined behaviour, and on
    // ESP32 it is trap-and-fixup on every point. v1's 13-byte header put EVERY point in EVERY
    // file on an odd address; gcc's UBSan caught it, clang on x86 did not. Assert the offsets
    // directly rather than trusting a sanitiser to be watching.
    for (uint16_t i = 0; i < m.data.npolys; i++)
      check(((const uint8_t *)m.polys[i].xy - lo) % 2 == 0, "polygon geometry is 2-byte aligned");
    for (uint16_t i = 0; i < m.data.nlines; i++)
      check(((const uint8_t *)m.lines[i].xy - lo) % 2 == 0, "line geometry is 2-byte aligned");
  }

  // ---- empty map is legal ------------------------------------------------------
  {
    Buf f;
    f.header();
    f.u16(0); f.u16(0); f.u16(0);
    MapParsed m;
    check(parse(f, m), "empty map (0 polys / 0 lines / 0 places) parses");
  }

  // ---- header rejections -------------------------------------------------------
  {
    Buf f = sample();
    f.b[0] ^= 0xFF;
    MapParsed m;
    const char *err = nullptr;
    check(!parse(f, m, &err) && strstr(err, "magic"), "bad magic rejected");
  }
  {
    Buf f;
    f.header(/*ver=*/0);
    f.u16(0); f.u16(0); f.u16(0);
    MapParsed m;
    const char *err = nullptr;
    check(!parse(f, m, &err) && strstr(err, "version"),
          "version 0 rejected (a pre-version file must not half-parse)");
  }
  {
    // Relative to MAPVEC_VERSION on purpose: hard-coding a "future" number means the test quietly
    // starts asserting the CURRENT version is rejected the next time the format is bumped.
    Buf f;
    f.header(/*ver=*/MAPVEC_VERSION + 1);
    f.u16(0); f.u16(0); f.u16(0);
    MapParsed m;
    check(!parse(f, m), "a future version is rejected, not guessed at");
  }
  {
    // v1 is not merely old, it is MISALIGNED: its 13-byte header put every point on an odd
    // address. Reading one as v2 would slip the whole file by a byte, so it must be refused by
    // version rather than half-parsed. (Built by hand -- Buf::header() writes the v2 pad.)
    // Given real content, so it clears the minimum-length check and actually reaches the version
    // test. An empty v1 file is only 19 bytes and would be caught as "too short" instead, which
    // would prove nothing about version handling.
    Buf f;
    f.i32((int32_t)MAPVEC_MAGIC);
    f.u8(1);                    // version 1, and NO pad byte: the v1 layout exactly
    f.i32(-353300000); f.i32(1490800000);
    f.u16(1);                                   // one polygon
    f.u8(MAPVEC_POLY_WATER); f.u8(0); f.u16(3); // cls, minz, 3 points
    f.i16(0); f.i16(0); f.i16(10); f.i16(0); f.i16(10); f.i16(10);
    f.u16(0); f.u16(0);                         // no lines, no places
    MapParsed m;
    const char *err = nullptr;
    check(!parse(f, m, &err) && strstr(err, "version"), "a v1 file is rejected, not misread");
  }

  // ---- truncation at EVERY section boundary -----------------------------------
  {
    Buf f = sample();
    bool allReject = true;
    // Any prefix of the sample shorter than the full file must fail (the sample has
    // no trailing slack, so every cut lands inside some section).
    for (size_t cut = 0; cut < f.b.size(); cut++) {
      MapParsed m;
      // Reset between iterations, not just arena-instead-of-malloc. Each rejected parse allocates
      // before it discovers the truncation and never frees (by contract), so without the reset this
      // one loop would burn arena proportional to the file size -- and if it ever ran dry, every
      // remaining parse would fail on "alloc" and this test would pass for the wrong reason.
      arenaReset();
      if (mapParse(f.b.data(), cut, arenaAlloc, m, nullptr)) allReject = false;
    }
    arenaReset();
    check(allReject, "every possible truncation of the sample is rejected");
  }

  // ---- degenerate records ------------------------------------------------------
  {
    Buf f;
    f.header();
    f.u16(1);
    f.u8(0); f.u8(0); f.u16(2);  // a 2-point "polygon"
    f.i16(0); f.i16(0); f.i16(1); f.i16(1);
    f.u16(0); f.u16(0);
    MapParsed m;
    const char *err = nullptr;
    check(!parse(f, m, &err) && strstr(err, "3 points"), "2-point polygon rejected");
  }
  {
    Buf f;
    f.header();
    f.u16(0);
    f.u16(1);
    f.u8(0); f.u8(0); f.u16(1);  // a 1-point "line"
    f.i16(0); f.i16(0);
    f.u16(0);
    MapParsed m;
    check(!parse(f, m), "1-point polyline rejected");
  }

  // ---- unknown classes are KEPT (renderers skip them; parsers must not) --------
  {
    Buf f;
    f.header();
    f.u16(1);
    f.u8(9); f.u8(0); f.u16(3);
    f.i16(0); f.i16(0); f.i16(1); f.i16(0); f.i16(0); f.i16(1);
    f.u16(0); f.u16(0);
    MapParsed m;
    check(parse(f, m) && m.polys[0].cls == 9,
          "unknown polygon class parses and is preserved (forward compat)");
  }

  // ---- allocator failure fails cleanly ----------------------------------------
  {
    Buf f = sample();
    MapParsed m;
    const char *err = nullptr;
    auto nullAlloc = [](size_t) -> void * { return nullptr; };
    check(!mapParse(f.b.data(), f.b.size(), nullAlloc, m, &err) && strstr(err, "alloc"),
          "allocation failure reports, doesn't crash");
  }

  printf("%s (%d failure%s)\n", failures ? "FAILURES" : "all passed", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
