/* bd_arm_core implemented with SkyEmu's ARM core (third_party/skyemu/arm7.h).
 *
 * That header defines everything as static functions, so all use of it is in
 * this one file. It expects four macros from another SkyEmu header; they are
 * supplied here with the same definitions.
 */
#include "arm_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE inline __attribute__((always_inline))
#define SB_LIKELY(x) __builtin_expect(!!(x), 1)
#define SB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#define SB_LIKELY(x) (x)
#define SB_UNLIKELY(x) (x)
#else
#define FORCE_INLINE inline
#define SB_LIKELY(x) (x)
#define SB_UNLIKELY(x) (x)
#endif
#define SB_BFE(VALUE, BITOFFSET, SIZE) (((VALUE) >> (BITOFFSET)) & ((1llu << (SIZE)) - 1))

/* The core reports what it cannot execute by printing to standard output.
 * The runtime detects those cases itself and reports them its own way, and a
 * library must not write to a program's output, so the core's messages are
 * discarded. Done here because the core is kept unmodified. */
#include <stdio.h>
static int bd_discard_message(const char* format, ...) {
  (void)format;
  return 0;
}
#define printf bd_discard_message
#include "arm7.h"
#undef printf

struct bd_arm_core {
  arm7_t cpu;
  bd_arm_host host;
};

/* SkyEmu passes addresses as the instruction formed them; the bus aligns. */
static uint32_t sky_read32(void* user, uint32_t address) {
  bd_arm_core* core = (bd_arm_core*)user;
  return core->host.read32(core->host.user, address & ~3u);
}
static uint32_t sky_read16(void* user, uint32_t address) {
  bd_arm_core* core = (bd_arm_core*)user;
  return core->host.read16(core->host.user, address & ~1u);
}
static uint8_t sky_read8(void* user, uint32_t address) {
  bd_arm_core* core = (bd_arm_core*)user;
  return core->host.read8(core->host.user, address);
}
static uint32_t sky_read32_seq(void* user, uint32_t address, bool sequential) {
  (void)sequential;
  return sky_read32(user, address);
}
static uint32_t sky_read16_seq(void* user, uint32_t address, bool sequential) {
  (void)sequential;
  return sky_read16(user, address);
}
static void sky_write32(void* user, uint32_t address, uint32_t value) {
  bd_arm_core* core = (bd_arm_core*)user;
  core->host.write32(core->host.user, address & ~3u, value);
}
static void sky_write16(void* user, uint32_t address, uint16_t value) {
  bd_arm_core* core = (bd_arm_core*)user;
  core->host.write16(core->host.user, address & ~1u, value);
}
static void sky_write8(void* user, uint32_t address, uint8_t value) {
  bd_arm_core* core = (bd_arm_core*)user;
  core->host.write8(core->host.user, address, value);
}
static uint32_t sky_cp_read(void* user, int cp, int opc1, int crn, int crm, int opc2) {
  bd_arm_core* core = (bd_arm_core*)user;
  return core->host.cp_read(core->host.user, cp, opc1, crn, crm, opc2);
}
static void sky_cp_write(void* user, int cp, int opc1, int crn, int crm, int opc2, uint32_t value) {
  bd_arm_core* core = (bd_arm_core*)user;
  core->host.cp_write(core->host.user, cp, opc1, crn, crm, opc2, value);
}

/* User and System mode have no saved status register of their own. */
static bool mode_has_spsr(uint32_t cpsr) {
  const uint32_t mode = cpsr & 0x1Fu;
  return mode != 0x10u && mode != 0x1Fu;
}

/* Discards the prefetched instructions, so execution resumes by fetching
 * from the current program counter. */
static void restart_pipeline(arm7_t* cpu) {
  cpu->phase = 0;
  cpu->phased_op_id = ARM_PHASED_FILL_PIPE;
  cpu->prefetch_pc = (uint32_t)-1;
  cpu->next_fetch_sequential = false;
  cpu->wait_for_interrupt = false;
}

bd_arm_core* bd_arm_core_create(const bd_arm_host* host) {
  bd_arm_core* core = (bd_arm_core*)calloc(1, sizeof *core);
  if (core == NULL) return NULL;
  core->host = *host;
  core->cpu = arm7_init(core);
  core->cpu.read32 = sky_read32;
  core->cpu.read16 = sky_read16;
  core->cpu.read8 = sky_read8;
  core->cpu.read32_seq = sky_read32_seq;
  core->cpu.read16_seq = sky_read16_seq;
  core->cpu.write32 = sky_write32;
  core->cpu.write16 = sky_write16;
  core->cpu.write8 = sky_write8;
  core->cpu.coprocessor_read = sky_cp_read;
  core->cpu.coprocessor_write = sky_cp_write;
  core->cpu.registers[CPSR] = 0x1Fu;
  restart_pipeline(&core->cpu);
  return core;
}

void bd_arm_core_destroy(bd_arm_core* core) { free(core); }

/* Where the core keeps r13 of each bank (r14 follows it), and each bank's
 * saved status register. Bank 0 (User and System) has no saved status
 * register. The core's register file holds every mode's registers side by
 * side; see the table at the top of arm7.h. */
