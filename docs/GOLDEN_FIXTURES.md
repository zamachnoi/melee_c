# WebGL2 migration fixtures and measurements

The phase-0 visual fixture is `fixtures/vertical.slp`, a Fox/Falco match on
Final Destination. The selected C reference frames are:

| Case | Frame | Evidence |
| --- | ---: | --- |
| idle | 8 | Falco state `0x000E` |
| dash | 11 | Falco state `0x0014` |
| airborne | 18 | Falco airborne flag |
| shield | 99 | Fox state `0x00B2` |
| facing flip | 275 | Falco changes `+1` to `-1` |
| run | 1004 | Fox state `0x0015` |
| attack | 1852 | Falco state `0x0033` |

`fixtures/ICs.slp` adds the real Popo/Nana visual cases that were previously
synthetic-only:

| Case | Frame | Evidence |
| --- | ---: | --- |
| Nana shield | 42 | follower state `0x00B2` |
| Nana airborne | 61 | follower airborne flag |
| Nana idle | 173 | follower state `0x000E` |
| Nana attack | 295 | follower state `0x002C` |
| Nana facing flip | 444 | follower changes facing |
| Nana separated | 2302 | Popo/Nana roots are about 179 world units apart |

Run `make golden-fixtures` while the worktree dev server is running. It writes
paired C reference PNG and decoded state JSON files under `build/golden/`.
Those generated files are intentionally not committed; the replay SHA-256 in
each JSON file makes a capture reproducible and reviewable.

The same command validates every present `animation_index` against the matching
schema-4 action table and writes raw index plus resolved action names to
`animation-index.json`. `0xFFFFFFFF` remains an explicit missing-animation
sentinel; any other out-of-range index fails the capture.

Rollback semantics remain covered by `tests/test_rollback.c`. Nana/follower
semantics are covered both by the deterministic C-to-TypeScript fixture in
`tests/test_timeline.c` and the real `ICs.slp` capture. Its completed timeline
contains 6,179 Nana frames in slot 3 and is 877,828 bytes uncompressed.

## Current schema-4 inventory

Raw transfer sizes for the first vertical slice are approximately:

| Asset | Wire bytes | Decoded typed-array bytes | GPU-upload upper bound |
| --- | ---: | ---: | ---: |
| Falco costume 2 model | 1,333,856 | 1,332,814 | 1,332,814 |
| Falco animations | 3,619,969 | 3,237,395 | 0 |
| Fox costume 0 model | 2,521,894 | 2,520,186 | 2,520,186 |
| Fox animations | 3,713,050 | 3,326,237 | 0 |
| Final Destination stage | 3,321,918 | 3,318,436 | 3,318,436 |
| Total reusable source data | 14,510,687 | 13,735,068 | 7,171,436 |

The GPU column is a conservative phase-1 upload candidate total, not a claim
that every decoded byte remains resident. Animation keys stay on the CPU;
actual phase-2 GPU allocation will be measured after VAO/texture upload.

Run `make measure-assets` to record wire bytes, decoded typed-array bytes, and
TypeScript parse time on the current machine. The completed vertical replay
timeline is 878,408 bytes uncompressed for 7,616 frames, below the 2 MB phase-1
target. Browser HTTP content compression can reduce transfer further without
changing the binary schema.

## Toolchain and coordinate contract

Browser source is strict TypeScript under `web/`. `npm run build` emits native
ES modules to `web/dist/`; there is no runtime bundler. `npm test` compiles and
runs Node's test runner, and `npm run typecheck` performs a no-emit strict
check. The C server only serves allowlisted compiled modules with JavaScript
MIME types.

The renderer boundary is `web/renderer/interface.ts`. Camera state and axes are
defined in `web/renderer/camera.ts`: model Z maps to screen horizontal, model Y
to vertical, and model X to depth/painter ordering. Negative replay facing
mirrors only the profile horizontal axis. Replay x/y remains authoritative.
