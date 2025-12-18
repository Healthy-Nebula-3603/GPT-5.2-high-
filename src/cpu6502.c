#include "cpu6502.h"
#include "nes.h"
#include <string.h>

enum {
  P_C = 1 << 0,
  P_Z = 1 << 1,
  P_I = 1 << 2,
  P_D = 1 << 3,
  P_B = 1 << 4,
  P_U = 1 << 5,
  P_V = 1 << 6,
  P_N = 1 << 7,
};

static uint8_t rd(nes_t *n, uint16_t a) { return nes_cpu_read(n, a); }
static void wr(nes_t *n, uint16_t a, uint8_t v) { nes_cpu_write(n, a, v); }

static void set_nz(cpu6502_t *c, uint8_t v) {
  if (v == 0) c->p |= P_Z; else c->p &= (uint8_t)~P_Z;
  if (v & 0x80) c->p |= P_N; else c->p &= (uint8_t)~P_N;
}

static void push(cpu6502_t *c, nes_t *n, uint8_t v) {
  wr(n, (uint16_t)(0x0100 | c->sp), v);
  c->sp--;
}

static uint8_t pull(cpu6502_t *c, nes_t *n) {
  c->sp++;
  return rd(n, (uint16_t)(0x0100 | c->sp));
}

static uint16_t rd16(nes_t *n, uint16_t a) {
  uint8_t lo = rd(n, a);
  uint8_t hi = rd(n, (uint16_t)(a + 1));
  return (uint16_t)(lo | ((uint16_t)hi << 8));
}

static uint16_t rd16_wrap_bug(nes_t *n, uint16_t a) {
  // JMP ($xxFF) reads high byte from $xx00 (6502 bug).
  uint8_t lo = rd(n, a);
  uint8_t hi = rd(n, (uint16_t)((a & 0xFF00) | ((a + 1) & 0x00FF)));
  return (uint16_t)(lo | ((uint16_t)hi << 8));
}

void cpu6502_set_nmi(cpu6502_t *c) { c->nmi_pending = true; }
void cpu6502_set_irq(cpu6502_t *c, bool level) { c->irq_pending = level; }

void cpu6502_reset(cpu6502_t *c, struct nes *nes) {
  memset(c, 0, sizeof(*c));
  c->sp = 0xFD;
  c->p = (uint8_t)(P_I | P_U);
  c->pc = rd16((nes_t *)nes, 0xFFFC);
  c->cycles = 7;
}

static int do_interrupt(cpu6502_t *c, nes_t *n, uint16_t vector, bool is_brk) {
  push(c, n, (uint8_t)(c->pc >> 8));
  push(c, n, (uint8_t)(c->pc & 0xFF));
  uint8_t p = c->p;
  p |= P_U;
  if (is_brk) p |= P_B; else p &= (uint8_t)~P_B;
  push(c, n, p);
  c->p |= P_I;
  c->pc = rd16(n, vector);
  return 7;
}

typedef struct { uint16_t addr; int page_cross; } ea_t;

static ea_t ea_imm(cpu6502_t *c) { ea_t r = { c->pc++, 0 }; return r; }
static ea_t ea_zp(cpu6502_t *c, nes_t *n) { ea_t r = { rd(n, c->pc++), 0 }; return r; }
static ea_t ea_zpx(cpu6502_t *c, nes_t *n) { ea_t r = { (uint8_t)(rd(n, c->pc++) + c->x), 0 }; return r; }
static ea_t ea_zpy(cpu6502_t *c, nes_t *n) { ea_t r = { (uint8_t)(rd(n, c->pc++) + c->y), 0 }; return r; }
static ea_t ea_abs(cpu6502_t *c, nes_t *n) { ea_t r = { rd16(n, c->pc), 0 }; c->pc += 2; return r; }
static ea_t ea_absx(cpu6502_t *c, nes_t *n, bool add_cycle) {
  uint16_t base = rd16(n, c->pc); c->pc += 2;
  uint16_t a = (uint16_t)(base + c->x);
  ea_t r = { a, (add_cycle && ((base & 0xFF00) != (a & 0xFF00))) ? 1 : 0 };
  return r;
}
static ea_t ea_absy(cpu6502_t *c, nes_t *n, bool add_cycle) {
  uint16_t base = rd16(n, c->pc); c->pc += 2;
  uint16_t a = (uint16_t)(base + c->y);
  ea_t r = { a, (add_cycle && ((base & 0xFF00) != (a & 0xFF00))) ? 1 : 0 };
  return r;
}
static ea_t ea_indx(cpu6502_t *c, nes_t *n) {
  uint8_t zp = (uint8_t)(rd(n, c->pc++) + c->x);
  uint16_t a = (uint16_t)(rd(n, zp) | ((uint16_t)rd(n, (uint8_t)(zp + 1)) << 8));
  ea_t r = { a, 0 };
  return r;
}
static ea_t ea_indy(cpu6502_t *c, nes_t *n, bool add_cycle) {
  uint8_t zp = rd(n, c->pc++);
  uint16_t base = (uint16_t)(rd(n, zp) | ((uint16_t)rd(n, (uint8_t)(zp + 1)) << 8));
  uint16_t a = (uint16_t)(base + c->y);
  ea_t r = { a, (add_cycle && ((base & 0xFF00) != (a & 0xFF00))) ? 1 : 0 };
  return r;
}

static int branch(cpu6502_t *c, nes_t *n, bool cond) {
  int8_t rel = (int8_t)rd(n, c->pc++);
  if (!cond) return 2;
  uint16_t old = c->pc;
  c->pc = (uint16_t)(c->pc + rel);
  return 2 + 1 + (((old & 0xFF00) != (c->pc & 0xFF00)) ? 1 : 0);
}

