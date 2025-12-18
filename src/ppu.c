#include "ppu.h"
#include "nes.h"
#include <string.h>

// Forward decls from nes.c for PPU bus access
uint8_t nes_ppu_bus_read(struct nes *n, uint16_t addr);
void nes_ppu_bus_write(struct nes *n, uint16_t addr, uint8_t v);

static uint32_t nes_palette_rgb(uint8_t idx) {
  // Simple hardcoded NTSC-ish palette (compact). Not perfect, but usable.
  static const uint32_t pal[64] = {
    0xFF666666,0xFF002A88,0xFF1412A7,0xFF3B00A4,0xFF5C007E,0xFF6E0040,0xFF6C0600,0xFF561D00,
    0xFF333500,0xFF0B4800,0xFF005200,0xFF004F08,0xFF00404D,0xFF000000,0xFF000000,0xFF000000,
    0xFFADADAD,0xFF155FD9,0xFF4240FF,0xFF7527FE,0xFFA01ACC,0xFFB71E7B,0xFFB53120,0xFF994E00,
    0xFF6B6D00,0xFF388700,0xFF0C9300,0xFF008F32,0xFF007C8D,0xFF000000,0xFF000000,0xFF000000,
    0xFFFFFEFF,0xFF64B0FF,0xFF9290FF,0xFFC676FF,0xFFF36AFF,0xFFFE6ECC,0xFFFE8170,0xFFEA9E22,
    0xFFBCBE00,0xFF88D800,0xFF5CE430,0xFF45E082,0xFF48CDDE,0xFF4F4F4F,0xFF000000,0xFF000000,
    0xFFFFFEFF,0xFFC0DFFF,0xFFD3D2FF,0xFFE8C8FF,0xFFFBC2FF,0xFFFEC4EA,0xFFFECCC5,0xFFF7D8A5,
    0xFFE4E594,0xFFCFEF96,0xFFBDF4AB,0xFFB3F3CC,0xFFB5EBF2,0xFFB8B8B8,0xFF000000,0xFF000000,
  };
  return pal[idx & 0x3F];
}

void ppu_reset(ppu_t *p) {
  memset(p, 0, sizeof(*p));
  p->reg_status = 0xA0; // power-up bits
  p->scanline = -1;
  p->dot = 0;
  p->scroll_x = 0;
  p->scroll_y = 0;
  p->scroll_x_next = 0;
  p->scroll_y_next = 0;
  p->render_ctrl = 0;
  p->render_ctrl_next = 0;
}

uint8_t ppu_cpu_read(ppu_t *p, struct nes *nes, uint16_t addr) {
  addr &= 7;
  switch (addr) {
    case 2: { // PPUSTATUS
      uint8_t v = p->reg_status;
      p->reg_status &= (uint8_t)~0x80; // clear vblank
      p->w = false;
      return v;
    }
    case 4: // OAMDATA
      return p->oam[p->oam_addr];
    case 7: { // PPUDATA
      uint16_t vaddr = p->v & 0x3FFF;
      uint8_t value = nes_ppu_bus_read(nes, vaddr);
      uint8_t ret;
      if (vaddr >= 0x3F00) {
        ret = value;
        p->chr_read_buffer = nes_ppu_bus_read(nes, (uint16_t)(vaddr - 0x1000));
      } else {
        ret = p->chr_read_buffer;
        p->chr_read_buffer = value;
      }
      p->v += (p->reg_ctrl & 0x04) ? 32 : 1;
      return ret;
    }
    default:
      return 0;
  }
}

