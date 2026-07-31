// test_mapvec_rotate.cpp — the course-up transform (MapView::up_deg).
//
// Rotation is one function (mapWorldToScreen) because every shape, fill span, place and
// marker projects through it. That makes it cheap to add and easy to get subtly wrong, so
// the assertions here are the ones that would actually bite:
//
//   * a ZERO-INITIALISED MapView must still be north-up. Every existing call site
//     brace-initialises the struct without the new fields, so if `up_deg == 0` were not
//     the identity, all of them would silently break at once.
//   * up_deg must put THAT BEARING at the top of the screen — the sign is the whole
//     feature, and getting it backwards yields a map that turns the wrong way, which
//     looks like a plausible design choice rather than a bug.
//   * rotation must be rigid: distances between projected points are preserved.
//   * the viewport reject must not cull what is visible at any angle. This is the one
//     that fails silently as geometry popping in and out while you turn a corner.
//
//   c++ -std=c++17 -O1 -I ../src -o t test_mapvec_rotate.cpp && ./t
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "mapvec_render.h"

// The partial brace-initialisers below are the POINT of the first test, not an oversight:
// they reproduce exactly how every pre-rotation caller builds a MapView, and the claim
// under test is that such a view is still north-up. Warning about them here would mean
// silencing the very thing being verified.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

static int failures = 0;
static void check(bool ok, const char *what) {
  printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) failures++;
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

