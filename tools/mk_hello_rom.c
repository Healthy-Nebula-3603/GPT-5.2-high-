#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PRG_SIZE = 16 * 1024, CHR_SIZE = 8 * 1024 };

typedef struct {
  size_t pos;
  int label;
} fixup_t;

enum { L_WAIT1, L_PAL_LOOP, L_ROW_LOOP, L_COL_LOOP, L_MAIN_LOOP, L_COUNT };

static void die(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  exit(1);
}

static void emit(uint8_t *prg, size_t *pc, uint8_t b) {
  if (*pc >= PRG_SIZE) die("program too large");
  prg[(*pc)++] = b;
}

static void emit16(uint8_t *prg, size_t *pc, uint16_t w) {
  emit(prg, pc, (uint8_t)(w & 0xFF));
  emit(prg, pc, (uint8_t)(w >> 8));
}

static void set_label(size_t labels[L_COUNT], int label, size_t pc) {
  labels[label] = pc;
}

static void emit_branch(uint8_t *prg, size_t *pc, fixup_t *fixups, size_t *fixup_n, uint8_t op, int label) {
  emit(prg, pc, op);
  if (*fixup_n >= 64) die("too many fixups");
  fixups[*fixup_n].pos = *pc;
  fixups[*fixup_n].label = label;
  (*fixup_n)++;
  emit(prg, pc, 0x00);
}

static void patch_branches(uint8_t *prg, size_t labels[L_COUNT], fixup_t *fixups, size_t fixup_n) {
  for (size_t i = 0; i < fixup_n; i++) {
    size_t off_pos = fixups[i].pos;
    size_t op_pos = off_pos - 1;
    size_t next = op_pos + 2;
    size_t target = labels[fixups[i].label];
    int rel = (int)target - (int)next;
    if (rel < -128 || rel > 127) die("branch out of range");
    prg[off_pos] = (uint8_t)(int8_t)rel;
  }
}

static void patch_abs16(uint8_t *prg, size_t pos, uint16_t addr) {
  prg[pos + 0] = (uint8_t)(addr & 0xFF);
  prg[pos + 1] = (uint8_t)(addr >> 8);
}

