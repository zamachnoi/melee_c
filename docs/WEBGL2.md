# WebGL2 replay renderer

WebGL2 is the default completed-replay viewer. Live WebSocket spectating
(Phase 5) is skipped; completed `.slp` files already contain the final
timeline.

```text
http://spire:8080/
/?replay={sha256}&slot=2&frame=8
```

The C software renderer remains the migration oracle and is not the product
path:

```text
/?renderer=software
```

`?renderer=webgl2` is still accepted as an explicit alias. Raster frame
endpoints stay available for reference images until they have no remaining
dependency.

The page runs its WebGL2, shader, floating-point texture, texture-size, and
vertex-capability gate before requesting a replay timeline or reusable model,
animation, and stage assets. It evaluates every active slot's keyed SRT tracks
in TypeScript, uploads independent bone hierarchies into RGBA32F textures, and
skins each fighter in the vertex shader. This supports multiple ports,
same-model instances, and followers such as Nana. The FD section uses the same
visibility rules as the C renderer. Replay and follow-target selectors use real
display names; `slot` and `frame` query parameters make a case reproducible.

Playback uses an integer 60 Hz replay clock. Space toggles play/pause, the arrow
keys or buttons step by one frame, and the range input scrubs. Original, fit,
and follow camera modes are available. Pointer drag and wheel switch to a free
local camera, while double-click resets the selected automatic mode. None of
these operations makes a network request.

Items are indexed by frame. Fox/Falco lasers, illusion/phantasm, shields, shine,
and firefox use DAT meshes from `extract --effects` when `effects.json` is in
the asset directory; procedural bubbles/cylinders remain the fallback. Remaining
items stay as local proxy discs. Shine and firefox still classify from figatree
action names so the extracted mesh can be placed. HTML HUD cards show names, percent, stocks,
and follower identity. The backtick key or Debug button shows raw/resolved
animation identity, item count, fighter state, and persistent FoD, Whispy, and
Stadium event state.

Missing fighter models, animation banks, or stage assets do not abort the
replay. The affected slot falls back to a bind pose or an explicit unavailable
HUD card, while a visible warning explains the failure. For testing this path,
`failAsset=<URL substring>` injects a deterministic asset failure.

The diagnostics row reports presented and dropped frames, longest consecutive
run, fighter/item/warning counts, pose and total-frame timing, transferred
bytes, resident GPU bytes, overlay truncation, and an explicit frame-image
request count. Context restoration re-creates all multi-fighter and overlay
resources from retained parsed assets without another HTTP request.

## Phase 3 acceptance evidence

Measured on 2026-08-17 in Chromium against the C reference renderer:

- A 960×720 frame-8 comparison averaged 2.15 RGB levels of absolute error
  after masking the second fighter (which is Phase 4 scope). The selected Falco
  pose region averaged 3.40 levels; WebGL reported no error.
- A clean live run presented 625 consecutive integer replay frames at 60 Hz
  with zero drops, no new resource requests, 0.07 ms average pose work, and
  0.17 ms average total frame work.
- The ten-minute soak presented 35,984 frames over four replay loops with zero
  drops. Its longest uninterrupted replay run was 7,615 frames. Resource count
  stayed at 17, the explicit frame-image request count stayed at zero, JS heap
  fell from 102.8 MB to 38.4 MB after collection, and WebGL reported no error.
- `make test`, `make typecheck`, and `make test-http` pass.

## Phase 4 acceptance evidence

Measured on 2026-08-17 in Chromium:

- The complete 960×720 Fox/Falco frame, including both fighters and item
  proxies, averaged 2.17 RGB levels of absolute error from the C reference.
- The ICs fixture renders Falco, Popo, and Nana as three independent posed
  instances. A separate synthetic scene rendered two instances of the exact
  same Falco model object with independent bone textures and no GL error.
- A clean ICs playback presented 626 consecutive frames with zero drops at
  0.19 ms average combined pose work and 0.30 ms average total frame work.
- ICs completed frame −123 through 6055 exactly: 6,171 presentations plus 7
  skipped presentations account for all 6,178 clock advances. Fox/Falco
  completed frame −123 through 7492 exactly: 7,136 presentations plus 479
  scheduler skips account for all 7,615 advances. Both runs made zero new
  requests, ended without scene or GL errors, and showed no upward heap trend.
- Forced context loss with Falco, Popo, Nana, items, and HUD restored with zero
  new requests and no GL error. Injected animation, model, and stage failures
  each produced visible warnings while the remaining replay stayed usable.

## Phase 6 cutover

- `/` serves the WebGL2 page. `/?renderer=software` serves the C raster viewer.
- Replay upload lives on the default page via `POST /api/replays`.
- `/api/frame`, `/api/frames`, and `/api/replays/{id}/reference` remain as the
  visual oracle. They are not used during WebGL2 playback.

## Known visual discrepancies

- Generic items use colored proxy discs. Fox/Falco lasers, illusion, phantasm,
  shine, firefox, and shields draw extracted DAT meshes from `effects.json`
  (`make cache` / `extract --effects`). Procedural geometry is the fallback
  when a catalog entry or model file is missing.
- Shields use gfx 11's in-game billboard: the I4 quadrant is mirrored into a
  full circle and used as the bubble's coverage alpha, the IA8 lighting shades
  the port tint (instead of darkening it toward black), and the result is
  scaled to remaining shield size.
- FoD's two moving platforms are the actual stage-section meshes (section 2
  bones 2/3 and the matching pass on section 3), re-posed so the mesh top
  matches the recorded world-space `fodLeft`/`fodRight` height after FoD's
  0.75 stage scale.  Slippi platform 0 is the right platform.  Whispy and
  Stadium records still drive persistent proxy indicators; their stage
  geometry is not yet deformed because the supplied fixtures contain no
  dynamic-stage events.
- Transparent model content remains ordered at primitive-group granularity;
  the C reference sorts individual triangles.
- Lighting does not emulate the full TEV pipeline, and the HUD is intentionally
  simpler than the in-game HUD.
- Full-match browser scheduling skipped presentations in this shared preview
  environment even though measured app work remained below 0.3 ms average.
  The anchored integer clock did not drift and reached every exact final frame.
