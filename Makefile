CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g
CPPFLAGS ?= -Isrc

BIN := bin
BUILD := build

all: $(BIN)/melee $(BIN)/test_rollback $(BIN)/test_timeline $(BIN)/viewer $(BIN)/test_asset $(BIN)/test_render $(BIN)/test_pose $(BIN)/extract_tool

$(BIN)/melee: $(BUILD)/main.o $(BUILD)/parser.o
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/test_rollback: $(BUILD)/test_rollback.o $(BUILD)/parser.o
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/test_timeline: tests/test_timeline.c src/timeline.c src/sha256.c src/parser.c src/timeline.h src/protocol.h src/sha256.h src/parser.h
	@mkdir -p $(BIN) $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_timeline.c src/timeline.c src/sha256.c src/parser.c

$(BIN)/test_asset: tests/test_asset.c src/asset.c src/render.c
	@mkdir -p $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm

$(BIN)/test_render: tests/test_render.c src/asset.c src/render.c
	@mkdir -p $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm

$(BIN)/test_pose: tests/test_pose.c src/asset.c src/render.c
	@mkdir -p $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm

$(BIN)/extract_tool: tools/extract/extract.c src/asset.h
	@mkdir -p $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm -lz

$(BIN)/viewer: src/viewer.c src/parser.c src/asset.c src/render.c src/timeline.c src/sha256.c src/parser.h src/asset.h src/render.h src/timeline.h src/protocol.h src/sha256.h
	@mkdir -p $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/viewer.c src/parser.c src/asset.c src/render.c src/timeline.c src/sha256.c -lpthread -lm -lz

$(BUILD)/%.o: src/%.c src/parser.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/test_rollback.o: tests/test_rollback.c src/parser.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BIN)/datdump: tools/extract/datdump.c
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

datdump: $(BIN)/datdump

test: $(BIN)/test_rollback $(BIN)/test_pose $(BIN)/test_timeline
	$(BIN)/test_rollback
	$(BIN)/test_pose
	$(BIN)/test_timeline $(BUILD)/timeline-golden.bin
	npm test

typecheck:
	npm run typecheck

web-build:
	npm run build

ASSET_SCHEMA := $(shell sed -n 's/^#define ASSET_SCHEMA_VERSION //p' src/asset.h)
ASSET_CACHE := $(if $(wildcard cache/v$(ASSET_SCHEMA)/.),cache/v$(ASSET_SCHEMA),cache)
FIXTURE_CACHE := $(if $(wildcard fixtures/cache/v$(ASSET_SCHEMA)/.),fixtures/cache/v$(ASSET_SCHEMA),fixtures/cache)

measure-assets:
	npm run build
	node tools/measure-assets.mjs $(FIXTURE_CACHE)/falco-2.model $(FIXTURE_CACHE)/falco-0.anims $(FIXTURE_CACHE)/fox-0.model $(FIXTURE_CACHE)/fox-0.anims $(FIXTURE_CACHE)/fd.stage

golden-fixtures: web-build
	@. ./dev-server.env && DEV_URL="$$DEV_URL" node tools/capture-golden.mjs
	@. ./dev-server.env && DEV_URL="$$DEV_URL" node tools/capture-golden.mjs fixtures/ICs.slp build/golden/ics

test-http: web-build
	@. ./dev-server.env && DEV_URL="$$DEV_URL" node tests/http.integration.mjs

cache: $(BIN)/extract_tool
	scripts/ensure-shared-cache.sh
	$(BIN)/extract_tool --iso=fixtures/game.iso --all --out=cache

# Point every local worktree's cache/ at the shared fixtures/cache.
share-cache:
	@git worktree list --porcelain | awk '/^worktree /{print $$2}' | while read -r wt; do \
		scripts/ensure-shared-cache.sh "$$wt"; \
	done

test_asset: cache $(BIN)/test_asset
	$(BIN)/test_asset $(ASSET_CACHE)/fox-0.model $(ASSET_CACHE)/fox-0.anims $(ASSET_CACHE)/fd.stage
	$(BIN)/test_asset $(ASSET_CACHE)/falco-2.model $(ASSET_CACHE)/falco-0.anims $(ASSET_CACHE)/fd.stage

run: $(BIN)/melee
	$(BIN)/melee fixtures/vertical.slp

viewer: $(BIN)/viewer
	$(BIN)/viewer

# Start the per-worktree dev server on the next free port and record it in
# dev-server.env (gitignored) so agents can discover DEV_URL from repo context.
devserver: $(BIN)/viewer web-build
	scripts/devserver.sh

# Stop this worktree's dev server (reads .devserver.pid) and clear its context.
devserver-stop:
	@pid=$$(cat .devserver.pid 2>/dev/null || true); \
	if [ -n "$$pid" ] && kill -0 "$$pid" 2>/dev/null; then \
		kill "$$pid" && echo "stopped dev server (pid $$pid)"; \
	else \
		echo "no running dev server for this worktree"; \
	fi
	@rm -f .devserver.pid dev-server.env .devserver.log

# Wire the auto-start hook for worktree creation (needs git >= 2.44).
worktree-hook:
	git config worktree.guiHook "$$(git rev-parse --git-common-dir)/../scripts/wt-hook.sh"
	git config worktree.resetHook "$$(git rev-parse --git-common-dir)/../scripts/wt-hook.sh"
	@echo "worktree auto-start hook configured (git >= 2.44)."

dump: $(BIN)/datdump
	$(BIN)/datdump --iso=fixtures/game.iso $(DAT)

clean:
	rm -rf $(BIN) $(BUILD)

.PHONY: all test typecheck web-build measure-assets golden-fixtures test-http cache share-cache test_asset run viewer dump clean