int main(int argc, char **argv) {
  const char *out_path = (argc >= 2) ? argv[1] : "roms/hello.nes";

  uint8_t header[16] = {0};
  header[0] = 'N'; header[1] = 'E'; header[2] = 'S'; header[3] = 0x1A;
  header[4] = 1; // 16KB PRG
  header[5] = 1; // 8KB CHR
  header[6] = 0; // mapper 0, horizontal mirroring

  uint8_t prg[PRG_SIZE];
  uint8_t chr[CHR_SIZE];
  memset(prg, 0x00, sizeof(prg));
  memset(chr, 0x00, sizeof(chr));

  // CHR tile 1: checkerboard in plane 0
  size_t t1 = 16 * 1;
  for (int row = 0; row < 8; row++) {
    chr[t1 + row] = (uint8_t)((row & 1) ? 0xAA : 0x55);
    chr[t1 + 8 + row] = 0x00;
  }

  // Palette (32 bytes)
  uint8_t pal[32] = {
    0x0F, 0x30, 0x21, 0x16,
    0x0F, 0x06, 0x16, 0x26,
    0x0F, 0x09, 0x19, 0x29,
    0x0F, 0x0C, 0x1C, 0x2C,
    0x0F, 0x11, 0x21, 0x31,
    0x0F, 0x15, 0x25, 0x35,
    0x0F, 0x18, 0x28, 0x38,
    0x0F, 0x1B, 0x2B, 0x3B,
  };

  size_t labels[L_COUNT];
  memset(labels, 0, sizeof(labels));
  fixup_t fixups[64];
  size_t fixup_n = 0;

  const uint16_t base = 0x8000;
  size_t pc = 0;

  // Reset routine at $8000
  emit(prg, &pc, 0x78);             // SEI
  emit(prg, &pc, 0xD8);             // CLD
  emit(prg, &pc, 0xA2); emit(prg, &pc, 0x40);         // LDX #$40
  emit(prg, &pc, 0x8E); emit16(prg, &pc, 0x4017);     // STX $4017
  emit(prg, &pc, 0xA2); emit(prg, &pc, 0xFF);         // LDX #$FF
  emit(prg, &pc, 0x9A);                                 // TXS
  emit(prg, &pc, 0xE8);                                 // INX (X=0)
  emit(prg, &pc, 0x8E); emit16(prg, &pc, 0x2000);     // STX $2000
  emit(prg, &pc, 0x8E); emit16(prg, &pc, 0x2001);     // STX $2001
  emit(prg, &pc, 0x8E); emit16(prg, &pc, 0x4010);     // STX $4010

  set_label(labels, L_WAIT1, pc);
  emit(prg, &pc, 0x2C); emit16(prg, &pc, 0x2002);     // BIT $2002
  emit_branch(prg, &pc, fixups, &fixup_n, 0x10, L_WAIT1); // BPL wait

  // PPUADDR $3F00
  emit(prg, &pc, 0xA9); emit(prg, &pc, 0x3F);         // LDA #$3F
  emit(prg, &pc, 0x8D); emit16(prg, &pc, 0x2006);     // STA $2006
  emit(prg, &pc, 0xA9); emit(prg, &pc, 0x00);         // LDA #$00
  emit(prg, &pc, 0x8D); emit16(prg, &pc, 0x2006);     // STA $2006

  emit(prg, &pc, 0xA2); emit(prg, &pc, 0x00);         // LDX #$00
  set_label(labels, L_PAL_LOOP, pc);
  emit(prg, &pc, 0xBD);                                // LDA abs,X
  size_t pal_addr_patch = pc;                           // patch lo/hi
  emit16(prg, &pc, 0x0000);
  emit(prg, &pc, 0x8D); emit16(prg, &pc, 0x2007);     // STA $2007
  emit(prg, &pc, 0xE8);                                 // INX
  emit(prg, &pc, 0xE0); emit(prg, &pc, 0x20);         // CPX #$20
  emit_branch(prg, &pc, fixups, &fixup_n, 0xD0, L_PAL_LOOP); // BNE

  // PPUADDR $2000
  emit(prg, &pc, 0xA9); emit(prg, &pc, 0x20);         // LDA #$20
  emit(prg, &pc, 0x8D); emit16(prg, &pc, 0x2006);     // STA $2006
  emit(prg, &pc, 0xA9); emit(prg, &pc, 0x00);         // LDA #$00
  emit(prg, &pc, 0x8D); emit16(prg, &pc, 0x2006);     // STA $2006

  emit(prg, &pc, 0xA0); emit(prg, &pc, 0x1E);         // LDY #30
  set_label(labels, L_ROW_LOOP, pc);
  emit(prg, &pc, 0xA2); emit(prg, &pc, 0x20);         // LDX #32
  set_label(labels, L_COL_LOOP, pc);
  emit(prg, &pc, 0xA9); emit(prg, &pc, 0x01);         // LDA #tile1
  emit(prg, &pc, 0x8D); emit16(prg, &pc, 0x2007);     // STA $2007
  emit(prg, &pc, 0xCA);                                 // DEX
  emit_branch(prg, &pc, fixups, &fixup_n, 0xD0, L_COL_LOOP); // BNE
  emit(prg, &pc, 0x88);                                 // DEY
  emit_branch(prg, &pc, fixups, &fixup_n, 0xD0, L_ROW_LOOP); // BNE

  // Scroll 0,0
  emit(prg, &pc, 0xA9); emit(prg, &pc, 0x00);         // LDA #0
  emit(prg, &pc, 0x8D); emit16(prg, &pc, 0x2005);     // STA $2005
  emit(prg, &pc, 0x8D); emit16(prg, &pc, 0x2005);     // STA $2005

  // Enable BG
  emit(prg, &pc, 0xA9); emit(prg, &pc, 0x00);         // LDA #0
  emit(prg, &pc, 0x8D); emit16(prg, &pc, 0x2000);     // STA $2000
  emit(prg, &pc, 0xA9); emit(prg, &pc, 0x0A);         // LDA #show bg + left 8px
  emit(prg, &pc, 0x8D); emit16(prg, &pc, 0x2001);     // STA $2001

  set_label(labels, L_MAIN_LOOP, pc);
  emit(prg, &pc, 0x4C); emit16(prg, &pc, (uint16_t)(base + labels[L_MAIN_LOOP])); // JMP main

  // Place palette data after code (aligned)
  while (pc & 0x0F) emit(prg, &pc, 0xEA); // NOP padding
  uint16_t pal_addr = (uint16_t)(base + pc);
  for (int i = 0; i < 32; i++) emit(prg, &pc, pal[i]);
  patch_abs16(prg, pal_addr_patch, pal_addr);

  patch_branches(prg, labels, fixups, fixup_n);

  // Vectors at end of 16KB bank (mirrored at $FFFA in CPU space).
  prg[0x3FFA] = 0x00; prg[0x3FFB] = 0x00; // NMI
  prg[0x3FFC] = 0x00; prg[0x3FFD] = 0x80; // RESET -> $8000
  prg[0x3FFE] = 0x00; prg[0x3FFF] = 0x00; // IRQ/BRK

  FILE *f = fopen(out_path, "wb");
  if (!f) die("failed to open output file");
  if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) die("write header failed");
  if (fwrite(prg, 1, sizeof(prg), f) != sizeof(prg)) die("write prg failed");
  if (fwrite(chr, 1, sizeof(chr), f) != sizeof(chr)) die("write chr failed");
  fclose(f);

  fprintf(stderr, "wrote %s\n", out_path);
  return 0;
}
