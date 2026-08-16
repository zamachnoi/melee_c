This is a C project that parses untrusted Super Smash Bros. Melee inputs: Slippi `.slp` replays (UBJSON + big-endian event stream) and DAT/HSD stage assets. Review only real defects with a file:line and a plausible failure mode. Ignore naming, comments, missing docs, and speculative refactors.

If a changed file reads DAT pointers, JOBJ trees, `map_head` sections, textures, or SLP event payloads without checking size/offset against the buffer, or uses an integer size/offset that can overflow, then:
- Add a blocking Bug titled "Unchecked binary bounds"
- Body: name the read/write, the missing bound, and how a truncated or hostile file reaches it.

If a changed file treats a big-endian Melee/Slippi integer or float as host-endian, or uses a DAT file offset as a host pointer, then:
- Add a blocking Bug titled "Endianness or offset-as-pointer"
- Body: cite the field and the correct conversion.

If a changed file mishandles live `.slp` files (raw length 0), unknown payload sizes, Ice Climbers follower slots, the 15 item-updates-per-frame cap, or version-gated fields as always present, then:
- Add a blocking Bug titled "Malformed or versioned replay not handled"
- Body: cite the event/code path and the bad input that breaks it.

If a changed file leaks, double-frees, uses memory after free, keeps renderer pointers after asset unload, or skips cleanup on `slp_error_t` / alloc failure, then:
- Add a blocking Bug titled "Memory lifetime bug"
- Body: cite the alloc, the error path, and the use after it.

If a changed file maps frame -123 incorrectly (index 0), mixes pre-frame and post-frame fields, or confuses slot vs port, then:
- Add a blocking Bug titled "Replay/rollback correctness"
- Body: cite the wrong field or index and the visible gameplay/stat error.

If a changed render/asset path can crash or silently draw the wrong geometry or texture, then:
- Add a blocking Bug titled "Render/asset correctness"
- Body: cite the bad draw path and the observable result.

Do not file bugs for style, naming, comments, or missing tests unless the missing test hides one of the defects above.
