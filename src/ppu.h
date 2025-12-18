#pragma once
#include "common.h"

struct nes;

typedef struct ppu {
  uint8_t reg_ctrl;
  uint8_t reg_mask;
  uint8_t reg_status;
  uint8_t oam_addr;

  uint8_t oam[256];

  uint8_t vram[2048];     // nametables (2KB)
  uint8_t palette[32];    // palette RAM
  uint8_t chr_read_buffer;

  uint16_t v; // current VRAM address
  uint16_t t; // temporary VRAM address
  uint8_t x;  // fine X
  bool w;     // write toggle

  // Simplified scroll latches (from $2005 writes)
  uint8_t scroll_x;       // active for current scanline
  uint8_t scroll_y;       // active for current scanline
  uint8_t scroll_x_next;  // last written
  uint8_t scroll_y_next;  // last written

  // Rendering control latch (nametable/pattern/sprite size). NMI bit still uses reg_ctrl.
  uint8_t render_ctrl;      // active for current scanline
  uint8_t render_ctrl_next; // last written

  int scanline; // -1..260
  int dot;      // 0..340
  bool frame_ready;

  uint32_t framebuffer[256 * 240]; // RGBA8888

  // cached sprite eval for current scanline (simplified)
  uint8_t scan_spr_count;
  uint8_t scan_spr_i[8];
  uint8_t scan_spr_y[8];
  uint8_t scan_spr_tile[8];
  uint8_t scan_spr_attr[8];
  uint8_t scan_spr_x[8];
} ppu_t;

void ppu_reset(ppu_t *p);
uint8_t ppu_cpu_read(ppu_t *p, struct nes *nes, uint16_t addr);
void ppu_cpu_write(ppu_t *p, struct nes *nes, uint16_t addr, uint8_t v);
void ppu_tick(ppu_t *p, struct nes *nes); // 1 PPU cycle