static void op_adc(cpu6502_t *c, uint8_t m) {
  uint16_t sum = (uint16_t)c->a + (uint16_t)m + ((c->p & P_C) ? 1u : 0u);
  uint8_t res = (uint8_t)sum;
  if (sum > 0xFF) c->p |= P_C; else c->p &= (uint8_t)~P_C;
  if (((c->a ^ res) & (m ^ res) & 0x80) != 0) c->p |= P_V; else c->p &= (uint8_t)~P_V;
  c->a = res;
  set_nz(c, c->a);
}

static void op_sbc(cpu6502_t *c, uint8_t m) {
  op_adc(c, (uint8_t)~m);
}

static void op_cmp(cpu6502_t *c, uint8_t r, uint8_t m) {
  uint16_t diff = (uint16_t)r - (uint16_t)m;
  if (r >= m) c->p |= P_C; else c->p &= (uint8_t)~P_C;
  set_nz(c, (uint8_t)diff);
}

static uint8_t op_asl(cpu6502_t *c, uint8_t v) {
  if (v & 0x80) c->p |= P_C; else c->p &= (uint8_t)~P_C;
  v = (uint8_t)(v << 1);
  set_nz(c, v);
  return v;
}

static uint8_t op_lsr(cpu6502_t *c, uint8_t v) {
  if (v & 0x01) c->p |= P_C; else c->p &= (uint8_t)~P_C;
  v = (uint8_t)(v >> 1);
  set_nz(c, v);
  return v;
}

static uint8_t op_rol(cpu6502_t *c, uint8_t v) {
  uint8_t cin = (c->p & P_C) ? 1 : 0;
  if (v & 0x80) c->p |= P_C; else c->p &= (uint8_t)~P_C;
  v = (uint8_t)((v << 1) | cin);
  set_nz(c, v);
  return v;
}

static uint8_t op_ror(cpu6502_t *c, uint8_t v) {
  uint8_t cin = (c->p & P_C) ? 0x80 : 0x00;
  if (v & 0x01) c->p |= P_C; else c->p &= (uint8_t)~P_C;
  v = (uint8_t)((v >> 1) | cin);
  set_nz(c, v);
  return v;
}

static void op_anc(cpu6502_t *c, uint8_t imm) {
  c->a &= imm;
  set_nz(c, c->a);
  if (c->a & 0x80) c->p |= P_C; else c->p &= (uint8_t)~P_C;
}

static void op_alr(cpu6502_t *c, uint8_t imm) {
  c->a &= imm;
  c->a = op_lsr(c, c->a);
}

static void op_arr(cpu6502_t *c, uint8_t imm) {
  // Approximate (good enough for many ROMs).
  c->a &= imm;
  c->a = op_ror(c, c->a);
  // Set V/C in a common (not perfect) way based on bits 5/6.
  uint8_t b5 = (c->a >> 5) & 1;
  uint8_t b6 = (c->a >> 6) & 1;
  if (b6) c->p |= P_C; else c->p &= (uint8_t)~P_C;
  if ((b5 ^ b6) != 0) c->p |= P_V; else c->p &= (uint8_t)~P_V;
}

static void op_sbx(cpu6502_t *c, uint8_t imm) {
  uint8_t t = (uint8_t)(c->a & c->x);
  uint16_t diff = (uint16_t)t - (uint16_t)imm;
  c->x = (uint8_t)diff;
  if (t >= imm) c->p |= P_C; else c->p &= (uint8_t)~P_C;
  set_nz(c, c->x);
}

