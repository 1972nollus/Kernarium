/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_BACKEND_H
#define UML_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UML_BACKEND_ABI_VERSION 2U

typedef int (*uml_backend_kernel_entry)(int argc, char *const argv[],
					char *const envp[]);

struct uml_backend_network_callbacks {
	uint32_t struct_size;
	void *context;
	int (*open)(void *context, uint8_t mac[6], unsigned int *mtu);
	int (*irq_fd)(void *context);
	int (*read)(void *context, void *buffer, size_t capacity);
	int (*write)(void *context, const void *buffer, size_t length);
};

enum uml_backend_error {
	UML_BACKEND_OK = 0,
	UML_BACKEND_INVALID_ARGUMENT = 1,
	UML_BACKEND_OUT_OF_MEMORY = 2,
	UML_BACKEND_UNSUPPORTED = 3,
	UML_BACKEND_RUNTIME_ERROR = 4,
};

enum uml_backend_execution_model {
	UML_BACKEND_EXECUTION_AUTO = 0,
	UML_BACKEND_STATIC_GADGETS = 1,
	UML_BACKEND_NATIVE_JIT = 2,
	UML_BACKEND_INTERPRETER = 3,
};

struct uml_backend_config {
	uint32_t abi_version;
	uint32_t struct_size;
	const char *rootfs_path;
	const void *kernel_image;
	size_t kernel_image_size;
	uml_backend_kernel_entry native_kernel_entry;
	const struct uml_backend_network_callbacks *network;
};

struct uml_backend_capabilities {
	uint32_t abi_version;
	uint32_t struct_size;
	enum uml_backend_execution_model userspace_execution;
	int can_run;
	int kernel_is_native_macho;
	int sandbox_safe;
	const char *platform;
};

struct uml_backend;

int uml_backend_create(const struct uml_backend_config *config,
		       struct uml_backend **result);
void uml_backend_destroy(struct uml_backend *backend);
int uml_backend_get_capabilities(const struct uml_backend *backend,
				 struct uml_backend_capabilities *capabilities);
int uml_backend_run(struct uml_backend *backend, int argc,
		    const char *const argv[], const char *const envp[]);
void uml_backend_request_stop(struct uml_backend *backend);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
void uml_backend_report_jit_gpr_link_profile(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
