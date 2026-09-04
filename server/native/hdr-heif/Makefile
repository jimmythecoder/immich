# Immich HDR HEIF -> AVIF helper
#
# Requires libheif (>= 1.17) and libavif (>= 0.11), located via pkg-config.

CC      ?= cc
BUILD   ?= build
BIN      = $(BUILD)/immich-hdr-heif

PKGS     = libheif libavif
CFLAGS  += -std=c11 -O2 -Wall -Wextra -Wpedantic $(shell pkg-config --cflags $(PKGS))
LDFLAGS += $(shell pkg-config --libs $(PKGS)) -lm

SRC = src/main.c src/heif_source.c src/color.c src/avif_encode.c src/avif_inspect.c
OBJ = $(SRC:src/%.c=$(BUILD)/%.o)

.PHONY: all clean check

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

$(BUILD)/%.o: src/%.c src/hdr_heif.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

# Fail early with a readable message when the dependencies are missing.
check:
	@pkg-config --exists $(PKGS) \
		&& echo "libheif $$(pkg-config --modversion libheif), libavif $$(pkg-config --modversion libavif)" \
		|| (echo "missing libheif/libavif development packages" >&2; exit 1)

clean:
	rm -rf $(BUILD)
