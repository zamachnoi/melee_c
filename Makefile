CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g
CPPFLAGS ?= -Isrc

BIN := bin
BUILD := build

all: $(BIN)/melee $(BIN)/test_rollback $(BIN)/viewer $(BIN)/test_asset $(BIN)/test_render $(BIN)/test_pose $(BIN)/extract_tool

$(BIN)/melee: $(BUILD)/main.o $(BUILD)/parser.o
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/test_rollback: $(BUILD)/test_rollback.o $(BUILD)/parser.o
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

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
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm

$(BIN)/viewer: src/viewer.c src/parser.c src/asset.c src/render.c src/parser.h src/asset.h src/render.h
	@mkdir -p $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lpthread -lm -lz

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

test: $(BIN)/test_rollback $(BIN)/test_pose
	$(BIN)/test_rollback
	$(BIN)/test_pose

cache: $(BIN)/extract_tool
	@rm -rf cache && mkdir -p cache
	$(BIN)/extract_tool --iso=fixtures/game.iso --char=fox --out=cache
	$(BIN)/extract_tool --iso=fixtures/game.iso --char=falco --out=cache
	$(BIN)/extract_tool --iso=fixtures/game.iso --stage=FD --out=cache

test_asset: cache $(BIN)/test_asset
	$(BIN)/test_asset cache/fox-0.model cache/fox-0.anims cache/fd.stage
	$(BIN)/test_asset cache/falco-2.model cache/falco-0.anims cache/fd.stage

run: $(BIN)/melee
	$(BIN)/melee fixtures/vertical.slp

viewer: $(BIN)/viewer
	$(BIN)/viewer

dump: $(BIN)/datdump
	$(BIN)/datdump --iso=fixtures/game.iso $(DAT)

clean:
	rm -rf $(BIN) $(BUILD)

.PHONY: all test cache test_asset run viewer dump clean
