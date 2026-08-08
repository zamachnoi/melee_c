CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g

all: melee

melee: main.c parser.c parser.h
	$(CC) $(CFLAGS) -o $@ main.c parser.c

run: melee
	./melee fixtures/vertical.slp

clean:
	rm -f melee

.PHONY: all run clean
