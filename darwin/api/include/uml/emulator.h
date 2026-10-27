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

/* How the guest of a run ended, as uml_emulator_get_run_end reports it. */
enum uml_emulator_run_end_kind {
	/* No run has begun on the handle, or the last one never ran a guest: it
	 * was refused, cancelled while queued, or only printed information. */
	UML_EMULATOR_RUN_END_NONE = 0,
	/* The guest powered off or halted itself. */
	UML_EMULATOR_RUN_END_POWER_OFF = 1,
	/* The guest asked to restart. */
	UML_EMULATOR_RUN_END_RESTART = 2,
	/* uml_emulator_request_stop ended the run; it wins over whatever the
	 * guest said on its way out. */
	UML_EMULATOR_RUN_END_STOPPED = 3,
	/* Any other end: the kernel returned, or the image failed to load,
	 * without saying how the guest ended. */
	UML_EMULATOR_RUN_END_FAILED = 4,
};

struct uml_emulator_run_end {
	/* Set by the caller to sizeof the structure. */
	uint32_t struct_size;
	enum uml_emulator_run_end_kind kind;
	/* The command the guest passed to its restart, cut to 255 bytes;
	 * empty when it gave none and for every other kind. */
	char restart_command[256];
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
 * most one run may be active on a handle; concurrent runs fail. An embedded
 * kernel run is admitted only while no other run is active. Native mappings
 * belong to the run and are flushed and unmapped before that run returns.
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
/* Reports how the guest of the handle's most recent run ended, once that run
 * has returned. A run that begins on the handle starts from
 * UML_EMULATOR_RUN_END_NONE, so a later refused run does not repeat an earlier
 * run's end. Safe to call from any thread, and idle or during a run, which
 * then reports the previous run. -UML_EMULATOR_INVALID_ARGUMENT for a null
 * handle or output, or a struct_size smaller than the structure. */
int uml_emulator_get_run_end(const struct uml_emulator *emulator,
			     struct uml_emulator_run_end *end);
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
/* Puts the guest somewhere it can be abandoned. It has the guest hand its cold
 * page cache and free memory to the host, as uml_emulator_trim_page_cache
 * does, writes the guest's filesystems back, enters full system suspend, and
 * then flushes what the host received. A suspended guest issues no further
 * writes and its monotonic time does not advance, so a host that never runs
 * this process again leaves the disk as it was left.
 * Reports the number of host files that could not be flushed, 0 when all of
 * them were, and the negated error constant for API failures. Pair it with
 * uml_emulator_resume; a guest with no management console is only flushed,
 * exactly as uml_emulator_sync would. */
int uml_emulator_quiesce(struct uml_emulator *emulator);
/* Wakes a quiesced guest. Harmless on a guest that is already running. */
void uml_emulator_resume(struct uml_emulator *emulator);
/* Does what a phone's power key does: ends a system suspend the guest entered
 * by itself, such as Android's once its display is off. A running guest is
 * left as it is, and a guest that uml_emulator_quiesce suspended stays
 * suspended until uml_emulator_resume. Send it after forwarding the key, which
 * the guest then receives once. It may wait for the guest's console, so call
 * it away from a user interface thread. */
void uml_emulator_wake(struct uml_emulator *emulator);
/* Every guest disk is a host file the host caches as well, so the guest's
 * page cache mostly holds memory twice, and only the guest's copy counts
 * against this process. A trimming guest hands its cold page cache to the
 * host every few seconds and pays a host read for each evicted page it reads
 * again, backing off while that happens often. A guest boots with trimming
 * off, which suits a guest in front of its user; turn it on while the guest
 * runs unattended, as in the background, and off again on return. A change
 * made while uml_emulator_quiesce has the guest suspended reaches the guest
 * when uml_emulator_resume wakes it.
 * Returns 0 once the guest has applied the change, -UML_EMULATOR_UNSUPPORTED
 * when no guest console accepts it, as with no running guest or a kernel
 * without trimming, and -UML_EMULATOR_RUNTIME_ERROR when the guest did not
 * answer in time. It may wait for the guest's console, so call it away from a
 * user interface thread. */
int uml_emulator_set_page_cache_trimming(struct uml_emulator *emulator,
					 int enabled);
/* Has the guest trim all of its cold page cache now and hand its free memory
 * to the host, and waits a bounded time for it to finish; whatever trimming
 * is set to stays. Returns as uml_emulator_set_page_cache_trimming does, and
 * -UML_EMULATOR_RUNTIME_ERROR leaves the guest finishing the pass. A guest
 * that uml_emulator_quiesce suspended was trimmed then, and this returns 0
 * without waking it. Call it away from a user interface thread. */
int uml_emulator_trim_page_cache(struct uml_emulator *emulator);

#ifdef __cplusplus
}
#endif

#endif
