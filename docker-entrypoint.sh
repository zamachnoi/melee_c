#!/bin/sh
set -eu

asset_dir="${ASSET_DIR:-/data/replays/cache}"
iso_path="${MELEE_ISO:-/data/replays/game.iso}"

need_cache=0
if [ ! -f "$asset_dir/grnla.stage" ] || \
   [ ! -f "$asset_dir/popo-3.model" ] || \
   [ ! -f "$asset_dir/falco-0.model" ] || \
   [ ! -f "$asset_dir/effects.json" ] || \
   ! grep -q '"schema_version":4' "$asset_dir/meta.json" 2>/dev/null; then
    need_cache=1
fi
need_icons=0
if [ ! -f "$asset_dir/icons/falco-0.png" ] && [ ! -f "$asset_dir/falco-0.png" ]; then
    need_icons=1
fi

if [ "$need_cache" = 1 ] || [ "$need_icons" = 1 ]; then
    if [ -f "$iso_path" ]; then
        mkdir -p "$asset_dir"
        if [ "$need_cache" = 1 ]; then
            echo "Building Melee render cache from mounted ISO..."
            extract_tool --iso="$iso_path" --all --out="$asset_dir"
        else
            echo "Extracting stock icons into $asset_dir/icons..."
            extract_tool --iso="$iso_path" --icons --out="$asset_dir"
        fi
    else
        echo "Warning: $iso_path is missing; 2D assets are unavailable" >&2
    fi
fi

exec viewer