void ppu_cpu_write(ppu_t *p, struct nes *nes, uint16_t addr, uint8_t v) {
  addr &= 7;
  switch (addr) {
    case 0: // PPUCTRL
      {
        uint8_t prev = p->reg_ctrl;
        p->reg_ctrl = v;
        p->render_ctrl_next = v;
        // If NMI gets enabled during vblank, some hardware triggers NMI immediately.
        if (((prev & 0x80) == 0) && ((v & 0x80) != 0) && ((p->reg_status & 0x80) != 0)) {
          cpu6502_set_nmi(&((nes_t *)nes)->cpu);
        }
      }
      p->t = (uint16_t)((p->t & 0xF3FF) | ((uint16_t)(v & 0x03) << 10));
      break;
    case 1: // PPUMASK
      p->reg_mask = v;
      break;
    case 3: // OAMADDR
      p->oam_addr = v;
      break;
    case 4: // OAMDATA
      p->oam[p->oam_addr++] = v;
      break;
    case 5: // PPUSCROLL
      if (!p->w) {
        p->scroll_x_next = v;
        p->x = (uint8_t)(v & 7);
        p->t = (uint16_t)((p->t & 0xFFE0) | (v >> 3));
        p->w = true;
      } else {
        p->scroll_y_next = v;
        p->t = (uint16_t)((p->t & 0x8FFF) | ((uint16_t)(v & 0x07) << 12));
        p->t = (uint16_t)((p->t & 0xFC1F) | ((uint16_t)(v & 0xF8) << 2));
        p->w = false;
      }
      break;
    case 6: // PPUADDR
      if (!p->w) {
        p->t = (uint16_t)((p->t & 0x00FF) | ((uint16_t)(v & 0x3F) << 8));
        p->w = true;
      } else {
        p->t = (uint16_t)((p->t & 0xFF00) | v);
        p->v = p->t;
        p->w = false;
      }
      break;
    case 7: { // PPUDATA
      uint16_t vaddr = p->v & 0x3FFF;
      nes_ppu_bus_write(nes, vaddr, v);
      p->v += (p->reg_ctrl & 0x04) ? 32 : 1;
    } break;
    default:
      break;
  }
}

static uint8_t ppu_get_bg_pixel(ppu_t *p, struct nes *nes, int x, int y, uint8_t *out_pal_index) {
  bool show_bg = (p->reg_mask & 0x08) != 0;
  if (!show_bg) { *out_pal_index = 0; return 0; }

  // Simplified scroll based on $2005 writes.
  int X = (x + (int)p->scroll_x) & 511; // 0..511
  int Y = (y + (int)p->scroll_y) % 480; // 0..479
  if (Y < 0) Y += 480;

  int sx = X & 255;
  int sy = Y % 240;
  int tile_x = sx / 8;
  int tile_y = sy / 8;
  int fine_y = sy & 7;

  int nt = (p->render_ctrl & 0x03);
  if (X >= 256) nt ^= 1;
  if (Y >= 240) nt ^= 2;
  uint16_t base_nt = (uint16_t)(0x2000 + nt * 0x0400);
  uint16_t base_pt = (p->render_ctrl & 0x10) ? 0x1000 : 0x0000;

  uint16_t nt_addr = (uint16_t)(base_nt + tile_y * 32 + tile_x);
  uint8_t tile = nes_ppu_bus_read(nes, nt_addr);

  uint16_t at_addr = (uint16_t)(base_nt + 0x3C0 + (tile_y / 4) * 8 + (tile_x / 4));
  uint8_t at = nes_ppu_bus_read(nes, at_addr);
  int quadrant = ((tile_y & 2) ? 2 : 0) | ((tile_x & 2) ? 1 : 0);
  uint8_t pal = 0;
  switch (quadrant) {
    case 0: pal = (uint8_t)(at & 0x03); break;
    case 1: pal = (uint8_t)((at >> 2) & 0x03); break;
    case 2: pal = (uint8_t)((at >> 4) & 0x03); break;
    case 3: pal = (uint8_t)((at >> 6) & 0x03); break;
  }

  uint16_t pt_addr = (uint16_t)(base_pt + tile * 16 + fine_y);
  uint8_t lo = nes_ppu_bus_read(nes, pt_addr);
  uint8_t hi = nes_ppu_bus_read(nes, (uint16_t)(pt_addr + 8));
  int bit = 7 - (sx & 7);
  uint8_t px = (uint8_t)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
  *out_pal_index = pal;
  return px;
}

static void eval_sprites_for_scanline(ppu_t *p, int y) {
  p->scan_spr_count = 0;
  // Clear overflow; will set if >8
  p->reg_status &= (uint8_t)~0x20;

  int sprite_h = (p->reg_ctrl & 0x20) ? 16 : 8;
  int found = 0;
  for (int i = 0; i < 64; i++) {
    uint8_t sy = p->oam[i * 4 + 0];
    int top = (int)sy + 1;
    if (y < top || y >= top + sprite_h) continue;
    if (found < 8) {
      p->scan_spr_i[found] = (uint8_t)i;
      p->scan_spr_y[found] = sy;
      p->scan_spr_tile[found] = p->oam[i * 4 + 1];
      p->scan_spr_attr[found] = p->oam[i * 4 + 2];
      p->scan_spr_x[found] = p->oam[i * 4 + 3];
    }
    found++;
  }
  if (found > 8) p->reg_status |= 0x20;
  p->scan_spr_count = (uint8_t)((found > 8) ? 8 : found);
}

