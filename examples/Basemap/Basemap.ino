// Basemap: draw a .mapvec on an ST7789 through Arduino_GFX, cycling zoom levels.
//
// Self-contained on purpose: the demo map is built in RAM at boot and fed through
// the REAL parser, so this runs on a bare board + panel with no SD card. A real
// project reads a file produced by tools/osm_to_vectors.py into a buffer
// (ps_malloc on an ESP32 with PSRAM) and calls the same mapParse.
//
// The five-line adapter below is the whole display integration. Adafruit_GFX is
// the same shape; LVGL or e-paper just implement line + hline differently.
#include <Arduino_GFX_Library.h>

#include <mapvec.h>
#include <mapvec_render.h>

// Wiring: change to match your panel.
static const int PIN_DC = 8, PIN_CS = 10, PIN_RST = 9;

// RGB565 colours (newer Arduino_GFX no longer exports the bare colour macros).
static const uint16_t C_BLACK = 0x0000, C_WHITE = 0xFFFF, C_YELLOW = 0xFFE0;
static const uint16_t C_LIGHTGREY = 0xC618, C_DARKGREY = 0x7BEF, C_BLUE = 0x001F;
static const uint16_t C_DARKCYAN = 0x03EF, C_DARKGREEN = 0x03E0;
Arduino_DataBus *bus = new Arduino_HWSPI(PIN_DC, PIN_CS);
Arduino_GFX *gfx = new Arduino_ST7789(bus, PIN_RST, 0 /*rotation*/, true /*IPS*/);

// ---- the display adapter: MapDraw -> Arduino_GFX --------------------------------
static void gfxLine(void *g, int x0, int y0, int x1, int y1, uint16_t c) {
  ((Arduino_GFX *)g)->drawLine(x0, y0, x1, y1, c);
}
static void gfxHline(void *g, int x0, int x1, int y, uint16_t c) {
  ((Arduino_GFX *)g)->drawFastHLine(x0, y, x1 - x0 + 1, c);
}
static void gfxPlace(void *g, int x, int y, uint8_t, const char *name) {
  Arduino_GFX *G = (Arduino_GFX *)g;
  G->fillRect(x - 2, y - 2, 5, 5, C_YELLOW);
  if (name && name[0]) {
    G->setCursor(x + 5, y - 3);
    G->setTextColor(C_WHITE);
    G->print(name);
  }
}

// ---- a little demo town, assembled as real .mapvec bytes ------------------------
static void putU16(uint8_t *b, size_t &o, uint16_t v) { b[o++] = v & 0xFF; b[o++] = v >> 8; }
static void putI16(uint8_t *b, size_t &o, int16_t v) { putU16(b, o, (uint16_t)v); }
static void putI32(uint8_t *b, size_t &o, int32_t v) {
  for (int i = 0; i < 4; i++) b[o++] = (uint8_t)(((uint32_t)v >> (8 * i)) & 0xFF);
}
static void putPts(uint8_t *b, size_t &o, const int16_t *xy, uint16_t n, uint8_t cls,
                   uint8_t minz) {
  b[o++] = cls;
  b[o++] = minz;
  putU16(b, o, n);
  for (uint16_t i = 0; i < 2 * n; i++) putI16(b, o, xy[i]);
}

static uint8_t mapBytes[512];
static size_t buildDemoMap() {
  size_t o = 0;
  putI32(mapBytes, o, (int32_t)MAPVEC_MAGIC);
  mapBytes[o++] = MAPVEC_VERSION;
  putI32(mapBytes, o, -350000000);  // a synthetic origin
  putI32(mapBytes, o, 1490000000);

  putU16(mapBytes, o, 2);  // polygons: a lake and a park
  static const int16_t lake[] = {40, 60, 120, 80, 140, 20, 90, -20, 30, 10};
  putPts(mapBytes, o, lake, 5, MAPVEC_POLY_WATER, 0);
  static const int16_t park[] = {-140, -40, -60, -40, -60, -120, -140, -120};
  putPts(mapBytes, o, park, 4, MAPVEC_POLY_PARK, 1);

  putU16(mapBytes, o, 4);  // lines: a main road, two streets, a creek
  static const int16_t main_rd[] = {-200, 0, -40, 0, 60, -40, 200, -60};
  putPts(mapBytes, o, main_rd, 4, MAPVEC_LINE_MAJOR, 0);
  static const int16_t st1[] = {-100, 160, -100, -40};
  putPts(mapBytes, o, st1, 2, MAPVEC_LINE_MINOR, 2);
  static const int16_t st2[] = {-100, 120, 80, 120, 100, 60};
  putPts(mapBytes, o, st2, 3, MAPVEC_LINE_MINOR, 2);
  static const int16_t creek[] = {-200, 180, -20, 120, 40, 60};
  putPts(mapBytes, o, creek, 3, MAPVEC_LINE_WATERWAY, 1);

  putU16(mapBytes, o, 1);  // one place
  putI16(mapBytes, o, -100);
  putI16(mapBytes, o, 130);
  mapBytes[o++] = 1;  // school
  mapBytes[o++] = 4;
  memcpy(mapBytes + o, "Demo", 4);
  o += 4;
  return o;
}

MapParsed parsed;
bool mapOk = false;

void setup() {
  Serial.begin(115200);
  gfx->begin();
  gfx->fillScreen(C_BLACK);

  const size_t n = buildDemoMap();
  const char *err = nullptr;
  mapOk = mapParse(mapBytes, n, malloc, parsed, &err);
  if (!mapOk) Serial.printf("mapParse failed: %s\n", err);
}

void loop() {
  if (!mapOk) return;
  static uint8_t z = 0;
  z = (uint8_t)((z + 1) % 4);

  MapView v;
  v.cx = 0;
  v.cy = 20;
  v.scale = 0.4f + 0.4f * z;  // zoom in each pass
  v.minz = z;                 // and reveal more detail (LOD)
  v.x = 0;
  v.y = 0;
  v.w = gfx->width();
  v.h = gfx->height();

  MapPalette pal;
  pal.line_cls[MAPVEC_LINE_MAJOR] = C_WHITE;
  pal.line_cls[MAPVEC_LINE_MINOR] = C_LIGHTGREY;
  pal.line_cls[MAPVEC_LINE_PATH] = C_DARKGREY;
  pal.line_cls[MAPVEC_LINE_WATERWAY] = C_BLUE;
  pal.poly_cls[MAPVEC_POLY_WATER] = C_DARKCYAN;
  pal.poly_cls[MAPVEC_POLY_PARK] = C_DARKGREEN;

  MapDraw dr{gfx, gfxLine, gfxHline, gfxPlace};
  gfx->fillScreen(C_BLACK);
  int segs = mapDrawBase(&dr, &parsed.data, &v, &pal);
  Serial.printf("zoom %u: %d segments\n", z, segs);
  delay(2500);
}
