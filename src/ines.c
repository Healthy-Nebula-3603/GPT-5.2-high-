#include "ines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t cap, const char *msg) {
  if (!err || cap == 0) return;
  snprintf(err, cap, "%s", msg);
}

void cart_free(cart_t *cart) {
  if (!cart) return;
  free(cart->prg_rom);
  free(cart->chr);
  memset(cart, 0, sizeof(*cart));
}

static bool read_exact(FILE *f, void *buf, size_t n) {
  return fread(buf, 1, n, f) == n;
}

bool ines_load(cart_t *cart, const char *path, char *err, size_t err_cap) {
  memset(cart, 0, sizeof(*cart));
  FILE *f = fopen(path, "rb");
  if (!f) {
    set_err(err, err_cap, "failed to open ROM");
    return false;
  }

  uint8_t h[16];
  if (!read_exact(f, h, sizeof(h))) {
    fclose(f);
    set_err(err, err_cap, "failed to read header");
    return false;
  }
  if (h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
    fclose(f);
    set_err(err, err_cap, "input is an ELF executable, not an iNES .nes ROM");
    return false;
  }
  if (!(h[0] == 'N' && h[1] == 'E' && h[2] == 'S' && h[3] == 0x1A)) {
    fclose(f);
    set_err(err, err_cap, "not an iNES ROM (missing NES\\x1A header)");
    return false;
  }

  uint8_t prg_chunks = h[4];
  uint8_t chr_chunks = h[5];
  uint8_t flags6 = h[6];
  uint8_t flags7 = h[7];
  uint8_t prg_ram_chunks = h[8];
  uint8_t flags9 = h[9];
  (void)flags9;

  bool nes2 = ((flags7 & 0x0C) == 0x08);
  if (nes2) {
    // Keep simple: treat as iNES1 for now.
  }

  cart->info.is_nes2 = nes2;
  cart->info.has_trainer = (flags6 & 0x04) != 0;
  cart->info.has_battery = (flags6 & 0x02) != 0;
  if (flags6 & 0x08) cart->info.mirror = NES_MIRROR_FOURSCREEN;
  else cart->info.mirror = (flags6 & 0x01) ? NES_MIRROR_VERTICAL : NES_MIRROR_HORIZONTAL;

  uint8_t mapper_lo = (flags6 >> 4);
  uint8_t mapper_hi = (flags7 >> 4);
  cart->info.mapper = (uint8_t)(mapper_lo | (mapper_hi << 4));

  cart->info.prg_rom_size = (uint32_t)prg_chunks * 16u * 1024u;
  cart->info.chr_rom_size = (uint32_t)chr_chunks * 8u * 1024u;
  cart->info.prg_ram_size = (prg_ram_chunks ? (uint32_t)prg_ram_chunks * 8u * 1024u : 8u * 1024u);

  if (cart->info.has_trainer) {
    uint8_t trainer[512];
    if (!read_exact(f, trainer, sizeof(trainer))) {
      fclose(f);
      set_err(err, err_cap, "failed to read trainer");
      return false;
    }
  }

  cart->prg_rom = (uint8_t *)malloc(cart->info.prg_rom_size ? cart->info.prg_rom_size : 1);
  if (!cart->prg_rom) {
    fclose(f);
    set_err(err, err_cap, "oom PRG");
    return false;
  }
  if (!read_exact(f, cart->prg_rom, cart->info.prg_rom_size)) {
    fclose(f);
    cart_free(cart);
    set_err(err, err_cap, "failed reading PRG ROM");
    return false;
  }

  if (cart->info.chr_rom_size == 0) {
    cart->chr_is_ram = true;
    cart->info.chr_rom_size = 8u * 1024u;
    cart->chr = (uint8_t *)calloc(1, cart->info.chr_rom_size);
  } else {
    cart->chr_is_ram = false;
    cart->chr = (uint8_t *)malloc(cart->info.chr_rom_size);
    if (cart->chr && !read_exact(f, cart->chr, cart->info.chr_rom_size)) {
      fclose(f);
      cart_free(cart);
      set_err(err, err_cap, "failed reading CHR");
      return false;
    }
  }
  if (!cart->chr) {
    fclose(f);
    cart_free(cart);
    set_err(err, err_cap, "oom CHR");
    return false;
  }

  fclose(f);
  return true;
}
