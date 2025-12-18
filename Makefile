CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter

SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)

SRC := \
  src/main.c \
  src/nes.c \
  src/ines.c \
  src/cpu6502.c \
  src/ppu.c

OBJ := $(SRC:.c=.o)

all: nes

tools/mk_hello_rom: tools/mk_hello_rom.c
	$(CC) $(CFLAGS) -o $@ $<

hello-rom: tools/mk_hello_rom
	@mkdir -p roms
	./tools/mk_hello_rom roms/hello.nes

nes: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(SDL_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) nes tools/mk_hello_rom

.PHONY: all clean hello-rom
