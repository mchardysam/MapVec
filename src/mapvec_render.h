// mapvec_render.h — transform, LOD, polyline drawing and polygon fill for .mapvec,
// against a pluggable drawing interface.
//
// The renderer never touches a display library: you hand it two function pointers
// (line + hline) and it hands you a basemap. Adapters for Arduino_GFX or
// Adafruit_GFX are ~5 lines (see the examples); LVGL, e-paper or a plain
// framebuffer are the same idea. That interface is also what makes every routine
// here host-testable — the fill tests draw into a byte grid.
//
// RENDER STRATEGY (inherited from the source project, proven on an ESP32-S3 panel):
//   * coordinates are int16 LOCAL METRES about a fixed origin, so world->screen is
//     integer scale + offset — no per-point trig in the inner loop;
//   * per-shape viewport bbox reject, so cost tracks what's ON SCREEN;
//   * zoom gating by class (minz), arterials at overview, everything up close;
//   * polygons draw first (under), lines second, places last (the file is stored
//     in that order too).
#pragma once
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "mapvec.h"

// ---- the pluggable drawing interface -----------------------------------------
struct MapDraw {
  void *ctx;
  void (*line)(void *ctx, int x0, int y0, int x1, int y1, uint16_t colour);
  // Inclusive ends. Fill performance lives here: back it with your display's
  // fast-horizontal-line, not with line().
  void (*hline)(void *ctx, int x0, int x1, int y, uint16_t colour);
  // Optional (may be null): a labelled point. The renderer does NO label layout —
  // collision-free label placement is an app-level policy, not a format property.
  void (*place)(void *ctx, int x, int y, uint8_t type, const char *name);
};

// Colours by class index. A cls beyond these arrays is SKIPPED (forward compat:
// files may carry classes this build doesn't know).
struct MapPalette {
  // Indexed by MapLineClass: major, minor, path, waterway, [4 = reserved rail], cycleway.
  // Index 4 is an unused hole rather than a renumbering, because class 4 was documented as
  // reserved for rail in v1 and a file someone built against that must keep its meaning.
  uint16_t line_cls[MAPVEC_LINE_CLASSES];
  uint16_t poly_cls[2];  // water, park
  // Stroke width per line class, in pixels (0 or 1 both mean a single-pixel line). This
  // used to be a hardcoded "class 0 draws twice" inside mapDrawBase; making it palette
  // data is what lets a caller emphasise a DIFFERENT class — a cycling map wants the bike
  // network heavy and the arterials thin, which is the exact inverse of a road map.
  // Zero-initialised palettes still work: 0 is treated as 1.
  uint8_t line_w[MAPVEC_LINE_CLASSES];
};

// The viewport: what part of the world fills the screen, how zoomed, and which way up.
struct MapView {
  float cx, cy;        // world-metre point at screen centre (pan)
  float scale;         // pixels per metre (zoom)
  uint8_t minz;        // draw shapes with minz <= this (LOD gate)
  int16_t x, y, w, h;  // screen rectangle to draw into

  // ---- rotation (optional) ----
  // Compass bearing that points UP the screen. 0 = north-up, the default and the only
  // behaviour before this existed. Set it through mapViewSetUp(), which fills the cached
  // sine/cosine alongside it.
  //
  // ⚠️ THE ANGLE IS STORED, NOT THE COSINE, and that is deliberate. `MapView` is
  // routinely brace-initialised (`MapView v = {cx, cy, scale, minz, 0, y, w, h};`), so
  // any field appended here is ZERO-initialised by every existing call site. With
  // up_deg that means north-up — correct. Had this been a raw `rot_cos`, zero would mean
  // cos = 0 and every one of those call sites would silently collapse the map onto a
  // line. Same reason `_c` is only ever consulted when up_deg != 0.
  float up_deg;
  float _s, _c;  // cached sinf/cosf of up_deg; do not set directly
};

// Set the screen-up bearing and cache its trig. Call once per frame, not per point:
// sinf/cosf at 1400+ points would cost more than the whole rest of the transform.
static inline void mapViewSetUp(MapView *v, float up_deg) {
  const float r = up_deg * 3.14159265358979f / 180.0f;
  v->up_deg = up_deg;
  v->_s = sinf(r);
  v->_c = cosf(r);
}

