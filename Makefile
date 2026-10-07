CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-format-truncation

SRC = src/overlayfs.c src/patchstore.c src/diff.c src/fuse.c
OBJ = $(SRC:src/%.c=build/%.o)
BIN = build/fusing-patches
STRESS = build/stress
DIFFTEST = build/difftest

all: $(BIN) $(STRESS)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) -lpthread

$(STRESS): stress/stress.c
	@mkdir -p build
	$(CC) -O2 -Wall -Wextra -Wno-format-truncation -pthread -o $@ stress/stress.c

$(DIFFTEST): tests/difftest.c src/diff.c src/diff.h
	@mkdir -p build
	$(CC) -O2 -Wall -Wextra -o $@ tests/difftest.c src/diff.c

test-diff: $(DIFFTEST)
	$(DIFFTEST)

# native C stress harness (multithreaded)
stress-test: $(STRESS) $(BIN)
	$(STRESS) $(BIN) --threads=2 --noop=200 --verify

stress-long: $(STRESS) $(BIN)
	$(STRESS) $(BIN) --duration=$(STRESS_DURATION) --threads=3 --verify

build/%.o: src/%.c src/patchstore.h src/fuse.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# static build: no runtime dynamic dependencies (glibc-static; the FUSE layer
# is our own, so no external libraries are needed at all)
static: $(SRC) src/patchstore.h src/fuse.h src/diff.h src/hash.h
	@mkdir -p build
	$(CC) -O2 -static $(SRC) -lpthread -o build/fusing-patches-static
	strip build/fusing-patches-static
	@echo ">>> built: build/fusing-patches-static"

clean:
	rm -rf build

.PHONY: all static clean stress-test stress-long test-diff