static const int kBankR13[6] = {13, R13_fiq, R13_irq, R13_svc, R13_abt, R13_und};
static const int kBankSpsr[6] = {-1, SPSR_fiq, SPSR_irq, SPSR_svc, SPSR_abt, SPSR_und};
enum { kFiqR8 = 17 }; /* r8-r12 of FIQ mode are registers 17-21. */

void bd_arm_core_set_state(bd_arm_core* core, const bd_arm_state* state) {
  arm7_t* cpu = &core->cpu;
  int i;
  memset(cpu->registers, 0, sizeof cpu->registers);
  for (i = 0; i < 6; ++i) {
    cpu->registers[kBankR13[i]] = state->bank_r13[i];
    cpu->registers[kBankR13[i] + 1] = state->bank_r14[i];
    if (kBankSpsr[i] >= 0) cpu->registers[kBankSpsr[i]] = state->bank_spsr[i];
  }
  for (i = 0; i < 5; ++i) {
    cpu->registers[8 + i] = state->bank_r8[0][i];
    cpu->registers[kFiqR8 + i] = state->bank_r8[1][i];
  }
  /* Now the current mode's view over that. The status register first: it
   * selects which banked registers the following writes go to. */
  cpu->registers[CPSR] = state->cpsr;
  for (i = 0; i < 15; ++i) arm7_reg_write(cpu, (unsigned)i, state->r[i]);
  if (mode_has_spsr(state->cpsr)) arm7_reg_write(cpu, SPSR, state->spsr);
  cpu->registers[PC] = state->r[15] & ((state->cpsr & 0x20u) ? ~1u : ~3u);
  restart_pipeline(cpu);
}

void bd_arm_core_get_state(bd_arm_core* core, bd_arm_state* state) {
  arm7_t* cpu = &core->cpu;
  int i;
  for (i = 0; i < 15; ++i) state->r[i] = arm7_reg_read(cpu, (unsigned)i);
  state->cpsr = cpu->registers[CPSR];
  /* Between instructions the core's program counter is the address of the
   * next instruction, both after a branch and in straight-line code. */
  state->r[15] = cpu->registers[PC] & ((state->cpsr & 0x20u) ? ~1u : ~3u);
  state->spsr = mode_has_spsr(state->cpsr) ? arm7_reg_read(cpu, SPSR) : 0;
  for (i = 0; i < 6; ++i) {
    state->bank_r13[i] = cpu->registers[kBankR13[i]];
    state->bank_r14[i] = cpu->registers[kBankR13[i] + 1];
    state->bank_spsr[i] = kBankSpsr[i] >= 0 ? cpu->registers[kBankSpsr[i]] : 0;
  }
  for (i = 0; i < 5; ++i) {
    state->bank_r8[0][i] = cpu->registers[8 + i];
    state->bank_r8[1][i] = cpu->registers[kFiqR8 + i];
  }
}

/* "ldm rn!, {..., pc}^": an exception return that also updates its base
 * register. Bits: 27-25 = 100, S (22), W (21), L (20) and pc in the list. */
static bool is_exception_return_with_writeback(uint32_t opcode) {
  return (opcode & 0x0E708000u) == 0x08708000u;
}

void bd_arm_core_step(bd_arm_core* core) {
  arm7_t* cpu = &core->cpu;
  bool fix_writeback = false;
  unsigned base = 0, index_before = 0;
  uint32_t registers_before[37];
  /* After a branch the core refills its pipeline over several calls before
   * the next instruction executes. */
  while (cpu->phased_op_id == ARM_PHASED_FILL_PIPE) arm9_exec_instruction(cpu);

  /* A defect in the core, corrected here because the core is kept unmodified.
   * For "ldm rn!, {..., pc}^" it writes the updated base register once more
   * after the status register has been restored. If that changed the mode and
   * rn is a register the two modes do not share, the value lands in the new
   * mode's register and destroys it. The usual victim is the stack pointer of
   * the interrupted program, on "ldmfd sp!, {..., pc}^". The processor writes
   * the base register in the mode the instruction started in. */
  if ((cpu->registers[CPSR] & 0x20u) == 0) {
    const uint32_t opcode = cpu->prefetch_opcode[0]; /* The instruction about to execute. */
    if (is_exception_return_with_writeback(opcode)) {
      base = (opcode >> 16) & 15u;
      if (base != 15u) {
        fix_writeback = true;
        index_before = arm7_reg_index(cpu, base);
        memcpy(registers_before, cpu->registers, sizeof registers_before);
      }
    }
  }

  arm9_exec_instruction(cpu);
  /* A multi-register transfer moves one register per call. */
  while (cpu->phased_op_id == ARM_PHASED_BLOCK_TRANSFER) arm9_exec_instruction(cpu);

  if (fix_writeback) {
    /* If the instruction's condition failed, or the mode's registers are the
     * same ones, the index is unchanged and there is nothing to correct. */
    const unsigned index_after = arm7_reg_index(cpu, base);
    if (index_after != index_before) {
      cpu->registers[index_before] = cpu->registers[index_after];
      cpu->registers[index_after] = registers_before[index_after];
    }
  }
}

int bd_arm_core_branched(const bd_arm_core* core) {
  return core->cpu.phased_op_id == ARM_PHASED_FILL_PIPE;
}

const char* bd_arm_core_name(void) { return "SkyEmu arm7.h, ARM9 tables (commit f4d3238)"; }
