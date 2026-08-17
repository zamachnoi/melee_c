#!/bin/sh
set -eu

asset_dir="${ASSET_DIR:-/data/replays/cache}"
iso_path="${MELEE_ISO:-/data/replays/game.iso}"
expected_id="$(extract_tool --print-cache-id)"

need_cache=0
if [ "${FORCE_CACHE_REBUILD:-}" = "1" ]; then
    need_cache=1
elif [ ! -f "$asset_dir/meta.json" ] || \
     ! grep -q ",\"cache_id\":${expected_id}}" "$asset_dir/meta.json" 2>/dev/null || \
     [ ! -f "$asset_dir/grnla.anims" ] || \
     [ ! -f "$asset_dir/effects.json" ] || \
     [ ! -f "$asset_dir/icons/falco-0.png" ]; then
    # Persistent /data/replays/cache survives deploys. Sentinel files from an
    # older --all pass (falco.model, grnla.stage, schema 4) are not enough:
    # new extract outputs 404 until cache_id matches ASSET_CACHE_ID.
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
