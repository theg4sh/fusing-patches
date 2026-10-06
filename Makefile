CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -DFUSE_USE_VERSION=31 -Wno-format-truncation
PKG_CFLAGS = $(shell pkg-config --cflags fuse3)
PKG_LIBS   = $(shell pkg-config --libs fuse3)

SRC = src/overlayfs.c src/patchstore.c
OBJ = $(SRC:src/%.c=build/%.o)
BIN = build/overlayfs
STRESS = build/stress

all: $(BIN) $(STRESS)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(PKG_LIBS)

$(STRESS): stress/stress.c
	@mkdir -p build
	$(CC) -O2 -Wall -Wextra -Wno-format-truncation -pthread -o $@ stress/stress.c

# native C stress harness (multithreaded); replaces the old shell stress scripts
stress-test: $(STRESS) $(BIN)
	$(STRESS) $(BIN) --threads=2 --noop=200 --verify

stress-long: $(STRESS) $(BIN)
	$(STRESS) $(BIN) --duration=$(STRESS_DURATION) --threads=3 --verify

build/%.o: src/%.c src/patchstore.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

# ---------------------------------------------------------------------------
# static build: no runtime dynamic dependencies (fully static libc + libfuse)
# ---------------------------------------------------------------------------
FUSE_VER  ?= 3.18.3
FUSE_ROOT  = build/fuse-$(FUSE_VER)
FUSE_LIB   = $(FUSE_ROOT)/build/lib/libfuse3.a
FUSE_CFLAGS = -I$(FUSE_ROOT)/include -I$(FUSE_ROOT)/build

$(FUSE_LIB):
	@echo ">>> building static libfuse $(FUSE_VER) (requires network + meson/ninja)"
	rm -rf $(FUSE_ROOT) && mkdir -p $(FUSE_ROOT)
	curl -sL https://github.com/libfuse/libfuse/releases/download/fuse-$(FUSE_VER)/fuse-$(FUSE_VER).tar.gz \
		| tar xz -C $(FUSE_ROOT) --strip-components=1
	cd $(FUSE_ROOT) && meson setup build -Ddefault_library=static \
		-Dutils=false -Dexamples=false -Duseroot=false -Denable-io-uring=false
	ninja -C $(FUSE_ROOT)/build

static: $(FUSE_LIB) $(SRC) src/patchstore.h
	@mkdir -p build
	$(CC) -O2 -DFUSE_USE_VERSION=31 $(FUSE_CFLAGS) -c src/overlayfs.c -o build/overlayfs_s.o
	$(CC) -O2 -DFUSE_USE_VERSION=31 $(FUSE_CFLAGS) -c src/patchstore.c -o build/patchstore_s.o
	$(CC) -static build/overlayfs_s.o build/patchstore_s.o $(FUSE_LIB) -lpthread -lrt -ldl -o build/overlayfs-static
	strip build/overlayfs-static
	@echo ">>> built: build/overlayfs-static"

clean:
	rm -rf build

.PHONY: all static clean stress-test stress-long
