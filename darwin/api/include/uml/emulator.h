/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_EMULATOR_H
#define UML_EMULATOR_H

#include <uml/abi_identity.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The callback table is copied by uml_emulator_create. Its context remains
 * caller-owned and must stay valid during every run that can invoke it.
 *
 * read dequeues one whole Ethernet frame into buffer and returns its length,
 * at most capacity. It returns 0 when no frame is pending and a negative errno
 * on failure. The guest drops a frame longer than capacity or shorter than an
 * Ethernet header. */
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

/* Retired selections keep their number, so a value this build no longer
 * produces is never reused for a different engine. */
enum uml_emulator_execution_model {
	UML_EMULATOR_EXECUTION_AUTO = 0,
	UML_EMULATOR_STATIC_GADGETS = 1,
	UML_EMULATOR_NATIVE_JIT = 2,
};

struct uml_emulator_config {
	uint32_t abi_version;
	uint32_t struct_size;
	const char *rootfs_path;
	const void *kernel_image;
	size_t kernel_image_size;
	const struct uml_emulator_network_callbacks *network;
};

struct uml_emulator_capabilities {
	uint32_t abi_version;
	uint32_t struct_size;
	enum uml_emulator_execution_model userspace_execution;
	int can_run;
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
 * most one run may be active on a handle; concurrent runs fail. A process
 * admits one run at a time; a run on another handle queues at process
 * admission until the active run returns. Native mappings belong to the run
 * and are flushed and unmapped before that run returns.
 *
 * A process runs one guest. A kernel claims guest RAM early in its boot, and
 * a process hands guest RAM out once. After a run has claimed it, every later
 * run in the process, on this handle or another, returns
 * -UML_EMULATOR_UNSUPPORTED before it names a console, maps a kernel window
 * or enters a kernel. A run that ends before its kernel claims guest RAM,
 * such as one refused for its arguments or cancelled while queued, leaves the
 * process's guest unspent. */
int uml_emulator_run(struct uml_emulator *emulator, int argc,
		    const char *const argv[], const char *const envp[]);
/* This and uml_emulator_sync are the operations intended to overlap
 * uml_emulator_run on the same handle. A stop request made while idle is not
 * inherited by a later run. A request made while a run is queued at process
 * admission cancels that run with -UML_EMULATOR_RUNTIME_ERROR and is not
 * inherited by a retry. */
void uml_emulator_request_stop(struct uml_emulator *emulator);
/* Flushes every host file the running guest can still write to, so that the
 * writes it has already issued survive the process. Like a stop request, this
 * is intended to overlap uml_emulator_run on the same handle; on an idle
 * handle it flushes whatever the previous run left open. It reports the number
 * of descriptors that could not be flushed, 0 when all of them were, and the
 * negated error constant for API failures. The guest's own page cache is
 * outside this boundary: only the guest kernel can write that back. */
int uml_emulator_sync(struct uml_emulator *emulator);
/* Puts the guest somewhere it can be abandoned. It writes the guest's
 * filesystems back, enters full system suspend, and then flushes what the host
 * received. A suspended guest issues no further writes and its monotonic time
 * does not advance, so a host that never runs this process again leaves the
 * disk as it was left.
 * Reports the number of host files that could not be flushed, 0 when all of
 * them were, and the negated error constant for API failures. Pair it with
 * uml_emulator_resume; a guest with no management console is only flushed,
 * exactly as uml_emulator_sync would. */
int uml_emulator_quiesce(struct uml_emulator *emulator);
/* Wakes a quiesced guest. Harmless on a guest that is already running. */
void uml_emulator_resume(struct uml_emulator *emulator);

#ifdef __cplusplus
}
#endif

#endif
