#!/usr/bin/env sh
# Git worktree hook: auto-start a per-worktree dev server when a worktree is
# checked out/created, and record the port for the agent via dev-server.env.
#
# Wire it up (git >= 2.44 provides a real "worktree created" hook):
#     git config worktree.guiHook "$(git rev-parse --git-common-dir)/hooks/wt-hook.sh"
#     git config worktree.resetHook "$(git rev-parse --git-common-dir)/hooks/wt-hook.sh"
#
# On older git (< 2.44) this hook never fires for `git worktree add`, so call
# `make devserver` (or scripts/devserver.sh) from inside the new worktree
# instead. That target is idempotent and writes dev-server.env in that root.
set -eu

# guiHook/resetHook are invoked with $1 = the new worktree path, cwd = worktree.
wt="${1:-$PWD}"

# Share the machine-wide DAT cache using this checkout's script so older
# worktree branches (without the file) still get cache -> fixtures/cache.
hook_dir="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
if [ -x "$hook_dir/ensure-shared-cache.sh" ]; then
    "$hook_dir/ensure-shared-cache.sh" "$wt"
fi

if [ ! -f "$wt/scripts/devserver.sh" ]; then
    exit 0
fi

# don't autostart on the main checkout unless asked
if [ "$wt" = "$(git rev-parse --show-toplevel 2>/dev/null)" ]; then
    exit 0
fi

log="$(git rev-parse --git-common-dir 2>/dev/null)/devserver-$(basename "$wt").log"
( "$wt/scripts/devserver.sh" >"$log" 2>&1 & ) &
echo "worktree dev server launching for $wt (log: $log)" >&2
exit 0
