/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_STATIC_MMU_H
#define UML_DARWIN_STATIC_MMU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum darwin_uml_static_access {
	DARWIN_UML_STATIC_READ = 0,
	DARWIN_UML_STATIC_WRITE = 1,
	DARWIN_UML_STATIC_EXECUTE = 2,
};

enum darwin_uml_static_translation_result {
	DARWIN_UML_STATIC_TRANSLATED = 0,
	DARWIN_UML_STATIC_UNMAPPED = 1,
	DARWIN_UML_STATIC_PERMISSION = 2,
	DARWIN_UML_STATIC_HOST_ERROR = -1,
};

struct darwin_uml_static_mapping_change {
	uint64_t address;
	uint64_t length;
};

struct darwin_uml_static_sequences {
	uint64_t mapping_changes;
	uint64_t code_changes;
	uint64_t execution_interrupt;
};

int darwin_uml_static_vm_map(void *address, int fd,
			     unsigned long long offset, unsigned long length,
			     int protection, int guest_code,
			     int guest_writable);
int darwin_uml_static_vm_map_generation(unsigned long generation,
					 void *address, int fd,
					 unsigned long long offset,
					 unsigned long length,
					 int protection, int guest_code,
					 int guest_writable);
int darwin_uml_static_vm_protect(void *address, unsigned long length,
				 int protection);
int darwin_uml_static_vm_unmap(void *address, unsigned long length);
int darwin_uml_static_vm_unmap_generation(unsigned long generation,
					    void *address,
					    unsigned long length);
int darwin_uml_static_vm_activate(unsigned long generation);
int darwin_uml_static_vm_destroy(unsigned long generation);
int darwin_uml_static_vm_mapping_matches(unsigned long address, int fd,
					   unsigned long long offset);
int darwin_uml_static_vm_rebind(unsigned long address, int fd,
				 unsigned long long offset);
int darwin_uml_static_vm_has_mapping(unsigned long address);
int darwin_uml_static_vm_copy_from_buffer(unsigned long address,
					   const void *buffer,
					   unsigned long length);
int darwin_uml_static_vm_retry_fault(unsigned long address, int execute,
				     int write);
int darwin_uml_static_vm_batch_begin(unsigned long generation);
int darwin_uml_static_vm_batch_end(void);
int darwin_uml_static_vm_walker_lock(unsigned long generation);
int darwin_uml_static_vm_walker_unlock(unsigned long generation);

/* Returns a host data pointer valid through the end of the translated 4 KiB
 * page.  The mapping is never executable.  Each vCPU observes concurrent
 * map/protect/unmap operations at its next bounded guest-dispatch boundary.
 */
int darwin_uml_static_vm_translate(unsigned long generation,
				   unsigned long address,
				   enum darwin_uml_static_access access,
				   void **host_address);
/* Allocation tags are attached to the shared physical backing rather than a
 * virtual mapping, so aliases in one or more generations observe one tag.
 * Return values use enum darwin_uml_static_translation_result. */
int darwin_uml_static_vm_load_allocation_tag(unsigned long generation,
					      unsigned long address,
					      uint8_t *tag);
int darwin_uml_static_vm_store_allocation_tag(unsigned long generation,
					       unsigned long address,
					       uint8_t tag);
uint64_t darwin_uml_static_vm_changes(unsigned long generation);
struct darwin_uml_static_sequences darwin_uml_static_vm_sequences(
	unsigned long generation);
/* Copies mapping ranges changed after `sequence` and returns their count.
 * A negative result means the caller must invalidate its complete TLB.  The
 * returned current_sequence is an atomic snapshot taken with the ranges.
 */
int darwin_uml_static_vm_mapping_changes(
	unsigned long generation, uint64_t sequence,
	struct darwin_uml_static_mapping_change *changes, size_t capacity,
	uint64_t *current_sequence);
int darwin_uml_static_vm_execution_begin(unsigned long generation,
					 unsigned long guest_pc);
int darwin_uml_static_vm_execution_end(unsigned long generation);
/* A cached execution context keeps the generation alive and avoids resolving
 * thread-local address-space state at every translated-block boundary. */
void *darwin_uml_static_vm_execution_context_create(unsigned long generation);
void darwin_uml_static_vm_execution_context_destroy(void *context);
int darwin_uml_static_vm_execution_context_begin(void *context,
						 unsigned long guest_pc);
int darwin_uml_static_vm_execution_context_end(void *context);
struct darwin_uml_static_sequences darwin_uml_static_vm_execution_context_sequences(
	void *context);
uint64_t darwin_uml_static_vm_code_changes(unsigned long generation);
void darwin_uml_static_vm_invalidate_code(unsigned long address);

#ifdef __cplusplus
}
#endif

#endif