int main() {
  printf("mapvec_rotate:\n");

  const int16_t VW = 240, VH = 278;

  // ---- a zero-initialised view is north-up ------------------------------------
  // Exactly how every pre-rotation call site builds one: positional, new fields absent.
  {
    MapView v = {0, 0, 1.0f, 3, 0, 0, VW, VH};
    check(v.up_deg == 0.0f, "brace-init leaves up_deg 0 (north-up)");
    int sx, sy;
    mapWorldToScreen(&v, 0, 100, &sx, &sy); // 100 m north
    check(sx == VW / 2, "north-up: due north stays on the centre column");
    check(sy < VH / 2, "north-up: due north is ABOVE centre");
    mapWorldToScreen(&v, 100, 0, &sx, &sy); // 100 m east
    check(sx > VW / 2 && sy == VH / 2, "north-up: due east is to the RIGHT");
  }

  // ---- up_deg puts that bearing at the top ------------------------------------
  // Riding east (bearing 90): east must now be up, and north must be to the LEFT.
  //
  // ⚠️ "On the centre line" is asserted to +/-1 px, not exactly. mapWorldToScreen casts
  // with (int), which TRUNCATES, and cosf(pi/2) is -4.4e-8 rather than 0 — so a point
  // that lands mathematically on x=120 comes out at 119.999996 and truncates to 119.
  // The claim under test is a DIRECTION, and one pixel on a 240 px panel is still far
  // too tight to let a sign error through. (Rounding instead of truncating would be
  // marginally better rasterisation, but it would shift every existing golden image by
  // a sub-pixel for no visible gain — not a change to make from a test failure.)
  {
    MapView v = {0, 0, 1.0f, 3, 0, 0, VW, VH};
    mapViewSetUp(&v, 90.0f);
    int sx, sy;
    mapWorldToScreen(&v, 100, 0, &sx, &sy); // 100 m EAST = the way we're heading
    check(std::abs(sx - VW / 2) <= 1 && sy < VH / 2, "up=90: east is UP the screen");
    mapWorldToScreen(&v, 0, 100, &sx, &sy); // 100 m north
    check(std::abs(sy - VH / 2) <= 1 && sx < VW / 2, "up=90: north is to the LEFT");
  }
  // ...and the other way round, because a sign error passes one of these and not both.
  {
    MapView v = {0, 0, 1.0f, 3, 0, 0, VW, VH};
    mapViewSetUp(&v, 270.0f); // heading west
    int sx, sy;
    mapWorldToScreen(&v, -100, 0, &sx, &sy); // 100 m WEST
    check(std::abs(sx - VW / 2) <= 1 && sy < VH / 2, "up=270: west is UP the screen");
    mapWorldToScreen(&v, 0, 100, &sx, &sy);
    check(std::abs(sy - VH / 2) <= 1 && sx > VW / 2, "up=270: north is to the RIGHT");
  }
  // A non-degenerate angle, where no trig value is near 0 or 1 and a transposed sine
  // or cosine cannot hide behind a zero. Heading NE (45): NE must be up, NW to the left.
  {
    MapView v = {0, 0, 1.0f, 3, 0, 0, VW, VH};
    mapViewSetUp(&v, 45.0f);
    int sx, sy;
    mapWorldToScreen(&v, 70.71f, 70.71f, &sx, &sy); // 100 m to the NORTH-EAST
    check(std::abs(sx - VW / 2) <= 1 && sy < VH / 2 - 90, "up=45: north-east is UP");
    mapWorldToScreen(&v, -70.71f, 70.71f, &sx, &sy); // 100 m to the NORTH-WEST
    check(std::abs(sy - VH / 2) <= 1 && sx < VW / 2 - 90, "up=45: north-west is LEFT");
  }

  // ---- rotation is rigid: it moves points, it does not stretch them -------------
  {
    for (float ang = 0; ang < 360.0f; ang += 37.0f) {
      MapView v = {0, 0, 1.0f, 3, 0, 0, VW, VH};
      mapViewSetUp(&v, ang);
      int ax, ay, bx, by;
      mapWorldToScreen(&v, 30, 40, &ax, &ay); // 50 m from the centre (3-4-5)
      mapWorldToScreen(&v, 0, 0, &bx, &by);
      const double d = std::hypot(ax - bx, ay - by);
      if (!near(d, 50.0, 1.5)) {
        printf("    angle %.0f gave %.2f px\n", (double)ang, d);
        check(false, "rotation preserves distance from centre");
        break;
      }
    }
    check(true, "rotation preserves distance from centre (every 37 deg)");
  }

  // ---- the viewport reject never culls something that is actually visible -------
  // THE regression that would show up as street segments blinking as you turn. Sweep
  // every angle; for each, take points that land inside the screen rect and demand the
  // bbox reject accepted them.
  {
    int culled = 0, tested = 0;
    for (float ang = 0; ang < 360.0f; ang += 5.0f) {
      MapView v = {0, 0, 0.18f, 3, 0, 0, VW, VH};
      mapViewSetUp(&v, ang);
      // A grid of candidate points across a region comfortably larger than the screen.
      for (int wy = -900; wy <= 900; wy += 50) {
        for (int wx = -900; wx <= 900; wx += 50) {
          int sx, sy;
          mapWorldToScreen(&v, (float)wx, (float)wy, &sx, &sy);
          const bool onScreen = sx >= 0 && sx < VW && sy >= 0 && sy < VH;
          if (!onScreen) continue;
          tested++;
          const int16_t xy[2] = {(int16_t)wx, (int16_t)wy};
          if (!mapBoundsOnScreen(&v, xy, 1)) culled++;
        }
      }
    }
    printf("    %d on-screen points across 72 angles, %d wrongly culled\n", tested, culled);
    check(tested > 1000, "the sweep actually exercised on-screen points");
    check(culled == 0, "no on-screen point is ever rejected by the bbox test");
  }

  // ---- north-up still uses the tight (un-inflated) reject -----------------------
  // The circumradius box is ~1.9x the area, so it must NOT be paid when up_deg is 0 —
  // that would slow every existing user down for a feature they don't use.
  {
    MapView v = {0, 0, 0.18f, 3, 0, 0, VW, VH};
    // A point beyond the screen's half-height but inside its circumradius: only the
    // rotated (inflated) test should accept it.
    const int16_t xy[2] = {0, 900};
    check(!mapBoundsOnScreen(&v, xy, 1), "north-up: far point rejected by the tight box");
    mapViewSetUp(&v, 45.0f);
    check(mapBoundsOnScreen(&v, xy, 1), "rotated: the same point is accepted (inflated box)");
  }

  printf("mapvec_rotate: %s\n", failures ? "FAILURES" : "all ok");
  return failures ? 1 : 0;
}
