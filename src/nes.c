#include "nes.h"
#include <stdio.h>
#include <string.h>

static uint16_t mirror_nametable_addr(nes_t *n, uint16_t ppu_addr) {
  // ppu_addr in 0x2000..0x2FFF
  uint16_t nt = (ppu_addr - 0x2000) & 0x0FFF;
  uint16_t table = nt / 0x0400; // 0..3
  uint16_t offset = nt & 0x03FF;

  nes_mirror_t m = n->cart.info.mirror;
  if (m == NES_MIRROR_FOURSCREEN) {
    // We don't have 4-screen VRAM; map as-is into 2KB (best effort).
    return (uint16_t)(nt & 0x07FF);
  }

  // 2KB VRAM: map 4 name tables into two.
  // Horizontal: [A A B B]
  // Vertical:   [A B A B]
  uint16_t vram_table = 0;
  if (m == NES_MIRROR_HORIZONTAL) {
    vram_table = (table < 2) ? 0 : 1;
  } else {
    vram_table = (table == 0 || table == 2) ? 0 : 1;
  }
  return (uint16_t)(vram_table * 0x0400 + offset);
}

static uint8_t ppu_bus_read(nes_t *n, uint16_t addr) {
  addr &= 0x3FFF;
  if (addr < 0x2000) {
    // CHR
    return n->cart.chr[addr % n->cart.info.chr_rom_size];
  }
  if (addr < 0x3F00) {
    uint16_t vram_addr = mirror_nametable_addr(n, addr);
    return n->ppu.vram[vram_addr & 0x07FF];
  }
  // palette
  uint16_t pal = addr & 0x1F;
  // mirrors
  if (pal == 0x10) pal = 0x00;
  if (pal == 0x14) pal = 0x04;
  if (pal == 0x18) pal = 0x08;
  if (pal == 0x1C) pal = 0x0C;
  return n->ppu.palette[pal];
}

static void ppu_bus_write(nes_t *n, uint16_t addr, uint8_t v) {
  addr &= 0x3FFF;
  if (addr < 0x2000) {
    if (n->cart.chr_is_ram) n->cart.chr[addr % n->cart.info.chr_rom_size] = v;
    return;
  }
  if (addr < 0x3F00) {
    uint16_t vram_addr = mirror_nametable_addr(n, addr);
    n->ppu.vram[vram_addr & 0x07FF] = v;
    return;
  }
  uint16_t pal = addr & 0x1F;
  if (pal == 0x10) pal = 0x00;
  if (pal == 0x14) pal = 0x04;
  if (pal == 0x18) pal = 0x08;
  if (pal == 0x1C) pal = 0x0C;
  n->ppu.palette[pal] = (uint8_t)(v & 0x3F);
}

bool nes_load(nes_t *n, const char *rom_path, char *err, size_t err_cap) {
  memset(n, 0, sizeof(*n));
  if (!ines_load(&n->cart, rom_path, err, err_cap)) return false;
  if (n->cart.info.mapper != 0) {
    // Mapper 0 only in this first version.
    cart_free(&n->cart);
    if (err && err_cap) {
      snprintf(err, err_cap, "unsupported mapper %u (this build supports mapper 0 only)", n->cart.info.mapper);
    }
    return false;
  }
  nes_reset(n);
  return true;
}

void nes_free(nes_t *n) {
  if (!n) return;
  cart_free(&n->cart);
}

void nes_reset(nes_t *n) {
  memset(n->ram, 0, sizeof(n->ram));
  ppu_reset(&n->ppu);
  n->pad1_state = 0;
  n->pad1_shift = 0;
  n->pad_strobe = false;
  n->last_bus = 0;
  n->cpu_stall = 0;
  n->dbg_nmi_count = 0;
  cpu6502_reset(&n->cpu, (struct nes *)n);
}

