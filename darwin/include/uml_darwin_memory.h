/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_MEMORY_ABI_H
#define UML_DARWIN_MEMORY_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

int darwin_uml_vm_map(void *address, int fd, unsigned long long offset,
		      unsigned long length, int protection, int guest_code,
		      int guest_writable);
int darwin_uml_vm_map_generation(unsigned long generation, void *address,
				 int fd, unsigned long long offset,
				 unsigned long length, int protection,
				 int guest_code, int guest_writable);
int darwin_uml_vm_protect(void *address, unsigned long length,
			  int protection);
int darwin_uml_vm_unmap(void *address, unsigned long length);
int darwin_uml_vm_unmap_generation(unsigned long generation, void *address,
				   unsigned long length);
int darwin_uml_vm_drop(void *address, unsigned long length);
int darwin_uml_vm_activate(unsigned long generation);
int darwin_uml_vm_batch_begin(unsigned long generation);
int darwin_uml_vm_batch_end(void);
int darwin_uml_vm_walker_lock(unsigned long generation);
int darwin_uml_vm_walker_unlock(unsigned long generation);
int darwin_uml_vm_destroy(unsigned long generation);
int darwin_uml_vm_flush_active(void);
int darwin_uml_vm_flush_generation(unsigned long generation);
int darwin_uml_vm_sync_to_backing(unsigned long address);
int darwin_uml_vm_sync_from_backing(unsigned long address);
int darwin_uml_vm_sync_from_buffer(unsigned long address,
				   const void *buffer, unsigned long length);
int darwin_uml_vm_mapping_matches(unsigned long address, int fd,
				  unsigned long long offset);
int darwin_uml_vm_rebind(unsigned long address, int fd,
			 unsigned long long offset);
int darwin_uml_vm_stage_stack(unsigned long address);
int darwin_uml_vm_finish_stack(unsigned long address);
int darwin_uml_vm_retry_fault(unsigned long address, int execute, int write);
int darwin_uml_begin_write_single_step(unsigned long address,
				       unsigned long guest_pc);

#ifdef __cplusplus
}
#endif

#endif
