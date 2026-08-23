/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_EMULATOR_H
#define UML_EMULATOR_H

#include <uml/abi_identity.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*uml_emulator_kernel_entry)(int argc, char *const argv[],
					char *const envp[]);

/* The callback table is copied by uml_emulator_create. Its context remains
 * caller-owned and must stay valid during every run that can invoke it. */
struct uml_emulator_network_callbacks {
	uint32_t struct_size;
	void *context;
	int (*open)(void *context, uint8_t mac[6], unsigned int *mtu);
	int (*irq_fd)(void *context);
	int (*read)(void *context, void *buffer, size_t capacity);
	int (*write)(void *context, const void *buffer, size_t length);
};

/* Public functions returning int use 0 for success and the negated value of
 * one of these positive error constants for API failures. A successful run
 * returns the embedded kernel's status unchanged, so positive values may also
 * be valid uml_emulator_run results. */
enum uml_emulator_error {
	UML_EMULATOR_OK = 0,
	UML_EMULATOR_INVALID_ARGUMENT = 1,
	UML_EMULATOR_OUT_OF_MEMORY = 2,
	UML_EMULATOR_UNSUPPORTED = 3,
	UML_EMULATOR_RUNTIME_ERROR = 4,
};

enum uml_emulator_execution_model {
	UML_EMULATOR_EXECUTION_AUTO = 0,
	UML_EMULATOR_STATIC_GADGETS = 1,
	UML_EMULATOR_NATIVE_JIT = 2,
	UML_EMULATOR_INTERPRETER = 3,
};

struct uml_emulator_config {
	uint32_t abi_version;
	uint32_t struct_size;
	const char *rootfs_path;
	const void *kernel_image;
	size_t kernel_image_size;
	uml_emulator_kernel_entry native_kernel_entry;
	const struct uml_emulator_network_callbacks *network;
};

struct uml_emulator_capabilities {
	uint32_t abi_version;
	uint32_t struct_size;
	enum uml_emulator_execution_model userspace_execution;
	int can_run;
	int kernel_is_native_macho;
	int sandbox_safe;
	const char *platform;
};

struct uml_emulator;

/* Creation copies rootfs_path, kernel_image, and the network callback table.
 * It does not retain the config object or those input buffers. */
int uml_emulator_create(const struct uml_emulator_config *config,
		       struct uml_emulator **result);
/* Destruction must not overlap any other operation on this handle. */
void uml_emulator_destroy(struct uml_emulator *emulator);
/* Capability queries may overlap a run or stop request on the same handle. */
int uml_emulator_get_capabilities(const struct uml_emulator *emulator,
				 struct uml_emulator_capabilities *capabilities);
/* A run is synchronous. It copies argv, envp, and all referenced strings
 * before entering the kernel and retains none of them after returning. At
 * most one run may be active on a handle; concurrent runs fail. Different
 * native-callback-backed handles may run concurrently in a 16 KiB guest-page
 * build when their native mappings do not overlap; an overlapping mapping
 * fails instead of replacing another run's mapping. Native mappings belong
 * to the run and are flushed and unmapped before that run returns. The 4 KiB
 * compatibility build serializes native callbacks
 * process-wide because identical guest addresses materialize in one host
 * address space. Embedded ELF kernel runs are also serialized process-wide
 * because they occupy one relocated native-runtime window at a time and use
 * process-global Linux state. */
int uml_emulator_run(struct uml_emulator *emulator, int argc,
		    const char *const argv[], const char *const envp[]);
/* This and uml_emulator_sync are the operations intended to overlap
 * uml_emulator_run on the same handle. A stop request made while idle is not
 * inherited by a later run. A request
 * made while a 4 KiB native or embedded run is queued at process admission
 * cancels that run with -UML_EMULATOR_RUNTIME_ERROR and is not inherited by a
 * retry. */
void uml_emulator_request_stop(struct uml_emulator *emulator);
/* Flushes every host file the running guest can still write to, so that the
 * writes it has already issued survive the process. Like a stop request, this
 * is intended to overlap uml_emulator_run on the same handle; on an idle
 * handle it flushes whatever the previous run left open. It reports the number
 * of descriptors that could not be flushed, 0 when all of them were, and the
 * negated error constant for API failures. The guest's own page cache is
 * outside this boundary: only the guest kernel can write that back. */
int uml_emulator_sync(struct uml_emulator *emulator);

#ifdef __cplusplus
}
#endif

#endif
