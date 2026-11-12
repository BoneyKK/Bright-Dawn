/* An ARM946E-S (ARMv5TE) interpreter that executes one instruction at a time
 * against memory supplied by the host.
 *
 * This is the only interface the rest of the runtime uses, so the interpreter
 * behind it can be replaced. The current implementation wraps SkyEmu's core
 * (third_party/skyemu).
 */
#ifndef BD_RUNTIME_INTERP_ARM_CORE_H
#define BD_RUNTIME_INTERP_ARM_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host services. Addresses passed to the 32- and 16-bit functions are already
 * aligned. Every function receives `user` unchanged. */
typedef struct bd_arm_host {
  void* user;
  uint32_t (*read32)(void* user, uint32_t address);
  uint32_t (*read16)(void* user, uint32_t address);
  uint8_t (*read8)(void* user, uint32_t address);
  void (*write32)(void* user, uint32_t address, uint32_t value);
  void (*write16)(void* user, uint32_t address, uint16_t value);
  void (*write8)(void* user, uint32_t address, uint8_t value);
  /* System coprocessor access (MRC / MCR). */
  uint32_t (*cp_read)(void* user, int cp, int opc1, int crn, int crm, int opc2);
  void (*cp_write)(void* user, int cp, int opc1, int crn, int crm, int opc2, uint32_t value);
} bd_arm_host;

/* Processor state as the program sees it in its current mode.
 * r[15] is the address of the next instruction to execute, not the
 * pipelined value. Bit 5 of cpsr selects Thumb state. spsr is meaningful only
 * in modes that have one (every mode but User and System). */
typedef struct bd_arm_state {
  uint32_t r[16];
  uint32_t cpsr;
  uint32_t spsr;
} bd_arm_state;

typedef struct bd_arm_core bd_arm_core;

/* Returns NULL if memory cannot be allocated. `host` is copied. */
bd_arm_core* bd_arm_core_create(const bd_arm_host* host);
void bd_arm_core_destroy(bd_arm_core* core);

/* Replaces the whole processor state. The mode in cpsr must be a valid one. */
void bd_arm_core_set_state(bd_arm_core* core, const bd_arm_state* state);
void bd_arm_core_get_state(bd_arm_core* core, bd_arm_state* state);

/* Executes exactly one instruction (a Thumb BL counts as two). */
void bd_arm_core_step(bd_arm_core* core);

/* Non-zero if the last step changed the flow of control, i.e. the next
 * instruction is not simply the following one. */
int bd_arm_core_branched(const bd_arm_core* core);

/* A short description of the implementation, for reports. */
const char* bd_arm_core_name(void);

#ifdef __cplusplus
}
#endif

#endif
