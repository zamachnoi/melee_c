# Dev server & worktree ports

Each git worktree gets its own instance of the SLP viewer dev server on a
free port. Ports are allocated per-worktree so they never collide and a stale
process can't squat one.

## Where the port lives

The chosen port is written to `dev-server.env` in the **worktree root**:

    DEV_URL=http://localhost:8081

Read that file to find where this worktree's dev server is served. If it is
absent, the server isn't running yet — start it with:

    make devserver

(or `scripts/devserver.sh`). This builds `bin/viewer` if needed, binds the
next free port starting at 8080, serves on `0.0.0.0`, and rewrites
`dev-server.env`. The local URL remains suitable for agent checks, while the
same port is reachable through the machine hostname (for example,
`http://spire:8080`). The viewer serves the web UI from `web/` and reads
replays from `replays/` (SLP_DIR) and extracted DAT assets from `cache/`
(ASSET_DIR). Missing `replays/` and `cache/` dirs are symlinked to the shared
`fixtures` tree (`fixtures` and `fixtures/cache`) so worktrees do not copy
`.slp` files or re-extract the ISO. See `src/viewer.c` for env vars (`PORT`,
`HOST`, `WEB_DIR`, `SLP_DIR`, `ASSET_DIR`).

Stop this worktree's server with:

    make devserver-stop

which kills the PID in `.devserver.pid` and clears `dev-server.env`.

## Auto-start on worktree creation

`scripts/wt-hook.sh` is the git worktree hook that launches the dev server for
a newly created worktree. It only works on git >= 2.44. Enable it with:

    make worktree-hook

On older git, `git worktree add` does not fire it — run `make devserver`
inside the new worktree.

## Notes for agents

- Never hardcode 8081; always read `dev-server.env` in the current root.
- `dev-server.env` and `.devserver.*` are gitignored on purpose.
- Multiple worktrees can run simultaneously on different ports.
- `replays/` and `cache/` are gitignored; they should symlink at `fixtures`
  and `fixtures/cache` (shared on the machine). Do not copy the ISO cache
  into a worktree.
