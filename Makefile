# Cosmo -- port nativo para macOS (Apple Silicon)
#
# Fase atual: camada de video. `make firstframe` valida leitura dos group
# files, EGA emulado e apresentacao.

CC      ?= cc
CFLAGS  ?= -std=gnu99 -O2 -Wall -Wextra -Iinclude
SDL_CFLAGS  := $(shell pkg-config --cflags sdl2)
SDL_LDFLAGS := $(shell pkg-config --libs sdl2)
LDLIBS  := $(SDL_LDFLAGS) -lz

BUILD   := build
DATADIR ?= gamedata

PLATFORM_SRC := src/platform/ega.c src/platform/video.c
PLATFORM_OBJ := $(PLATFORM_SRC:%.c=$(BUILD)/%.o)

.PHONY: all firstframe clean prep

all: $(BUILD)/firstframe

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

$(BUILD)/firstframe: $(PLATFORM_OBJ) $(BUILD)/tools/firstframe.o
	$(CC) $^ -o $@ $(LDLIBS)

# Gera as fontes tratadas do Cosmore em build/gen/
prep:
	./tools/prep.sh

firstframe: $(BUILD)/firstframe
	./$(BUILD)/firstframe $(DATADIR) $(ENTRY) $(PNG)

clean:
	rm -rf $(BUILD)
