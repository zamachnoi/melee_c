#!/usr/bin/env sh
# Point $root/cache at the machine-shared fixtures/cache. Never copy the ISO extract.
# Usage: ensure-shared-cache.sh [worktree-root]
set -eu
if [ -n "${1:-}" ]; then
    root="$1"
else
    root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
fi
mkdir -p "$root/fixtures/cache"
wanted="fixtures/cache"
if [ -L "$root/cache" ]; then
    target="$(readlink "$root/cache")"
    if [ "$target" != "$wanted" ] && [ "$target" != "$root/$wanted" ]; then
        echo "ensure-shared-cache: retargeting $root/cache ($target -> $wanted)" >&2
        rm -f "$root/cache"
    fi
elif [ -e "$root/cache" ]; then
    echo "ensure-shared-cache: replacing private $root/cache with symlink to $wanted" >&2
    rm -rf "$root/cache"
fi
if [ ! -e "$root/cache" ]; then
    ln -s "$wanted" "$root/cache"
    echo "ensure-shared-cache: $root/cache -> $wanted" >&2
fi
