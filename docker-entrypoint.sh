#!/bin/sh
set -eu

asset_dir="${ASSET_DIR:-/data/replays/cache}"
iso_path="${MELEE_ISO:-/data/replays/game.iso}"

if [ ! -f "$asset_dir/fd.stage" ] || \
   [ ! -f "$asset_dir/fox-0.model" ] || \
   [ ! -f "$asset_dir/falco-0.model" ] || \
   [ ! -f "$asset_dir/effects.json" ] || \
   ! grep -q '"schema_version":4' "$asset_dir/meta.json" 2>/dev/null; then
    if [ -f "$iso_path" ]; then
        echo "Building Melee render cache from mounted ISO..."
        mkdir -p "$asset_dir"
        extract_tool --iso="$iso_path" --char=fox --stage=FD --out="$asset_dir"
        extract_tool --iso="$iso_path" --char=falco --out="$asset_dir"
        extract_tool --iso="$iso_path" --effects --out="$asset_dir"
    else
        echo "Warning: $iso_path is missing; 2D assets are unavailable" >&2
    fi
fi

exec viewer
