CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g
CPPFLAGS ?= -Isrc

BIN := bin
BUILD := build

all: $(BIN)/melee $(BIN)/test_rollback $(BIN)/viewer

$(BIN)/melee: $(BUILD)/main.o $(BUILD)/parser.o
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/test_rollback: $(BUILD)/test_rollback.o $(BUILD)/parser.o
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/viewer: $(BUILD)/viewer.o $(BUILD)/parser.o
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread -lm -lz

$(BUILD)/%.o: src/%.c src/parser.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/test_rollback.o: tests/test_rollback.c src/parser.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: $(BIN)/test_rollback
	$(BIN)/test_rollback

run: $(BIN)/melee
	$(BIN)/melee fixtures/vertical.slp

viewer: $(BIN)/viewer
	$(BIN)/viewer

clean:
	rm -rf $(BIN) $(BUILD)

.PHONY: all test run viewer clean
