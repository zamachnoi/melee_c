#!/bin/sh
set -eu

asset_dir="${ASSET_DIR:-/data/replays/cache}"
iso_path="${MELEE_ISO:-/data/replays/game.iso}"
expected_id="$(extract_tool --print-cache-id)"

need_cache=0
if [ "${FORCE_CACHE_REBUILD:-}" = "1" ]; then
    need_cache=1
elif [ ! -f "$asset_dir/v5/meta.json" ] || \
     ! grep -q ",\"cache_id\":${expected_id}}" "$asset_dir/v5/meta.json" 2>/dev/null || \
     [ ! -f "$asset_dir/v5/stages/grnla.anims" ] || \
     [ ! -f "$asset_dir/v5/effects.json" ] || \
     [ ! -f "$asset_dir/icons/falco-0.png" ]; then
    # Persistent /data/replays/cache survives deploys, so we only rebuild when
    # the on-disk cache is genuinely missing or stale.
    #
    # extract_tool --all writes the schema-versioned cache UNDER $asset_dir/v5/
    # (meta.json, effects.json, stages/...), with stock icons in $asset_dir/icons/.
    # Earlier versions wrote these files flat at $asset_dir/ root (schema 4); any
    # cache in that old layout is invalid for the current ASSET_SCHEMA_VERSION, so
    # a missing/mismatched $asset_dir/v5/meta.json forces a full rebuild.
    need_cache=1
fi

if [ "$need_cache" = 1 ]; then
    if [ -f "$iso_path" ]; then
        mkdir -p "$asset_dir"
        echo "Building full Melee asset cache (cache_id=$expected_id) from mounted ISO..."
        extract_tool --iso="$iso_path" --all --out="$asset_dir"
    else
        echo "Warning: $iso_path is missing; 2D assets are unavailable" >&2
    fi
fi

exec viewer
