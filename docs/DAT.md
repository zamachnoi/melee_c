# Melee: ISO/FST layer and DAT container

Ground-truth model of how a Melee asset lives on the disc and how the `.dat`
container is laid out. Reading this plus the community
[DAT format thread](https://smashboards.com/threads/melee-dat-format.292603/)
should let us explain every byte of a `Pl*.dat` file header.

Measured against `fixtures/game.iso` (1.46 GB) using `tools/extract/datdump.c`.

## 1. GameCube disc (ISO) layout

The ISO is a byte image of a GameCube disc (1.46 GB in our fixture). Two fixed
disc-header fields matter for asset access:

| Disc offset | Size | Meaning |
| --- | --- | --- |
| `0x0000` | 4 | Disc magic (`GALE` for GameCube) |
| `0x0424` | 4 | **FST file offset** (byte offset into the ISO) |
| `0x0428` | 4 | **FST size** (bytes) |
| `0x042C` | 4 | FST max size (same as size in practice) |

In our fixture: FST at `0x00456E00`, size `0x7529` (30009 bytes).

### The FST (file system table)

The FST is a flat array of 12-byte entries, plus a trailing string table. Entry
count is in the **root entry**.

**Root entry (entry index 0), 12 bytes:**

| Offset | Size | Meaning |
| --- | --- | --- |
| `0x00` | 1 | Type: `0x01` for root (directory) |
| `0x01` | 3 | Name offset: `0` for root (unused) |
| `0x04` | 4 | Unused (`0`) |
| `0x08` | 4 | **Total number of entries** (including root) |

Our fixture root: type `0x01`, entry count `1212` (`0x04BC`).

**Subsequent entries (index 1..count-1), 12 bytes:**

| Offset | Size | Meaning |
| --- | --- | --- |
| `0x00` | 1 | Type: `0x00` = file, `0x01` = directory |
| `0x01` | 3 | **Name offset**, relative to start of the string table |
| `0x04` | 4 | File: byte offset into ISO. Directory: unused |
| `0x08` | 4 | File: size in bytes. Directory: **one-past-last-child index** |

The **string table starts at `entry_count * 12`** (immediately after the last
FST entry). Names are NUL-terminated; the name of entry *i* is
`fst[entry_count*12 + name_offset_i ..]`.

Directory hierarchy: a directory's children are the contiguous range of entries
`(dir_index + 1) .. (dir_next_index - 1)` where `dir_next_index` is the value in
the directory entry's `0x08` field. This gives a depth-first, pre-order tree:
walk entries in order, maintaining a small stack of open directories, and push
child directories when `dir_next_index` exceeds the current index.

### Locating `.dat` files

Walk the FST tree, accumulating the full path (`dir/sub/file.dat`), and keep any
file whose name ends `.dat` (case-insensitive). The fixture has **838 `.dat`
files**. Examples:

```
.../PlFc.dat        (Falco fighter core; root ftDataFalco)
.../PlFcNr.dat      (Falco default-colour mesh)
.../PlFcBu.dat      (Falco blue mesh)
.../PlFcAJ.dat      (Falco animation container)
.../GrNLa.dat       (Final Destination)
```

Measured offsets in the fixture:

| File | ISO offset | Size |
| --- | --- | --- |
| `PlFc.dat` | `0x50868000` | 237784 |
| `PlFcNr.dat` | `0x508A8000` | 243994 |
| `PlFcAJ.dat` | `0x50AE0000` | 1495328 |
| `GrNLa.dat` | `0x4C4A0000` | 611125 |

## 2. DAT container

A `.dat` file (the container, not the assets) has this top-level layout:

```
0x00  File header (0x20 bytes)
0x20  Data block (dataBlockSize0x04 bytes)
      Relocation table (relocationTableCount0x08 * 4 bytes)
      Root node table (rootCount0x0C * 8 bytes)
      Reference node table (referenceCount0x10 * 8 bytes)
      String table (NUL-terminated names, runs to EOF)
```

**All scalar fields are big-endian.** (The figatree *keyframe sample stream* is
little-endian — see ANIM.md §3/§6. Everything this document covers is BE.)

### 2.1 Header (`0x00`..`0x1F`) — the HSD Archive header

The container is an **HSD Archive** (HAL's general asset container). The
authoritative struct from the SSBM decomp (`src/sysdolphin/baselib/archive.h`):

```c
struct HSD_ArchiveHeader {   /* STATIC_ASSERT(sizeof == 0x20) */
    u32 file_size;   /* 0x00 */
    u32 data_size;   /* 0x04 */
    u32 nb_reloc;    /* 0x08 */
    u32 nb_public;   /* 0x0C */
    u32 nb_extern;   /* 0x10 */
    u8  version[4];  /* 0x14 ASCII version tag, NOT used by the loader */
    u32 pad[2];      /* 0x18, 0x1C unused */
};
```

| Offset | Size | Name | Meaning |
| --- | --- | --- | --- |
| `0x00` | 4 | `fileSize` | Total file size in bytes |
| `0x04` | 4 | `dataSize` | Size of data block following the header |
| `0x08` | 4 | `nbReloc` | Number of relocation entries |
| `0x0C` | 4 | `nbPublic` | Number of root ("public") nodes |
| `0x10` | 4 | `nbExtern` | Number of reference ("extern") nodes |
| `0x14` | 4 | `version[4]` | 0–4 ASCII chars; **`"001B"` in main `Pl*.dat` / `Gr*.dat`** |
| `0x18` | 4 | `pad` | Unused |
| `0x1C` | 4 | `pad` | Unused |

`version` is purely informational — `HSD_ArchiveParse` never reads it, and
treating it as corruption rejects valid files. `0x18`/`0x1C` are explicit
padding. (The community thread's `rootCount0x0C` / `rootCount0x10` are the same
fields as `nbPublic` / `nbExtern`.)

Measured in the fixture:

* `PlFc.dat`: fileSize `0x0003A0D8`, dataSize `0x00036780`, nbReloc `3657`,
  nbPublic `1`, nbExtern `0`, version `"001B"`.
* `PlFcNr.dat`: fileSize `0x0003B91A`, nbReloc `1541`, nbPublic `2`, nbExtern
  `0`, version `\0\0\0\0` (no tag).
* `GrNLa.dat`: fileSize `0x00095335`, nbReloc `7999`, nbPublic `27`, nbExtern
  `0`, version `"001B"`.

Validation: `fileSize0x00` must equal the actual file length.

### 2.2 Node entry format (root and reference)

Both root and reference entries are 8 bytes:

```c
struct NODE_ENTRY {
    u32 dataOffset;         // relative to data block base (file offset 0x20)
    u32 stringTableOffset;  // relative to string table start
};
```

### 2.3 Offset rules

* Data pointers are relative to `data_base = 0x20`.
  Absolute file offset: `abs = 0x20 + dataOffset`.
* String pointers are relative to `string_table_start`.
  Absolute file offset: `abs = string_table_start + stringTableOffset`.
* Relocation entries are offsets (relative to data base) that point at **other
  pointer fields inside the data block**.

### 2.4 Section start formulas

```text
data_start          = 0x20
reloc_start         = 0x20 + dataBlockSize0x04
roots_start         = reloc_start + (relocationTableCount0x08 * 4)
references_start    = roots_start + (rootCount0x0C * 8)
string_table_start  = references_start + (referenceCount0x10 * 8)
```

`string_table_start` through EOF is the string table.

### 2.5 Root names as type hints

Root node names are the primary payload discriminator. Observed on the fixture
plus the community thread:

| Name pattern | Meaning |
| --- | --- |
| `ftData*` | Fighter core file (action table lives inside) |
| `*figatree*` | Animation data (in `Pl*AJ.dat` slices) |
| `*_joint` | JOBJ mesh root (in `Ty*.dat`, `Pl*Nr.dat` colour files) |
| `*matanim*` | Material animation joint data |
| `*shapeanim*` | Shape animation |
| `coll_data` | Collision payload (stages) |
| `map_head`, `grGroundParam`, `map_plit`, `map_ptcl`, `map_texg` | Stage structure |
| `*_image` | Texture image payload (format embedded in the name, e.g. `GrdLastGround2_CMPR_image`) |

Measured examples:

* `PlFc.dat` root: `ftDataFalco`
* `PlFcNr.dat` roots: `PlyFalco5K_Share_joint`, `PlyFalco5K_Share_matanim_joint`
* `PlFcAJ.dat` slice root: `PlyFalco5K_Share_ACTION_Wait1_figatree`
* `GrNLa.dat` roots: `map_head`, `grGroundParam`, `coll_data`, `map_plit`,
  `GrdLastGround2_CMPR_image`, `GrdLastLine1_I4_image`, `ALDYakuAll`, …

## 3. Relocation table

Each relocation entry is a `u32` offset (relative to data base) giving the file
location of a pointer field inside the data block. Its purpose is to locate
every data-relative pointer so that (a) the game can rebase them to absolute
memory addresses at load, and (b) we can derive object boundaries.

Useful consequences:

* **Reloc field index** — the set of absolute offsets of relocatable pointer
  fields. `readRelocDataRef`: a field is a real data pointer iff its offset is in
  this index.
* **Node span index** — every root/reference node offset, plus every *target*
  pointed at by a relocation field, is treated as a "node start". The byte span
  of a node runs from its start to the next node start. This is how counts like
  "number of action-table entries" are derived without explicit lengths.

## 4. Fighter action animation linkage (container → slices)

* In fighter core files, the `ftData*` root points (field `0x0C`, data-relative)
  to the **fighter action table**.
* The action table is an array of **`0x18`-byte entries**. `PlFc.dat` has
  `0x1EA8` bytes of action table (327 slots, span-derived), and the first entry
  resolves `Wait1`/`Wait2`/`Walk*`/`Dash`/`Run`/… by name.
* Each action entry:
  * `0x00`: optional **action name pointer** (data-relative string).
  * `0x04`: **byte offset into the companion `Pl*AJ.dat`**.
  * `0x08`: **byte size** of the embedded animation DAT slice.
  * `0x10`: flags.
* `Pl*AJ.dat` is therefore a **byte pool of back-to-back standalone DAT
  payloads**, not a single DAT document. The first slice starts at offset `0`
  (`PlFcAJ.dat` slice 0 = fileSize `0x2637`, the `Wait1` clip).

Measured action entries in `PlFc.dat`:

```
[0] name=...ACTION_WallDamage_figatree  animOff=0x00115F20 animSize=0x00001CC0
[1] name=...ACTION_DamageFall_figatree  animOff=0x00017C60 animSize=0x00001214
[2] name=...ACTION_Wait1_figatree       animOff=0x00000000 animSize=0x00002637
[3] name=...ACTION_Wait2_figatree       animOff=0x00002640 animSize=0x00002EBB
```

## 5. Practical parse flow

1. Read ISO disc header `0x424`/`0x428` → FST offset/size.
2. Parse FST; build `path -> (iso_offset, size)` for `*.dat`.
3. Extract the target DAT bytes.
4. Validate `fileSize == bytes.len`.
5. Compute section starts from header counts.
6. Read root/reference entries; resolve node offsets with `0x20 + dataOffset`.
7. Resolve names from `string_table_start + stringTableOffset`.
8. Use root/reference names to select structure decoders (HSD.md, ANIM.md,
   STAGES.md).

## 6. Open questions (verify in Phase B)

* The exact action-table span rule (node-span heuristic) should produce a count
  consistent with the independently-verified subaction dump for Falco: **294
  subaction records, 246 named, 189 distinct figatree archives** (see ANIM.md
  §7). Our span-derived count (327 slots to `ftData`) unnecessarily overcounts
  empty trailing slots; verify the row-scan rule in Phase B.

## 7. Naming note

The community thread's lowercase names (`dataBlockSize0x04`, `rootCount0x0C`,
`referenceCount0x10`) are the same fields as the decomp's canonical HSD names
(`data_size`, `nb_public`, `nb_extern`). We use the decomp names; both are
equivalent bit layouts. Some third-party material calls the container a
"HAL/DAT" — the canonical term is **HSD Archive**.