// ---- projection (must agree with the producer; the round-trip test pins it) ---
static inline void mapProjectE7(const MapData *d, int32_t lat_e7, int32_t lon_e7, float *x,
                                float *y) {
  double lat0 = d->lat0_e7 / 1e7;
  double mlon = 111320.0 * cos(lat0 * 3.14159265358979 / 180.0);
  *x = (float)((double)(lon_e7 - d->lon0_e7) / 1e7 * mlon);
  *y = (float)((double)(lat_e7 - d->lat0_e7) / 1e7 * 111132.0);
}

// THE choke point: polylines, polygon fill, place icons and any marker overlay all
// project through here, which is why rotation is one function rather than a rewrite.
static inline void mapWorldToScreen(const MapView *v, float wx, float wy, int *sx, int *sy) {
  float dx = wx - v->cx, dy = wy - v->cy;
  if (v->up_deg != 0.0f) {
    // Rotate the world by -up_deg so the given bearing ends up pointing up the screen.
    const float rx = dx * v->_c - dy * v->_s;
    const float ry = dx * v->_s + dy * v->_c;
    dx = rx;
    dy = ry;
  }
  *sx = (int)(v->x + v->w * 0.5f + dx * v->scale);
  *sy = (int)(v->y + v->h * 0.5f - dy * v->scale);  // +y world = north = up (when up_deg 0)
}

// Does this shape's world bbox touch the viewport (+margin)? The spatial reject
// that keeps per-frame cost proportional to what's visible.
static inline bool mapBoundsOnScreen(const MapView *v, const int16_t *xy, uint16_t n) {
  int16_t minx = 32767, miny = 32767, maxx = -32768, maxy = -32768;
  for (uint16_t i = 0; i < n; i++) {
    int16_t x = xy[2 * i], y = xy[2 * i + 1];
    if (x < minx) minx = x;
    if (x > maxx) maxx = x;
    if (y < miny) miny = y;
    if (y > maxy) maxy = y;
  }
  float halfW = (v->w * 0.5f) / v->scale, halfH = (v->h * 0.5f) / v->scale;
  if (v->up_deg != 0.0f) {
    // Under rotation the screen is a rotated rectangle in world space, and an
    // axis-aligned bbox test against the un-rotated half-extents would cull shapes that
    // are genuinely on screen — visible as geometry popping in and out as you turn.
    // Reject against the screen's CIRCUMRADIUS instead: rotation-invariant, so it is
    // correct at every angle, at the cost of considering the corners' worth of extra
    // shapes. For a 240x278 viewport that is a 368x368 test box, about 1.9x the area.
    const float r = 0.5f * sqrtf((float)v->w * v->w + (float)v->h * v->h) / v->scale;
    halfW = halfH = r;
  }
  float m = 20.0f;  // metres of margin so shapes entering the edge still draw
  return !(maxx < v->cx - halfW - m || minx > v->cx + halfW + m || maxy < v->cy - halfH - m ||
           miny > v->cy + halfH + m);
}
static inline bool mapPolyOnScreen(const MapView *v, const MapPolyline *p) {
  return mapBoundsOnScreen(v, p->xy, p->n);
}

// ---- polygon fill --------------------------------------------------------------
// Rings longer than this fall back to an outline (the producer caps rings at 128,
// so a produced file never triggers it; a hand-made one degrades honestly).
#ifndef MAPVEC_FILL_MAX_PTS
#define MAPVEC_FILL_MAX_PTS 128
#endif
// Crossings per scanline. 16 pairs handles any sane lake; beyond it the extra
// spans are dropped for that row (visible as a thin gap, never a crash).
#ifndef MAPVEC_FILL_MAX_XINGS
#define MAPVEC_FILL_MAX_XINGS 32
#endif

