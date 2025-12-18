#pragma once
#include "common.h"
#include "ines.h"
#include "cpu6502.h"
#include "ppu.h"

typedef struct nes {
  cart_t cart;
  cpu6502_t cpu;
  ppu_t ppu;

  uint8_t ram[2048];

  // CPU stalls (e.g., OAMDMA) in CPU cycles
  int cpu_stall;

  // controller
  uint8_t pad1_state;
  uint8_t pad1_shift;
  bool pad_strobe;

  // APU/IO open bus-ish
  uint8_t last_bus;

  // Debug counters
  uint64_t dbg_nmi_count;
} nes_t;

bool nes_load(nes_t *n, const char *rom_path, char *err, size_t err_cap);
void nes_reset(nes_t *n);
void nes_free(nes_t *n);

uint8_t nes_cpu_read(nes_t *n, uint16_t addr);
void nes_cpu_write(nes_t *n, uint16_t addr, uint8_t v);

// Internal: PPU bus callbacks (used by ppu.c)
uint8_t nes_ppu_bus_read(struct nes *n, uint16_t addr);
void nes_ppu_bus_write(struct nes *n, uint16_t addr, uint8_t v);

// runs until a frame is ready; returns true on frame
bool nes_run_frame(nes_t *n, int max_cpu_steps);
