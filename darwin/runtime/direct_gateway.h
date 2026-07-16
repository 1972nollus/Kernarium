/* SPDX-License-Identifier: GPL-2.0 */
#ifndef UML_DARWIN_DIRECT_GATEWAY_H
#define UML_DARWIN_DIRECT_GATEWAY_H

/* Keep this small prefix independent of Darwin SDK types: hand-written
 * AArch64 code uses it while moving between the guest and the UML kernel.
 */
#define DG_REGISTERS 0
#define DG_FP_REGISTERS 8
#define DG_HOST_FP_REGISTERS 16
#define DG_KERNEL_SWITCH 24
#define DG_DIRECT_SWITCH 32
#define DG_HOST_TLS 40
#define DG_HOST_X18 48
#define DG_GUEST_TLS 56
#define DG_GUEST_X18 64
#define DG_SCRATCH_X16 72
#define DG_RESUME_STUB 80
#define DG_GUEST_PC 88
#define DG_EVENT 96
#define DG_SUSPENDED 100
#define DG_SIZE 104

#define ARM64_STATE_X16 128
#define ARM64_STATE_X17 136
#define ARM64_STATE_X18 144
#define ARM64_STATE_FP 232
#define ARM64_STATE_LR 240
#define ARM64_STATE_SP 248
#define ARM64_STATE_PC 256
#define ARM64_STATE_CPSR 264

#ifndef __ASSEMBLER__
#include <stdint.h>

struct direct_gateway_state {
	void *registers;
	void *fp_registers;
	void *host_fp_registers;
	void *kernel_switch;
	void *direct_switch;
	uint64_t host_tls;
	uint64_t host_x18;
	uint64_t guest_tls;
	uint64_t guest_x18;
	uint64_t scratch_x16;
	uint64_t resume_stub;
	uint64_t guest_pc;
	int event;
	int suspended;
};
#endif

#endif
