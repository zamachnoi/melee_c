# Replay web protocols

This document freezes the phase-0 byte contracts. All multibyte values are
big-endian. Asset, timeline, and live versions are independent:

| Contract | Version | Definition |
| --- | ---: | --- |
| extracted assets | 4 | `ASSET_SCHEMA_VERSION` in `src/asset.h` |
| completed timeline | 1 | `TIMELINE_SCHEMA_VERSION` in `src/protocol.h` |
| live messages | 1 | `LIVE_PROTOCOL_VERSION` in `src/protocol.h` |

Schema mismatch is an error. A reader may skip unknown trailing data only when
an enclosing offset/length describes it; it must never reinterpret a known
field using a different schema.

## Replay identity and HTTP routes

A replay id is the lowercase, 64-character SHA-256 digest of the exact SLP
bytes. It is immutable and safe to resolve without a selected process-global
replay.

```text
POST /api/replays
GET  /api/replays
GET  /api/replays/{sha256}/manifest
GET  /api/replays/{sha256}/timeline
GET  /api/replays/{sha256}/reference?n={frame}  (migration oracle only)
```

`POST /api/replays` accepts raw SLP bytes and an `X-Replay-Name` display-name
header, validates them before storage, and returns `id`, `name`, and
`manifestUrl`. The original name is display metadata; storage and routing still
use the immutable content id. The manifest supplies player identity, schema
versions, replay bounds, timeline URL, and allowlisted asset URLs. Manifest and
timeline responses use content-derived ETags and immutable cache headers.

## Timeline schema 1

The timeline begins with a fixed 64-byte header:

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | `char[4]` | `RPL2` |
| 4 | `u16` | timeline schema |
| 6 | `u16` | flags: bit 0 completed, bit 1 camera present |
| 8 | `i32` | start frame |
| 12 | `i32` | end frame, inclusive |
| 16 | `u16` | stage id |
| 18 | `u8` | slot count; schema 1 requires 8 |
| 19 | `u8` | player count |
| 20 | `u32` | frame count |
| 24 | `u32` | player records offset |
| 28 | `u32` | slot descriptors offset |
| 32/36 | `u32` | item records offset/count |
| 40/44 | `u32` | stage-event records offset/count |
| 48 | `u32` | camera section offset, or zero |
| 52 | `u32` | logical payload end |
| 56 | `u16` | required asset schema |
| 58 | `u16` | related live protocol version |
| 60 | `u32` | reserved, zero |

Each 64-byte player record stores port, external character id, costume,
starting stocks, team, player type, a 32-byte null-terminated display name, and
a 16-byte null-terminated connect code.

Each of the eight 56-byte slot descriptors starts with port, follower, and
active bytes, followed by thirteen `u32` offsets. The first twelve point to
frame-count-length structure-of-arrays fields in this order:

```text
presence:u8 character:u8 animation_index:u32 action_state:u16
animation_frame:f32 x:f32 y:f32 facing:f32 percent:f32 shield:f32
stocks:u8 flags:u8
```

The thirteenth value is the slot section end. In schema 1, flags bit 0 means
airborne. Missing frames have `presence == 0` and must not be sampled. Inactive
slot descriptors may share a compact zero backing range.

Item records are 40 bytes: frame, spawn id, type, state, signed owner, facing,
velocity x/y, position x/y, damage, and instance id. Stage-event records are 16
bytes: frame, kind, index, reserved, `data0`, and `data1`. Kind 1 is FoD (index
is platform and `data0` is float height), kind 2 is Whispy, and kind 3 is
Stadium.

The camera section has a 16-byte header containing offsets for `x:f32[]`,
`y:f32[]`, and `zoom:f32[]`. These required samples reproduce the C reference
gameplay camera for every integer replay frame.

## Live protocol 1 (specified, transport deferred)

Live support is a later milestone and does not block completed replay playback.
When implemented, the socket route is `/api/replays/{id}/live`. Every binary
message starts with `RPLV`, `u16 protocol`, `u8 type`, `u8 flags`, `u32 sequence`,
and `u32 payload_bytes`. Types are HELLO=1, SNAPSHOT=2, FRAMES=3, REPLACE=4,
GAME_END=5, PING=6, PONG=7, ERROR=8.

Sequence numbers are monotonic. Gaps require a new snapshot. `FRAMES` and
`REPLACE` contain a first frame, frame count, and batched records compatible
with the timeline field meanings. Replacement is by integer frame number and
latest sequence wins. Network arrival only adds/replaces timeline data; it
never advances playback. Reconnect sends the last applied sequence and falls
back to SNAPSHOT when resumption is unavailable.

## Asset routes

```text
/assets/v4/models/{character}-{costume}.model
/assets/v4/anims/{character}-{costume}.anims
/assets/v4/stages/fd.stage
```

Only lowercase alphanumeric, `_`, `-`, and the expected suffix are accepted.
Files are validated for `MDL\0` and schema 4 before delivery, then served with a
SHA-256 ETag and `Cache-Control: public, max-age=31536000, immutable`. The
TypeScript parsers apply strict count, offset, dimension, and decoded-byte
limits before allocation.

Nana uses `nana-{costume}.model` with the shared `popo-{costume}.anims` action
bank. `PlNnAJ` contains only Nana-specific clips; common replay animation
indices align with Popo's full action table.