static uint8_t cart_cpu_read(nes_t *n, uint16_t addr) {
  // Mapper 0 (NROM): $8000-$FFFF maps to PRG ROM (16KB or 32KB)
  if (addr < 0x8000) return 0;
  uint32_t prg_size = n->cart.info.prg_rom_size;
  uint32_t offset = (uint32_t)(addr - 0x8000);
  if (prg_size == 16u * 1024u) offset %= (16u * 1024u);
  return n->cart.prg_rom[offset % prg_size];
}

static void cart_cpu_write(nes_t *n, uint16_t addr, uint8_t v) {
  // NROM ignores writes
  (void)n; (void)addr; (void)v;
}

uint8_t nes_cpu_read(nes_t *n, uint16_t addr) {
  uint8_t v = 0;
  if (addr < 0x2000) {
    v = n->ram[addr & 0x07FF];
  } else if (addr < 0x4000) {
    v = ppu_cpu_read(&n->ppu, (struct nes *)n, (uint16_t)(0x2000 | (addr & 7)));
  } else if (addr == 0x4016) {
    // controller 1
    if (n->pad_strobe) {
      v = (uint8_t)(0x40 | (n->pad1_state & 1));
    } else {
      v = (uint8_t)(0x40 | (n->pad1_shift & 1));
      // Shift in 1s (after 8 reads, controller returns 1s on hardware).
      n->pad1_shift = (uint8_t)((n->pad1_shift >> 1) | 0x80);
    }
  } else if (addr == 0x4017) {
    v = 0x40;
  } else if (addr >= 0x8000) {
    v = cart_cpu_read(n, addr);
  } else {
    // APU / IO not implemented
    v = n->last_bus;
  }
  n->last_bus = v;
  return v;
}

void nes_cpu_write(nes_t *n, uint16_t addr, uint8_t v) {
  n->last_bus = v;
  if (addr < 0x2000) {
    n->ram[addr & 0x07FF] = v;
  } else if (addr < 0x4000) {
    ppu_cpu_write(&n->ppu, (struct nes *)n, (uint16_t)(0x2000 | (addr & 7)), v);
  } else if (addr == 0x4014) {
    // OAMDMA: copy 256 bytes from CPU page to OAM
    uint16_t base = (uint16_t)v << 8;
    for (int i = 0; i < 256; i++) {
      n->ppu.oam[(uint8_t)(n->ppu.oam_addr + i)] = nes_cpu_read(n, (uint16_t)(base + (uint16_t)i));
    }
    // CPU is stalled; PPU continues to run during this time.
    // Real hardware: 513 or 514 cycles depending on alignment.
    n->cpu_stall += 513 + (int)(n->cpu.cycles & 1);
  } else if (addr == 0x4016) {
    bool strobe = (v & 1) != 0;
    bool prev = n->pad_strobe;
    n->pad_strobe = strobe;
    // Latch on 1, and also on falling edge 1->0 (common pattern: write 1 then 0).
    if (strobe || (prev && !strobe)) n->pad1_shift = n->pad1_state;
  } else if (addr >= 0x8000) {
    cart_cpu_write(n, addr, v);
  } else {
    // APU not implemented
  }
}

bool nes_run_frame(nes_t *n, int max_cpu_steps) {
  n->ppu.frame_ready = false;
  for (int i = 0; i < max_cpu_steps; i++) {
    int cpu_cycles = cpu6502_step(&n->cpu, (struct nes *)n);
    for (int c = 0; c < cpu_cycles * 3; c++) {
      ppu_tick(&n->ppu, (struct nes *)n);
    }
    if (n->ppu.frame_ready) return true;
  }
  return false;
}

// Exposed for PPU implementation:
uint8_t nes_ppu_bus_read(struct nes *nn, uint16_t addr) { return ppu_bus_read((nes_t *)nn, addr); }
void nes_ppu_bus_write(struct nes *nn, uint16_t addr, uint8_t v) { ppu_bus_write((nes_t *)nn, addr, v); }
