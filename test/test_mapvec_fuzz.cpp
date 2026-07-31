// test_mapvec_fuzz.cpp — deterministic random-input sweeps over the parser and the
// polygon fill. A .mapvec comes off a filesystem, which means it will one day be
// truncated, corrupt, or hostile; the parser must fail cleanly and the fill must
// never draw outside the viewport. Run under `make SAN=1 check` and the sanitisers
// turn any out-of-bounds access into a failure instead of luck.
//
//   c++ -std=c++17 -O1 -I ../src -o test_mapvec_fuzz test_mapvec_fuzz.cpp && ./test_mapvec_fuzz
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <mapvec.h>
#include <mapvec_render.h>

static int failures = 0;
static void check(bool ok, const char *what) {
  printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) failures++;
}

static uint32_t rng = 0xBA5EBA11;
static uint32_t xr() {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}

// A bump arena so 100k parses need no frees and leak checkers stay quiet.
static uint8_t arena[64 * 1024];
static size_t arenaAt = 0;
static void *arenaAlloc(size_t n) {
  n = (n + 7) & ~(size_t)7;
  if (arenaAt + n > sizeof arena) return nullptr;
  void *p = arena + arenaAt;
  arenaAt += n;
  return p;
}

int main() {
  printf("mapvec_fuzz (deterministic, seed 0xBA5EBA11):\n");

  // ---- 1. random buffers: parse never crashes; successes hold their invariants -
  {
    int bad = 0, parsedOk = 0;
    uint8_t buf[512];
    for (int it = 0; it < 100000; it++) {
      size_t len = xr() % (sizeof buf + 1);
      for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)xr();
      // Half the runs get a valid magic+version stamp so the deeper paths fuzz too.
      if (len >= 5 && (xr() & 1)) {
        buf[0] = 0x43; buf[1] = 0x45; buf[2] = 0x56; buf[3] = 0x4D;  // 'MVEC' LE
        buf[4] = MAPVEC_VERSION;
      }
      arenaAt = 0;
      MapParsed m;
      if (!mapParse(buf, len, arenaAlloc, m, nullptr)) continue;
      parsedOk++;
      const uint8_t *lo = buf, *hi = buf + len;
      for (uint16_t i = 0; i < m.data.npolys; i++) {
        const uint8_t *g0 = (const uint8_t *)m.polys[i].xy;
        if (g0 < lo || g0 + (size_t)m.polys[i].n * 4 > hi) bad++;
        if (m.polys[i].n < 3) bad++;
      }
      for (uint16_t i = 0; i < m.data.nlines; i++) {
        const uint8_t *g0 = (const uint8_t *)m.lines[i].xy;
        if (g0 < lo || g0 + (size_t)m.lines[i].n * 4 > hi) bad++;
        if (m.lines[i].n < 2) bad++;
      }
      for (uint16_t i = 0; i < m.data.nplaces; i++)
        if (!m.places[i].name) bad++;
    }
    check(bad == 0, "100k random buffers: every successful parse holds its invariants");
    printf("    (%d of 100k random buffers happened to parse)\n", parsedOk);
  }

  // ---- 2. random rings and views: fill never escapes the viewport --------------
  {
    struct Clip {
      int vx0, vx1, vy0, vy1;
      int escapes = 0;
    } clip;
    MapDraw dr;
    dr.ctx = &clip;
    dr.line = [](void *, int, int, int, int, uint16_t) {};
    dr.hline = [](void *c, int x0, int x1, int y, uint16_t) {
      Clip *cl = (Clip *)c;
      if (y < cl->vy0 || y > cl->vy1 || x0 < cl->vx0 || x1 > cl->vx1 || x0 > x1) cl->escapes++;
    };
    dr.place = nullptr;

    for (int it = 0; it < 20000; it++) {
      int16_t xy[24 * 2];
      const uint16_t n = 3 + (uint16_t)(xr() % 21);
      for (uint16_t i = 0; i < 2 * n; i++) xy[i] = (int16_t)xr();
      MapPolygon p{0, 0, n, xy};

      MapView v;
      v.x = (int16_t)(xr() % 64);
      v.y = (int16_t)(xr() % 64);
      v.w = (int16_t)(8 + xr() % 240);
      v.h = (int16_t)(8 + xr() % 240);
      v.cx = (float)(int16_t)xr();
      v.cy = (float)(int16_t)xr();
      v.scale = 0.02f + (float)(xr() % 1000) / 100.0f;
      v.minz = 3;
      clip.vx0 = v.x;
      clip.vx1 = v.x + v.w - 1;
      clip.vy0 = v.y;
      clip.vy1 = v.y + v.h - 1;
      mapFillPolygon(&dr, &v, &p, 1);
    }
    check(clip.escapes == 0, "20k random rings/views: no span ever leaves the viewport");
  }

  printf("%s (%d failure%s)\n", failures ? "FAILURES" : "all passed", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
