# Melee stages: DAT layout, sections, camera, lights, stage animations

How a stage DAT (`GrNLa.dat` = Final Destination, `GrIz.dat` = FD "Iz",
`GrPs.dat` = Stadium, etc.) is laid out, and where camera/scale/lights and
stage animations live.

Measured against `fixtures/game.iso` (Final Destination `GrNLa.dat`).

## 1. Stage DAT root nodes

All six supported stages share the **same structural root set** (measured with
`tools/extract/datdump.c`):

| Root | Purpose |
| --- | --- |
| `map_head` | main stage descriptor (below) |
| `Grd*_image` | texture images (format embedded in the name) |
| `Grd*_tlut` / `Grd*_tlut_desc` | palette data + descriptor for indexed (`C4`/`C8`) images |
| `grGroundParam` | stage scale, camera, misc params |
| `coll_data` | collision geometry (vertices + links) |
| `itemdata` / `yakumono_param` | item / misc spawn params |
| `map_plit` | light list |
| `map_ptcl` | particle params |
| `map_texg` | texgen params |
| `quake_model_set` | screen-shake model set |
| `ALDYakuAll` | stage-object animation scaffold |

Measured root counts: FD (`GrNLa`) 27, Battlefield (`GrNBa`) 37, FoD (`GrIz`)
89, Stadium (`GrPs`) 97, Dream Land (`GrOp`) 72, Yoshi's Story (`GrSt`) 63.

Image names carry the format: `GrdLastGround2_CMPR_image`,
`GrdLastLine1_I4_image`, `GrdBattleWall0_C8_image`, `GrdIzumiRbw3_RGBA8_image`,
`GrdOldpupupuWgrass4_RGB5A3_image`, `GrdStoryShadowB_IA4_image`. `_image_desc`
roots (e.g. `GrdPS*_image_desc`, `GrdIzumi_cd_wt_*_image_desc`) are image
descriptor blocks. Stage JOBJ roots appear as `Grd*_TopN_joint` (e.g.
`GrdIzumiStar_TopN_joint`) or via `map_head` model groups.

Measured FD roots (first/last few):

```
ALDYakuAll
GrdLastCloud2_I8_image
GrdLastGround2_CMPR_image
...
GrdLastWall1_RGBA8_image
coll_data
grGroundParam
itemdata
map_head
map_plit
map_ptcl
map_texg
quake_model_set
yakumono_param
```

## 2. `map_head`

`map_head` is a table of `(offset, count)` pairs; each offset is data-relative.
Measured FD layout:

| Offset | Field | Value |
| --- | --- | --- |
| `0x00` | ptr/count | `0x54` / 1 |
| `0x08` | **model-group ptr/count** | `0xAC` / 10 |
| `0x10` | ptr/count | `0x2B4` / 2 |
| `0x18` | ptr/count | `0x2BC` / 32 |
| `0x20` | ptr/count | `0x33C` / 3 |
| `0x28` | ptr/count | `0x354` / 1 |

The **model-group array** at `map_head+0x08` is the important part: it lists the
stage's *sections* (platforms, background pieces, and the FD's halves) as
`0x34`-byte (52-byte) entries. Each entry:

| Offset | Size | Meaning |
| --- | --- | --- |
| `0x00` | 4 | root JOBJ offset (skeleton for this section) |
| `0x04` | 4 | Joint AnimJoint / AObjDesc |
| `0x08` | 4 | MatAnimJoint / material-anim root |
| `0x0C` | 4 | ShapeAnim |
| `0x10` | 4 | camera (CObj) |
| `0x14` | 4 | unknown |
| `0x18` | 4 | light |
| `0x1C` | 4 | unknown |
| `0x20` | 4 | unknown array ptr |
| `0x24` | 4 | unknown array count |
| `0x28` | 4 | unknown array ptr (moving-platform collision links) |
| `0x2C` | 4 | unknown array ptr (AObj loop-enable flags) |
| `0x30` | 4 | count |

The stage is rendered by walking each model group's root JOBJ chain and decoding
the DOBJ/POBJ/MOBJ/TOBJ graph exactly as for a fighter mesh (see HSD.md).
The entire model group array is `10 * 0x34` bytes in FD.

## 3. `grGroundParam`

| Offset | Size | Meaning |
| --- | --- | --- |
| `0x00` | 4 | **stage scale** (f32) |
| … | | misc movement/bounds params |
| `0x4C` | 4 | fixed-camera flag (1 in PokeFloats/Icicle) |
| `0x50` | 12 | camera position (f32 x/y/z) |
| `0x5C` | 4 | camera FOV (f32, degrees) |
| `0x60` | 4 | vertical angle (f32, degrees) |
| `0x64` | 4 | horizontal angle (f32, degrees) |

Measured FD: scale `1.0`, camera position `(0, 45, 356)`, fov `30.0°`,
vertical `-2.0°`, horizontal `0.0°`.

## 4. Lights: `map_plit`

`map_plit` is a null-terminated list of data-relative pointers; each points to a
**LightRef (0x08)** → **LObj (0x1C)**. LObj layout (see HSD.md):

| Offset | Size | Meaning |
| --- | --- | --- |
| `0x08` | 2 | type flags: 0 ambient, 1 infinite, 2 point, 3 spot |
| `0x0A` | 2 | attenuation flag |
| `0x0C` | 4 | RGBA color |
| `0x10` | 4 | WObj position ref |
| `0x14` | 4 | WObj interest ref |
| `0x18` | 4 | point/spot/attenuation params ref |

## 5. `coll_data`

Not needed for 2D rendering but quick to record:

| Offset | Size | Meaning |
| --- | --- | --- |
| `0x00` | 4 | vertex-array offset (f32 x/y pairs) |
| `0x04` | 4 | vertex count |
| `0x08` | 4 | link-description array offset |
| `0x0C` | 4 | link count |

## 6. Stage animation

Stage animation is driven by AnimJoint/MatAnimJoint structures wired through
`map_head` model groups and the `ALDYakuAll` root (Fountain's floating
platforms, Whispy, Stadium transforms). Per the community thread:

- `map_head` model-group entries carry **AnimJoint** (joint anim) and
  **MatAnimJoint** (material anim) pointers; the game casts DAT data directly
  onto `JObj`/`AnimJoint`/`MatAnimJoint`/`ShapeAnimJoint` descriptors.
- `ALDYakuAll` is the stage-object animation scaffold; several map entries
  embed the object arrays that the game's Whispy/Fountain/Stadium logic reads
  during play.

Stage animation exact decoding is deferred to Phase B–C; for Phase A the goal is
to document the entry points (map_head groups, ALDYakuAll) and the AObj/FObj
runtime shape (ANIM.md §7) so the wiring is understood.

## 7. Practical decode flow

1. Parse the DAT (DAT.md).
2. Find `map_head`; read the model-group array (offset/count at `+0x08`).
3. For each model group, walk its root JOBJ → decode geometry (HSD.md).
4. Read `grGroundParam` scale + camera; read `map_plit` lights.
5. Leave stage animations wired-but-unplayed until Phase C.

## 8. Open questions (verify in Phase B)

* Exact per-field meaning of the `0x34` model-group entry beyond `0x10`.
* `ALDYakuAll` internal layout (Whispy/FD/Stadium transform tables).
* Which cameras/lights correspond to which model group (fixed default set is
  enough for the side-view 2D projection for now).