// Even-odd scanline fill of one ring, clipped to the viewport. The half-open
// vertex rule ([ymin, ymax) per edge) counts each vertex exactly once, which is
// what makes diamonds and touching edges fill without seams or double-counts.
static inline void mapFillPolygon(const MapDraw *dr, const MapView *v, const MapPolygon *p,
                                  uint16_t colour) {
  const int n = p->n;
  if (n < 3) return;
  if (n > MAPVEC_FILL_MAX_PTS) {  // outline fallback, never misrender
    int px, py;
    mapWorldToScreen(v, p->xy[0], p->xy[1], &px, &py);
    for (int i = 1; i <= n; i++) {
      const int j = i % n;
      int nx2, ny2;
      mapWorldToScreen(v, p->xy[2 * j], p->xy[2 * j + 1], &nx2, &ny2);
      dr->line(dr->ctx, px, py, nx2, ny2, colour);
      px = nx2;
      py = ny2;
    }
    return;
  }

  int sx[MAPVEC_FILL_MAX_PTS], sy[MAPVEC_FILL_MAX_PTS];
  int ymin = INT_MAX, ymax = INT_MIN;
  for (int i = 0; i < n; i++) {
    mapWorldToScreen(v, p->xy[2 * i], p->xy[2 * i + 1], &sx[i], &sy[i]);
    if (sy[i] < ymin) ymin = sy[i];
    if (sy[i] > ymax) ymax = sy[i];
  }
  const int vy0 = v->y, vy1 = v->y + v->h - 1;
  const int vx0 = v->x, vx1 = v->x + v->w - 1;
  if (ymin > vy1 || ymax < vy0) return;
  if (ymin < vy0) ymin = vy0;
  if (ymax > vy1) ymax = vy1;

  for (int y = ymin; y <= ymax; y++) {
    int xs[MAPVEC_FILL_MAX_XINGS];
    int nx = 0;
    for (int i = 0; i < n; i++) {
      const int j = (i + 1) % n;
      const int ya = sy[i], yb = sy[j];
      if (ya == yb) continue;  // horizontal edge contributes no crossing
      if ((y >= ya && y < yb) || (y >= yb && y < ya)) {
        const float t = (float)(y - ya) / (float)(yb - ya);
        const int x = sx[i] + (int)lroundf(t * (float)(sx[j] - sx[i]));
        if (nx < MAPVEC_FILL_MAX_XINGS) xs[nx++] = x;
      }
    }
    for (int i = 1; i < nx; i++) {  // insertion sort; nx is tiny
      const int k = xs[i];
      int j2 = i - 1;
      while (j2 >= 0 && xs[j2] > k) {
        xs[j2 + 1] = xs[j2];
        j2--;
      }
      xs[j2 + 1] = k;
    }
    for (int i = 0; i + 1 < nx; i += 2) {
      int xa = xs[i], xb = xs[i + 1];
      if (xb < vx0 || xa > vx1) continue;
      if (xa < vx0) xa = vx0;
      if (xb > vx1) xb = vx1;
      dr->hline(dr->ctx, xa, xb, y, colour);
    }
  }
}

// ---- the basemap ----------------------------------------------------------------
// Polygons under, lines over, places on top. Returns line segments drawn (a cheap
// cost proxy for profiling). Unknown classes are skipped, not guessed at.
static inline int mapDrawBase(const MapDraw *dr, const MapData *d, const MapView *v,
                              const MapPalette *pal) {
  int segs = 0;
  for (uint16_t i = 0; i < d->npolys; i++) {
    const MapPolygon *p = &d->polys[i];
    if (p->cls >= 2) continue;
    if (p->minz > v->minz) continue;
    if (!mapBoundsOnScreen(v, p->xy, p->n)) continue;
    mapFillPolygon(dr, v, p, pal->poly_cls[p->cls]);
  }
  for (uint16_t li = 0; li < d->nlines; li++) {
    const MapPolyline *p = &d->lines[li];
    if (p->cls >= MAPVEC_LINE_CLASSES) continue;
    if (p->minz > v->minz) continue;
    if (!mapBoundsOnScreen(v, p->xy, p->n)) continue;
    const uint16_t col = pal->line_cls[p->cls];
    const uint8_t w = pal->line_w[p->cls] ? pal->line_w[p->cls] : 1;
    int px, py;
    mapWorldToScreen(v, p->xy[0], p->xy[1], &px, &py);
    for (uint16_t i = 1; i < p->n; i++) {
      int nx2, ny2;
      mapWorldToScreen(v, p->xy[2 * i], p->xy[2 * i + 1], &nx2, &ny2);
      dr->line(dr->ctx, px, py, nx2, ny2, col);
      // Thickness by vertical offset. Crude, but it is what the ST7789 build already
      // did for arterials and it costs one extra line() per step rather than a stroker.
      for (uint8_t k = 1; k < w; k++)
        dr->line(dr->ctx, px, py + k, nx2, ny2 + k, col);
      px = nx2;
      py = ny2;
      segs++;
    }
  }
  if (dr->place) {
    for (uint16_t i = 0; i < d->nplaces; i++) {
      const MapPlace *pl = &d->places[i];
      int sx, sy;
      mapWorldToScreen(v, pl->x, pl->y, &sx, &sy);
      if (sx < v->x - 30 || sx > v->x + v->w + 30 || sy < v->y - 10 || sy > v->y + v->h + 10)
        continue;
      dr->place(dr->ctx, sx, sy, pl->type, pl->name);
    }
  }
  return segs;
}
