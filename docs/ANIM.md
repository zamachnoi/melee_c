# Melee animation: figatree, keyframe tracks, and the fighter action table

How a fighter action maps to an animation and how that animation is decoded.
Covers `Pl*Aj.dat` slices, the `figatree` root, per-bone keyframe tracks, and
the `ftData*` action table in `Pl*.dat`.

Measured against `fixtures/game.iso` (Falco: `PlFc.dat` + `PlFcAJ.dat`).

## 1. Action table → animation slice

`Pl*.dat` (fighter core) root `ftData*` has at `+0x0C` a data-relative pointer
to the **fighter action table**. The table is an array of **0x18-byte entries**:

```c
struct ACTION_ENTRY {
    u32 nameRef;    // 0x00 data-relative string (action name), optional
    u32 animOffset; // 0x04 byte offset into the companion Pl*AJ.dat
    u32 animSize;   // 0x08 byte length of the embedded animation DAT slice
    u32 unknown0x0C;// 0x0C
    u32 flags;      // 0x10
    u32 unknown0x14;// 0x14
};
```

`Pl*AJ.dat` is a **byte pool**: each action's animation is a standalone DAT
document embedded at `animOffset` with length `animSize`. The first slice
starts at file offset `0`.

Measured (`PlFc.dat`):

```
[0] WallDamage_figatree  animOff=0x00115F20 animSize=0x00001CC0
[1] DamageFall_figatree  animOff=0x00017C60 animSize=0x00001214
[2] Wait1_figatree       animOff=0x00000000 animSize=0x00002637
[3] Wait2_figatree       animOff=0x00002640 animSize=0x00002EBB
```

Wait1 (slice at offset 0) has fileSize `0x2637` — consistent.

## 2. Embedded animation DAT

Each slice is a normal DAT container (see DAT.md) with a single root whose name
contains `figatree`. For Falco `Wait1`, the root name is
`PlyFalco5K_Share_ACTION_Wait1_figatree`.

### 2.1 Figatree root

```c
struct FigaTree {          // from the SSBM decomp (lbanim.h), matches our bytes
    u32  unknown0x00;   // 0x00 type (0x00000001)
    u32  unknown0x04;   // 0x04 flags (0)
    f32  frameCount;    // 0x08 total frames
    u8*  nodes;         // 0x0C track-info (per-bone track counts)
    FigaTrack* tracks;  // 0x10 packed track-descriptor array
    ...                 // 0x14/0x18/0x1C further per-bone data refs
};
```

Measured `Wait1` figatree: frameCount = `240.0`, trackInfo → `0x23C8`,
trackData → `0x1E28`.

### 2.2 Track-info table

`trackInfo` is a **byte array, one byte per bone** (index = bone index in the
skeleton). Each byte is the number of tracks that bone has. The table is
terminated by `0xFF`.

Measured `Wait1` (first bytes): `00 00 02 06 00 03 03 00 ...` → bone 0: 0
tracks, bone 1: 0, bone 2: 2, bone 3: 6, bone 4: 0, …

### 2.3 Track structs

`trackData` is a packed array of 0x0C-byte `FigaTrack` structs. Bones with N
tracks consume N consecutive structs (in order). Matches the decomp
(`struct FigaTrack`):

```c
struct FigaTrack {
    u16  length;        // 0x00
    u16  startFrame;    // 0x02
    u8   obj_type;      // 0x04
    u8   frac_value;    // 0x05 quantized value format+scale
    u8   frac_slope;    // 0x06 tangent format+scale
    u8   pad;           // 0x07
    u8*  ad_head;       // 0x08 data-relative keyframe stream
};
```

Measured `Wait1` tracks:

```
[0] startFrame=0  type=6  valFlag=0x2D tanFlag=0x2E dataRef=0x00000000
[1] startFrame=0  type=7  valFlag=0x2D tanFlag=0x2E dataRef=0x0000008C
[2] startFrame=0  type=1  valFlag=0x2E tanFlag=0x66 dataRef=0x000000B8
[3] startFrame=0  type=2  valFlag=0x2D tanFlag=0x2E dataRef=0x000000D4
[4] startFrame=0  type=3  valFlag=0x2E tanFlag=0x4F dataRef=0x000000F0
[5] startFrame=0  type=5  valFlag=0x66 tanFlag=0x88 dataRef=0x00000110
```

### 2.4 Track types

The `obj_type` byte selects which FObj operation/register this track drives.
Per the SSBM decomp, each `FigaTrack` becomes a `HSD_FObj` whose `obj_type`
maps to a bone transform channel (1..4 JOBJ rotation/translation/scale axes,
selectively per the existing community mapping); `obj_type` also subsumes the
material/material-color/material-alpha and texture U/V channels in matanim
contexts (`TYPE_JOBJ`, material, texture op codes from the decomp). Measured
track values in Wait1: `1, 2, 3, 5, 6, 7` — the joint channels below.

## 3. Keyframe data

The `ad_head`/`dataRef` stream is the keyframe data for a track: a sequence of
**packed keyframes**. Crucially, everything inside this stream — the packed
varints and the f32/s16/u16 value samples — is **LITTLE-endian** (only the
stream is; the container/header/root/track structs are big-endian). This is
confirmed by the SSBM decomp (`HSD_FObj` `parseFloat`/`parsePackInfo` in
`src/sysdolphin/baselib/fobj.{c,h}`) and noclip.website's HSD reader
(`getFloat32(..., true)` etc.), and matches revelation's animation-workshop
writeup.

### 3.1 Packed integer (variable length)

```
read byte a
if a & 0x80:  read byte b;  return (a & 0x7F) | (b << 7)
else:         return a
```

### 3.2 Keyframe opcode

The stream is a series of keyframe groups. Each group begins with a packed
value whose **low nibble is the interpolation/opcode** and whose **upper bits
encode the key count** (`(packed >> 4) + 1` keys). Opcodes (decomp names in
parentheses):

| Op | Name | Meaning |
| --- | --- | --- |
| 0 | NONE | end of stream |
| 1 | CON (constant) | step key: `value`, then `frameDelta` |
| 2 | LIN (linear) | linear key: `value`, then `frameDelta` |
| 3 | SPL0 (hermite) | hermite key (no stored tangents): `value`, then `frameDelta` |
| 4 | SPL (hermite+tangent) | hermite key with tangent: `value`, `tangent`, then `frameDelta` |
| 5 | SLP (slope) | set out-tangent on the previous key |
| 6 | KEY (skip) | skip a value (no key emitted) |

Frames are **displacements**: a key's absolute frame = accumulated sum of
`frameDelta`s from the previous key (relative to `startFrame`).

### 3.3 Value/tangent quantization

`frac_value`/`frac_slope` bytes encode the format and scale of each value:

```
bits 0..4  scale shift S:  divide raw by 2^S
bits 5..7  format (HSD_A_FRAC_*):
  0x00  FLOAT f32 (4 bytes)
  0x20  S16  (2 bytes, LSB-first)
  0x40  U16  (2 bytes, LSB-first)
  0x60  S8   (1 byte)
  0x80  U8   (1 byte)
```

Measured `Wait1` track 1 (`type=7` translate z): `valFlag=0x2D` → s16 with
`2^13` divisor; `tanFlag=0x2E` → s16 with `2^14` divisor. Decoded keys:

```
key 0: frame  0  value -0.6876  tangent 0.4062
key 1: frame 12  value  1.2510  tangent 1.1251
key 2: frame 24  value  2.3768  tangent 0.4376
...
```

### 3.4 Interpolation

Given a target frame, find the bracketing keys and:

* **step / CON**: hold the previous key's value.
* **linear / LIN**: `v = lerp(k0.v, k1.v, t)`.
* **hermite / SPL**, **SPL0**: cubic Hermite with tangents `k0.out` and `k1.in`:
  `v = h00*v0 + h10*m0 + h01*v1 + h11*m1` where `m0 = k0.out * span`,
  `m1 = k1.in * span` and `h00..h11` are the Hermite basis of `t`.

Rotation tracks animate the bone's rotation; apply per-channel like the other
transforms.

## 4. Material / texture animation (matanim)

`Pl*Nr.dat` files carry a second root named `*_matanim_joint` (measured:
`PlyFalco5K_Share_matanim_joint`). The layout follows the general HSD animation
scaffolding:

```
AnimJoint      { child; next; AObjDesc(anim) }
MatAnimJoint   { child; next; MatAnim }
MatAnim        { next; AObjDesc(for MObj); TexAnim; RenderAnim? }
TexAnim        { next; TexMapID; AObjDesc(for TObj); ImageDesc*; TLutDesc*; nImage; nTLut }
```

Texture-frame animation (e.g. blinking eyes, glowing effects) is driven by
AObj → FObj chains that swap `ImageDesc`/`TLutDesc` at keyframes (op types
`01`/`0A`).

## 5. AObj / FObj (runtime animation objects)

The on-disk descriptors the game casts directly:

```c
struct AOBJDESC {   // 0x10 bytes
    u32  unk;       // 0x00
    f32  endFrame;  // 0x04
    u32  fobj;      // 0x08 root FObjDesc
    u32  objId;     // 0x0C
};

struct FOBJDESC {
    u32  next;          // 0x00
    u16  dataLen;       // 0x04 data-string length (incl NUL)
    f32  startFrame;    // 0x08
    u8   opType;        // 0x0C operation type (joint/weight/camera/material/tex)
    u8   qType;         // 0x0D high 3 bits: quantized data type
    u8   qShift;        // 0x0D low 5 bits: dequant shift
    u32  data;          // 0x10 data string
};
```

Quantized data types: `0` none, `1` s16, `2` u16, `3` s8, `4` u8. Dequant:
`raw / 2^qShift`.

## 6. Endianness caveat — RESOLVED

Everything in the DAT is big-endian (the HSD archive header, relocation/root
lists, FigaTree/FigaTrack structs, the FObjDesc chain). **Only the packed
keyframe sample stream is little-endian** (f32/s16/u16 and the packed
op/count/duration varints). Confirmed by three independent sources: the SSBM
decomp `HSD_FObj` (`fobj.c` byte-by-byte LSB-first), noclip.website's HSD
reader (`DataView … true`), and the community animation-workshop writeup
("read … as little endian"). Do **not** byte-swap header/root-pointer fields.

## 7. Validation targets (Falco)

* bones = 67 (confirmed by JOBJ walk, see HSD.md).
* actions: the community subaction dump (huang-hobbs melee_subaction_unpacker)
  lists **294 subaction records** in `PlFc.dat`'s ftData table — **246 named**
  (some names reused across variants, e.g. Landing/DamageFall), referencing
  **189 distinct figatree archives** in `PlFcAJ.dat`; Fox has 188. The plan's
  ">300 actions" most likely refers to the fuller action-state enumeration, not
  this table. Our measured span (327 slots / 0x1EA8 via node-span to ftData)
  brackets the named subset; count rule to be pinned in Phase B.
* `Wait1` resolves at action index 2 → slice `[0, 0x2637)` in `PlFcAJ.dat`.

## 8. Sources

* SSBM decomp `src/sysdolphin/baselib/fobj.{c,h}` and `src/melee/lb/lbanim.h`
* noclip.website `src/SYSDOLPHIN/SYSDOLPHIN.ts`, `src/SuperSmashBrosMelee/Melee_ft.ts`
* huang-hobbs.co melee_subaction_unpacker (`subactions/PlFc.html`,
  `autoanimtables/PlFc/`)
* Community "Melee DAT format" + "Melee Animation / Model Workshop" threads