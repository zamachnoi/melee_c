#!/usr/bin/env sh
# Per-worktree dev server for the SLP viewer.
#
#   * Picks the lowest free port >= START (default 8080) so ports never collide
#     and a stale process on 8081 can't be silently squatted.
#   * Builds bin/viewer if missing, then serves it on that port.
#   * Writes the chosen port to ./dev-server.env (gitignored) so the agent can
#     discover it from repo context:
#         DEV_URL=http://localhost:8081
#   * Idempotent: if a viewer is already serving on the selected port from THIS
#     worktree, it just prints the URL and exits 0.
#
# Usage:  scripts/devserver.sh [START_PORT]
set -eu

start="${1:-8080}"
root="$(cd "$(dirname "$0")/.." && pwd)"
env_file="$root/dev-server.env"

log() { printf 'devserver: %s\n' "$*" >&2; }

# True if nothing is bound to $1 on 127.0.0.1 (a real bind probe).
port_free() {
    if command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | awk -v p=":$1" '$4 ~ p { found=1 } END { exit found }'
        return
    fi
    if command -v python3 >/dev/null 2>&1; then
        python3 -c "import socket,sys
s=socket.socket()
try:
    s.bind(('127.0.0.1',$1)); sys.exit(0)
except OSError: sys.exit(1)" && return 1 || return 0
    fi
    # fallback: a connect fails (refused) when nothing listens
    if ! (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null; then
        return 0
    fi
    exec 3>&- 2>/dev/null || true
    return 1
}

# --- build if needed -------------------------------------------------------
log "building viewer..."
# Always run make so a stale bin/viewer cannot keep serving old parser code.
# The file target is used (the phony `viewer` target would start the server).
(cd "$root" && make bin/viewer >&2)

# Compile strict TypeScript sources to native browser ES modules. Fresh
# worktrees may not have local dependencies yet, so hydrate them from the lock
# file once before building.
if [ ! -f "$root/web/dist/main.js" ] || \
   find "$root/web" -path "$root/web/dist" -prune -o -name '*.ts' \
     -newer "$root/web/dist/main.js" -print -quit 2>/dev/null | grep -q .; then
    if [ ! -x "$root/node_modules/.bin/tsc" ]; then
        log "installing TypeScript toolchain..."
        (cd "$root" && npm ci >&2)
    fi
    log "building browser modules..."
    (cd "$root" && npm run build >&2)
fi

# --- find a free port, reusing the one we recorded if still ours ----------
free_port=""
if [ -f "$env_file" ]; then
    old="$(sed -n 's/^DEV_URL=.*:\([0-9][0-9]*\)$/\1/p' "$env_file" | head -n1)"
    if [ -n "$old" ] && ! kill -0 "$(cat "$root/.devserver.pid" 2>/dev/null)" 2>/dev/null; then
        # recorded port but our process is dead -> candidate, confirm free below
        free_port="$old"
    fi
fi

if [ -z "$free_port" ]; then
    p="$start"
    while [ "$p" -lt 65535 ]; do
        if port_free "$p"; then
            free_port="$p"
            break
        fi
        p=$((p + 1))
    done
fi
if [ -z "$free_port" ]; then
    log "no free port found from $start" >&2
    exit 1
fi

# --- write context file -----------------------------------------------------
printf 'DEV_URL=http://localhost:%s\n' "$free_port" > "$env_file"
public_host="${DEV_PUBLIC_HOST:-$(hostname -s)}"
log "dev server URL: http://$public_host:$free_port  (local context written to $env_file)"

# --- serve -------------------------------------------------------------------
export HOST="${HOST:-0.0.0.0}"
export PORT="$free_port"
# SLP_DIR defaults to ./replays; keep it in the worktree root
export SLP_DIR="${SLP_DIR:-$root/replays}"
export WEB_DIR="${WEB_DIR:-$root/web}"

# run in background, tracking PID so the next call can reclaim the port
"$root/bin/viewer" &
pid=$!
echo "$pid" > "$root/.devserver.pid"
log "viewer pid $pid on port $free_port (log: $root/.devserver.log)"
wait "$pid"
