#!/bin/sh
set -eu

asset_root="${ASSET_DIR:-/data/replays/cache}"
iso_path="${MELEE_ISO:-/data/replays/game.iso}"
expected_id="$(extract_tool --print-cache-id)"
schema="$(extract_tool --print-schema)"
schema_dir="$(extract_tool --print-schema-dir)"
versioned="$asset_root/$schema_dir"

need_cache=0
if [ "${FORCE_CACHE_REBUILD:-}" = "1" ]; then
    need_cache=1
else
    check_dir="$versioned"
    if [ ! -d "$check_dir" ]; then
        check_dir="$asset_root"
    fi
    if [ ! -f "$check_dir/meta.json" ] || \
       ! grep -q ",\"cache_id\":${expected_id}}" "$check_dir/meta.json" 2>/dev/null || \
       [ ! -f "$check_dir/grnla.anims" ] || \
       [ ! -f "$check_dir/effects.json" ] || \
       [ ! -f "$check_dir/icons/falco-0.png" ]; then
        # Persistent /data/replays/cache survives deploys. Older schema dirs
        # (v4, unversioned root) stay on disk; a mismatch rebuilds only vN.
        need_cache=1
    fi
fi

if [ "$need_cache" = 1 ]; then
    if [ -f "$iso_path" ]; then
        mkdir -p "$asset_root"
        echo "Building Melee asset cache schema $schema ($schema_dir, cache_id=$expected_id) from mounted ISO..."
        extract_tool --iso="$iso_path" --all --out="$asset_root"
    else
        echo "Warning: $iso_path is missing; 2D assets are unavailable" >&2
    fi
fi

exec viewer
