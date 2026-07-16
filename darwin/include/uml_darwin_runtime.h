/* SPDX-License-Identifier: GPL-2.0 */
#ifndef UML_DARWIN_RUNTIME_H
#define UML_DARWIN_RUNTIME_H

#include <stddef.h>

int darwin_uml_open(const char *path, int flags, int mode);
int darwin_uml_fcntl(int fd, int command, long argument);
int darwin_uml_ioctl(int fd, unsigned long command, void *argument);
int darwin_uml_net_open(unsigned char mac[6], unsigned int *mtu);
int darwin_uml_net_irq_fd(void);
int darwin_uml_net_read(void *buffer, unsigned long capacity);
int darwin_uml_net_write(const void *buffer, unsigned long length);
void darwin_uml_abort(void) __attribute__((noreturn));
void darwin_uml_exit(int status, void *caller) __attribute__((noreturn));

int darwin_uml_vm_map(void *address, int fd, unsigned long long offset,
		      unsigned long length, int protection, int guest_code,
		      int guest_writable);
int darwin_uml_vm_protect(void *address, unsigned long length,
			  int protection);
int darwin_uml_vm_unmap(void *address, unsigned long length);
int darwin_uml_vm_drop(void *address, unsigned long length);
int darwin_uml_vm_activate(unsigned long generation);
int darwin_uml_vm_batch_begin(void);
int darwin_uml_vm_batch_end(void);
int darwin_uml_vm_destroy(unsigned long generation);
int darwin_uml_vm_flush_active(void);
int darwin_uml_vm_refresh_active(void);
int darwin_uml_vm_sync_to_backing(unsigned long address);
int darwin_uml_vm_sync_from_backing(unsigned long address);
int darwin_uml_vm_stage_stack(unsigned long address);
int darwin_uml_vm_finish_stack(unsigned long address);
int darwin_uml_vm_has_mapping(unsigned long address);
int darwin_uml_vm_prepare_execute(unsigned long address);
int darwin_uml_vm_promote_syscall(unsigned long address);
int darwin_uml_vm_validate_syscall(unsigned long address);
int darwin_uml_vm_resolve_fault(unsigned long address, int execute, int write);
int darwin_uml_vm_retry_fault(unsigned long address, int execute, int write);
int darwin_uml_vm_begin_write_step(unsigned long address);
int darwin_uml_vm_finish_write_step(unsigned long address);
int darwin_uml_vm_x18_instruction(unsigned long address,
				  unsigned int *instruction);
int darwin_uml_vm_patch_x18_fault(unsigned long address);
int darwin_uml_vm_x18_stub_fault(unsigned long native_pc,
				 unsigned long *site,
				 unsigned int *scratch);
int darwin_uml_vm_gateway_x18_fault(unsigned long native_pc,
				    unsigned int *instruction);
int darwin_uml_vm_indirect_instruction(unsigned long address,
				       unsigned int *instruction,
				       unsigned long *site);
int darwin_uml_vm_cache_indirect(unsigned long site,
				 unsigned int instruction,
				 unsigned long target);
int darwin_uml_vm_cache_direct_indirect(unsigned long resume_stub,
					unsigned long target);

int darwin_uml_enter_guest(void *registers, void *fp_registers,
			   unsigned long fp_size, unsigned long generation,
			   unsigned long *tls);
int darwin_uml_begin_write_single_step(unsigned long address,
				       unsigned long guest_pc);
void darwin_uml_runtime_init(void);

#endif
