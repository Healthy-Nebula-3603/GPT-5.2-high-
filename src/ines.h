#pragma once
#include "common.h"

typedef enum {
  NES_MIRROR_HORIZONTAL = 0,
  NES_MIRROR_VERTICAL = 1,
  NES_MIRROR_FOURSCREEN = 2,
} nes_mirror_t;

typedef struct {
  uint8_t mapper;
  nes_mirror_t mirror;
  bool has_battery;
  bool has_trainer;
  bool is_nes2;
  uint32_t prg_rom_size;
  uint32_t chr_rom_size;
  uint32_t prg_ram_size;
} ines_info_t;

typedef struct {
  ines_info_t info;
  uint8_t *prg_rom;
  uint8_t *chr;      // CHR ROM or CHR RAM
  bool chr_is_ram;
} cart_t;

bool ines_load(cart_t *cart, const char *path, char *err, size_t err_cap);
void cart_free(cart_t *cart);