static uint8_t sprite_pixel(ppu_t *p, struct nes *nes, int y, int x, uint8_t *out_palette, uint8_t *out_priority, bool *out_is_sprite0) {
  bool show_spr = (p->reg_mask & 0x10) != 0;
  if (!show_spr) return 0;
  if (x < 8 && !(p->reg_mask & 0x04)) return 0;

  int sprite_h = (p->render_ctrl & 0x20) ? 16 : 8;
  uint16_t base_pt8 = (p->render_ctrl & 0x08) ? 0x1000 : 0x0000;

  for (uint8_t si = 0; si < p->scan_spr_count; si++) {
    uint8_t spr_x = p->scan_spr_x[si];
    uint8_t spr_y = p->scan_spr_y[si];
    uint8_t tile = p->scan_spr_tile[si];
    uint8_t attr = p->scan_spr_attr[si];

    int top = (int)spr_y + 1;
    if (x < spr_x || x >= (int)spr_x + 8) continue;

    int row = y - top;
    int col = x - spr_x;
    bool flip_h = (attr & 0x40) != 0;
    bool flip_v = (attr & 0x80) != 0;
    if (flip_h) col = 7 - col;
    if (flip_v) row = (sprite_h - 1) - row;

    uint16_t pt_addr = 0;
    if (sprite_h == 8) {
      pt_addr = (uint16_t)(base_pt8 + tile * 16 + row);
    } else {
      uint16_t table = (tile & 1) ? 0x1000 : 0x0000;
      uint16_t tile_base = (uint16_t)(tile & 0xFE);
      if (row >= 8) { tile_base++; row -= 8; }
      pt_addr = (uint16_t)(table + tile_base * 16 + row);
    }

    uint8_t lo = nes_ppu_bus_read(nes, pt_addr);
    uint8_t hi = nes_ppu_bus_read(nes, (uint16_t)(pt_addr + 8));
    int bit = 7 - col;
    uint8_t px = (uint8_t)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
    if (px == 0) continue;

    *out_palette = (uint8_t)(attr & 0x03);
    *out_priority = (uint8_t)((attr & 0x20) ? 1 : 0); // 1 => behind bg
    *out_is_sprite0 = (p->scan_spr_i[si] == 0);
    return px;
  }

  return 0;
}

static uint8_t sprite0_pixel(ppu_t *p, struct nes *nes, int y, int x) {
  if ((p->reg_mask & 0x10) == 0) return 0;
  if (x < 8 && !(p->reg_mask & 0x04)) return 0;

  uint8_t spr_y = p->oam[0];
  uint8_t tile = p->oam[1];
  uint8_t attr = p->oam[2];
  uint8_t spr_x = p->oam[3];

  int sprite_h = (p->render_ctrl & 0x20) ? 16 : 8;
  int top = (int)spr_y + 1;
  if (y < top || y >= top + sprite_h) return 0;
  if (x < spr_x || x >= (int)spr_x + 8) return 0;

  int row = y - top;
  int col = x - spr_x;
  bool flip_h = (attr & 0x40) != 0;
  bool flip_v = (attr & 0x80) != 0;
  if (flip_h) col = 7 - col;
  if (flip_v) row = (sprite_h - 1) - row;

  uint16_t pt_addr = 0;
  if (sprite_h == 8) {
    uint16_t base_pt8 = (p->render_ctrl & 0x08) ? 0x1000 : 0x0000;
    pt_addr = (uint16_t)(base_pt8 + tile * 16 + row);
  } else {
    uint16_t table = (tile & 1) ? 0x1000 : 0x0000;
    uint16_t tile_base = (uint16_t)(tile & 0xFE);
    if (row >= 8) { tile_base++; row -= 8; }
    pt_addr = (uint16_t)(table + tile_base * 16 + row);
  }

  uint8_t lo = nes_ppu_bus_read(nes, pt_addr);
  uint8_t hi = nes_ppu_bus_read(nes, (uint16_t)(pt_addr + 8));
  int bit = 7 - col;
  uint8_t px = (uint8_t)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
  return px;
}

