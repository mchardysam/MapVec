# The .mapvec format, version 2

One little-endian binary blob. Every multi-byte read in the reference parser is
byte-wise, so the format is endian-correct on any host.

**Geometry is 2-byte aligned, on purpose.** The fixed header is 14 bytes, and every
shape header and every point is 4 bytes, so every `xy` array starts on an even offset.
That is what lets the parser hand geometry out zero-copy as `const int16_t *` without
unaligned loads. **Anything added ahead of the geometry must keep the running offset
even.** See "Alignment" at the end.

```
u32  magic      'MVEC' (0x4D564543 read as LE u32)
u8   version    2
u8   pad        0. Load-bearing -- see "Alignment"
i32  lat0_e7    projection origin latitude,  degrees * 1e7
i32  lon0_e7    projection origin longitude, degrees * 1e7

u16  npolys                       filled areas, drawn FIRST (under everything)
     repeated npolys times:
u8     cls      0 water, 1 park          (2 reserved: building)
u8     minz     lowest zoom level it appears at
u16    npts     ring points; first != last, the ring closes implicitly
i16    xy[2*npts]   local metres, x east, y north

u16  nlines                       polylines, drawn over the polygons
     repeated nlines times:
u8     cls      0 major road, 1 minor road, 2 path, 3 waterway,
                5 cycleway                                      (4 reserved: rail)
u8     minz     lowest zoom level it appears at
u16    npts
i16    xy[2*npts]

u16  nplaces                      labelled points, drawn last
     repeated nplaces times:
i16    x, y     local metres
u8     type     0 suburb, 1 school, 2 pool   (3+ app-defined)
u8     len      name length in bytes (not null-terminated in the file)
char   name[len]
```

## Projection

Flat local frame about the origin, metres, quantised to int16:

```
x = (lon - lon0) * 111320 * cos(lat0)
y = (lat - lat0) * 111132
```

`cos(lat0)` is baked in at build time, so the device does no per-point trig. int16
metres caps an extract at roughly +/-32 km from the origin; that's a deliberate
limit (see the README for the cell-atlas pattern past it).

## Rules

- Version bumps only for changes a v1 parser would misread. Adding a NEW cls value
  is not a bump: parsers must skip records whose cls they don't recognise, which
  the length-prefixed layout makes free. Line class 5 (cycleway) was added this
  way — an older parser drops those lines and draws the rest of the map correctly.
- Class numbers are never reused or renumbered. Class 4 stays a reserved hole for
  rail even though nothing produces it yet, because a file built by someone reading
  an earlier version of this page has to keep meaning what it meant.
- Polygon rings are simple (no holes, no self-intersection is enforced by the
  producer's simplifier only in practice, not by the format). A renderer using
  even-odd fill degrades gracefully on a bad ring.
- npts of 0 or 1 in any record is invalid; the reference parser rejects the file.
- Producers should cap polygon rings at 128 points (raise the simplification
  tolerance rather than exceed it) so renderers can project a ring on the stack.

## Alignment

The parser hands polygon and polyline geometry out **zero-copy**: `xy` is a
`const int16_t *` pointing straight into your file buffer, not a copy. That only works
if the geometry is 2-byte aligned, and the format guarantees it:

```
 0  magic    4
 4  version  1
 5  pad      1      <-- this byte
 6  lat0_e7  4
10  lon0_e7  4
14  npolys   2
16  poly hdr 4
20  xy ...          <-- even, and every header and point is 4 bytes, so it stays even
```

Places are read byte-wise, so their variable-length names can't disturb it, and they
sit after all geometry anyway.

**v1 got this wrong.** Without the pad the fixed header was 13 bytes, the first `xy`
began at offset 19, and every point in every file sat on an odd address. That is
undefined behaviour in C++ regardless of platform; gcc's UBSan reports it, while clang
on x86 silently does the right thing, which is how it shipped. On ESP32 an unaligned
16-bit load is trap-and-fixup, so it cost exactly the speed the zero-copy design
exists to buy. v2 adds the pad and is not compatible with v1 files -- the parser
rejects them by version rather than misreading them.

**Your file buffer must itself be at least 2-byte aligned.** The format guarantees an
even *offset*; the buffer's own address supplies the rest. `malloc`, `ps_malloc` and
any sane arena all satisfy this, but a buffer carved out of a larger blob at an odd
index does not.

If you extend the header, **keep the running offset to the first `xy` even.**
`test/test_mapvec_parse.cpp` asserts the alignment, so a regression fails CI.

## Licensing

The FORMAT and the code are MIT. A .mapvec file built from OpenStreetMap data is a
derived database under ODbL: keep the "(c) OpenStreetMap contributors" attribution
with any file you distribute, and see https://www.openstreetmap.org/copyright.
