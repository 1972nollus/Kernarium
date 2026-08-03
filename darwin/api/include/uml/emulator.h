/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_EMULATOR_H
#define UML_EMULATOR_H

#include <uml/abi_identity.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UML_EMULATOR_ABI_VERSION 3U

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
 * native-callback-backed handles may run concurrently. Embedded ELF kernel
 * runs are serialized process-wide because they occupy a fixed linked address
 * window and use process-global Linux state. */
int uml_emulator_run(struct uml_emulator *emulator, int argc,
		    const char *const argv[], const char *const envp[]);
/* This is the sole operation intended to overlap uml_emulator_run on the same
 * handle. A request made while idle is not inherited by a later run. */
void uml_emulator_request_stop(struct uml_emulator *emulator);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
void uml_emulator_report_jit_gpr_link_profile(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