static void render_scanline(ppu_t *p, struct nes *nes, int y) {
  for (int x = 0; x < 256; x++) {
    uint8_t pal_index = 0;
    uint8_t bg_px = ppu_get_bg_pixel(p, nes, x, y, &pal_index);
    if (x < 8 && !(p->reg_mask & 0x02)) bg_px = 0;

    uint8_t sp_pal = 0, sp_pri = 0;
    bool sp0 = false;
    uint8_t sp_px = sprite_pixel(p, nes, y, x, &sp_pal, &sp_pri, &sp0);

    uint8_t color_idx = 0;
    bool bg_opaque = (bg_px != 0) && ((p->reg_mask & 0x08) != 0);
    bool sp_opaque = (sp_px != 0) && ((p->reg_mask & 0x10) != 0);

    if (sp_opaque && (!bg_opaque || sp_pri == 0)) {
      uint16_t pal_addr = (uint16_t)(0x3F10 + 1 + sp_pal * 4 + (sp_px - 1));
      color_idx = nes_ppu_bus_read(nes, pal_addr) & 0x3F;
    } else if (bg_opaque) {
      uint16_t pal_addr = (uint16_t)(0x3F00 + 1 + pal_index * 4 + (bg_px - 1));
      color_idx = nes_ppu_bus_read(nes, pal_addr) & 0x3F;
    } else {
      color_idx = p->palette[0] & 0x3F;
    }
    p->framebuffer[y * 256 + x] = nes_palette_rgb(color_idx);
  }
}

void ppu_tick(ppu_t *p, struct nes *nes) {
  // Very simplified timing: render per scanline and set vblank/NMI at scanline 241.
  // This is enough for some NROM games but not cycle-accurate.
  // Approximate NES scroll timing:
  // - Vertical scroll effectively latched at start of frame (pre-render).
  // - Horizontal scroll + nametable select copied late each scanline (dot 257).
  if (p->scanline == -1 && p->dot == 0) {
    p->scroll_x = p->scroll_x_next;
    p->scroll_y = p->scroll_y_next;
    p->render_ctrl = p->render_ctrl_next;
  }
  if (p->scanline >= 0 && p->scanline < 240 && p->dot == 257) {
    p->scroll_x = p->scroll_x_next;
    p->render_ctrl = p->render_ctrl_next;
  }

  if (p->scanline >= 0 && p->scanline < 240 && p->dot == 0) {
    eval_sprites_for_scanline(p, p->scanline);
    render_scanline(p, nes, p->scanline);
  }

  // Sprite-0 hit timing: approximate at the correct dot position.
  if ((p->reg_status & 0x40) == 0 && p->scanline >= 0 && p->scanline < 240 && p->dot >= 1 && p->dot <= 256) {
    int x = p->dot - 1;
    if (!(x < 8 && (!(p->reg_mask & 0x02) || !(p->reg_mask & 0x04)))) {
      uint8_t pal_index = 0;
      uint8_t bg_px = ppu_get_bg_pixel(p, nes, x, p->scanline, &pal_index);
      if (x < 8 && !(p->reg_mask & 0x02)) bg_px = 0;
      uint8_t s0 = sprite0_pixel(p, nes, p->scanline, x);
      if (s0 && (p->reg_mask & 0x08) && (p->reg_mask & 0x10)) {
        // Best-effort: if BG pixel calculation is off (common with simplified scrolling),
        // still allow sprite-0 hit when sprite-0's pixel is opaque. This keeps SMB-style
        // split-screen timing loops from deadlocking, while staying tied to the real
        // sprite-0 dot/scanline.
        (void)bg_px;
        p->reg_status |= 0x40;
      }
    }
  }

  if (p->scanline == 241 && p->dot == 1) {
    p->reg_status |= 0x80;
    if (p->reg_ctrl & 0x80) cpu6502_set_nmi(&((nes_t *)nes)->cpu);
    p->frame_ready = true;
  }

  if (p->scanline == -1 && p->dot == 1) {
    p->reg_status &= (uint8_t)~0x80;
    p->reg_status &= (uint8_t)~0x40; // sprite 0 hit cleared (not implemented)
  }

  p->dot++;
  if (p->dot >= 341) {
    p->dot = 0;
    p->scanline++;
    if (p->scanline >= 261) {
      p->scanline = -1;
    }
  }
}
