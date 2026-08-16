# Melee HSD object model: joints, draw objects, materials, geometry, textures

The DAT data block is a scene graph of HSD (Half-Life Software Development /
"Super Smash Bros Melee" engine) objects. This document enumerates the object
types we must decode for fighters and stages, their on-disk layouts, the GX
geometry encoding, and the TPL texture formats.

Measured against `fixtures/game.iso` via `tools/extract/datdump.c`; layout
facts cross-checked against the community
[DAT format thread](https://smashboards.com/threads/melee-dat-format.292603/)
and the HSDLib reference. All scalars big-endian; all object pointers are
**data-relative** (`abs = 0x20 + offset`), and `0` means null.

## 1. Object model overview

HSD scene graph (the common objects, per the community object reference):

```
SObj   Scene
JObj   Joint         -> DObj -> MObj -> TObj   (material)
                             |  -> PObj        (geometry)
                   -> RObj
AObj   Animation     (see ANIM.md)
FObj   Frame
LObj   Light         (0xD4)
WObj   World         (0x20)
CObj   Camera        (0x8C)
Figatree / MatAnim / ShapeAnim   (animation containers)
```

A fighter mesh DAT (`Pl*Nr.dat`) has a root named `*_joint`; walking the JOBJ
tree from that root yields every bone. The measured Falco skeleton
(`PlFcNr.dat`) has **67 JOBJ nodes**, matching the known-good number.

## 2. JOBJ — joint / bone (0x40 bytes)

```c
struct JOBJ {
    u32  className;       // 0x00 data-relative string ref (often 0)
    u32  flags;           // 0x04
    u32  child;           // 0x08 first child JOBJ
    u32  next;            // 0x0C next sibling JOBJ
    u32  dobj;            // 0x10 DOBJ list head (draw objects)
    f32  rotation[3];     // 0x14 rx, ry, rz
    f32  scale[3];        // 0x20 sx, sy, sz
    f32  translation[3];  // 0x2C tx, ty, tz
    u32  transformOffset; // 0x38 inverse-world matrix (see below)
    u32  unknown0x3C;     // 0x3C (ROBJ list in some layouts)
};
```

Hierarchy: follow `child` (deep) and `next` (wide). The tree is the skeleton;
each JOBJ's local transform is `T * R * S` composed in that order from
`translation`, `rotation`, `scale`. World transforms chain through parents.

**Flags** (bit meanings, from HSDLib/`JOBJFlags`):

| Bits | Name | Notes |
| --- | --- | --- |
| `1<<0` | SKELETON | this JOBJ is a skeleton root |
| `1<<1` | SKELETON_ROOT | |
| `1<<2` | ENVELOPE_MODEL | skinned model: POBJ uses envelopes |
| `1<<3` | CLASSICAL_SCALING | |
| `1<<4` | HIDDEN | skip rendering |
| `1<<7` | LIGHTING | |
| `1<<8` | TEXGEN | |
| `1<<9..11` | billboard modes | |
| `1<<12` | INSTANCE | |
| `1<<18` | OPA | opaque |
| `1<<19` | XLU | translucent |
| `1<<21..22` | joint class | JOINT1/JOINT2/EFFECTOR |
| `1<<28` | ROOT_OPA | |
| `1<<29` | ROOT_XLU | |

Measured: Falco root flags `0x1005018E`, a leaf `0x10000009`.

**Inverse world transform (`0x38`)**: on-disk this is a **data-relative pointer**
to a 12-f32 (3x4) matrix block, non-null for most bones
(`PlFcNr.dat` child1 `0x00016EE8` → identity-ish matrix bytes). The root bone
has `0x38 = 0`. (The in-memory HSD_JObj embeds `Mtx inv` inline; the serialized
form stores it out-of-line.) Decode as `[12]f32` when non-null.

## 3. DOBJ — draw object (0x10 bytes)

```c
struct DOBJ {
    u32  unknown0x00;   // className string ref in some files
    u32  next;          // 0x04 next DOBJ in this JOBJ's list
    u32  mobj;          // 0x08 material
    u32  pobj;          // 0x0C geometry (POBJ list head)
};
```

A JOBJ may carry multiple DOBJs (e.g. multiple material passes on one bone).

## 4. POBJ — polygon / primitive object (0x18 bytes)

```c
struct POBJ {
    u32  unknown0x00;   // className string ref
    u32  next;          // 0x04 next POBJ
    u32  attributes;    // 0x08 vertex-attribute array (ATTR list)
    u16  flags;         // 0x0C
    u16  displayListSizeDiv32;  // 0x0E number of 0x20-byte blocks
    u32  displayList;   // 0x10 display-list bytes
    u32  weightList;    // 0x14 envelope (bone weight) list
};
```

**POBJ flags** (relevant bits): `1<<13` ENVELOPE (skinned), `1<<14` CULLBACK,
`1<<15` CULLFRONT, `1<<3` ANIM, `1<<12` SHAPEANIM. Measured Falco root POBJ
flags `0xA001` (ENVELOPE + CULLFRONT + shapeset).

`displayListSizeDiv32 * 32` = display-list byte length. Display list is usually
32-byte aligned.

### 4.1 Vertex attribute array (ATTR)

At `POBJ.attributes`, an array of 0x18-byte entries terminated by an entry whose
`vtxAttr == GX_VA_NULL (0xFF)`:

```c
struct ATTR {
    u32  vtxAttr;        // 0x00 GXAttr
    u32  vtxAttrType;    // 0x04 GXAttrType (direct/index8/index16)
    u32  compCnt;        // 0x08 GXCompCnt (position/normal/color/tex dims)
    u32  compType;       // 0x0C GXCompType (u8/s8/u16/s16/f32, or color type)
    u8   scale;          // 0x10 value divisor: component / 2^scale
    u8   unknown0x11;    // 0x11
    u16  vtxStride;      // 0x12 bytes between consecutive values
    u32  dataOffset;     // 0x14 data-relative base of the attribute array
};
```

Measured `PlFcNr.dat` root POBJ attributes:

```
[0] GX_VA_PNMTXIDX DIRECT  compCnt=0 compType=f32 scale=0 stride=0
[1] GX_VA_POS      INDEX16 compCnt=XYZ compType=s16 scale=11 stride=6
[2] GX_VA_NRM      INDEX16 compCnt=XYZ compType=s8  scale=6  stride=3
[3] GX_VA_TEX0     INDEX16 compCnt=ST  compType=s16 scale=12 stride=4
[4] GX_VA_NULL (terminator)
```

**GXAttr enum** (selected): `0` PNMTXIDX, `9` POS, `10` NRM, `11` CLR0,
`12` CLR1, `13` TEX0, … `25` NBT, `0xFF` NULL.

**GXAttrType**: `0` NONE, `1` DIRECT, `2` INDEX8, `3` INDEX16.

**Component counts**: POS: `0`=XY, `1`=XYZ; NRM: `0`=XYZ, `1`=NBT; CLR:
`0`=RGB, `1`=RGBA; TEX: `0`=S, `1`=ST.

**Component types**: `0` u8, `1` s8, `2` u16, `3` s16, `4` f32. Color types:
`0`=RGB565, `1`=RGB8, `2`=RGBX8, `3`=RGBA4, `4`=RGBA6, `5`=RGBA8.

**Scale**: integer components are divided by `2^scale` to get the real value
(e.g. POS s16 with scale 11 → `raw / 2048`; NRM s8 scale 6 → `raw / 64`; TEX0
s16 scale 12 → `raw / 4096`). This is what turns a 12-byte F32 vertex array into
a compact 6-byte S16 array.

### 4.2 Display list

A stream of GX display-list packets. Each packet:

```c
struct DL_PACKET {
    u8   primitiveFlags;   // 0x80 quads, 0x90 triangles, 0x98 tri-strip, 0xA0 tri-fan
    u16  indexCount;
    // per-vertex: one token per attribute, in ATTR order
    //   DIRECT:    1 byte (matrix idx) or color bytes
    //   INDEX8:    1 byte index
    //   INDEX16:   2 byte index
};
```

Primitive types (mask `0xF8`): `0x80` quads, `0x90` triangles, `0x98`
triangle strip, `0xA0` triangle fan, `0xA8` lines, `0xB0` line strip, `0xB8`
points. Packets repeat until the buffer is exhausted (`0` flag ends the list).

Each vertex token is: for INDEX8/INDEX16, an index into the attribute array at
`ATTR.dataOffset` (data-relative); for DIRECT, the value inline (matrix index is
a u8; a DIRECT color is the full color size). Indices are in ATTR order, so the
per-vertex token size = sum over attributes.

### 4.3 Envelopes (skinning)

When `POBJ.flags & ENVELOPE`, `POBJ.weightList` (data-relative) points to a list
of pointers; each is an `ENVELOPE`: up to 6 `(JOBJ_offset, f32 weight)` pairs
(8 bytes each). A vertex's `PNMTXIDX` DIRECT value selects an envelope:
`envelope_index = pnmtxidx / 3`. Up to 4 bone influences contribute.

## 5. MOBJ — material (0x18 bytes)

```c
struct MOBJ {
    u32  unknown0x00;   // className string ref
    u32  renderFlags;   // 0x04 render-mode flags
    u32  tobj;          // 0x08 TOBJ list head (textures)
    u32  materials;     // 0x0C MATERIAL colors
    u32  unknown0x10;   // 0x10
    u32  peDesc;        // 0x14 pixel-ops descriptor (blending/alpha test)
};
```

Render-mode flag highlights: `1<<2` DIFFUSE, `1<<3` SPECULAR, `1<<4..11`
TEX0..TEX7 (per-texture enabled bits), `1<<13..14` alpha source (MAT/VTX/BOTH),
`1<<30` XLU.

### 5.1 MATERIAL colors (0x14 bytes)

```c
struct MATERIAL {
    u8 ambient[4];    // 0x00 RGBA
    u8 diffuse[4];    // 0x04 RGBA
    u8 specular[4];   // 0x08 RGBA
    f32 alpha;        // 0x0C
    f32 shininess;    // 0x10
};
```

### 5.2 PE DESC — pixel processing (0x0C bytes)

```c
struct PEDESC {
    u8 pixelProcessFlags;  // 0x00 (alpha update, z update, dither, compare...)
    u8 blendMode;          // 0x04 (0 none, 1 blend, 2 logic, 3 subtract)
    u8 depthFunction;      // 0x08 compare type
};
```

## 6. TOBJ — texture object (0x5C bytes)

```c
struct TOBJ {
    u32  className;     // 0x00
    u32  next;          // 0x04 next TOBJ in MOBJ list
    ...                 // 0x08..0x1B TEV / gen setup
    f32  scale[3];      // 0x1C texcoord scale
    ...
    u32  texGenSrc;     // 0x0C (0 POS, 1 NRM, 4.. TEX0..)
    u32  wrapModeS;     // 0x34 (0 clamp, 1 repeat, 2 mirror)
    u32  wrapModeT;     // 0x38
    u8   repeatU;       // 0x3C
    u8   repeatV;       // 0x3D
    u32  flags;         // 0x40 coord/lighmap/colormap mode bits
    u32  blending;      // 0x44 f32 blend factor
    u32  image;         // 0x4C IMAGE (TPL header)
    u32  tlut;          // 0x50 TLUT (palette) header, if indexed
    u32  unknown0x54;   // 0x54
    u32  unknown0x58;   // 0x58
};
```

TOBJ flags: coord mode bits `0..2` (UV/reflection/hilight/shadow/toon/
gradation), lightmap bits `4..8`, colormap mode bits `16..18`, alphamap bits
`20..22`, `1<<31` MTX_DIRTY.

## 7. IMAGE — TPL texture (0x18 bytes)

```c
struct IMAGE {
    u32  dataOffset;   // 0x00 data-relative raw texture data
    u16  width;        // 0x04
    u16  height;       // 0x06
    u32  format;       // 0x08 TexFormat
    u16  mipmapCount;  // 0x0C
    ...
};
```

## 8. TLUT — palette (0x20 bytes)

```c
struct TLUT {
    u32  dataOffset;    // 0x00 data-relative palette data
    u32  format;        // 0x04 TlutFormat
    ...
    u16  colorCount;    // 0x0C
    ...
};
```

Palette byte length = `colorCount * 2`. Indexed images may use fewer palette
entries than the format maximum (e.g. an 8-bit image with only 136 colors).

## 9. Texture formats (GX / TPL)

### 9.1 Image formats

| Id | Name | Block | Notes |
| --- | --- | --- | --- |
| 0 | I4 | 8x8, 32 B | 4-bit intensity, alpha=intensity |
| 1 | I8 | 8x4, 32 B | 8-bit intensity |
| 2 | IA4 | 8x4, 32 B | 4-bit intensity + 4-bit alpha |
| 3 | IA8 | 4x4, 32 B | 8-bit intensity + 8-bit alpha |
| 4 | RGB565 | 4x4, 32 B | 16-bit RGB |
| 5 | RGB5A3 | 4x4, 32 B | 16-bit RGBA (1-bit alpha mode) |
| 6 | RGBA8 | 4x4, 64 B | 32-bit RGBA (two 16-bit planes) |
| 8 | CI4 (C4) | 8x8, 32 B | 4-bit palette index |
| 9 | CI8 (C8) | 8x4, 32 B | 8-bit palette index |
| 10 | CI14X2 | linear | 14-bit palette index, 2 bytes/px |
| 14 | CMPR | 8x8, 32 B | S3TC-style DXT1 |

(Stage DAT names spell the indexed formats `_C4_image` / `_C8_image`; the
community thread calls 8/9 "index4"/"index8".)

Texture data size = `ceil(w/block_w) * ceil(h/block_h) * block_bytes`.

Block layouts (like all GameCube formats, tiles are stored top-down, rows within
a tile top-down):

* I4/I8/IA4/IA8/RGB565/RGB5A3: one u16 per pixel within the tile.
* RGBA8: two u16 planes per pixel — AR and GB, interleaved by 4x4 sub-tile
  halves (first pass writes R+G+A? see codec; treat as A/R + G/B halves).
* CMPR: 8x8 tile = four 4x4 sub-blocks, each 8 bytes (2 colors + 4-bit indices;
  if `c0 > c1` 4-color mode else 3-color + transparent).
* CI14X2: linear u16 per pixel, index `& 0x3FFF`.

### 9.2 Palette (TLUT) formats

| Id | Name |
| --- | --- |
| 0 | IA8 |
| 1 | RGB565 |
| 2 | RGB5A3 |

## 10. LOBJ/WOBJ/COBJ — lights, world objects, cameras

* **WObj (0x20)**: `position[3] f32` at `0x04` (a world transform holder).
* **LObj (0x1C)**: flags u16 `0x08`, attenuation flags u16 `0x0A`, color RGBA
  u8 `0x0C`, position WObj `0x10`, interest WObj `0x14`, data `0x18`
  (point/spot/attenuation params).
* **CObj (0x8C)**: stage camera. In stage files the camera is stored inside
  `grGroundParam` (see STAGES.md) rather than as a CObj root.

## 11. Practical decode flow

1. From a `*_joint` root, walk JOBJ tree → bones (67 for Falco).
2. For each JOBJ, walk DOBJ list; for each DOBJ read MOBJ (material + textures)
   and POBJ (geometry).
3. From POBJ read ATTR array → per-attribute format/scale/stride/data.
4. Parse display-list packets → per-vertex tokens → indices + DIRECT values.
5. Resolve envelopes when ENVELOPE flag set → per-vertex bone weights.
6. Decode IMAGE+TLUT → RGBA.

## 12. Open questions (verify in Phase B)

* Full TOBJ field semantics between `0x08` and `0x1C` (TEV regs) and `0x54/0x58`.
* Stage image roots named `*_MIPMAP_*` (e.g. `GrdPStadiumSteelA_MIPMAP_I8_image`)
  and `*_image_desc` blocks (e.g. `GrdIzumi_cd_wt_*_image_desc`) carry mipmap /
  image-descriptor metadata; confirm how they relate to `IMAGE` headers.