int cpu6502_step(cpu6502_t *c, struct nes *nes_) {
  nes_t *n = (nes_t *)nes_;

  // CPU stall cycles (e.g., OAM DMA): no instruction executed.
  if (n->cpu_stall > 0) {
    n->cpu_stall--;
    c->cycles += 1;
    return 1;
  }

  if (c->nmi_pending) {
    c->nmi_pending = false;
    n->dbg_nmi_count++;
    int cyc = do_interrupt(c, n, 0xFFFA, false);
    c->cycles += (uint64_t)cyc;
    return cyc;
  }
  if (c->irq_pending && !(c->p & P_I)) {
    int cyc = do_interrupt(c, n, 0xFFFE, false);
    c->cycles += (uint64_t)cyc;
    return cyc;
  }

  uint8_t op = rd(n, c->pc++);
  int cycles = 2;

  switch (op) {
    // ADC
    case 0x69: { ea_t e = ea_imm(c); op_adc(c, rd(n, e.addr)); cycles = 2; } break;
    case 0x65: { ea_t e = ea_zp(c, n); op_adc(c, rd(n, e.addr)); cycles = 3; } break;
    case 0x75: { ea_t e = ea_zpx(c, n); op_adc(c, rd(n, e.addr)); cycles = 4; } break;
    case 0x6D: { ea_t e = ea_abs(c, n); op_adc(c, rd(n, e.addr)); cycles = 4; } break;
    case 0x7D: { ea_t e = ea_absx(c, n, true); op_adc(c, rd(n, e.addr)); cycles = 4 + e.page_cross; } break;
    case 0x79: { ea_t e = ea_absy(c, n, true); op_adc(c, rd(n, e.addr)); cycles = 4 + e.page_cross; } break;
    case 0x61: { ea_t e = ea_indx(c, n); op_adc(c, rd(n, e.addr)); cycles = 6; } break;
    case 0x71: { ea_t e = ea_indy(c, n, true); op_adc(c, rd(n, e.addr)); cycles = 5 + e.page_cross; } break;

    // SBC
    case 0xE9: { ea_t e = ea_imm(c); op_sbc(c, rd(n, e.addr)); cycles = 2; } break;
    case 0xE5: { ea_t e = ea_zp(c, n); op_sbc(c, rd(n, e.addr)); cycles = 3; } break;
    case 0xF5: { ea_t e = ea_zpx(c, n); op_sbc(c, rd(n, e.addr)); cycles = 4; } break;
    case 0xED: { ea_t e = ea_abs(c, n); op_sbc(c, rd(n, e.addr)); cycles = 4; } break;
    case 0xFD: { ea_t e = ea_absx(c, n, true); op_sbc(c, rd(n, e.addr)); cycles = 4 + e.page_cross; } break;
    case 0xF9: { ea_t e = ea_absy(c, n, true); op_sbc(c, rd(n, e.addr)); cycles = 4 + e.page_cross; } break;
    case 0xE1: { ea_t e = ea_indx(c, n); op_sbc(c, rd(n, e.addr)); cycles = 6; } break;
    case 0xF1: { ea_t e = ea_indy(c, n, true); op_sbc(c, rd(n, e.addr)); cycles = 5 + e.page_cross; } break;

    // AND
    case 0x29: { ea_t e = ea_imm(c); c->a &= rd(n, e.addr); set_nz(c, c->a); cycles = 2; } break;
    case 0x25: { ea_t e = ea_zp(c, n); c->a &= rd(n, e.addr); set_nz(c, c->a); cycles = 3; } break;
    case 0x35: { ea_t e = ea_zpx(c, n); c->a &= rd(n, e.addr); set_nz(c, c->a); cycles = 4; } break;
    case 0x2D: { ea_t e = ea_abs(c, n); c->a &= rd(n, e.addr); set_nz(c, c->a); cycles = 4; } break;
    case 0x3D: { ea_t e = ea_absx(c, n, true); c->a &= rd(n, e.addr); set_nz(c, c->a); cycles = 4 + e.page_cross; } break;
    case 0x39: { ea_t e = ea_absy(c, n, true); c->a &= rd(n, e.addr); set_nz(c, c->a); cycles = 4 + e.page_cross; } break;
    case 0x21: { ea_t e = ea_indx(c, n); c->a &= rd(n, e.addr); set_nz(c, c->a); cycles = 6; } break;
    case 0x31: { ea_t e = ea_indy(c, n, true); c->a &= rd(n, e.addr); set_nz(c, c->a); cycles = 5 + e.page_cross; } break;

    // ORA
    case 0x09: { ea_t e = ea_imm(c); c->a |= rd(n, e.addr); set_nz(c, c->a); cycles = 2; } break;
    case 0x05: { ea_t e = ea_zp(c, n); c->a |= rd(n, e.addr); set_nz(c, c->a); cycles = 3; } break;
    case 0x15: { ea_t e = ea_zpx(c, n); c->a |= rd(n, e.addr); set_nz(c, c->a); cycles = 4; } break;
    case 0x0D: { ea_t e = ea_abs(c, n); c->a |= rd(n, e.addr); set_nz(c, c->a); cycles = 4; } break;
    case 0x1D: { ea_t e = ea_absx(c, n, true); c->a |= rd(n, e.addr); set_nz(c, c->a); cycles = 4 + e.page_cross; } break;
    case 0x19: { ea_t e = ea_absy(c, n, true); c->a |= rd(n, e.addr); set_nz(c, c->a); cycles = 4 + e.page_cross; } break;
    case 0x01: { ea_t e = ea_indx(c, n); c->a |= rd(n, e.addr); set_nz(c, c->a); cycles = 6; } break;
    case 0x11: { ea_t e = ea_indy(c, n, true); c->a |= rd(n, e.addr); set_nz(c, c->a); cycles = 5 + e.page_cross; } break;

    // EOR
    case 0x49: { ea_t e = ea_imm(c); c->a ^= rd(n, e.addr); set_nz(c, c->a); cycles = 2; } break;
    case 0x45: { ea_t e = ea_zp(c, n); c->a ^= rd(n, e.addr); set_nz(c, c->a); cycles = 3; } break;
    case 0x55: { ea_t e = ea_zpx(c, n); c->a ^= rd(n, e.addr); set_nz(c, c->a); cycles = 4; } break;
    case 0x4D: { ea_t e = ea_abs(c, n); c->a ^= rd(n, e.addr); set_nz(c, c->a); cycles = 4; } break;
    case 0x5D: { ea_t e = ea_absx(c, n, true); c->a ^= rd(n, e.addr); set_nz(c, c->a); cycles = 4 + e.page_cross; } break;
    case 0x59: { ea_t e = ea_absy(c, n, true); c->a ^= rd(n, e.addr); set_nz(c, c->a); cycles = 4 + e.page_cross; } break;
    case 0x41: { ea_t e = ea_indx(c, n); c->a ^= rd(n, e.addr); set_nz(c, c->a); cycles = 6; } break;
    case 0x51: { ea_t e = ea_indy(c, n, true); c->a ^= rd(n, e.addr); set_nz(c, c->a); cycles = 5 + e.page_cross; } break;

    // LDA
    case 0xA9: { ea_t e = ea_imm(c); c->a = rd(n, e.addr); set_nz(c, c->a); cycles = 2; } break;
    case 0xA5: { ea_t e = ea_zp(c, n); c->a = rd(n, e.addr); set_nz(c, c->a); cycles = 3; } break;
    case 0xB5: { ea_t e = ea_zpx(c, n); c->a = rd(n, e.addr); set_nz(c, c->a); cycles = 4; } break;
    case 0xAD: { ea_t e = ea_abs(c, n); c->a = rd(n, e.addr); set_nz(c, c->a); cycles = 4; } break;
    case 0xBD: { ea_t e = ea_absx(c, n, true); c->a = rd(n, e.addr); set_nz(c, c->a); cycles = 4 + e.page_cross; } break;
    case 0xB9: { ea_t e = ea_absy(c, n, true); c->a = rd(n, e.addr); set_nz(c, c->a); cycles = 4 + e.page_cross; } break;
    case 0xA1: { ea_t e = ea_indx(c, n); c->a = rd(n, e.addr); set_nz(c, c->a); cycles = 6; } break;
    case 0xB1: { ea_t e = ea_indy(c, n, true); c->a = rd(n, e.addr); set_nz(c, c->a); cycles = 5 + e.page_cross; } break;

    // LDX
    case 0xA2: { ea_t e = ea_imm(c); c->x = rd(n, e.addr); set_nz(c, c->x); cycles = 2; } break;
    case 0xA6: { ea_t e = ea_zp(c, n); c->x = rd(n, e.addr); set_nz(c, c->x); cycles = 3; } break;
    case 0xB6: { ea_t e = ea_zpy(c, n); c->x = rd(n, e.addr); set_nz(c, c->x); cycles = 4; } break;
    case 0xAE: { ea_t e = ea_abs(c, n); c->x = rd(n, e.addr); set_nz(c, c->x); cycles = 4; } break;
    case 0xBE: { ea_t e = ea_absy(c, n, true); c->x = rd(n, e.addr); set_nz(c, c->x); cycles = 4 + e.page_cross; } break;

    // LDY
    case 0xA0: { ea_t e = ea_imm(c); c->y = rd(n, e.addr); set_nz(c, c->y); cycles = 2; } break;
    case 0xA4: { ea_t e = ea_zp(c, n); c->y = rd(n, e.addr); set_nz(c, c->y); cycles = 3; } break;
    case 0xB4: { ea_t e = ea_zpx(c, n); c->y = rd(n, e.addr); set_nz(c, c->y); cycles = 4; } break;
    case 0xAC: { ea_t e = ea_abs(c, n); c->y = rd(n, e.addr); set_nz(c, c->y); cycles = 4; } break;
    case 0xBC: { ea_t e = ea_absx(c, n, true); c->y = rd(n, e.addr); set_nz(c, c->y); cycles = 4 + e.page_cross; } break;

    // STA
    case 0x85: { ea_t e = ea_zp(c, n); wr(n, e.addr, c->a); cycles = 3; } break;
    case 0x95: { ea_t e = ea_zpx(c, n); wr(n, e.addr, c->a); cycles = 4; } break;
    case 0x8D: { ea_t e = ea_abs(c, n); wr(n, e.addr, c->a); cycles = 4; } break;
    case 0x9D: { ea_t e = ea_absx(c, n, false); wr(n, e.addr, c->a); cycles = 5; } break;
    case 0x99: { ea_t e = ea_absy(c, n, false); wr(n, e.addr, c->a); cycles = 5; } break;
    case 0x81: { ea_t e = ea_indx(c, n); wr(n, e.addr, c->a); cycles = 6; } break;
    case 0x91: { ea_t e = ea_indy(c, n, false); wr(n, e.addr, c->a); cycles = 6; } break;

    // STX
    case 0x86: { ea_t e = ea_zp(c, n); wr(n, e.addr, c->x); cycles = 3; } break;
    case 0x96: { ea_t e = ea_zpy(c, n); wr(n, e.addr, c->x); cycles = 4; } break;
    case 0x8E: { ea_t e = ea_abs(c, n); wr(n, e.addr, c->x); cycles = 4; } break;

    // STY
    case 0x84: { ea_t e = ea_zp(c, n); wr(n, e.addr, c->y); cycles = 3; } break;
    case 0x94: { ea_t e = ea_zpx(c, n); wr(n, e.addr, c->y); cycles = 4; } break;
    case 0x8C: { ea_t e = ea_abs(c, n); wr(n, e.addr, c->y); cycles = 4; } break;

    // CMP
    case 0xC9: { ea_t e = ea_imm(c); op_cmp(c, c->a, rd(n, e.addr)); cycles = 2; } break;
    case 0xC5: { ea_t e = ea_zp(c, n); op_cmp(c, c->a, rd(n, e.addr)); cycles = 3; } break;
    case 0xD5: { ea_t e = ea_zpx(c, n); op_cmp(c, c->a, rd(n, e.addr)); cycles = 4; } break;
    case 0xCD: { ea_t e = ea_abs(c, n); op_cmp(c, c->a, rd(n, e.addr)); cycles = 4; } break;
    case 0xDD: { ea_t e = ea_absx(c, n, true); op_cmp(c, c->a, rd(n, e.addr)); cycles = 4 + e.page_cross; } break;
    case 0xD9: { ea_t e = ea_absy(c, n, true); op_cmp(c, c->a, rd(n, e.addr)); cycles = 4 + e.page_cross; } break;
    case 0xC1: { ea_t e = ea_indx(c, n); op_cmp(c, c->a, rd(n, e.addr)); cycles = 6; } break;
    case 0xD1: { ea_t e = ea_indy(c, n, true); op_cmp(c, c->a, rd(n, e.addr)); cycles = 5 + e.page_cross; } break;

    // CPX
    case 0xE0: { ea_t e = ea_imm(c); op_cmp(c, c->x, rd(n, e.addr)); cycles = 2; } break;
    case 0xE4: { ea_t e = ea_zp(c, n); op_cmp(c, c->x, rd(n, e.addr)); cycles = 3; } break;
    case 0xEC: { ea_t e = ea_abs(c, n); op_cmp(c, c->x, rd(n, e.addr)); cycles = 4; } break;

    // CPY
    case 0xC0: { ea_t e = ea_imm(c); op_cmp(c, c->y, rd(n, e.addr)); cycles = 2; } break;
    case 0xC4: { ea_t e = ea_zp(c, n); op_cmp(c, c->y, rd(n, e.addr)); cycles = 3; } break;
    case 0xCC: { ea_t e = ea_abs(c, n); op_cmp(c, c->y, rd(n, e.addr)); cycles = 4; } break;

    // BIT
    case 0x24: { ea_t e = ea_zp(c, n); uint8_t m = rd(n, e.addr); uint8_t r = (uint8_t)(c->a & m);
      if (r == 0) c->p |= P_Z; else c->p &= (uint8_t)~P_Z;
      if (m & 0x80) c->p |= P_N; else c->p &= (uint8_t)~P_N;
      if (m & 0x40) c->p |= P_V; else c->p &= (uint8_t)~P_V;
      cycles = 3;
    } break;
    case 0x2C: { ea_t e = ea_abs(c, n); uint8_t m = rd(n, e.addr); uint8_t r = (uint8_t)(c->a & m);
      if (r == 0) c->p |= P_Z; else c->p &= (uint8_t)~P_Z;
      if (m & 0x80) c->p |= P_N; else c->p &= (uint8_t)~P_N;
      if (m & 0x40) c->p |= P_V; else c->p &= (uint8_t)~P_V;
      cycles = 4;
    } break;

    // INC/DEC memory
    case 0xE6: { ea_t e = ea_zp(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) + 1); wr(n, e.addr, v); set_nz(c, v); cycles = 5; } break;
    case 0xF6: { ea_t e = ea_zpx(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) + 1); wr(n, e.addr, v); set_nz(c, v); cycles = 6; } break;
    case 0xEE: { ea_t e = ea_abs(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) + 1); wr(n, e.addr, v); set_nz(c, v); cycles = 6; } break;
    case 0xFE: { ea_t e = ea_absx(c, n, false); uint8_t v = (uint8_t)(rd(n, e.addr) + 1); wr(n, e.addr, v); set_nz(c, v); cycles = 7; } break;

    case 0xC6: { ea_t e = ea_zp(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) - 1); wr(n, e.addr, v); set_nz(c, v); cycles = 5; } break;
    case 0xD6: { ea_t e = ea_zpx(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) - 1); wr(n, e.addr, v); set_nz(c, v); cycles = 6; } break;
    case 0xCE: { ea_t e = ea_abs(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) - 1); wr(n, e.addr, v); set_nz(c, v); cycles = 6; } break;
    case 0xDE: { ea_t e = ea_absx(c, n, false); uint8_t v = (uint8_t)(rd(n, e.addr) - 1); wr(n, e.addr, v); set_nz(c, v); cycles = 7; } break;

    // INX/INY/DEX/DEY
    case 0xE8: c->x++; set_nz(c, c->x); cycles = 2; break;
    case 0xC8: c->y++; set_nz(c, c->y); cycles = 2; break;
    case 0xCA: c->x--; set_nz(c, c->x); cycles = 2; break;
    case 0x88: c->y--; set_nz(c, c->y); cycles = 2; break;

    // ASL
    case 0x0A: c->a = op_asl(c, c->a); cycles = 2; break;
    case 0x06: { ea_t e = ea_zp(c, n); uint8_t v = op_asl(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 5; } break;
    case 0x16: { ea_t e = ea_zpx(c, n); uint8_t v = op_asl(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 6; } break;
    case 0x0E: { ea_t e = ea_abs(c, n); uint8_t v = op_asl(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 6; } break;
    case 0x1E: { ea_t e = ea_absx(c, n, false); uint8_t v = op_asl(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 7; } break;

    // LSR
    case 0x4A: c->a = op_lsr(c, c->a); cycles = 2; break;
    case 0x46: { ea_t e = ea_zp(c, n); uint8_t v = op_lsr(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 5; } break;
    case 0x56: { ea_t e = ea_zpx(c, n); uint8_t v = op_lsr(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 6; } break;
    case 0x4E: { ea_t e = ea_abs(c, n); uint8_t v = op_lsr(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 6; } break;
    case 0x5E: { ea_t e = ea_absx(c, n, false); uint8_t v = op_lsr(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 7; } break;

    // ROL
    case 0x2A: c->a = op_rol(c, c->a); cycles = 2; break;
    case 0x26: { ea_t e = ea_zp(c, n); uint8_t v = op_rol(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 5; } break;
    case 0x36: { ea_t e = ea_zpx(c, n); uint8_t v = op_rol(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 6; } break;
    case 0x2E: { ea_t e = ea_abs(c, n); uint8_t v = op_rol(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 6; } break;
    case 0x3E: { ea_t e = ea_absx(c, n, false); uint8_t v = op_rol(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 7; } break;

    // ROR
    case 0x6A: c->a = op_ror(c, c->a); cycles = 2; break;
    case 0x66: { ea_t e = ea_zp(c, n); uint8_t v = op_ror(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 5; } break;
    case 0x76: { ea_t e = ea_zpx(c, n); uint8_t v = op_ror(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 6; } break;
    case 0x6E: { ea_t e = ea_abs(c, n); uint8_t v = op_ror(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 6; } break;
    case 0x7E: { ea_t e = ea_absx(c, n, false); uint8_t v = op_ror(c, rd(n, e.addr)); wr(n, e.addr, v); cycles = 7; } break;

    // Jumps/calls
    case 0x4C: { uint16_t a = rd16(n, c->pc); c->pc = a; cycles = 3; } break;
    case 0x6C: { uint16_t ptr = rd16(n, c->pc); c->pc = rd16_wrap_bug(n, ptr); cycles = 5; } break;
    case 0x20: { uint16_t a = rd16(n, c->pc); c->pc += 2;
      uint16_t ret = (uint16_t)(c->pc - 1);
      push(c, n, (uint8_t)(ret >> 8));
      push(c, n, (uint8_t)(ret & 0xFF));
      c->pc = a;
      cycles = 6;
    } break;
    case 0x60: { uint8_t lo = pull(c, n); uint8_t hi = pull(c, n); c->pc = (uint16_t)(((uint16_t)hi << 8) | lo); c->pc++; cycles = 6; } break;
    case 0x40: { c->p = (uint8_t)((pull(c, n) | P_U) & (uint8_t)~P_B); uint8_t lo = pull(c, n); uint8_t hi = pull(c, n); c->pc = (uint16_t)(((uint16_t)hi << 8) | lo); cycles = 6; } break;

    // Branches
    case 0x10: cycles = branch(c, n, !(c->p & P_N)); break;
    case 0x30: cycles = branch(c, n, (c->p & P_N)); break;
    case 0x50: cycles = branch(c, n, !(c->p & P_V)); break;
    case 0x70: cycles = branch(c, n, (c->p & P_V)); break;
    case 0x90: cycles = branch(c, n, !(c->p & P_C)); break;
    case 0xB0: cycles = branch(c, n, (c->p & P_C)); break;
    case 0xD0: cycles = branch(c, n, !(c->p & P_Z)); break;
    case 0xF0: cycles = branch(c, n, (c->p & P_Z)); break;

    // Transfers
    case 0xAA: c->x = c->a; set_nz(c, c->x); cycles = 2; break;
    case 0x8A: c->a = c->x; set_nz(c, c->a); cycles = 2; break;
    case 0xA8: c->y = c->a; set_nz(c, c->y); cycles = 2; break;
    case 0x98: c->a = c->y; set_nz(c, c->a); cycles = 2; break;
    case 0xBA: c->x = c->sp; set_nz(c, c->x); cycles = 2; break;
    case 0x9A: c->sp = c->x; cycles = 2; break;

    // Stack
    case 0x48: push(c, n, c->a); cycles = 3; break;
    case 0x68: c->a = pull(c, n); set_nz(c, c->a); cycles = 4; break;
    case 0x08: push(c, n, (uint8_t)(c->p | P_B | P_U)); cycles = 3; break;
    case 0x28: c->p = (uint8_t)((pull(c, n) | P_U) & (uint8_t)~P_B); cycles = 4; break;

    // Flags
    case 0x18: c->p &= (uint8_t)~P_C; cycles = 2; break;
    case 0x38: c->p |= P_C; cycles = 2; break;
    case 0x58: c->p &= (uint8_t)~P_I; cycles = 2; break;
    case 0x78: c->p |= P_I; cycles = 2; break;
    case 0xB8: c->p &= (uint8_t)~P_V; cycles = 2; break;
    case 0xD8: c->p &= (uint8_t)~P_D; cycles = 2; break;
    case 0xF8: c->p |= P_D; cycles = 2; break;

    // BRK
    case 0x00: c->pc++; cycles = do_interrupt(c, n, 0xFFFE, true); break;

    // NOPs (official + many common unofficial)
    case 0xEA: cycles = 2; break;
    case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: cycles = 2; break;
    case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: c->pc++; cycles = 2; break; // imm
    case 0x04: case 0x44: case 0x64: c->pc++; cycles = 3; break; // zp
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: c->pc++; cycles = 4; break; // zpx
    case 0x0C: c->pc += 2; cycles = 4; break; // abs
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: c->pc += 2; cycles = 4; break; // absx (ignoring page add)

    // Common illegal opcodes (used by many commercial ROMs)
    // LAX: load A and X
    case 0xA7: { ea_t e = ea_zp(c, n); uint8_t v = rd(n, e.addr); c->a = v; c->x = v; set_nz(c, v); cycles = 3; } break;
    case 0xB7: { ea_t e = ea_zpy(c, n); uint8_t v = rd(n, e.addr); c->a = v; c->x = v; set_nz(c, v); cycles = 4; } break;
    case 0xAF: { ea_t e = ea_abs(c, n); uint8_t v = rd(n, e.addr); c->a = v; c->x = v; set_nz(c, v); cycles = 4; } break;
    case 0xBF: { ea_t e = ea_absy(c, n, true); uint8_t v = rd(n, e.addr); c->a = v; c->x = v; set_nz(c, v); cycles = 4 + e.page_cross; } break;
    case 0xA3: { ea_t e = ea_indx(c, n); uint8_t v = rd(n, e.addr); c->a = v; c->x = v; set_nz(c, v); cycles = 6; } break;
    case 0xB3: { ea_t e = ea_indy(c, n, true); uint8_t v = rd(n, e.addr); c->a = v; c->x = v; set_nz(c, v); cycles = 5 + e.page_cross; } break;

    // SAX: store A & X
    case 0x87: { ea_t e = ea_zp(c, n); wr(n, e.addr, (uint8_t)(c->a & c->x)); cycles = 3; } break;
    case 0x97: { ea_t e = ea_zpy(c, n); wr(n, e.addr, (uint8_t)(c->a & c->x)); cycles = 4; } break;
    case 0x8F: { ea_t e = ea_abs(c, n); wr(n, e.addr, (uint8_t)(c->a & c->x)); cycles = 4; } break;
    case 0x83: { ea_t e = ea_indx(c, n); wr(n, e.addr, (uint8_t)(c->a & c->x)); cycles = 6; } break;

    // SLO: ASL then ORA
    case 0x07: { ea_t e = ea_zp(c, n); uint8_t v = op_asl(c, rd(n, e.addr)); wr(n, e.addr, v); c->a |= v; set_nz(c, c->a); cycles = 5; } break;
    case 0x17: { ea_t e = ea_zpx(c, n); uint8_t v = op_asl(c, rd(n, e.addr)); wr(n, e.addr, v); c->a |= v; set_nz(c, c->a); cycles = 6; } break;
    case 0x0F: { ea_t e = ea_abs(c, n); uint8_t v = op_asl(c, rd(n, e.addr)); wr(n, e.addr, v); c->a |= v; set_nz(c, c->a); cycles = 6; } break;
    case 0x1F: { ea_t e = ea_absx(c, n, false); uint8_t v = op_asl(c, rd(n, e.addr)); wr(n, e.addr, v); c->a |= v; set_nz(c, c->a); cycles = 7; } break;
    case 0x1B: { ea_t e = ea_absy(c, n, false); uint8_t v = op_asl(c, rd(n, e.addr)); wr(n, e.addr, v); c->a |= v; set_nz(c, c->a); cycles = 7; } break;
    case 0x03: { ea_t e = ea_indx(c, n); uint8_t v = op_asl(c, rd(n, e.addr)); wr(n, e.addr, v); c->a |= v; set_nz(c, c->a); cycles = 8; } break;
    case 0x13: { ea_t e = ea_indy(c, n, false); uint8_t v = op_asl(c, rd(n, e.addr)); wr(n, e.addr, v); c->a |= v; set_nz(c, c->a); cycles = 8; } break;

    // RLA: ROL then AND
    case 0x27: { ea_t e = ea_zp(c, n); uint8_t v = op_rol(c, rd(n, e.addr)); wr(n, e.addr, v); c->a &= v; set_nz(c, c->a); cycles = 5; } break;
    case 0x37: { ea_t e = ea_zpx(c, n); uint8_t v = op_rol(c, rd(n, e.addr)); wr(n, e.addr, v); c->a &= v; set_nz(c, c->a); cycles = 6; } break;
    case 0x2F: { ea_t e = ea_abs(c, n); uint8_t v = op_rol(c, rd(n, e.addr)); wr(n, e.addr, v); c->a &= v; set_nz(c, c->a); cycles = 6; } break;
    case 0x3F: { ea_t e = ea_absx(c, n, false); uint8_t v = op_rol(c, rd(n, e.addr)); wr(n, e.addr, v); c->a &= v; set_nz(c, c->a); cycles = 7; } break;
    case 0x3B: { ea_t e = ea_absy(c, n, false); uint8_t v = op_rol(c, rd(n, e.addr)); wr(n, e.addr, v); c->a &= v; set_nz(c, c->a); cycles = 7; } break;
    case 0x23: { ea_t e = ea_indx(c, n); uint8_t v = op_rol(c, rd(n, e.addr)); wr(n, e.addr, v); c->a &= v; set_nz(c, c->a); cycles = 8; } break;
    case 0x33: { ea_t e = ea_indy(c, n, false); uint8_t v = op_rol(c, rd(n, e.addr)); wr(n, e.addr, v); c->a &= v; set_nz(c, c->a); cycles = 8; } break;

    // SRE: LSR then EOR
    case 0x47: { ea_t e = ea_zp(c, n); uint8_t v = op_lsr(c, rd(n, e.addr)); wr(n, e.addr, v); c->a ^= v; set_nz(c, c->a); cycles = 5; } break;
    case 0x57: { ea_t e = ea_zpx(c, n); uint8_t v = op_lsr(c, rd(n, e.addr)); wr(n, e.addr, v); c->a ^= v; set_nz(c, c->a); cycles = 6; } break;
    case 0x4F: { ea_t e = ea_abs(c, n); uint8_t v = op_lsr(c, rd(n, e.addr)); wr(n, e.addr, v); c->a ^= v; set_nz(c, c->a); cycles = 6; } break;
    case 0x5F: { ea_t e = ea_absx(c, n, false); uint8_t v = op_lsr(c, rd(n, e.addr)); wr(n, e.addr, v); c->a ^= v; set_nz(c, c->a); cycles = 7; } break;
    case 0x5B: { ea_t e = ea_absy(c, n, false); uint8_t v = op_lsr(c, rd(n, e.addr)); wr(n, e.addr, v); c->a ^= v; set_nz(c, c->a); cycles = 7; } break;
    case 0x43: { ea_t e = ea_indx(c, n); uint8_t v = op_lsr(c, rd(n, e.addr)); wr(n, e.addr, v); c->a ^= v; set_nz(c, c->a); cycles = 8; } break;
    case 0x53: { ea_t e = ea_indy(c, n, false); uint8_t v = op_lsr(c, rd(n, e.addr)); wr(n, e.addr, v); c->a ^= v; set_nz(c, c->a); cycles = 8; } break;

    // RRA: ROR then ADC
    case 0x67: { ea_t e = ea_zp(c, n); uint8_t v = op_ror(c, rd(n, e.addr)); wr(n, e.addr, v); op_adc(c, v); cycles = 5; } break;
    case 0x77: { ea_t e = ea_zpx(c, n); uint8_t v = op_ror(c, rd(n, e.addr)); wr(n, e.addr, v); op_adc(c, v); cycles = 6; } break;
    case 0x6F: { ea_t e = ea_abs(c, n); uint8_t v = op_ror(c, rd(n, e.addr)); wr(n, e.addr, v); op_adc(c, v); cycles = 6; } break;
    case 0x7F: { ea_t e = ea_absx(c, n, false); uint8_t v = op_ror(c, rd(n, e.addr)); wr(n, e.addr, v); op_adc(c, v); cycles = 7; } break;
    case 0x7B: { ea_t e = ea_absy(c, n, false); uint8_t v = op_ror(c, rd(n, e.addr)); wr(n, e.addr, v); op_adc(c, v); cycles = 7; } break;
    case 0x63: { ea_t e = ea_indx(c, n); uint8_t v = op_ror(c, rd(n, e.addr)); wr(n, e.addr, v); op_adc(c, v); cycles = 8; } break;
    case 0x73: { ea_t e = ea_indy(c, n, false); uint8_t v = op_ror(c, rd(n, e.addr)); wr(n, e.addr, v); op_adc(c, v); cycles = 8; } break;

    // DCP: DEC then CMP
    case 0xC7: { ea_t e = ea_zp(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) - 1); wr(n, e.addr, v); op_cmp(c, c->a, v); cycles = 5; } break;
    case 0xD7: { ea_t e = ea_zpx(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) - 1); wr(n, e.addr, v); op_cmp(c, c->a, v); cycles = 6; } break;
    case 0xCF: { ea_t e = ea_abs(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) - 1); wr(n, e.addr, v); op_cmp(c, c->a, v); cycles = 6; } break;
    case 0xDF: { ea_t e = ea_absx(c, n, false); uint8_t v = (uint8_t)(rd(n, e.addr) - 1); wr(n, e.addr, v); op_cmp(c, c->a, v); cycles = 7; } break;
    case 0xDB: { ea_t e = ea_absy(c, n, false); uint8_t v = (uint8_t)(rd(n, e.addr) - 1); wr(n, e.addr, v); op_cmp(c, c->a, v); cycles = 7; } break;
    case 0xC3: { ea_t e = ea_indx(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) - 1); wr(n, e.addr, v); op_cmp(c, c->a, v); cycles = 8; } break;
    case 0xD3: { ea_t e = ea_indy(c, n, false); uint8_t v = (uint8_t)(rd(n, e.addr) - 1); wr(n, e.addr, v); op_cmp(c, c->a, v); cycles = 8; } break;

    // ISC: INC then SBC
    case 0xE7: { ea_t e = ea_zp(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) + 1); wr(n, e.addr, v); op_sbc(c, v); cycles = 5; } break;
    case 0xF7: { ea_t e = ea_zpx(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) + 1); wr(n, e.addr, v); op_sbc(c, v); cycles = 6; } break;
    case 0xEF: { ea_t e = ea_abs(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) + 1); wr(n, e.addr, v); op_sbc(c, v); cycles = 6; } break;
    case 0xFF: { ea_t e = ea_absx(c, n, false); uint8_t v = (uint8_t)(rd(n, e.addr) + 1); wr(n, e.addr, v); op_sbc(c, v); cycles = 7; } break;
    case 0xFB: { ea_t e = ea_absy(c, n, false); uint8_t v = (uint8_t)(rd(n, e.addr) + 1); wr(n, e.addr, v); op_sbc(c, v); cycles = 7; } break;
    case 0xE3: { ea_t e = ea_indx(c, n); uint8_t v = (uint8_t)(rd(n, e.addr) + 1); wr(n, e.addr, v); op_sbc(c, v); cycles = 8; } break;
    case 0xF3: { ea_t e = ea_indy(c, n, false); uint8_t v = (uint8_t)(rd(n, e.addr) + 1); wr(n, e.addr, v); op_sbc(c, v); cycles = 8; } break;

    // Illegal immediate ops used occasionally
    case 0x0B: { ea_t e = ea_imm(c); op_anc(c, rd(n, e.addr)); cycles = 2; } break; // ANC
    case 0x2B: { ea_t e = ea_imm(c); op_anc(c, rd(n, e.addr)); cycles = 2; } break; // ANC
    case 0x4B: { ea_t e = ea_imm(c); op_alr(c, rd(n, e.addr)); cycles = 2; } break; // ALR
    case 0x6B: { ea_t e = ea_imm(c); op_arr(c, rd(n, e.addr)); cycles = 2; } break; // ARR
    case 0xCB: { ea_t e = ea_imm(c); op_sbx(c, rd(n, e.addr)); cycles = 2; } break; // SBX/AXS
    case 0xEB: { ea_t e = ea_imm(c); op_sbc(c, rd(n, e.addr)); cycles = 2; } break; // SBC (illegal alias)

    // More illegal opcodes used in some NES ROMs (incl. SMB PRG as data/code).
    case 0x8B: { ea_t e = ea_imm(c); uint8_t imm = rd(n, e.addr); c->a = (uint8_t)(c->x & imm); set_nz(c, c->a); cycles = 2; } break; // XAA/ANE (approx)
    case 0xAB: { ea_t e = ea_imm(c); uint8_t imm = rd(n, e.addr); c->a = imm; c->x = imm; set_nz(c, imm); cycles = 2; } break; // LXA/OAL (approx)
    case 0xBB: { ea_t e = ea_absy(c, n, true); uint8_t v = (uint8_t)(rd(n, e.addr) & c->sp); c->sp = v; c->a = v; c->x = v; set_nz(c, v); cycles = 4 + e.page_cross; } break; // LAS
    case 0x9B: { ea_t e = ea_absy(c, n, false); uint8_t sp = (uint8_t)(c->a & c->x); c->sp = sp; uint8_t m = (uint8_t)(((e.addr >> 8) + 1) & 0xFF); wr(n, e.addr, (uint8_t)(sp & m)); cycles = 5; } break; // TAS/SHS
    case 0x9C: { ea_t e = ea_absx(c, n, false); uint8_t m = (uint8_t)(((e.addr >> 8) + 1) & 0xFF); wr(n, e.addr, (uint8_t)(c->y & m)); cycles = 5; } break; // SHY
    case 0x9E: { ea_t e = ea_absy(c, n, false); uint8_t m = (uint8_t)(((e.addr >> 8) + 1) & 0xFF); wr(n, e.addr, (uint8_t)(c->x & m)); cycles = 5; } break; // SHX
    case 0x9F: { ea_t e = ea_absy(c, n, false); uint8_t m = (uint8_t)(((e.addr >> 8) + 1) & 0xFF); wr(n, e.addr, (uint8_t)(c->a & c->x & m)); cycles = 5; } break; // AHX
    case 0x93: { ea_t e = ea_indy(c, n, false); uint8_t m = (uint8_t)(((e.addr >> 8) + 1) & 0xFF); wr(n, e.addr, (uint8_t)(c->a & c->x & m)); cycles = 6; } break; // AHX (ind),Y

    default:
      // Best-effort: treat unknown as 1-byte NOP.
      cycles = 2;
      break;
  }

  c->cycles += (uint64_t)cycles;
  return cycles;
}
