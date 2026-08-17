# WebGL2 replay renderer

Phase 4 is available without changing the default software viewer:

```text
http://spire:8080/?renderer=webgl2
```

The page runs its WebGL2, shader, floating-point texture, texture-size, and
vertex-capability gate before requesting a replay timeline or reusable model,
animation, and stage assets. It evaluates every active slot's keyed SRT tracks
in TypeScript, uploads independent bone hierarchies into RGBA32F textures, and
skins each fighter in the vertex shader. This supports multiple ports,
same-model instances, and followers such as Nana. The FD section uses the same
visibility rules as the C renderer. Replay and follow-target selectors use real
display names; `slot` and `frame` query parameters make a case reproducible.

```text
/?renderer=webgl2&replay={sha256}&slot=2&frame=8
```

Playback uses an integer 60 Hz replay clock. Space toggles play/pause, the arrow
keys or buttons step by one frame, and the range input scrubs. Original, fit,
and follow camera modes are available. Pointer drag and wheel switch to a free
local camera, while double-click resets the selected automatic mode. None of
these operations makes a network request.

Items are indexed by frame and rendered as local laser or item proxies. Shields
are translucent world-space discs. HTML HUD cards show names, percent, stocks,
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

## Known visual discrepancies

- Generic items and Ice Climber effects use colored proxy discs; only Fox/Falco
  lasers reproduce the C line treatment. Item models and particles are not in
  the schema-4 asset set.
- Shields are flat translucent discs rather than Melee's full shield material.
- FoD, Whispy, and Stadium records drive persistent proxy indicators. Moving
  stage geometry is not yet deformed because the supplied fixtures contain
  Final Destination and no dynamic-stage events.
- Transparent model content remains ordered at primitive-group granularity;
  the C reference sorts individual triangles.
- Lighting does not emulate the full TEV pipeline, and the HUD is intentionally
  simpler than the in-game HUD.
- Full-match browser scheduling skipped presentations in this shared preview
  environment even though measured app work remained below 0.3 ms average.
  The anchored integer clock did not drift and reached every exact final frame.
