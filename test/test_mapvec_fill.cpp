// test_mapvec_fill.cpp — the polygon scanline fill against a byte grid.
//
// The draw callbacks paint into a 64x64 byte framebuffer, and the assertions are
// about pixels: inside points filled, notch points empty, clipping exact, the
// outline fallback firing. This is the layer a screenshot can't prove and a
// round-trip can't see.
//
//   c++ -std=c++17 -O1 -I ../src -o test_mapvec_fill test_mapvec_fill.cpp && ./test_mapvec_fill
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <mapvec_render.h>

static int failures = 0;
static void check(bool ok, const char *what) {
  printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) failures++;
}

// ---- a recording framebuffer ---------------------------------------------------
static const int W = 64, H = 64;
struct Grid {
  uint8_t px[H][W];
  int hlines = 0, lines = 0;
  bool outOfBounds = false;
  void clear() {
    memset(px, 0, sizeof px);
    hlines = lines = 0;
    outOfBounds = false;
  }
};

static void gHline(void *ctx, int x0, int x1, int y, uint16_t) {
  Grid *g = (Grid *)ctx;
  g->hlines++;
  if (y < 0 || y >= H || x0 < 0 || x1 >= W || x0 > x1) {
    g->outOfBounds = true;  // the fill is CLIPPED; any escape is a bug
    return;
  }
  for (int x = x0; x <= x1; x++) g->px[y][x] = 1;
}
static void gLine(void *ctx, int, int, int, int, uint16_t) { ((Grid *)ctx)->lines++; }

static Grid g;
static MapDraw draw() { return MapDraw{&g, gLine, gHline, nullptr}; }

// Viewport: whole 64x64 grid, 1 px per metre, world origin at the centre.
// world (wx, wy) -> screen (32 + wx, 32 - wy).
static MapView view() { return MapView{0, 0, 1.0f, 3, 0, 0, W, H}; }

static MapPolygon ring(const int16_t *xy, uint16_t n) { return MapPolygon{0, 0, n, xy}; }

int main() {
  printf("mapvec_fill:\n");
  MapDraw dr = draw();
  MapView v = view();

  // ---- a convex square: exact coverage, half-open bottom edge -----------------
  {
    static const int16_t sq[] = {-10, -10, 10, -10, 10, 10, -10, 10};
    g.clear();
    MapPolygon p_sq = ring(sq, 4);
    mapFillPolygon(&dr, &v, &p_sq, 1);
    check(g.px[32][32] == 1, "centre of the square is filled");
    check(g.px[22][32] == 1, "top row (world y=+10 -> screen y=22) is filled");
    check(g.px[42][32] == 0, "bottom edge row is open (half-open rule, no double edges)");
    check(g.px[32][21] == 0 && g.px[32][43] == 0, "left/right of the square untouched");
    check(!g.outOfBounds, "no pixel escaped the viewport");
    int count = 0;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) count += g.px[y][x];
    check(count == 20 * 21, "filled area is exactly 20 rows x 21 columns");
  }

  // ---- a concave U: the notch must stay empty ---------------------------------
  {
    // A U opening upward (+y world): arms at x in [-20,-10] and [10,20].
    static const int16_t u[] = {-20, -20, 20, -20, 20, 20, 10, 20,
                                10,  -10, -10, -10, -10, 20, -20, 20};
    g.clear();
    MapPolygon p_u = ring(u, 8);
    mapFillPolygon(&dr, &v, &p_u, 1);
    check(g.px[22][17] == 1, "left arm filled (world -15,+10)");
    check(g.px[22][47] == 1, "right arm filled (world +15,+10)");
    check(g.px[22][32] == 0, "the notch is EMPTY (world 0,+10) - concavity respected");
    check(g.px[47][32] == 1, "the base is filled (world 0,-15)");
    check(!g.outOfBounds, "no pixel escaped the viewport");
  }

  // ---- a diamond: vertex-on-scanline handled once, not twice ------------------
  {
    static const int16_t di[] = {0, 12, 12, 0, 0, -12, -12, 0};
    g.clear();
    MapPolygon p_di = ring(di, 4);
    mapFillPolygon(&dr, &v, &p_di, 1);
    check(g.px[32][32] == 1, "diamond centre filled");
    // The apex row gets its single pixel (both edges cross there under the
    // half-open rule); the rows above it must be empty.
    check(g.px[20][32] == 1, "apex pixel drawn exactly once");
    check(g.px[19][32] == 0 && g.px[18][32] == 0, "above the apex empty");
    // Every filled row must be one contiguous span (a double-counted vertex would
    // split a row or leak past an edge).
    bool contiguous = true;
    for (int y = 0; y < H; y++) {
      int first = -1, last = -1;
      for (int x = 0; x < W; x++)
        if (g.px[y][x]) {
          if (first < 0) first = x;
          last = x;
        }
      if (first >= 0)
        for (int x = first; x <= last; x++)
          if (!g.px[y][x]) contiguous = false;
    }
    check(contiguous, "every filled row is one contiguous span (vertex rule holds)");
  }

  // ---- clipping: a polygon far bigger than the viewport -----------------------
  {
    static const int16_t big[] = {-2000, -2000, 2000, -2000, 2000, 2000, -2000, 2000};
    g.clear();
    MapPolygon p_big = ring(big, 4);
    mapFillPolygon(&dr, &v, &p_big, 1);
    check(!g.outOfBounds, "giant polygon never draws outside the viewport");
    int count = 0;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) count += g.px[y][x];
    check(count == W * H, "giant polygon fills the entire viewport exactly");
  }

  // ---- fully off-screen: zero work --------------------------------------------
  {
    static const int16_t off[] = {2000, 2000, 2010, 2000, 2005, 2010};
    g.clear();
    MapPolygon p_off = ring(off, 3);
    mapFillPolygon(&dr, &v, &p_off, 1);
    check(g.hlines == 0, "off-screen polygon emits no spans");
  }

  // ---- over the point cap: outline fallback, never misrender ------------------
  {
    static int16_t many[2 * (MAPVEC_FILL_MAX_PTS + 8)];
    const int n = MAPVEC_FILL_MAX_PTS + 8;
    for (int i = 0; i < n; i++) {  // a rough circle, r=20
      const float a = (float)i / n * 6.2831853f;
      many[2 * i] = (int16_t)(20 * cosf(a));
      many[2 * i + 1] = (int16_t)(20 * sinf(a));
    }
    g.clear();
    MapPolygon p = ring(many, (uint16_t)n);
    mapFillPolygon(&dr, &v, &p, 1);
    check(g.hlines == 0 && g.lines == n, "over-cap ring outlines instead of filling");
  }

  printf("%s (%d failure%s)\n", failures ? "FAILURES" : "all passed", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
