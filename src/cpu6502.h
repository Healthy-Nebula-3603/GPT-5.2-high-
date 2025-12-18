#pragma once
#include "common.h"

struct nes;

typedef struct cpu6502 {
  uint16_t pc;
  uint8_t a, x, y;
  uint8_t sp;
  uint8_t p;
  uint64_t cycles;
  bool nmi_pending;
  bool irq_pending;
} cpu6502_t;

void cpu6502_reset(cpu6502_t *c, struct nes *nes);
int cpu6502_step(cpu6502_t *c, struct nes *nes); // returns CPU cycles used
void cpu6502_set_nmi(cpu6502_t *c);
void cpu6502_set_irq(cpu6502_t *c, bool level);

