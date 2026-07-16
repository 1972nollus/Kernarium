/* SPDX-License-Identifier: GPL-2.0 */
#include "../include/uml_darwin_runtime.h"
#include "direct_gateway.h"

#include <errno.h>
#include <capstone.h>
#include <arm64.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define GUEST_PAGE_SIZE 4096UL
#define GUEST_ADDRESS_LIMIT 0x600000000000ULL
#define LINUX_SVC_0 0xd4000001U
#define UML_SYSCALL_BRK 0xd4209880U /* brk #0x4c4 */
#define AARCH64_MSR_TPIDR_EL0 0xd51bd040U
#define AARCH64_MRS_TPIDR_EL0 0xd53bd040U
#define UML_TLS_WRITE_BRK_BASE 0xd4209a00U /* brk #(0x4d0 + Xt) */
#define UML_TLS_READ_BRK_BASE 0xd4209e00U  /* brk #(0x4f0 + Xt) */
#define UML_X18_BRK 0xd420ac00U            /* brk #0x560 */
#define UML_INDIRECT_BRK 0xd420ae00U        /* brk #0x570 */
#define MAPPING_HASH_SIZE 16384U
#define BACKING_HASH_SIZE 16384U
#define KNOWN_CODE_HASH_SIZE 16384U
#define CODE_CACHE_HASH_SIZE 4096U
#define SYSCALL_STUB_HASH_SIZE 4096U
#define TLS_STUB_HASH_SIZE 4096U
#define INDIRECT_STUB_HASH_SIZE 4096U
#define X18_STUB_HASH_SIZE 4096U
#define SYSCALL_GATEWAY_REGION_SIZE (128UL * 1024 * 1024)
#define SYSCALL_GATEWAY_REGION_MIDDLE (SYSCALL_GATEWAY_REGION_SIZE / 2)
#define SYSCALL_STUB_SIZE 88U
#define TLS_STUB_SIZE 32U
#define INDIRECT_STUB_TARGETS 12U
#define INDIRECT_STUB_HIT_OFFSET (12U * INDIRECT_STUB_TARGETS + 4U)
#define INDIRECT_STUB_SLOW_OFFSET (INDIRECT_STUB_HIT_OFFSET + 12U)
#define INDIRECT_DIRECT_RESUME_OFFSET 36U
#define INDIRECT_DIRECT_SIZE 88U
#define INDIRECT_STUB_TARGET_BASE \
	(INDIRECT_STUB_SLOW_OFFSET + INDIRECT_DIRECT_SIZE)
#define INDIRECT_STUB_RETURN_OFFSET \
	(INDIRECT_STUB_TARGET_BASE + 8U * INDIRECT_STUB_TARGETS)
#define INDIRECT_STUB_SIZE (INDIRECT_STUB_RETURN_OFFSET + 8U)
#define X18_STUB_SIZE 48U
#define DIRECT_SYSCALL_PROMOTION_THRESHOLD 1U

extern void darwin_uml_direct_syscall(void);
extern void darwin_uml_direct_indirect(void);
extern struct direct_gateway_state *darwin_uml_active_direct_gateway;

struct page_mapping {
	uintptr_t address;
	unsigned long generation;
	unsigned long long offset;
	int fd;
	int protection;
	int guest_code;
	int guest_writable;
	int active;
	int loaded;
	int discard_changes;
	int special_scanned;
	int instrumentation_valid;
	int write_validated;
	int writable_exposed;
	unsigned long refresh_epoch;
	unsigned char *original;
	unsigned char syscall_patched[GUEST_PAGE_SIZE / 32];
	unsigned char syscall_direct_patched[GUEST_PAGE_SIZE / 32];
	unsigned char syscall_trap_count[GUEST_PAGE_SIZE / sizeof(uint32_t)];
	unsigned char tls_write_patched[GUEST_PAGE_SIZE / 32];
	unsigned char tls_read_patched[GUEST_PAGE_SIZE / 32];
	unsigned char x18_patched[GUEST_PAGE_SIZE / 32];
	unsigned char indirect_patched[GUEST_PAGE_SIZE / 32];
	unsigned char code_known[GUEST_PAGE_SIZE / 32];
	unsigned char pending_target[GUEST_PAGE_SIZE / 32];
	struct page_mapping *next;
	struct page_mapping *previous;
	struct page_mapping *hash_next;
	struct page_mapping *backing_hash_next;
};

struct staged_quarter {
	uintptr_t address;
	unsigned long generation;
	int valid;
	unsigned char page[GUEST_PAGE_SIZE];
};

struct known_code_target {
	uintptr_t address;
	unsigned long generation;
	int resolved;
	struct known_code_target *hash_next;
	struct known_code_target *page_hash_next;
	struct known_code_target *slot_hash_next;
	struct known_code_target *all_next;
};

struct pending_slot {
	uintptr_t address;
};

struct dirty_slot {
	uintptr_t address;
	unsigned long generation;
};

struct code_cache_entry;

struct indirect_history_target {
	int64_t delta;
	unsigned int page_offset;
	struct code_cache_entry *target_page;
};

struct indirect_history_site {
	unsigned int index;
	uint32_t original;
	unsigned int target_count;
	struct indirect_history_target targets[INDIRECT_STUB_TARGETS];
	struct indirect_history_site *next;
};

struct code_cache_entry {
	uint64_t hash;
	int instrumentation_valid;
	int special_scanned;
	unsigned char original[GUEST_PAGE_SIZE];
	unsigned char syscall_patched[GUEST_PAGE_SIZE / 32];
	unsigned char tls_write_patched[GUEST_PAGE_SIZE / 32];
	unsigned char tls_read_patched[GUEST_PAGE_SIZE / 32];
	unsigned char x18_patched[GUEST_PAGE_SIZE / 32];
	unsigned char indirect_patched[GUEST_PAGE_SIZE / 32];
	unsigned char code_known[GUEST_PAGE_SIZE / 32];
	unsigned char pending_target[GUEST_PAGE_SIZE / 32];
	struct indirect_history_site *indirect_history;
	struct code_cache_entry *next;
};

struct backing_view {
	int fd;
	size_t length;
	unsigned char *base;
	struct backing_view *next;
};

struct syscall_gateway_region {
	uintptr_t key;
	uintptr_t address;
	uintptr_t writable_page;
	size_t used;
	size_t capacity;
	struct syscall_gateway_region *next;
};

struct syscall_stub {
	uintptr_t site;
	uintptr_t address;
	struct syscall_stub *hash_next;
};

struct tls_stub {
	uintptr_t site;
	uintptr_t address;
	struct tls_stub *hash_next;
};

struct indirect_stub {
	uintptr_t site;
	uintptr_t address;
	unsigned long generation;
	uint32_t original;
	struct code_cache_entry *source_page;
	uint64_t cached_targets[INDIRECT_STUB_TARGETS];
	unsigned int target_count;
	struct indirect_stub *site_hash_next;
	struct indirect_stub *slow_hash_next;
	struct indirect_stub *inactive_next;
};

struct x18_stub {
	uintptr_t site;
	uintptr_t address;
	uintptr_t native_pc;
	unsigned long generation;
	uint32_t original;
	unsigned int scratch;
	struct x18_stub *hash_next;
	struct x18_stub *fault_hash_next;
	struct x18_stub *all_next;
};

static struct page_mapping *mappings;
static struct page_mapping *mapping_hash[MAPPING_HASH_SIZE];
static struct page_mapping *backing_hash[BACKING_HASH_SIZE];
static unsigned long active_generation;
static size_t darwin_page_size;
static unsigned long current_refresh_epoch;
static struct staged_quarter staged_stack;
static struct known_code_target *known_code_hash[KNOWN_CODE_HASH_SIZE];
static struct known_code_target *known_code_page_hash[KNOWN_CODE_HASH_SIZE];
static struct known_code_target *known_code_slot_hash[KNOWN_CODE_HASH_SIZE];
static struct known_code_target *known_code_targets;
static struct code_cache_entry *code_cache_hash[CODE_CACHE_HASH_SIZE];
static struct backing_view *backing_views;
static csh arm64_disassembler;
static int arm64_disassembler_ready;
static struct pending_slot *pending_slots;
static size_t pending_slot_count;
static size_t pending_slot_capacity;
static struct dirty_slot *dirty_slots;
static size_t dirty_slot_count;
static size_t dirty_slot_capacity;
static int dirty_slot_overflow;
static unsigned int mapping_batch_depth;
static struct syscall_gateway_region *syscall_gateway_regions;
static struct syscall_stub *syscall_stub_hash[SYSCALL_STUB_HASH_SIZE];
static struct tls_stub *tls_stub_hash[TLS_STUB_HASH_SIZE];
static struct indirect_stub *indirect_stub_site_hash[INDIRECT_STUB_HASH_SIZE];
static struct indirect_stub *indirect_stub_slow_hash[INDIRECT_STUB_HASH_SIZE];
static struct indirect_stub *inactive_indirect_stubs;
static struct x18_stub *x18_stub_hash[X18_STUB_HASH_SIZE];
static struct x18_stub *x18_stub_fault_hash[X18_STUB_HASH_SIZE];
static struct x18_stub *x18_stubs;
static int gateway_profile_reported;
static int gateway_profile_registered;
static unsigned long syscall_stub_count;
static unsigned long tls_stub_count;
static unsigned long indirect_stub_count;
static unsigned long x18_stub_count;
static unsigned long vm_profile_loads;
static uint64_t vm_profile_total;
static uint64_t vm_profile_flush;
static uint64_t vm_profile_prepare;
static uint64_t vm_profile_read;
static uint64_t vm_profile_populate;
static uint64_t vm_profile_protect;
static int vm_profile_mode = -1;
static unsigned int gateway_failure_count;
static int direct_syscall_mode = -1;

static int instrument_guest_code(uintptr_t entry);
static int instrument_known_code_in_slot(uintptr_t slot);
static unsigned int sanitize_mapping_instrumentation(
	struct page_mapping *mapping);
static unsigned int sanitize_cached_instrumentation(
	struct code_cache_entry *entry, uintptr_t address);
static uintptr_t direct_branch_target(uintptr_t address,
				      uint32_t immediate, unsigned int bits);
static uintptr_t tls_gateway_stub(uintptr_t site, unsigned int destination);
static uint32_t indirect_replacement(uintptr_t site,
				     unsigned long generation);
static uintptr_t warm_indirect_gateway(uintptr_t site,
				       unsigned long generation);

static void report_gateway_profile(void)
{
	mach_timebase_info_data_t timebase;

	(void)mach_timebase_info(&timebase);
	fprintf(stderr,
		"Darwin gateway stubs: syscall=%lu tls_read=%lu indirect=%lu "
		"x18=%lu\n",
		syscall_stub_count, tls_stub_count, indirect_stub_count,
		x18_stub_count);
	fprintf(stderr,
		"Darwin VM load ticks: count=%lu total=%llu flush=%llu "
		"prepare=%llu read=%llu populate=%llu protect=%llu "
		"timebase=%u/%u\n",
		vm_profile_loads, (unsigned long long)vm_profile_total,
		(unsigned long long)vm_profile_flush,
		(unsigned long long)vm_profile_prepare,
		(unsigned long long)vm_profile_read,
		(unsigned long long)vm_profile_populate,
		(unsigned long long)vm_profile_protect,
		timebase.numer, timebase.denom);
}

static void profile_gateway_failure(const char *kind, uintptr_t site,
				    uintptr_t stub)
{
	if (getenv("UML_PROFILE_TRAPS") != NULL &&
	    gateway_failure_count++ < 8)
		fprintf(stderr, "Darwin gateway failure: %s site=%p stub=%p\n",
			kind, (void *)site, (void *)stub);
}

static uintptr_t align_down(uintptr_t value, size_t alignment)
{
	return value & ~(uintptr_t)(alignment - 1);
}

static int direct_gateway_enabled(void)
{
	if (direct_syscall_mode < 0)
		direct_syscall_mode = getenv("UML_NO_DIRECT_GATEWAY") == NULL;
	return direct_syscall_mode;
}

static int vm_profile_enabled(void)
{
	if (vm_profile_mode < 0)
		vm_profile_mode = getenv("UML_PROFILE_TRAPS") != NULL;
	return vm_profile_mode;
}

static int aarch64_branch(uintptr_t from, uintptr_t to, uint32_t *instruction)
{
	int64_t offset = (int64_t)(to - from);

	if ((offset & 3) != 0 || offset < -(1LL << 27) ||
	    offset >= (1LL << 27))
		return -1;
	*instruction = 0x14000000U |
		(uint32_t)(((uint64_t)(offset >> 2)) & 0x03ffffffU);
	return 0;
}

static uint32_t aarch64_str64(unsigned int source, unsigned int base,
			      unsigned int offset)
{
	return 0xf9000000U | ((offset / 8U) << 10) | (base << 5) | source;
}

static uint32_t aarch64_ldr64(unsigned int destination, unsigned int base,
			      unsigned int offset)
{
	return 0xf9400000U | ((offset / 8U) << 10) | (base << 5) |
		destination;
}

static uint32_t tls_write_replacement(unsigned int source)
{
	return UML_TLS_WRITE_BRK_BASE + (source << 5);
}

static uint32_t tls_read_replacement(uintptr_t site,
				     unsigned int destination)
{
	uintptr_t stub;
	uint32_t instruction;

	if (destination == 31)
		return 0xd503201fU; /* nop: MRS XZR discards the value */
	if (destination != 18) {
		stub = tls_gateway_stub(site, destination);
		if (stub != 0 && aarch64_branch(site, stub, &instruction) == 0)
			return instruction;
	}
	return UML_TLS_READ_BRK_BASE + (destination << 5);
}

static int aarch64_ldr_literal(unsigned int destination, uintptr_t from,
			       uintptr_t to, uint32_t *instruction)
{
	int64_t offset = (int64_t)(to - from);

	if ((offset & 3) != 0 || offset < -(1LL << 20) ||
	    offset >= (1LL << 20))
		return -1;
	*instruction = 0x58000000U |
		((uint32_t)(offset >> 2) & 0x7ffffU) << 5 |
		destination;
	return 0;
}

static int aarch64_adr(unsigned int destination, uintptr_t from,
		       uintptr_t to, uint32_t *instruction)
{
	int64_t offset = (int64_t)(to - from);
	uint32_t immediate;

	if (offset < -(1LL << 20) || offset >= (1LL << 20))
		return -1;
	immediate = (uint32_t)offset & 0x1fffffU;
	*instruction = 0x10000000U | ((immediate & 3U) << 29) |
		(((immediate >> 2) & 0x7ffffU) << 5) | destination;
	return 0;
}

static uint32_t aarch64_eor64(unsigned int destination, unsigned int left,
			      unsigned int right)
{
	return 0xca000000U | (right << 16) | (left << 5) | destination;
}

static int aarch64_cbnz64(unsigned int reg, uintptr_t from, uintptr_t to,
			  uint32_t *instruction)
{
	int64_t offset = (int64_t)(to - from);

	if ((offset & 3) != 0 || offset < -(1LL << 20) ||
	    offset >= (1LL << 20))
		return -1;
	*instruction = 0xb5000000U |
		((uint32_t)(offset >> 2) & 0x7ffffU) << 5 | reg;
	return 0;
}

static int aarch64_cbz64(unsigned int reg, uintptr_t from, uintptr_t to,
			 uint32_t *instruction)
{
	int64_t offset = (int64_t)(to - from);

	if ((offset & 3) != 0 || offset < -(1LL << 20) ||
	    offset >= (1LL << 20))
		return -1;
	*instruction = 0xb4000000U |
		((uint32_t)(offset >> 2) & 0x7ffffU) << 5 | reg;
	return 0;
}

static struct syscall_gateway_region *find_syscall_gateway_region(
		uintptr_t site)
{
	struct syscall_gateway_region *region;
	uintptr_t key = align_down(site, SYSCALL_GATEWAY_REGION_SIZE);
	uintptr_t middle = key + SYSCALL_GATEWAY_REGION_MIDDLE;
	uintptr_t address = middle;
	unsigned int attempt;

	if (darwin_page_size == 0)
		darwin_page_size = (size_t)getpagesize();
	if (!gateway_profile_registered &&
	    getenv("UML_PROFILE_TRAPS") != NULL) {
		atexit(report_gateway_profile);
		gateway_profile_registered = 1;
	}
	middle = align_down(middle, darwin_page_size);
	for (region = syscall_gateway_regions; region != NULL;
	     region = region->next)
		if (region->key == key)
			return region;
	/* mincore() is not a reliable occupancy probe on Darwin: it can succeed
	 * for addresses that vmmap reports as holes.  A fixed Mach allocation is
	 * atomic and, unlike MAP_FIXED, never replaces an existing mapping.  A
	 * collision simply tries the next nearby host-page slot.
	 */
	for (attempt = 0; attempt < 2048; attempt++) {
		intptr_t slots = attempt == 0 ? 0 :
			(attempt & 1U ? (intptr_t)((attempt + 1) / 2) :
			 -(intptr_t)(attempt / 2));
		mach_vm_address_t candidate;

		address = middle + slots * (intptr_t)darwin_page_size;
		candidate = (mach_vm_address_t)address;
		if (mach_vm_allocate(mach_task_self(), &candidate,
				     (mach_vm_size_t)darwin_page_size,
				     VM_FLAGS_FIXED) == KERN_SUCCESS)
			break;
	}
	if (attempt == 2048) {
		if (!gateway_profile_reported && getenv("UML_PROFILE_TRAPS") != NULL) {
			fprintf(stderr,
				"Darwin gateway: no free slot near region=%p\n",
				(void *)key);
			gateway_profile_reported = 1;
		}
		return NULL;
	}
	if (mprotect((void *)address, darwin_page_size,
		     PROT_READ | PROT_WRITE) < 0) {
		(void)mach_vm_deallocate(mach_task_self(),
					 (mach_vm_address_t)address,
					 (mach_vm_size_t)darwin_page_size);
		return NULL;
	}
	if (!gateway_profile_reported && getenv("UML_PROFILE_TRAPS") != NULL) {
		fprintf(stderr, "Darwin gateway: reserved candidate=%p\n",
			(void *)address);
		gateway_profile_reported = 1;
	}
	region = calloc(1, sizeof(*region));
	if (region == NULL) {
		(void)mach_vm_deallocate(mach_task_self(),
					 (mach_vm_address_t)address,
					 (mach_vm_size_t)darwin_page_size);
		return NULL;
	}
	region->key = key;
	region->address = address;
	region->capacity = darwin_page_size;
	region->next = syscall_gateway_regions;
	syscall_gateway_regions = region;
	return region;
}

static int grow_syscall_gateway_region(struct syscall_gateway_region *region,
				       size_t required)
{
	while (required > region->capacity) {
		uintptr_t address = region->address + region->capacity;
		mach_vm_address_t candidate = (mach_vm_address_t)address;

		if (mach_vm_allocate(mach_task_self(), &candidate,
				     (mach_vm_size_t)darwin_page_size,
				     VM_FLAGS_FIXED) != KERN_SUCCESS)
			return -1;
		if (mprotect((void *)address, darwin_page_size,
			     PROT_READ | PROT_WRITE) < 0) {
			(void)mach_vm_deallocate(mach_task_self(), candidate,
						 (mach_vm_size_t)darwin_page_size);
			return -1;
		}
		region->capacity += darwin_page_size;
	}
	return 0;
}

static struct syscall_gateway_region *find_gateway_region_address(
							uintptr_t address)
{
	struct syscall_gateway_region *region;

	for (region = syscall_gateway_regions; region != NULL;
	     region = region->next)
		if (address >= region->address &&
		    address < region->address + region->capacity)
			return region;
	return NULL;
}

/*
 * Stub generation happens while the UP UML kernel owns execution.  Keep the
 * current gateway page writable while a batch of guest code is translated,
 * then seal every dirty page before returning to guest mode.  This preserves
 * W^X without paying two host mprotect calls for every individual stub.
 */
static int gateway_make_writable(struct syscall_gateway_region *region,
				 uintptr_t page)
{
	if (region->writable_page == page)
		return 0;
	if (region->writable_page != 0) {
		if (mprotect((void *)region->writable_page, darwin_page_size,
			     PROT_READ | PROT_EXEC) < 0)
			return -1;
		region->writable_page = 0;
	}
	if (mprotect((void *)page, darwin_page_size,
		     PROT_READ | PROT_WRITE) < 0)
		return -1;
	region->writable_page = page;
	return 0;
}

static int seal_gateway_pages(void)
{
	struct syscall_gateway_region *region;
	int result = 0;

	for (region = syscall_gateway_regions; region != NULL;
	     region = region->next) {
		if (region->writable_page == 0)
			continue;
		if (mprotect((void *)region->writable_page, darwin_page_size,
			     PROT_READ | PROT_EXEC) < 0) {
			result = -1;
			continue;
		}
		region->writable_page = 0;
	}
	return result;
}

static uintptr_t syscall_gateway_stub(uintptr_t site)
{
	struct syscall_gateway_region *region;
	struct syscall_stub *record;
	uint32_t *instructions;
	uintptr_t stub;
	uintptr_t stub_page;
	uintptr_t guest_pc = site + sizeof(uint32_t);
	uint64_t target = (uintptr_t)darwin_uml_direct_syscall;
	uint64_t active_gateway =
		(uintptr_t)&darwin_uml_active_direct_gateway;
	const char *failure = "encoding";
	unsigned int bucket = (unsigned int)(site >> 2) &
		(SYSCALL_STUB_HASH_SIZE - 1);

	for (record = syscall_stub_hash[bucket]; record != NULL;
	     record = record->hash_next)
		if (record->site == site)
			return record->address;
	region = find_syscall_gateway_region(site);
	if (region != NULL &&
	    region->used % darwin_page_size > darwin_page_size - SYSCALL_STUB_SIZE)
		region->used = (region->used + darwin_page_size - 1) &
			~(darwin_page_size - 1);
	if (region == NULL || grow_syscall_gateway_region(region,
		region->used + SYSCALL_STUB_SIZE) < 0) {
		profile_gateway_failure("reserve", site, 0);
		return 0;
	}
	stub = region->address + region->used;
	stub_page = align_down(stub, darwin_page_size);
	if (gateway_make_writable(region, stub_page) < 0) {
		profile_gateway_failure("write protect", site, stub);
		return 0;
	}
	instructions = (uint32_t *)stub;
	/* x18 is runtime-owned while guest code executes, so it can acquire the
	 * active direct state without borrowing a guest register.  x16 then
	 * spills into ordinary runtime data and the generated page remains W^X.
	 * The return thunk restores x16 and uses an immediate branch, leaving x18
	 * as the runtime state pointer just like the regular Mach resume path.
	 */
	failure = "active literal";
	if (aarch64_ldr_literal(18, stub, stub + 64,
				&instructions[0]) < 0)
		goto fail;
	instructions[1] = aarch64_ldr64(18, 18, 0);
	instructions[2] = aarch64_str64(16, 18, DG_SCRATCH_X16);
	failure = "resume address";
	if (aarch64_adr(16, stub + 12, stub + 36,
			&instructions[3]) < 0)
		goto fail;
	instructions[4] = aarch64_str64(16, 18, DG_RESUME_STUB);
	failure = "guest pc literal";
	if (aarch64_ldr_literal(16, stub + 20, stub + 72,
				&instructions[5]) < 0)
		goto fail;
	instructions[6] = aarch64_str64(16, 18, DG_GUEST_PC);
	failure = "target literal";
	if (aarch64_ldr_literal(16, stub + 28, stub + 80,
				&instructions[7]) < 0)
		goto fail;
	instructions[8] = 0xd61f0200U; /* br x16 */
	if (aarch64_ldr_literal(18, stub + 36, stub + 64,
				&instructions[9]) < 0)
		goto fail;
	instructions[10] = aarch64_ldr64(18, 18, 0);
	instructions[11] = aarch64_ldr64(16, 18, DG_REGISTERS);
	instructions[12] = aarch64_ldr64(16, 16, ARM64_STATE_X16);
	failure = "resume branch";
	if (aarch64_branch(stub + 52, guest_pc, &instructions[13]) < 0)
		goto fail;
	for (unsigned int index = 14; index < 16; index++)
		instructions[index] = 0xd503201fU;
	memcpy((void *)(stub + 64), &active_gateway, sizeof(active_gateway));
	memcpy((void *)(stub + 72), &guest_pc, sizeof(guest_pc));
	memcpy((void *)(stub + 80), &target, sizeof(target));
	__builtin___clear_cache((char *)stub,
				  (char *)(stub + SYSCALL_STUB_SIZE));
	record = malloc(sizeof(*record));
	if (record == NULL) {
		profile_gateway_failure("record allocation", site, stub);
		return 0;
	}
	record->site = site;
	record->address = stub;
	record->hash_next = syscall_stub_hash[bucket];
	syscall_stub_hash[bucket] = record;
	region->used += SYSCALL_STUB_SIZE;
	syscall_stub_count++;
	return stub;
fail:
	profile_gateway_failure(failure, site, stub);
	(void)seal_gateway_pages();
	return 0;
}

static uintptr_t tls_gateway_stub(uintptr_t site, unsigned int destination)
{
	struct syscall_gateway_region *region;
	struct tls_stub *record;
	uint32_t *instructions;
	uintptr_t stub;
	uintptr_t stub_page;
	uintptr_t guest_pc = site + sizeof(uint32_t);
	uint64_t active_gateway =
		(uintptr_t)&darwin_uml_active_direct_gateway;
	unsigned int bucket = (unsigned int)(site >> 2) &
		(TLS_STUB_HASH_SIZE - 1);

	for (record = tls_stub_hash[bucket]; record != NULL;
	     record = record->hash_next)
		if (record->site == site)
			return record->address;
	region = find_syscall_gateway_region(site);
	if (region != NULL &&
	    region->used % darwin_page_size > darwin_page_size - TLS_STUB_SIZE)
		region->used = (region->used + darwin_page_size - 1) &
			~(darwin_page_size - 1);
	if (region == NULL || grow_syscall_gateway_region(region,
		region->used + TLS_STUB_SIZE) < 0)
		return 0;
	stub = region->address + region->used;
	stub_page = align_down(stub, darwin_page_size);
	if (gateway_make_writable(region, stub_page) < 0)
		return 0;
	instructions = (uint32_t *)stub;
	if (aarch64_ldr_literal(destination, stub, stub + 16,
				&instructions[0]) < 0)
		goto fail;
	instructions[1] = aarch64_ldr64(destination, destination, 0);
	instructions[2] = aarch64_ldr64(destination, destination, DG_GUEST_TLS);
	if (aarch64_branch(stub + 12, guest_pc, &instructions[3]) < 0)
		goto fail;
	memcpy((void *)(stub + 16), &active_gateway, sizeof(active_gateway));
	memset((void *)(stub + 24), 0, 8);
	__builtin___clear_cache((char *)stub,
				  (char *)(stub + TLS_STUB_SIZE));
	record = malloc(sizeof(*record));
	if (record == NULL)
		return 0;
	record->site = site;
	record->address = stub;
	record->hash_next = tls_stub_hash[bucket];
	tls_stub_hash[bucket] = record;
	region->used += TLS_STUB_SIZE;
	tls_stub_count++;
	return stub;
fail:
	(void)seal_gateway_pages();
	return 0;
}

static unsigned int indirect_site_hash_index(uintptr_t site,
					     unsigned long generation)
{
	uint64_t key = ((uint64_t)site >> 2) ^
		((uint64_t)generation * 0x9e3779b97f4a7c15ULL);

	key ^= key >> 33;
	return (unsigned int)key & (INDIRECT_STUB_HASH_SIZE - 1);
}

static unsigned int indirect_slow_hash_index(uintptr_t slow)
{
	uint64_t key = (uint64_t)slow >> 2;

	key ^= key >> 33;
	return (unsigned int)key & (INDIRECT_STUB_HASH_SIZE - 1);
}

static struct indirect_stub *find_indirect_stub(uintptr_t site,
						unsigned long generation)
{
	struct indirect_stub *record;
	unsigned int bucket = indirect_site_hash_index(site, generation);

	for (record = indirect_stub_site_hash[bucket]; record != NULL;
	     record = record->site_hash_next)
		if (record->site == site && record->generation == generation)
			return record;
	return NULL;
}

static struct indirect_stub *find_indirect_slow_stub(uintptr_t slow)
{
	struct indirect_stub *record;
	unsigned int bucket = indirect_slow_hash_index(slow);

	for (record = indirect_stub_slow_hash[bucket]; record != NULL;
	     record = record->slow_hash_next)
		if (record->address + INDIRECT_STUB_SLOW_OFFSET == slow)
			return record;
	return NULL;
}

static struct indirect_stub *take_inactive_indirect_stub(
		uintptr_t site, uint32_t original,
		struct code_cache_entry *source_page)
{
	struct indirect_stub **link;

	for (link = &inactive_indirect_stubs; *link != NULL;
	     link = &(*link)->inactive_next) {
		struct indirect_stub *record = *link;

		if (record->site != site || record->original != original ||
		    record->source_page != source_page)
			continue;
		*link = record->inactive_next;
		record->inactive_next = NULL;
		return record;
	}
	return NULL;
}

static void retire_indirect_stub(struct indirect_stub *record)
{
	record->generation = 0;
	record->target_count = 0;
	record->site_hash_next = NULL;
	record->slow_hash_next = NULL;
	record->inactive_next = inactive_indirect_stubs;
	inactive_indirect_stubs = record;
}

static uint64_t indirect_target_value(const uint64_t *targets,
				      unsigned int target_count,
				      unsigned int target_index)
{
	if (target_count == 0)
		return 0;
	return targets[target_index < target_count ? target_index : 0];
}

static uintptr_t indirect_gateway_stub(uintptr_t site,
				       unsigned long generation,
				       uint32_t original,
				       struct code_cache_entry *source_page,
				       const uint64_t *targets,
				       unsigned int target_count)
{
	struct syscall_gateway_region *region;
	struct indirect_stub *record;
	uint32_t *instructions;
	uintptr_t stub;
	uintptr_t stub_page;
	uint64_t return_pc = site + sizeof(uint32_t);
	uint64_t active_gateway =
		(uintptr_t)&darwin_uml_active_direct_gateway;
	uint64_t direct_target = (uintptr_t)darwin_uml_direct_indirect;
	unsigned int reg = (original >> 5) & 31U;
	int is_call = (original & 0xfffffc1fU) == 0xd63f0000U;
	unsigned int site_bucket;
	unsigned int slow_bucket;
	unsigned int target_index;
	unsigned int compare_end = 3U * INDIRECT_STUB_TARGETS;
	unsigned int hit_index = INDIRECT_STUB_HIT_OFFSET / sizeof(uint32_t);
	unsigned int slow_index = INDIRECT_STUB_SLOW_OFFSET / sizeof(uint32_t);
	uintptr_t slow;

	if (((original & 0xfffffc1fU) != 0xd61f0000U && !is_call) ||
	    reg == 18 || reg == 31 || source_page == NULL ||
	    target_count > INDIRECT_STUB_TARGETS)
		return 0;
	record = find_indirect_stub(site, generation);
	if (record != NULL)
		return record->address;
	record = take_inactive_indirect_stub(site, original, source_page);
	if (record != NULL) {
		stub = record->address;
		region = find_gateway_region_address(stub);
		stub_page = align_down(stub, darwin_page_size);
		if (region == NULL ||
		    gateway_make_writable(region, stub_page) < 0) {
			retire_indirect_stub(record);
			return 0;
		}
		for (target_index = 0; target_index < INDIRECT_STUB_TARGETS;
		     target_index++) {
			uint64_t target = indirect_target_value(targets,
				target_count, target_index);

			memcpy((void *)(stub + INDIRECT_STUB_TARGET_BASE +
				target_index * sizeof(target)), &target,
			       sizeof(target));
			record->cached_targets[target_index] = target;
		}
		record->generation = generation;
		record->target_count = target_count;
		site_bucket = indirect_site_hash_index(site, generation);
		slow_bucket = indirect_slow_hash_index(
			stub + INDIRECT_STUB_SLOW_OFFSET);
		record->site_hash_next = indirect_stub_site_hash[site_bucket];
		indirect_stub_site_hash[site_bucket] = record;
		record->slow_hash_next = indirect_stub_slow_hash[slow_bucket];
		indirect_stub_slow_hash[slow_bucket] = record;
		indirect_stub_count++;
		return stub;
	}
	region = find_syscall_gateway_region(site);
	if (region != NULL &&
	    region->used % darwin_page_size >
		darwin_page_size - INDIRECT_STUB_SIZE)
		region->used = (region->used + darwin_page_size - 1) &
			~(darwin_page_size - 1);
	if (region == NULL || grow_syscall_gateway_region(region,
		region->used + INDIRECT_STUB_SIZE) < 0)
		return 0;
	stub = region->address + region->used;
	record = malloc(sizeof(*record));
	if (record == NULL)
		return 0;
	slow = stub + INDIRECT_STUB_SLOW_OFFSET;
	stub_page = align_down(stub, darwin_page_size);
	if (gateway_make_writable(region, stub_page) < 0)
		goto fail_record;
	instructions = (uint32_t *)stub;
	for (target_index = 0; target_index < INDIRECT_STUB_TARGETS;
	     target_index++) {
		unsigned int instruction_index = target_index * 3;
		uintptr_t instruction_address = stub + instruction_index * 4;
		uintptr_t target_address = stub + INDIRECT_STUB_TARGET_BASE +
			target_index * sizeof(uint64_t);

		if (aarch64_ldr_literal(18, instruction_address,
					target_address,
					&instructions[instruction_index]) < 0)
			goto fail_page;
		instructions[instruction_index + 1] =
			aarch64_eor64(18, 18, reg);
		if (target_index + 1 == INDIRECT_STUB_TARGETS) {
			if (aarch64_cbnz64(18, instruction_address + 8,
					    stub + INDIRECT_STUB_SLOW_OFFSET,
					    &instructions[instruction_index + 2]) < 0)
				goto fail_page;
		} else if (aarch64_cbz64(18, instruction_address + 8,
					   stub + INDIRECT_STUB_HIT_OFFSET,
					   &instructions[instruction_index + 2]) < 0) {
			goto fail_page;
		}
	}
	instructions[compare_end] = 0xd503201fU;
	if (!is_call) {
		instructions[hit_index] = 0xd61f0000U | (reg << 5); /* br Xn */
		instructions[hit_index + 1] = 0xd503201fU;
		instructions[hit_index + 2] = 0xd503201fU;
	} else if (reg == 30) {
		instructions[hit_index] = 0xaa1e03f2U; /* mov x18, x30 */
		if (aarch64_ldr_literal(30,
					stub + (hit_index + 1) * 4,
					stub + INDIRECT_STUB_RETURN_OFFSET,
					&instructions[hit_index + 1]) < 0)
			goto fail_page;
		instructions[hit_index + 2] = 0xd61f0240U; /* br x18 */
	} else {
		if (aarch64_ldr_literal(30, stub + hit_index * 4,
					stub + INDIRECT_STUB_RETURN_OFFSET,
					&instructions[hit_index]) < 0)
			goto fail_page;
		instructions[hit_index + 1] =
			0xd61f0000U | (reg << 5); /* br Xn */
		instructions[hit_index + 2] = 0xd503201fU;
	}
	if (direct_gateway_enabled()) {
		uint32_t *direct = &instructions[slow_index];

		/* Cache misses use the same cooperative host-stack coroutine as a
		 * direct syscall.  Capture the dynamic branch target as the saved
		 * Linux PC; BLR also installs its architectural return address before
		 * the common gateway snapshots the remaining registers.
		 */
		if (aarch64_ldr_literal(18, slow, slow + 64,
					&direct[0]) < 0)
			goto fail_page;
		direct[1] = aarch64_ldr64(18, 18, 0);
		direct[2] = aarch64_str64(16, 18, DG_SCRATCH_X16);
		direct[3] = aarch64_str64(reg, 18, DG_GUEST_PC);
		if (is_call) {
			if (aarch64_ldr_literal(30, slow + 16, slow + 72,
						&direct[4]) < 0)
				goto fail_page;
		} else {
			direct[4] = 0xd503201fU;
		}
		if (aarch64_adr(16, slow + 20,
				slow + INDIRECT_DIRECT_RESUME_OFFSET,
				&direct[5]) < 0)
			goto fail_page;
		direct[6] = aarch64_str64(16, 18, DG_RESUME_STUB);
		if (aarch64_ldr_literal(16, slow + 28, slow + 80,
					&direct[7]) < 0)
			goto fail_page;
		direct[8] = 0xd61f0200U; /* br x16 */
		if (aarch64_ldr_literal(18,
				slow + INDIRECT_DIRECT_RESUME_OFFSET, slow + 64,
				&direct[9]) < 0)
			goto fail_page;
		direct[10] = aarch64_ldr64(18, 18, 0);
		direct[11] = aarch64_ldr64(16, 18, DG_REGISTERS);
		direct[12] = aarch64_ldr64(16, 16, ARM64_STATE_X16);
		direct[13] = 0xd61f0000U | (reg << 5); /* br Xn */
		direct[14] = 0xd503201fU;
		direct[15] = 0xd503201fU;
		memcpy((void *)(slow + 64), &active_gateway,
		       sizeof(active_gateway));
		memcpy((void *)(slow + 72), &return_pc, sizeof(return_pc));
		memcpy((void *)(slow + 80), &direct_target,
		       sizeof(direct_target));
	} else {
		unsigned int direct_index;

		instructions[slow_index] = UML_INDIRECT_BRK;
		for (direct_index = 1;
		     direct_index < INDIRECT_DIRECT_SIZE / sizeof(uint32_t);
		     direct_index++)
			instructions[slow_index + direct_index] = 0xd503201fU;
	}
	for (target_index = 0; target_index < INDIRECT_STUB_TARGETS;
	     target_index++) {
		uint64_t target = indirect_target_value(targets, target_count,
			target_index);

		memcpy((void *)(stub + INDIRECT_STUB_TARGET_BASE +
			target_index * sizeof(target)), &target, sizeof(target));
	}
	memcpy((void *)(stub + INDIRECT_STUB_RETURN_OFFSET), &return_pc,
	       sizeof(return_pc));
	__builtin___clear_cache((char *)stub,
				  (char *)(stub + INDIRECT_STUB_SIZE));
	record->site = site;
	record->address = stub;
	record->generation = generation;
	record->original = original;
	record->source_page = source_page;
	for (target_index = 0; target_index < INDIRECT_STUB_TARGETS;
	     target_index++)
		record->cached_targets[target_index] = indirect_target_value(
			targets, target_count, target_index);
	record->target_count = target_count;
	record->inactive_next = NULL;
	site_bucket = indirect_site_hash_index(site, generation);
	slow_bucket = indirect_slow_hash_index(
		stub + INDIRECT_STUB_SLOW_OFFSET);
	record->site_hash_next = indirect_stub_site_hash[site_bucket];
	indirect_stub_site_hash[site_bucket] = record;
	record->slow_hash_next = indirect_stub_slow_hash[slow_bucket];
	indirect_stub_slow_hash[slow_bucket] = record;
	region->used += INDIRECT_STUB_SIZE;
	indirect_stub_count++;
	return stub;
fail_page:
	(void)seal_gateway_pages();
fail_record:
	free(record);
	return 0;
}

static uint32_t indirect_replacement(uintptr_t site,
				     unsigned long generation)
{
	struct indirect_stub *record = find_indirect_stub(site, generation);
	uintptr_t stub;
	uint32_t instruction;

	if (record != NULL &&
	    aarch64_branch(site, record->address, &instruction) == 0)
		return instruction;
	stub = warm_indirect_gateway(site, generation);
	if (stub != 0 && aarch64_branch(site, stub, &instruction) == 0)
		return instruction;
	return UML_INDIRECT_BRK;
}

static void destroy_indirect_stubs(unsigned long generation)
{
	unsigned int bucket;

	for (bucket = 0; bucket < INDIRECT_STUB_HASH_SIZE; bucket++) {
		struct indirect_stub **link = &indirect_stub_site_hash[bucket];

		while (*link != NULL) {
			struct indirect_stub *record = *link;
			struct indirect_stub **slow_link;
			unsigned int slow_bucket;

			if (record->generation != generation) {
				link = &record->site_hash_next;
				continue;
			}
			*link = record->site_hash_next;
			slow_bucket = indirect_slow_hash_index(
				record->address + INDIRECT_STUB_SLOW_OFFSET);
			for (slow_link = &indirect_stub_slow_hash[slow_bucket];
			     *slow_link != NULL;
			     slow_link = &(*slow_link)->slow_hash_next) {
				if (*slow_link != record)
					continue;
				*slow_link = record->slow_hash_next;
				break;
			}
			retire_indirect_stub(record);
			if (indirect_stub_count != 0)
				indirect_stub_count--;
		}
	}
}

static uint32_t syscall_replacement(uintptr_t site)
{
	uintptr_t stub;
	uint32_t instruction;

	if (!direct_gateway_enabled())
		return UML_SYSCALL_BRK;
	stub = syscall_gateway_stub(site);
	if (stub != 0 && aarch64_branch(site, stub, &instruction) == 0)
		return instruction;
	return UML_SYSCALL_BRK;
}

static unsigned char *backing_address(int fd, unsigned long long offset)
{
	struct backing_view *view;
	struct stat status;
	void *base;

	for (view = backing_views; view != NULL; view = view->next)
		if (view->fd == fd)
			return offset <= view->length &&
			       GUEST_PAGE_SIZE <= view->length - (size_t)offset ?
				view->base + (size_t)offset : NULL;
	if (fstat(fd, &status) < 0 || status.st_size <= 0 ||
	    (uint64_t)status.st_size > SIZE_MAX)
		return NULL;
	base = mmap(NULL, (size_t)status.st_size, PROT_READ | PROT_WRITE,
		    MAP_SHARED, fd, 0);
	if (base == MAP_FAILED)
		return NULL;
	view = malloc(sizeof(*view));
	if (view == NULL) {
		munmap(base, (size_t)status.st_size);
		return NULL;
	}
	view->fd = fd;
	view->length = (size_t)status.st_size;
	view->base = base;
	view->next = backing_views;
	backing_views = view;
	return offset <= view->length &&
	       GUEST_PAGE_SIZE <= view->length - (size_t)offset ?
		view->base + (size_t)offset : NULL;
}

static int read_guest_page(int fd, void *buffer, unsigned long long offset)
{
	const unsigned char *source = backing_address(fd, offset);
	size_t completed = 0;

	if (source != NULL) {
		memcpy(buffer, source, GUEST_PAGE_SIZE);
		return 0;
	}

	while (completed < GUEST_PAGE_SIZE) {
		ssize_t count = pread(fd, (unsigned char *)buffer + completed,
				      GUEST_PAGE_SIZE - completed,
				      (off_t)(offset + completed));

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			fprintf(stderr,
				"Darwin VM: backing read failed fd=%d off=%#llx "
				"completed=%zu count=%zd errno=%d\n",
				fd, offset, completed, count, errno);
			return -1;
		}
		completed += (size_t)count;
	}
	return 0;
}

static int write_guest_page(int fd, const void *buffer,
			    unsigned long long offset)
{
	unsigned char *destination = backing_address(fd, offset);
	size_t completed = 0;

	if (destination != NULL) {
		memcpy(destination, buffer, GUEST_PAGE_SIZE);
		return 0;
	}

	while (completed < GUEST_PAGE_SIZE) {
		ssize_t count = pwrite(fd, (const unsigned char *)buffer + completed,
				       GUEST_PAGE_SIZE - completed,
				       (off_t)(offset + completed));

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			fprintf(stderr,
				"Darwin VM: backing write failed fd=%d off=%#llx "
				"completed=%zu count=%zd errno=%d\n",
				fd, offset, completed, count, errno);
			return -1;
		}
		completed += (size_t)count;
	}
	return 0;
}

static unsigned long mapping_generation(uintptr_t address)
{
	return address < GUEST_ADDRESS_LIMIT ? active_generation : 0;
}

static int is_shadow_mapping(uintptr_t address, unsigned long long offset,
			     unsigned long length)
{
	if (darwin_page_size == 0)
		darwin_page_size = (size_t)getpagesize();
	/* User mappings must remain visible to the 4 KiB shadow MMU even when
	 * a run happens to be 16 KiB aligned.  A direct host mapping cannot be
	 * removed one Linux page at a time and bypasses syscall patching and
	 * per-mm generation tracking.
	 */
	if (address < GUEST_ADDRESS_LIMIT)
		return 1;
	return (address & (darwin_page_size - 1)) != 0 ||
		(offset & (darwin_page_size - 1)) != 0 ||
		(length & (darwin_page_size - 1)) != 0;
}

static struct page_mapping *find_mapping(uintptr_t address,
					 unsigned long generation)
{
	struct page_mapping *mapping;
	uint64_t key = ((uint64_t)address >> 12) ^
		((uint64_t)generation * 0x9e3779b97f4a7c15ULL);
	unsigned int bucket;

	key ^= key >> 33;
	bucket = (unsigned int)key & (MAPPING_HASH_SIZE - 1);
	for (mapping = mapping_hash[bucket]; mapping != NULL;
	     mapping = mapping->hash_next)
		if (mapping->address == address &&
		    mapping->generation == generation)
			return mapping;
	return NULL;
}

static void insert_mapping(struct page_mapping *mapping)
{
	uint64_t key = ((uint64_t)mapping->address >> 12) ^
		((uint64_t)mapping->generation * 0x9e3779b97f4a7c15ULL);
	unsigned int bucket;

	key ^= key >> 33;
	bucket = (unsigned int)key & (MAPPING_HASH_SIZE - 1);
	mapping->next = mappings;
	mapping->previous = NULL;
	if (mappings != NULL)
		mappings->previous = mapping;
	mappings = mapping;
	mapping->hash_next = mapping_hash[bucket];
	mapping_hash[bucket] = mapping;
}

static void remove_mapping_list(struct page_mapping *mapping)
{
	if (mapping->previous != NULL)
		mapping->previous->next = mapping->next;
	else
		mappings = mapping->next;
	if (mapping->next != NULL)
		mapping->next->previous = mapping->previous;
}

static void remove_mapping_hash(struct page_mapping *mapping)
{
	uint64_t key = ((uint64_t)mapping->address >> 12) ^
		((uint64_t)mapping->generation * 0x9e3779b97f4a7c15ULL);
	unsigned int bucket;
	struct page_mapping **link;

	key ^= key >> 33;
	bucket = (unsigned int)key & (MAPPING_HASH_SIZE - 1);
	for (link = &mapping_hash[bucket]; *link != NULL;
	     link = &(*link)->hash_next)
		if (*link == mapping) {
			*link = mapping->hash_next;
			return;
		}
}

static unsigned int backing_hash_index(int fd, unsigned long long offset)
{
	uint64_t key = (offset >> 12) ^
		((uint64_t)(unsigned int)fd * 0x9e3779b97f4a7c15ULL);

	key ^= key >> 33;
	return (unsigned int)key & (BACKING_HASH_SIZE - 1);
}

static void insert_backing_hash(struct page_mapping *mapping)
{
	unsigned int bucket = backing_hash_index(mapping->fd, mapping->offset);

	mapping->backing_hash_next = backing_hash[bucket];
	backing_hash[bucket] = mapping;
}

static void remove_backing_hash(struct page_mapping *mapping)
{
	unsigned int bucket = backing_hash_index(mapping->fd, mapping->offset);
	struct page_mapping **link;

	for (link = &backing_hash[bucket]; *link != NULL;
	     link = &(*link)->backing_hash_next)
		if (*link == mapping) {
			*link = mapping->backing_hash_next;
			mapping->backing_hash_next = NULL;
			return;
		}
}

static unsigned int known_code_hash_index(uintptr_t address,
					   unsigned long generation)
{
	uint64_t key = ((uint64_t)address >> 2) ^
		((uint64_t)generation * 0x9e3779b97f4a7c15ULL);

	key ^= key >> 33;
	return (unsigned int)key & (KNOWN_CODE_HASH_SIZE - 1);
}

static unsigned int known_code_region_hash_index(uintptr_t address,
						  unsigned long generation,
						  size_t alignment)
{
	return known_code_hash_index(align_down(address, alignment), generation);
}

static int remember_code_target(uintptr_t address, unsigned long generation)
{
	struct known_code_target *target;
	unsigned int bucket = known_code_hash_index(address, generation);
	unsigned int page_bucket;
	unsigned int slot_bucket;

	for (target = known_code_hash[bucket]; target != NULL;
	     target = target->hash_next)
		if (target->address == address && target->generation == generation)
			return 0;
	target = malloc(sizeof(*target));
	if (target == NULL)
		return -1;
	target->address = address;
	target->generation = generation;
	target->resolved = 0;
	target->hash_next = known_code_hash[bucket];
	known_code_hash[bucket] = target;
	page_bucket = known_code_region_hash_index(address, generation,
						   GUEST_PAGE_SIZE);
	target->page_hash_next = known_code_page_hash[page_bucket];
	known_code_page_hash[page_bucket] = target;
	if (darwin_page_size == 0)
		darwin_page_size = (size_t)getpagesize();
	slot_bucket = known_code_region_hash_index(address, generation,
						   darwin_page_size);
	target->slot_hash_next = known_code_slot_hash[slot_bucket];
	known_code_slot_hash[slot_bucket] = target;
	target->all_next = known_code_targets;
	known_code_targets = target;
	return 0;
}

static void mark_code_target_resolved(uintptr_t address,
				      unsigned long generation)
{
	struct known_code_target *target;
	unsigned int bucket = known_code_hash_index(address, generation);

	for (target = known_code_hash[bucket]; target != NULL;
	     target = target->hash_next)
		if (target->address == address && target->generation == generation) {
			target->resolved = 1;
			return;
		}
}

static int code_target_is_resolved(uintptr_t address,
				   unsigned long generation)
{
	struct known_code_target *target;
	unsigned int bucket = known_code_hash_index(address, generation);

	for (target = known_code_hash[bucket]; target != NULL;
	     target = target->hash_next)
		if (target->address == address && target->generation == generation)
			return target->resolved;
	return 0;
}

static void destroy_code_targets(unsigned long generation)
{
	struct known_code_target **link = &known_code_targets;

	while (*link != NULL) {
		struct known_code_target *target = *link;
		struct known_code_target **hash_link;
		unsigned int bucket;

		if (target->generation != generation) {
			link = &target->all_next;
			continue;
		}
		*link = target->all_next;
		bucket = known_code_hash_index(target->address, generation);
		for (hash_link = &known_code_hash[bucket]; *hash_link != NULL;
		     hash_link = &(*hash_link)->hash_next)
			if (*hash_link == target) {
				*hash_link = target->hash_next;
				break;
			}
		bucket = known_code_region_hash_index(target->address, generation,
			GUEST_PAGE_SIZE);
		for (hash_link = &known_code_page_hash[bucket]; *hash_link != NULL;
		     hash_link = &(*hash_link)->page_hash_next)
			if (*hash_link == target) {
				*hash_link = target->page_hash_next;
				break;
			}
		bucket = known_code_region_hash_index(target->address, generation,
			darwin_page_size);
		for (hash_link = &known_code_slot_hash[bucket]; *hash_link != NULL;
		     hash_link = &(*hash_link)->slot_hash_next)
			if (*hash_link == target) {
				*hash_link = target->slot_hash_next;
				break;
			}
		free(target);
	}
}

static int slot_protection(uintptr_t slot)
{
	unsigned long position;
	int protection = PROT_NONE;

	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t address = slot + position;
		struct page_mapping *mapping = find_mapping(
			address, mapping_generation(address));

		if (mapping != NULL && mapping->active &&
		    align_down(mapping->address, darwin_page_size) == slot)
			protection |= mapping->protection;
	}
	return protection;
}

static int slot_has_mixed_write_permissions(uintptr_t slot)
{
	unsigned long position;
	int writable = 0;
	int restricted = 0;

	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t address = slot + position;
		struct page_mapping *mapping = find_mapping(
			address, mapping_generation(address));

		if (mapping == NULL || !mapping->active ||
		    align_down(mapping->address, darwin_page_size) != slot)
			continue;
		if (mapping->protection & PROT_WRITE)
			writable = 1;
		else if (mapping->guest_writable)
			restricted = 1;
	}
	return writable && restricted;
}

static int slot_write_validated(uintptr_t slot)
{
	unsigned long position;
	int found = 0;

	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t address = slot + position;
		struct page_mapping *mapping = find_mapping(
			address, mapping_generation(address));

		if (mapping == NULL || !mapping->active ||
		    align_down(mapping->address, darwin_page_size) != slot)
			continue;
		found = 1;
		if (!mapping->write_validated)
			return 0;
	}
	return found;
}

static void set_slot_write_validated(uintptr_t slot, int validated)
{
	unsigned long position;

	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t address = slot + position;
		struct page_mapping *mapping = find_mapping(
			address, mapping_generation(address));

		if (mapping != NULL && mapping->active &&
		    align_down(mapping->address, darwin_page_size) == slot)
			mapping->write_validated = validated;
	}
}

static int slot_writable_exposed(uintptr_t slot)
{
	unsigned long position;

	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t address = slot + position;
		struct page_mapping *mapping = find_mapping(
			address, mapping_generation(address));

		if (mapping != NULL && mapping->active && mapping->loaded &&
		    mapping->writable_exposed)
			return 1;
	}
	return 0;
}

static int slot_has_loaded_mapping(uintptr_t slot)
{
	unsigned long position;

	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t address = slot + position;
		struct page_mapping *mapping = find_mapping(
			address, mapping_generation(address));

		if (mapping != NULL && mapping->active && mapping->loaded)
			return 1;
	}
	return 0;
}

static void queue_dirty_slot(uintptr_t slot)
{
	struct dirty_slot *grown;
	unsigned long generation = mapping_generation(slot);

	if (generation == 0 || dirty_slot_overflow)
		return;
	if (dirty_slot_count == dirty_slot_capacity) {
		size_t capacity = dirty_slot_capacity == 0 ? 64 :
			dirty_slot_capacity * 2;

		grown = realloc(dirty_slots, capacity * sizeof(*grown));
		if (grown == NULL) {
			dirty_slot_overflow = 1;
			return;
		}
		dirty_slots = grown;
		dirty_slot_capacity = capacity;
	}
	dirty_slots[dirty_slot_count].address = slot;
	dirty_slots[dirty_slot_count].generation = generation;
	dirty_slot_count++;
}

static void discard_dirty_generation(unsigned long generation)
{
	size_t source;
	size_t target = 0;

	for (source = 0; source < dirty_slot_count; source++) {
		if (dirty_slots[source].generation == generation)
			continue;
		if (target != source)
			dirty_slots[target] = dirty_slots[source];
		target++;
	}
	dirty_slot_count = target;
}

static void mark_slot_writable_exposed(uintptr_t slot)
{
	unsigned long position;
	int was_exposed = slot_writable_exposed(slot);

	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t address = slot + position;
		struct page_mapping *mapping = find_mapping(
			address, mapping_generation(address));

		if (mapping != NULL && mapping->active && mapping->loaded)
			mapping->writable_exposed = 1;
	}
	if (!was_exposed)
		queue_dirty_slot(slot);
}

static void clear_slot_writable_exposed(uintptr_t slot)
{
	unsigned long position;

	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t address = slot + position;
		struct page_mapping *mapping = find_mapping(
			address, mapping_generation(address));

		if (mapping != NULL && mapping->active && mapping->loaded)
			mapping->writable_exposed = 0;
	}
}

static int runtime_slot_protection(uintptr_t slot)
{
	int protection = slot_protection(slot);

	/* A 16 KiB Darwin slot can contain independent executable and writable
	 * 4 KiB Linux pages.  A writable quarter must never make a read-only or
	 * COW sibling writable at the host granule.  Such mixed slots remain
	 * read-only until the fault path grants one breakpoint-guarded store.
	 * This
	 * also covers the traditional writable/executable mixed-slot case.
	 */
	if ((protection & PROT_WRITE) &&
	    slot_has_mixed_write_permissions(slot))
		protection &= ~PROT_WRITE;
	else if ((protection & (PROT_WRITE | PROT_EXEC)) ==
	    (PROT_WRITE | PROT_EXEC)) {
		if (slot_write_validated(slot))
			protection = (protection | PROT_READ | PROT_WRITE) &
				~PROT_EXEC;
		else
			protection = (protection | PROT_READ | PROT_EXEC) &
				~PROT_WRITE;
	}
	return protection;
}

static void mark_patch(unsigned char *bitmap, unsigned int index)
{
	bitmap[index / 8] |= (unsigned char)(1U << (index % 8));
}

static int was_patched(const unsigned char *bitmap, unsigned int index)
{
	return bitmap[index / 8] & (unsigned char)(1U << (index % 8));
}

static void clear_patch(unsigned char *bitmap, unsigned int index)
{
	bitmap[index / 8] &= (unsigned char)~(1U << (index % 8));
}

static uint64_t code_page_hash(const unsigned char *page)
{
	const uint64_t *words = (const uint64_t *)page;
	uint64_t hash = 0xcbf29ce484222325ULL;
	unsigned int index;

	for (index = 0; index < GUEST_PAGE_SIZE / sizeof(*words); index++) {
		hash ^= words[index];
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

static struct code_cache_entry *find_code_cache(const unsigned char *page,
						 uint64_t hash)
{
	struct code_cache_entry *entry;
	unsigned int bucket = (unsigned int)hash & (CODE_CACHE_HASH_SIZE - 1);

	for (entry = code_cache_hash[bucket]; entry != NULL;
	     entry = entry->next)
		if (entry->hash == hash &&
		    memcmp(entry->original, page, GUEST_PAGE_SIZE) == 0)
			return entry;
	return NULL;
}

static struct code_cache_entry *ensure_code_cache(const unsigned char *page)
{
	struct code_cache_entry *entry;
	uint64_t hash = code_page_hash(page);
	unsigned int bucket;

	entry = find_code_cache(page, hash);
	if (entry != NULL)
		return entry;
	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return NULL;
	entry->hash = hash;
	memcpy(entry->original, page, GUEST_PAGE_SIZE);
	bucket = (unsigned int)hash & (CODE_CACHE_HASH_SIZE - 1);
	entry->next = code_cache_hash[bucket];
	code_cache_hash[bucket] = entry;
	return entry;
}

static int restore_code_cache(struct page_mapping *mapping)
{
	struct code_cache_entry *entry;
	uint64_t hash;

	if (mapping->original == NULL)
		return 0;
	hash = code_page_hash(mapping->original);
	entry = find_code_cache(mapping->original, hash);
	if (entry == NULL || !entry->instrumentation_valid)
		return 0;
	mapping->special_scanned = entry->special_scanned;
	memcpy(mapping->syscall_patched, entry->syscall_patched,
	       sizeof(mapping->syscall_patched));
	memcpy(mapping->tls_write_patched, entry->tls_write_patched,
	       sizeof(mapping->tls_write_patched));
	memcpy(mapping->tls_read_patched, entry->tls_read_patched,
	       sizeof(mapping->tls_read_patched));
	memcpy(mapping->x18_patched, entry->x18_patched,
	       sizeof(mapping->x18_patched));
	memcpy(mapping->indirect_patched, entry->indirect_patched,
	       sizeof(mapping->indirect_patched));
	memcpy(mapping->code_known, entry->code_known,
	       sizeof(mapping->code_known));
	memcpy(mapping->pending_target, entry->pending_target,
	       sizeof(mapping->pending_target));
	if (sanitize_mapping_instrumentation(mapping) != 0)
		(void)sanitize_cached_instrumentation(entry, mapping->address);
	return 1;
}

static int cached_branch_target(uintptr_t address, uint32_t instruction,
				uintptr_t *target)
{
	if ((instruction & 0x7c000000U) == 0x14000000U)
		*target = direct_branch_target(address,
			instruction & 0x03ffffffU, 26);
	else if ((instruction & 0xff000010U) == 0x54000000U ||
		 (instruction & 0x7e000000U) == 0x34000000U)
		*target = direct_branch_target(address,
			(instruction >> 5) & 0x7ffffU, 19);
	else if ((instruction & 0x7e000000U) == 0x36000000U)
		*target = direct_branch_target(address,
			(instruction >> 5) & 0x3fffU, 14);
	else if ((instruction & 0xfffffc1fU) == 0xd63f0000U)
		*target = address + sizeof(uint32_t);
	else
		return 0;
	return 1;
}

static int store_code_cache(struct page_mapping *mapping)
{
	struct code_cache_entry *entry;
	unsigned int index;

	if (mapping->original == NULL || !mapping->instrumentation_valid ||
	    !mapping->guest_code)
		return 0;
	(void)sanitize_mapping_instrumentation(mapping);
	entry = ensure_code_cache(mapping->original);
	if (entry == NULL)
		return -1;
	entry->instrumentation_valid = 1;
	entry->special_scanned |= mapping->special_scanned;
	for (index = 0; index < sizeof(entry->code_known); index++) {
		entry->syscall_patched[index] |= mapping->syscall_patched[index];
		entry->tls_write_patched[index] |= mapping->tls_write_patched[index];
		entry->tls_read_patched[index] |= mapping->tls_read_patched[index];
		entry->x18_patched[index] |= mapping->x18_patched[index];
		entry->indirect_patched[index] |= mapping->indirect_patched[index];
		entry->code_known[index] |= mapping->code_known[index];
	}
	for (index = 0; index < GUEST_PAGE_SIZE / sizeof(uint32_t); index++) {
		uintptr_t target;
		uint32_t instruction;

		if (!was_patched(mapping->code_known, index))
			continue;
		memcpy(&instruction,
		       mapping->original + index * sizeof(instruction),
		       sizeof(instruction));
		if (!cached_branch_target(mapping->address +
				index * sizeof(instruction), instruction, &target))
			continue;
		if (code_target_is_resolved(target, mapping->generation))
			clear_patch(entry->pending_target, index);
		else
			mark_patch(entry->pending_target, index);
	}
	(void)sanitize_cached_instrumentation(entry, mapping->address);
	return 0;
}

static uintptr_t direct_branch_target(uintptr_t address,
				      uint32_t immediate, unsigned int bits)
{
	int64_t offset = (int64_t)((uint64_t)immediate << (64 - bits)) >>
		(64 - bits);

	return address + (uintptr_t)(offset * (int64_t)sizeof(uint32_t));
}

static int replay_cached_code_targets(struct page_mapping *mapping)
{
	const uint32_t *instructions = (const uint32_t *)mapping->original;
	unsigned int index;

	for (index = 0; index < GUEST_PAGE_SIZE / sizeof(*instructions);
	     index++) {
		uint32_t instruction;
		uintptr_t address;
		uintptr_t target;

		if (!was_patched(mapping->pending_target, index))
			continue;
		instruction = instructions[index];
		address = mapping->address + index * sizeof(*instructions);
		if (!cached_branch_target(address, instruction, &target))
			continue;
		if (remember_code_target(target, mapping->generation) < 0)
			return -1;
	}
	return 0;
}

static void mark_code_targets_unresolved(uintptr_t page,
					 unsigned long generation)
{
	struct known_code_target *target;
	unsigned int bucket = known_code_region_hash_index(
		page, generation, GUEST_PAGE_SIZE);

	for (target = known_code_page_hash[bucket]; target != NULL;
	     target = target->page_hash_next)
		if (align_down(target->address, GUEST_PAGE_SIZE) == page &&
		    target->generation == generation)
			target->resolved = 0;
}

static void reset_mapping_instrumentation(struct page_mapping *mapping)
{
	int had_code = mapping->special_scanned || mapping->guest_code;

	memset(mapping->syscall_patched, 0, sizeof(mapping->syscall_patched));
	memset(mapping->syscall_direct_patched, 0,
	       sizeof(mapping->syscall_direct_patched));
	memset(mapping->syscall_trap_count, 0,
	       sizeof(mapping->syscall_trap_count));
	memset(mapping->tls_write_patched, 0,
	       sizeof(mapping->tls_write_patched));
	memset(mapping->tls_read_patched, 0,
	       sizeof(mapping->tls_read_patched));
	memset(mapping->x18_patched, 0, sizeof(mapping->x18_patched));
	memset(mapping->indirect_patched, 0,
	       sizeof(mapping->indirect_patched));
	memset(mapping->code_known, 0, sizeof(mapping->code_known));
	memset(mapping->pending_target, 0, sizeof(mapping->pending_target));
	mapping->special_scanned = 0;
	mapping->instrumentation_valid = 0;
	if (had_code)
		mark_code_targets_unresolved(mapping->address,
					     mapping->generation);
}

static int raw_instruction_uses_x18(uint32_t instruction)
{
	unsigned int rd = instruction & 31U;
	unsigned int rn = (instruction >> 5) & 31U;
	unsigned int rm = (instruction >> 16) & 31U;

	/* ADR/ADRP and MOV-wide instructions only name a destination. */
	if ((instruction & 0x1f000000U) == 0x10000000U ||
	    (instruction & 0x1f800000U) == 0x12800000U)
		return rd == 18;

	/* ADD/SUB immediate and bitfield instructions name Rd and Rn. */
	if ((instruction & 0x1f000000U) == 0x11000000U ||
	    (instruction & 0x1f800000U) == 0x13000000U ||
	    (instruction & 0x1f800000U) == 0x12000000U)
		return rd == 18 || rn == 18;

	/* ADD/SUB and logical shifted-register instructions also name Rm. */
	if ((instruction & 0x1f200000U) == 0x0b000000U ||
	    (instruction & 0x1f000000U) == 0x0a000000U)
		return rd == 18 || rn == 18 || rm == 18;

	/* MADD/MSUB (including MUL/MNEG aliases) also name Ra. */
	if ((instruction & 0x7fe00000U) == 0x1b000000U) {
		unsigned int ra = (instruction >> 10) & 31U;

		return rd == 18 || rn == 18 || rm == 18 || ra == 18;
	}

	/* Integer load/store, unsigned scaled immediate. */
	if ((instruction & 0x3b000000U) == 0x39000000U)
		return rd == 18 || rn == 18;

	/* Integer load/store pair, including its second data register. */
	if ((instruction & 0x3a000000U) == 0x28000000U &&
	    !(instruction & (1U << 26))) {
		unsigned int rt2 = (instruction >> 10) & 31U;

		return rd == 18 || rt2 == 18 || rn == 18;
	}

	return 0;
}

static int instruction_uses_x18(uint32_t instruction, uintptr_t address)
{
	cs_insn *decoded;
	const uint8_t *bytes = (const uint8_t *)&instruction;
	size_t size = sizeof(instruction);
	uint64_t pc = address;
	unsigned int rd = instruction & 31U;
	unsigned int rn = (instruction >> 5) & 31U;
	unsigned int ra = (instruction >> 10) & 31U;
	unsigned int rm = (instruction >> 16) & 31U;
	unsigned int index;

	if (raw_instruction_uses_x18(instruction))
		return 1;
	/* Every general-register operand in the fixed-width AArch64 encoding
	 * occupies one of these four fields.  Avoid invoking the full decoder
	 * for the overwhelmingly common case where none can name x18.
	 */
	if (rd != 18 && rn != 18 && ra != 18 && rm != 18)
		return 0;

	if (!arm64_disassembler_ready) {
		if (cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN,
			    &arm64_disassembler) != CS_ERR_OK)
			return 0;
		if (cs_option(arm64_disassembler, CS_OPT_DETAIL,
			      CS_OPT_ON) != CS_ERR_OK) {
			cs_close(&arm64_disassembler);
			return 0;
		}
		arm64_disassembler_ready = 1;
	}
	decoded = cs_malloc(arm64_disassembler);
	if (decoded == NULL)
		return 0;
	if (!cs_disasm_iter(arm64_disassembler, &bytes, &size, &pc, decoded) ||
	    decoded->detail == NULL) {
		cs_free(decoded, 1);
		return 0;
	}
	for (index = 0; index < decoded->detail->arm64.op_count; index++) {
		const cs_arm64_op *operand =
			&decoded->detail->arm64.operands[index];

		if ((operand->type == ARM64_OP_REG &&
		     (operand->reg == ARM64_REG_X18 ||
		      operand->reg == ARM64_REG_W18)) ||
		    (operand->type == ARM64_OP_MEM &&
		     (operand->mem.base == ARM64_REG_X18 ||
		      operand->mem.index == ARM64_REG_X18))) {
			cs_free(decoded, 1);
			return 1;
		}
	}
	cs_free(decoded, 1);
	return 0;
}

static unsigned int sanitize_instrumentation_bitmaps(
	const unsigned char *original, uintptr_t address,
	unsigned char *syscall_patched, unsigned char *syscall_direct_patched,
	unsigned char *syscall_trap_count, unsigned char *tls_write_patched,
	unsigned char *tls_read_patched, unsigned char *x18_patched,
	unsigned char *indirect_patched, unsigned char *pending_target)
{
	const uint32_t *instructions = (const uint32_t *)original;
	unsigned int invalid = 0;
	unsigned int index;

	for (index = 0; index < GUEST_PAGE_SIZE / sizeof(*instructions);
	     index++) {
		uint32_t instruction = instructions[index];
		uintptr_t pc = address + index * sizeof(*instructions);
		uintptr_t target;

		if (was_patched(syscall_patched, index) &&
		    instruction != LINUX_SVC_0) {
			clear_patch(syscall_patched, index);
			if (syscall_direct_patched != NULL)
				clear_patch(syscall_direct_patched, index);
			if (syscall_trap_count != NULL)
				syscall_trap_count[index] = 0;
			invalid++;
		}
		if (syscall_direct_patched != NULL &&
		    was_patched(syscall_direct_patched, index) &&
		    !was_patched(syscall_patched, index)) {
			clear_patch(syscall_direct_patched, index);
			if (syscall_trap_count != NULL)
				syscall_trap_count[index] = 0;
			invalid++;
		}
		if (was_patched(tls_write_patched, index) &&
		    (instruction & ~31U) != AARCH64_MSR_TPIDR_EL0) {
			clear_patch(tls_write_patched, index);
			invalid++;
		}
		if (was_patched(tls_read_patched, index) &&
		    (instruction & ~31U) != AARCH64_MRS_TPIDR_EL0) {
			clear_patch(tls_read_patched, index);
			invalid++;
		}
		if (was_patched(x18_patched, index) &&
		    !instruction_uses_x18(instruction, pc)) {
			clear_patch(x18_patched, index);
			invalid++;
		}
		if (was_patched(indirect_patched, index) &&
		    (instruction & 0xfffffc1fU) != 0xd61f0000U &&
		    (instruction & 0xfffffc1fU) != 0xd63f0000U) {
			clear_patch(indirect_patched, index);
			invalid++;
		}
		if (was_patched(pending_target, index) &&
		    !cached_branch_target(pc, instruction, &target))
			clear_patch(pending_target, index);
	}
	return invalid;
}

static unsigned int sanitize_mapping_instrumentation(
	struct page_mapping *mapping)
{
	if (mapping->original == NULL)
		return 0;
	return sanitize_instrumentation_bitmaps(mapping->original,
		mapping->address, mapping->syscall_patched,
		mapping->syscall_direct_patched, mapping->syscall_trap_count,
		mapping->tls_write_patched, mapping->tls_read_patched,
		mapping->x18_patched, mapping->indirect_patched,
		mapping->pending_target);
}

static unsigned int sanitize_cached_instrumentation(
	struct code_cache_entry *entry, uintptr_t address)
{
	return sanitize_instrumentation_bitmaps(entry->original, address,
		entry->syscall_patched, NULL, NULL, entry->tls_write_patched,
		entry->tls_read_patched, entry->x18_patched,
		entry->indirect_patched, entry->pending_target);
}

static int arm64_register_number(unsigned int reg, unsigned int number)
{
	switch (number) {
	case 16:
		return reg == ARM64_REG_X16 || reg == ARM64_REG_W16;
	case 17:
		return reg == ARM64_REG_X17 || reg == ARM64_REG_W17;
	case 18:
		return reg == ARM64_REG_X18 || reg == ARM64_REG_W18;
	default:
		return 0;
	}
}

static int analyze_x18_stub(uint32_t instruction, uintptr_t address,
			    int *reads_x18, int *writes_x18,
			    unsigned int *scratch)
{
	cs_insn *decoded;
	cs_regs read_registers;
	cs_regs written_registers;
	uint8_t read_count = 0;
	uint8_t written_count = 0;
	const uint8_t *bytes = (const uint8_t *)&instruction;
	size_t size = sizeof(instruction);
	uint64_t pc = address;
	int uses_x16 = 0;
	int uses_x17 = 0;
	unsigned int index;

	*reads_x18 = 0;
	*writes_x18 = 0;
	if (!arm64_disassembler_ready) {
		if (cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN,
			    &arm64_disassembler) != CS_ERR_OK)
			return 0;
		if (cs_option(arm64_disassembler, CS_OPT_DETAIL,
			      CS_OPT_ON) != CS_ERR_OK) {
			cs_close(&arm64_disassembler);
			return 0;
		}
		arm64_disassembler_ready = 1;
	}
	decoded = cs_malloc(arm64_disassembler);
	if (decoded == NULL)
		return 0;
	if (!cs_disasm_iter(arm64_disassembler, &bytes, &size, &pc, decoded) ||
	    decoded->detail == NULL)
		goto unsupported;
	if (decoded->id == ARM64_INS_ADR || decoded->id == ARM64_INS_ADRP ||
	    decoded->id == ARM64_INS_MRS || decoded->id == ARM64_INS_MSR ||
	    cs_insn_group(arm64_disassembler, decoded, ARM64_GRP_JUMP) ||
	    cs_insn_group(arm64_disassembler, decoded, ARM64_GRP_CALL) ||
	    cs_insn_group(arm64_disassembler, decoded, ARM64_GRP_RET) ||
	    cs_insn_group(arm64_disassembler, decoded,
			  ARM64_GRP_BRANCH_RELATIVE))
		goto unsupported;
	if (cs_regs_access(arm64_disassembler, decoded, read_registers,
			   &read_count, written_registers,
			   &written_count) != CS_ERR_OK)
		goto unsupported;
	for (index = 0; index < read_count; index++) {
		*reads_x18 |= arm64_register_number(read_registers[index], 18);
		uses_x16 |= arm64_register_number(read_registers[index], 16);
		uses_x17 |= arm64_register_number(read_registers[index], 17);
	}
	for (index = 0; index < written_count; index++) {
		*writes_x18 |=
			arm64_register_number(written_registers[index], 18);
		uses_x16 |= arm64_register_number(written_registers[index], 16);
		uses_x17 |= arm64_register_number(written_registers[index], 17);
	}
	for (index = 0; index < decoded->detail->arm64.op_count; index++) {
		const cs_arm64_op *operand =
			&decoded->detail->arm64.operands[index];

		if (operand->type == ARM64_OP_REG) {
			uses_x16 |= arm64_register_number(operand->reg, 16);
			uses_x17 |= arm64_register_number(operand->reg, 17);
			if (arm64_register_number(operand->reg, 18)) {
				*reads_x18 |= !!(operand->access & CS_AC_READ);
				*writes_x18 |= !!(operand->access & CS_AC_WRITE);
			}
		} else if (operand->type == ARM64_OP_MEM) {
			uses_x16 |= arm64_register_number(operand->mem.base, 16) ||
				arm64_register_number(operand->mem.index, 16);
			uses_x17 |= arm64_register_number(operand->mem.base, 17) ||
				arm64_register_number(operand->mem.index, 17);
			if (arm64_register_number(operand->mem.base, 18)) {
				*reads_x18 = 1;
				if (decoded->detail->arm64.writeback)
					*writes_x18 = 1;
			}
			if (arm64_register_number(operand->mem.index, 18))
				*reads_x18 = 1;
		} else if (operand->type == ARM64_OP_IMM &&
			   (decoded->id == ARM64_INS_LDR ||
			    decoded->id == ARM64_INS_LDRSW ||
			    decoded->id == ARM64_INS_PRFM)) {
			/* Literal loads are relative to the original PC. */
			goto unsupported;
		}
	}
	if (!*reads_x18 && !*writes_x18)
		goto unsupported;
	if (!uses_x16)
		*scratch = 16;
	else if (!uses_x17)
		*scratch = 17;
	else
		goto unsupported;
	cs_free(decoded, 1);
	return 1;
unsupported:
	cs_free(decoded, 1);
	return 0;
}

static uint32_t replace_x18_field(uint32_t instruction, unsigned int shift,
				  unsigned int scratch)
{
	if (((instruction >> shift) & 31U) == 18)
		instruction = (instruction & ~(31U << shift)) |
			(scratch << shift);
	return instruction;
}

static int rewrite_x18_instruction(uint32_t original, unsigned int scratch,
				   uint32_t *rewritten)
{
	uint32_t instruction = original;

	/* ADR/ADRP and MOV{N,Z,K}: Rd. */
	if ((instruction & 0x1f000000U) == 0x10000000U ||
	    (instruction & 0x1f800000U) == 0x12800000U) {
		instruction = replace_x18_field(instruction, 0, scratch);
	/* Immediate arithmetic, logical-immediate and bitfield: Rd, Rn. */
	} else if ((instruction & 0x1f000000U) == 0x11000000U ||
		   (instruction & 0x1f800000U) == 0x12000000U ||
		   (instruction & 0x1f800000U) == 0x13000000U) {
		instruction = replace_x18_field(instruction, 0, scratch);
		instruction = replace_x18_field(instruction, 5, scratch);
	/* Shifted-register arithmetic and logical operations: Rd, Rn, Rm. */
	} else if ((instruction & 0x1f200000U) == 0x0b000000U ||
		   (instruction & 0x1f000000U) == 0x0a000000U) {
		instruction = replace_x18_field(instruction, 0, scratch);
		instruction = replace_x18_field(instruction, 5, scratch);
		instruction = replace_x18_field(instruction, 16, scratch);
	/* MADD/MSUB: Rd, Rn, Ra, Rm. */
	} else if ((instruction & 0x7fe00000U) == 0x1b000000U) {
		instruction = replace_x18_field(instruction, 0, scratch);
		instruction = replace_x18_field(instruction, 5, scratch);
		instruction = replace_x18_field(instruction, 10, scratch);
		instruction = replace_x18_field(instruction, 16, scratch);
	/* Integer load/store, unsigned scaled immediate: Rt, Rn. */
	} else if ((instruction & 0x3b000000U) == 0x39000000U) {
		instruction = replace_x18_field(instruction, 0, scratch);
		instruction = replace_x18_field(instruction, 5, scratch);
	/* Integer load/store pair: Rt, Rn, Rt2. */
	} else if ((instruction & 0x3a000000U) == 0x28000000U &&
		   !(instruction & (1U << 26))) {
		instruction = replace_x18_field(instruction, 0, scratch);
		instruction = replace_x18_field(instruction, 5, scratch);
		instruction = replace_x18_field(instruction, 10, scratch);
	} else {
		return 0;
	}
	*rewritten = instruction;
	return 1;
}

static unsigned int x18_stub_hash_index(uintptr_t site,
					uint32_t original)
{
	uint64_t key = ((uint64_t)site >> 2) ^ original;

	key ^= key >> 33;
	return (unsigned int)key & (X18_STUB_HASH_SIZE - 1);
}

static unsigned int x18_stub_fault_hash_index(uintptr_t native_pc)
{
	uint64_t key = (uint64_t)native_pc >> 2;

	key ^= key >> 33;
	return (unsigned int)key & (X18_STUB_HASH_SIZE - 1);
}

static struct x18_stub *find_x18_stub(uintptr_t site,
				      uint32_t original)
{
	struct x18_stub *record;
	unsigned int bucket = x18_stub_hash_index(site, original);

	for (record = x18_stub_hash[bucket]; record != NULL;
	     record = record->hash_next)
		if (record->site == site && record->original == original)
			return record;
	return NULL;
}

static uintptr_t x18_gateway_stub(uintptr_t site, unsigned long generation,
				  uint32_t original)
{
	struct syscall_gateway_region *region;
	struct x18_stub *record;
	uint32_t *instructions;
	uintptr_t stub;
	uintptr_t stub_page;
	uintptr_t guest_pc = site + sizeof(uint32_t);
	uint64_t active_gateway =
		(uintptr_t)&darwin_uml_active_direct_gateway;
	int reads_x18;
	int writes_x18;
	unsigned int scratch = 0;
	uint32_t relocated;
	unsigned int bucket;

	record = find_x18_stub(site, original);
	if (record != NULL)
		return record->address;
	if (!analyze_x18_stub(original, site, &reads_x18, &writes_x18,
			       &scratch))
		return 0;
	if (!rewrite_x18_instruction(original, scratch, &relocated))
		return 0;
	region = find_syscall_gateway_region(site);
	if (region != NULL &&
	    region->used % darwin_page_size > darwin_page_size - X18_STUB_SIZE)
		region->used = (region->used + darwin_page_size - 1) &
			~(darwin_page_size - 1);
	if (region == NULL || grow_syscall_gateway_region(region,
		region->used + X18_STUB_SIZE) < 0)
		return 0;
	record = malloc(sizeof(*record));
	if (record == NULL)
		return 0;
	stub = region->address + region->used;
	stub_page = align_down(stub, darwin_page_size);
	if (gateway_make_writable(region, stub_page) < 0)
		goto fail_record;
	instructions = (uint32_t *)stub;
	if (aarch64_ldr_literal(18, stub, stub + 40, &instructions[0]) < 0)
		goto fail_page;
	instructions[1] = aarch64_ldr64(18, 18, 0);
	instructions[2] = aarch64_str64(scratch, 18, DG_SCRATCH_X16);
	instructions[3] = reads_x18 ?
		aarch64_ldr64(scratch, 18, DG_GUEST_X18) : 0xd503201fU;
	instructions[4] = relocated;
	instructions[5] = writes_x18 ?
		aarch64_str64(scratch, 18, DG_GUEST_X18) : 0xd503201fU;
	instructions[6] = aarch64_ldr64(scratch, 18, DG_SCRATCH_X16);
	if (aarch64_branch(stub + 28, guest_pc, &instructions[7]) < 0)
		goto fail_page;
	instructions[8] = 0xd503201fU;
	instructions[9] = 0xd503201fU;
	memcpy((void *)(stub + 40), &active_gateway, sizeof(active_gateway));
	__builtin___clear_cache((char *)stub,
				  (char *)(stub + X18_STUB_SIZE));
	record->site = site;
	record->address = stub;
	record->native_pc = stub + 16;
	record->generation = generation;
	record->original = original;
	record->scratch = scratch;
	bucket = x18_stub_hash_index(site, original);
	record->hash_next = x18_stub_hash[bucket];
	x18_stub_hash[bucket] = record;
	bucket = x18_stub_fault_hash_index(record->native_pc);
	record->fault_hash_next = x18_stub_fault_hash[bucket];
	x18_stub_fault_hash[bucket] = record;
	record->all_next = x18_stubs;
	x18_stubs = record;
	region->used += X18_STUB_SIZE;
	x18_stub_count++;
	return stub;
fail_page:
	(void)seal_gateway_pages();
fail_record:
	free(record);
	return 0;
}

static uint32_t x18_replacement(uintptr_t site, unsigned long generation,
				uint32_t original)
{
	uintptr_t stub;
	uint32_t instruction;

	if (getenv("UML_NO_X18_GATEWAY") != NULL)
		return UML_X18_BRK;
	stub = x18_gateway_stub(site, generation, original);
	if (stub != 0 && aarch64_branch(site, stub, &instruction) == 0)
		return instruction;
	return UML_X18_BRK;
}

static int patch_known_instruction(struct page_mapping *mapping,
				   unsigned int index, uint32_t replacement,
				   unsigned char *bitmap)
{
	uintptr_t slot = align_down(mapping->address, darwin_page_size);
	uint32_t *word = (uint32_t *)(mapping->address +
					 index * sizeof(uint32_t));
	int protection = runtime_slot_protection(slot);

	if (was_patched(bitmap, index))
		return 0;
	if (mprotect((void *)slot, darwin_page_size,
		     PROT_READ | PROT_WRITE) < 0)
		return -1;
	*word = replacement;
	mark_patch(bitmap, index);
	__builtin___clear_cache((char *)word, (char *)(word + 1));
	if (mprotect((void *)slot, darwin_page_size, protection) < 0)
		return -1;
	return 0;
}

static int instrument_special_code_page(struct page_mapping *mapping)
{
	const uint32_t *instructions = (const uint32_t *)mapping->original;
	uint32_t *words = (uint32_t *)mapping->address;
	uintptr_t slot = align_down(mapping->address, darwin_page_size);
	int protection = runtime_slot_protection(slot);
	unsigned int index;
	int changed = 0;

	if (mprotect((void *)slot, darwin_page_size,
		     PROT_READ | PROT_WRITE) < 0)
		return -1;

	/* These encodings are sufficiently specific to scan across a 4 KiB
	 * page once any address on it has been proven executable.  This covers
	 * compiler-generated local blocks which the direct-branch walk cannot
	 * reach, without revisiting non-code pages in an executable PT_LOAD.
	 */
	for (index = 0; index < GUEST_PAGE_SIZE / sizeof(*instructions);
	     index++) {
		uint32_t instruction = instructions[index];

		if (instruction == LINUX_SVC_0) {
			if (!was_patched(mapping->syscall_patched, index)) {
				words[index] = UML_SYSCALL_BRK;
				mark_patch(mapping->syscall_patched, index);
				changed = 1;
			}
		} else if ((instruction & ~31U) == AARCH64_MSR_TPIDR_EL0) {
			unsigned int reg = instruction & 31U;

			if (!was_patched(mapping->tls_write_patched, index)) {
				words[index] = tls_write_replacement(reg);
				mark_patch(mapping->tls_write_patched, index);
				changed = 1;
			}
		} else if ((instruction & ~31U) == AARCH64_MRS_TPIDR_EL0) {
			unsigned int reg = instruction & 31U;

			if (!was_patched(mapping->tls_read_patched, index)) {
				words[index] = tls_read_replacement(mapping->address +
					index * sizeof(*instructions), reg);
				mark_patch(mapping->tls_read_patched, index);
				changed = 1;
			}
		} else if ((instruction & 0xfffffc1fU) == 0xd61f0000U ||
			   (instruction & 0xfffffc1fU) == 0xd63f0000U) {
			/* BR/BLR may discover a code target the direct-branch walk
			 * has not seen.  RET always returns to an already executed
			 * call site, so trapping every function return is unnecessary
			 * and prohibitively expensive.
			 */
			if (!was_patched(mapping->indirect_patched, index)) {
				words[index] = indirect_replacement(mapping->address +
					index * sizeof(*instructions),
					mapping->generation);
				mark_patch(mapping->indirect_patched, index);
				changed = 1;
			}
		}
	}
	mapping->special_scanned = 1;
	if (changed)
		__builtin___clear_cache((char *)mapping->address,
				      (char *)(mapping->address + GUEST_PAGE_SIZE));
	if (mprotect((void *)slot, darwin_page_size, protection) < 0)
		return -1;
	return 0;
}

static int instrument_mandatory_exec_page(struct page_mapping *mapping)
{
	const uint32_t *original = (const uint32_t *)mapping->original;
	uint32_t *words = (uint32_t *)mapping->address;
	unsigned int index;
	int changed = 0;

	/* VMA-derived execute permission means these bytes belong to a Linux
	 * executable mapping rather than generic UML's historical read/execute
	 * alias.  Batch-patch the privileged encodings which must never execute
	 * natively, including paths not yet reached by the control-flow walk.
	 */
	for (index = 0; index < GUEST_PAGE_SIZE / sizeof(*original); index++) {
		uint32_t instruction = original[index];

		if (instruction == LINUX_SVC_0) {
			words[index] = UML_SYSCALL_BRK;
			mark_patch(mapping->syscall_patched, index);
			changed = 1;
		} else if ((instruction & ~31U) == AARCH64_MSR_TPIDR_EL0) {
			unsigned int reg = instruction & 31U;

			words[index] = tls_write_replacement(reg);
			mark_patch(mapping->tls_write_patched, index);
			changed = 1;
		} else if ((instruction & ~31U) == AARCH64_MRS_TPIDR_EL0) {
			unsigned int reg = instruction & 31U;

			words[index] = tls_read_replacement(mapping->address +
				index * sizeof(*original), reg);
			mark_patch(mapping->tls_read_patched, index);
			changed = 1;
		}
	}
	return changed;
}

static int reapply_mapping_instrumentation(struct page_mapping *mapping)
{
	const uint32_t *original = (const uint32_t *)mapping->original;
	uint32_t *words = (uint32_t *)mapping->address;
	unsigned int index;
	int changed = 0;

	/* Instrumentation belongs to the pristine 4 KiB contents, not merely
	 * to a guest virtual address.  Reject stale cache bits before any word
	 * can be replaced with a syscall or emulation trap.
	 */
	(void)sanitize_mapping_instrumentation(mapping);

	for (index = 0; index < GUEST_PAGE_SIZE / sizeof(*words); index++) {
		if (was_patched(mapping->syscall_patched, index)) {
			words[index] =
				was_patched(mapping->syscall_direct_patched, index) ?
				syscall_replacement(mapping->address +
					index * sizeof(*words)) : UML_SYSCALL_BRK;
			changed = 1;
		} else if (was_patched(mapping->tls_write_patched, index)) {
			words[index] = tls_write_replacement(original[index] & 31U);
			changed = 1;
		} else if (was_patched(mapping->tls_read_patched, index)) {
			words[index] = tls_read_replacement(mapping->address +
				index * sizeof(*words), original[index] & 31U);
			changed = 1;
		} else if (was_patched(mapping->x18_patched, index)) {
			words[index] = x18_replacement(mapping->address +
				index * sizeof(*words), mapping->generation,
				original[index]);
			changed = 1;
		} else if (was_patched(mapping->indirect_patched, index)) {
			words[index] = indirect_replacement(mapping->address +
				index * sizeof(*words), mapping->generation);
			changed = 1;
		}
	}
	return changed;
}

static int instrument_x18_code_page(struct page_mapping *mapping)
{
	const uint32_t *instructions = (const uint32_t *)mapping->original;
	uint32_t *words = (uint32_t *)mapping->address;
	uintptr_t slot = align_down(mapping->address, darwin_page_size);
	int protection = runtime_slot_protection(slot);
	unsigned int index;
	int changed = 0;

	if (mprotect((void *)slot, darwin_page_size,
		     PROT_READ | PROT_WRITE) < 0)
		return -1;

	/*
	 * A direct branch can enter a compiler-generated local block which is
	 * not present in the path discovered from an earlier indirect target.
	 * Once control flow has proved that a 4 KiB page contains code using
	 * Darwin's reserved x18, prepatch every decoded x18 instruction on that
	 * same page.  Limiting this fallback to a proven code page avoids the ELF
	 * strings and hash tables which may share an executable PT_LOAD, while
	 * ensuring a missed local block traps instead of silently running with
	 * Darwin's physical-zero x18.
	 */
	for (index = 0; index < GUEST_PAGE_SIZE / sizeof(*instructions);
	     index++) {
		if (!instruction_uses_x18(instructions[index],
					       mapping->address +
					       index * sizeof(*instructions)))
			continue;
		if (!was_patched(mapping->x18_patched, index)) {
			words[index] = x18_replacement(mapping->address +
				index * sizeof(*instructions), mapping->generation,
				instructions[index]);
			mark_patch(mapping->x18_patched, index);
			changed = 1;
		}
	}
	if (changed)
		__builtin___clear_cache((char *)mapping->address,
				      (char *)(mapping->address + GUEST_PAGE_SIZE));
	if (mprotect((void *)slot, darwin_page_size, protection) < 0)
		return -1;
	return 0;
}

static int flush_mapping(struct page_mapping *mapping)
{
	unsigned char page[GUEST_PAGE_SIZE];
	unsigned char backing[GUEST_PAGE_SIZE];

	if (!mapping->active || !mapping->loaded)
		return 0;
	if (mapping->discard_changes)
		return 0;
	memcpy(page, (void *)mapping->address, sizeof(page));
	if (mapping->guest_code) {
		uint32_t *instructions = (uint32_t *)page;
		unsigned int index;

		for (index = 0;
		     index < GUEST_PAGE_SIZE / sizeof(*instructions); index++) {
			if (was_patched(mapping->syscall_patched, index) ||
			    was_patched(mapping->tls_write_patched, index) ||
			    was_patched(mapping->tls_read_patched, index) ||
			    was_patched(mapping->x18_patched, index) ||
			    was_patched(mapping->indirect_patched, index))
				instructions[index] =
					((const uint32_t *)mapping->original)[index];
		}
		(void)sanitize_mapping_instrumentation(mapping);
	}
	/*
	 * Shadow quarters are MAP_PRIVATE because Darwin's 16 KiB VM granule
	 * cannot directly map an arbitrary 4 KiB file offset.  Do not write an
	 * unchanged shadow back: the shared low-memory alias may have been
	 * modified, freed, and reused since this quarter was loaded.
	 */
	if (mapping->original != NULL &&
	    memcmp(page, mapping->original, sizeof(page)) == 0)
		return 0;
	if (mapping->original != NULL) {
		if (read_guest_page(mapping->fd, backing,
				    mapping->offset) < 0)
			return -1;
		if (memcmp(backing, mapping->original, sizeof(backing)) != 0) {
			/*
			 * The shared physical-memory alias changed after this private
			 * quarter was loaded.  It may have been freed and reused, so
			 * an older shadow must never win that race.  The next slot
			 * rebuild reloads the authoritative backing contents.
			 */
			mapping->discard_changes = 1;
			reset_mapping_instrumentation(mapping);
			return 0;
		}
	}
	if (write_guest_page(mapping->fd, page, mapping->offset) < 0)
		return -1;
	if (mapping->original != NULL)
		memcpy(mapping->original, page, sizeof(page));
	reset_mapping_instrumentation(mapping);
	return 0;
}

static int flush_mapping_aliases(const struct page_mapping *mapping)
{
	struct page_mapping *alias;
	unsigned int bucket = backing_hash_index(mapping->fd, mapping->offset);

	/*
	 * A Linux physical 4 KiB page can be visible through a kernel mapping
	 * and one or more userspace mappings at the same time.  Darwin cannot
	 * MAP_SHARED those independent quarters of 16 KiB VM pages directly,
	 * so synchronize an existing writable alias before materializing a new
	 * shadow alias from the physical-memory backing file.
	 */
	for (alias = backing_hash[bucket]; alias != NULL;
	     alias = alias->backing_hash_next)
		if (alias != mapping && alias->active && alias->loaded &&
		    alias->writable_exposed &&
		    alias->fd == mapping->fd && alias->offset == mapping->offset &&
		    flush_mapping(alias) < 0)
			return -1;
	return 0;
}

static int prepare_slot(uintptr_t slot)
{
	void *result = mmap((void *)slot, darwin_page_size,
			    PROT_READ | PROT_WRITE,
			    MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);

	return result == MAP_FAILED ? -1 : 0;
}

static int load_mapping(struct page_mapping *mapping)
{
	uintptr_t slot = align_down(mapping->address, darwin_page_size);
	struct page_mapping *item;
	unsigned long position;
	int protection;
	int slot_code_changed = 0;
	int profiling = vm_profile_enabled();
	uint64_t profile_total_start = profiling ? mach_absolute_time() : 0;
	uint64_t profile_start = profile_total_start;

	if (profiling)
		vm_profile_loads++;

	/* MAP_FIXED replaces the entire 16 KiB slot.  Preserve dirty writable
	 * quarters before doing that, including siblings of mapping.
	 */
	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t address = slot + position;

		item = find_mapping(address, mapping_generation(address));
		if (item != NULL && item->active && item->loaded &&
		    item->writable_exposed &&
		    flush_mapping(item) < 0)
			return -1;
	}
	if (profiling) {
		vm_profile_flush += mach_absolute_time() - profile_start;
		profile_start = mach_absolute_time();
	}
	if (prepare_slot(slot) < 0) {
		fprintf(stderr, "Darwin VM: mmap slot failed slot=%p errno=%d\n",
			(void *)slot, errno);
		return -1;
	}
	if (profiling) {
		vm_profile_prepare += mach_absolute_time() - profile_start;
		profile_start = mach_absolute_time();
	}
	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t address = slot + position;

		item = find_mapping(address, mapping_generation(address));
		if (item != NULL)
			item->loaded = 0;
	}
	/* Reconstitute every active quarter because MAP_FIXED reset the slot. */
	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t address = slot + position;
		int cache_restored = 0;

		item = find_mapping(address, mapping_generation(address));
		if (item == NULL || !item->active)
			continue;
		if (flush_mapping_aliases(item) < 0)
			return -1;
		if (read_guest_page(item->fd, (void *)item->address,
				    item->offset) < 0)
			return -1;
		if (profiling) {
			vm_profile_read += mach_absolute_time() - profile_start;
			profile_start = mach_absolute_time();
		}
		if (staged_stack.valid &&
		    staged_stack.address == item->address &&
		    staged_stack.generation == item->generation) {
			memcpy((void *)item->address, staged_stack.page,
			       GUEST_PAGE_SIZE);
			if (write_guest_page(item->fd, staged_stack.page,
					     item->offset) < 0)
				return -1;
			staged_stack.valid = 0;
		}
		/* A slot rebuild can reload a different physical page into an
		 * existing virtual mapping.  Save instrumentation for the old bytes,
		 * then discard every address-indexed bitmap before adopting the new
		 * pristine contents.  Keeping those bitmaps was able to turn an
		 * ordinary libc instruction into a synthetic syscall trap.
		 */
		if (item->original != NULL &&
		    memcmp(item->original, (const void *)item->address,
			   GUEST_PAGE_SIZE) != 0) {
			if (store_code_cache(item) < 0)
				return -1;
			reset_mapping_instrumentation(item);
		}
		/*
		 * Darwin applies VM permissions to the complete 16 KiB host slot.
		 * If one Linux quarter is writable, a clean/read-only sibling can
		 * be changed without producing the 4 KiB write fault that would
		 * normally set its Linux PTE dirty.  Snapshot every quarter and use
		 * content changes, rather than the current PTE protection, as the
		 * shadow writeback criterion.  This is also required for loader stack
		 * locals created between two Linux syscalls.
		 */
		if (item->original == NULL)
			item->original = malloc(GUEST_PAGE_SIZE);
		if (item->original == NULL)
			return -1;
		memcpy(item->original, (void *)item->address, GUEST_PAGE_SIZE);
		item->discard_changes = 0;
		item->writable_exposed = 0;
		if (item->guest_code && !item->instrumentation_valid) {
			reset_mapping_instrumentation(item);
			cache_restored = restore_code_cache(item);
			if (cache_restored)
				item->instrumentation_valid = 1;
		}
		item->loaded = 1;
		if (item->guest_code) {
			if (item->instrumentation_valid)
				slot_code_changed |=
					reapply_mapping_instrumentation(item);
			else
				slot_code_changed |=
					instrument_mandatory_exec_page(item);
			item->instrumentation_valid = 1;
		} else {
			item->instrumentation_valid = 0;
		}
		if (cache_restored && replay_cached_code_targets(item) < 0)
			return -1;
		if (profiling) {
			vm_profile_populate += mach_absolute_time() - profile_start;
			profile_start = mach_absolute_time();
		}
	}
	if (slot_code_changed)
		__builtin___clear_cache((char *)slot,
				      (char *)(slot + darwin_page_size));
	protection = runtime_slot_protection(slot);
	/* Keep a freshly reconstructed writable slot clean until the guest's
	 * first store.  The access-signal path can grant write permission in
	 * place when the live Linux PTEs are writable; a COW-protected PTE still
	 * returns to Linux.  This lets clone snapshot only slots the guest
	 * actually changed without adding a kernel round trip to ordinary first
	 * writes.
	 */
	if (mapping->generation != 0 && slot < GUEST_ADDRESS_LIMIT &&
	    (protection & PROT_WRITE))
		protection = (protection | PROT_READ) & ~PROT_WRITE;
	if (mprotect((void *)slot, darwin_page_size, protection) < 0) {
		fprintf(stderr,
			"Darwin VM: mprotect failed slot=%p prot=%x union=%x errno=%d\n",
			(void *)slot, protection, slot_protection(slot), errno);
		return -1;
	}
	if (mapping->generation == 0 && (protection & PROT_WRITE))
		mark_slot_writable_exposed(slot);
	if ((slot_protection(slot) & PROT_EXEC) &&
	    instrument_known_code_in_slot(slot) < 0)
		return -1;
	if (profiling) {
		uint64_t now = mach_absolute_time();

		vm_profile_protect += now - profile_start;
		vm_profile_total += now - profile_total_start;
	}
	return 0;
}

static int rebuild_slot(uintptr_t slot)
{
	unsigned long position;

	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t address = slot + position;
		struct page_mapping *mapping = find_mapping(
			address, mapping_generation(address));

		if (mapping != NULL && mapping->active)
			return load_mapping(mapping);
	}
	return munmap((void *)slot, darwin_page_size);
}

static int queue_slot_rebuild(uintptr_t slot)
{
	struct pending_slot *grown;
	size_t index;

	for (index = 0; index < pending_slot_count; index++)
		if (pending_slots[index].address == slot)
			return 0;
	if (pending_slot_count == pending_slot_capacity) {
		size_t capacity = pending_slot_capacity == 0 ? 64 :
			pending_slot_capacity * 2;

		grown = realloc(pending_slots, capacity * sizeof(*grown));
		if (grown == NULL)
			return -1;
		pending_slots = grown;
		pending_slot_capacity = capacity;
	}
	pending_slots[pending_slot_count++].address = slot;
	return 0;
}

int darwin_uml_vm_batch_begin(void)
{
	if (mapping_batch_depth++ == 0)
		pending_slot_count = 0;
	return 0;
}

int darwin_uml_vm_batch_end(void)
{
	size_t index;
	int result = 0;

	if (mapping_batch_depth == 0)
		return -1;
	if (--mapping_batch_depth != 0)
		return 0;
	for (index = 0; index < pending_slot_count; index++)
		if (rebuild_slot(pending_slots[index].address) < 0 &&
		    errno != EINVAL)
			result = -1;
	pending_slot_count = 0;
	return result;
}

int darwin_uml_vm_stage_stack(unsigned long address_value)
{
	uintptr_t address = align_down((uintptr_t)address_value,
				       GUEST_PAGE_SIZE);
	unsigned long generation = mapping_generation(address);
	uintptr_t slot;
	struct page_mapping *mapping;
	unsigned long position;
	int sibling_loaded = 0;

	if (find_mapping(address, generation) != NULL)
		return 0;
	if (darwin_page_size == 0)
		darwin_page_size = (size_t)getpagesize();
	slot = align_down(address, darwin_page_size);
	for (position = 0; position < darwin_page_size;
	     position += GUEST_PAGE_SIZE) {
		uintptr_t sibling = slot + position;

		mapping = find_mapping(sibling, mapping_generation(sibling));
		if (mapping != NULL && mapping->generation == generation &&
		    mapping->active && mapping->loaded) {
			sibling_loaded = 1;
			break;
		}
	}
	if (!sibling_loaded)
		return 0;
	staged_stack.address = address;
	staged_stack.generation = generation;
	memcpy(staged_stack.page, (const void *)address, GUEST_PAGE_SIZE);
	staged_stack.valid = 1;
	return 1;
}

int darwin_uml_vm_finish_stack(unsigned long address_value)
{
	uintptr_t address = align_down((uintptr_t)address_value,
				       GUEST_PAGE_SIZE);

	if (!staged_stack.valid || staged_stack.address != address)
		return 0;
	fprintf(stderr,
		"Darwin VM: Linux did not materialize staged stack quarter %p\n",
		(void *)address);
	staged_stack.valid = 0;
	return -1;
}

int darwin_uml_vm_has_mapping(unsigned long address_value)
{
	uintptr_t address = align_down((uintptr_t)address_value,
				       GUEST_PAGE_SIZE);
	struct page_mapping *mapping = find_mapping(
		address, mapping_generation(address));

	return mapping != NULL && mapping->active && mapping->loaded;
}

int darwin_uml_vm_activate(unsigned long generation)
{
	struct page_mapping *mapping;
	unsigned long position;
	unsigned long old_generation = active_generation;

	if (generation == active_generation)
		return 0;
	for (mapping = mappings; mapping != NULL; mapping = mapping->next) {
		if (mapping->generation == old_generation && mapping->active &&
		    mapping->writable_exposed &&
		    flush_mapping(mapping) < 0)
			return -1;
	}
	discard_dirty_generation(old_generation);
	/* Unmap each materialized old slot once.  Its four mapping records remain
	 * as the demand-load cache for the next time this mm is scheduled.
	 */
	for (mapping = mappings; mapping != NULL; mapping = mapping->next) {
		uintptr_t slot;

		if (mapping->generation != old_generation || !mapping->active ||
		    !mapping->loaded || mapping->address >= GUEST_ADDRESS_LIMIT)
			continue;
		slot = align_down(mapping->address, darwin_page_size);
		munmap((void *)slot, darwin_page_size);
		for (position = 0; position < darwin_page_size;
		     position += GUEST_PAGE_SIZE) {
			struct page_mapping *item = find_mapping(
				slot + position, old_generation);

			if (item != NULL)
				item->loaded = 0;
		}
	}
	for (mapping = mappings; mapping != NULL; mapping = mapping->next)
		if (old_generation != 0 &&
		    mapping->generation == old_generation)
			mapping->active = mapping->loaded = 0;
	active_generation = generation;
	for (mapping = mappings; mapping != NULL; mapping = mapping->next)
		if (mapping->generation == generation)
			mapping->active = 1;
	return 0;
}

int darwin_uml_vm_destroy(unsigned long generation)
{
	struct page_mapping *mapping;
	struct page_mapping **link;
	unsigned long position;

	if (generation == 0)
		return 0;
	/* A process can exit immediately after changing a shared userspace page.
	 * Preserve those bytes before discarding its private 4 KiB shadows.
	 */
	for (mapping = mappings; mapping != NULL; mapping = mapping->next)
		if (mapping->generation == generation && mapping->active &&
		    mapping->loaded && mapping->writable_exposed &&
		    flush_mapping(mapping) < 0)
			return -1;
	discard_dirty_generation(generation);
	for (link = &mappings; *link != NULL;) {
		mapping = *link;
		if (mapping->generation != generation) {
			link = &mapping->next;
			continue;
		}
		if (mapping->loaded && mapping->address < GUEST_ADDRESS_LIMIT) {
			uintptr_t slot = align_down(mapping->address, darwin_page_size);

			(void)munmap((void *)slot, darwin_page_size);
			for (position = 0; position < darwin_page_size;
			     position += GUEST_PAGE_SIZE) {
				struct page_mapping *item = find_mapping(
					slot + position, generation);

				if (item != NULL)
					item->loaded = 0;
			}
		}
		if (store_code_cache(mapping) < 0)
			return -1;
		*link = mapping->next;
		if (mapping->next != NULL)
			mapping->next->previous = mapping->previous;
		remove_mapping_hash(mapping);
		remove_backing_hash(mapping);
		free(mapping->original);
		free(mapping);
	}
	if (staged_stack.valid && staged_stack.generation == generation)
		staged_stack.valid = 0;
	destroy_indirect_stubs(generation);
	destroy_code_targets(generation);
	return 0;
}

int darwin_uml_vm_flush_active(void)
{
	struct page_mapping *mapping;
	size_t index;
	unsigned long position;

	/* Snapshot only shadows that were actually host-writable.  Then close
	 * every such host slot before Linux builds the child's COW page tables.
	 * The first subsequent store must fault and revalidate the current
	 * task's 4 KiB PTEs, so a parent cannot modify a page still shared by a
	 * child through Darwin's coarser 16 KiB write permission.
	 */
	if (!dirty_slot_overflow) {
		for (index = 0; index < dirty_slot_count; index++) {
			uintptr_t slot = dirty_slots[index].address;

			if (dirty_slots[index].generation != active_generation)
				continue;
			for (position = 0; position < darwin_page_size;
			     position += GUEST_PAGE_SIZE) {
				mapping = find_mapping(slot + position,
						       active_generation);
				if (mapping != NULL && mapping->active &&
				    mapping->loaded && mapping->writable_exposed &&
				    flush_mapping(mapping) < 0)
					return -1;
			}
		}
		for (index = 0; index < dirty_slot_count; index++) {
			uintptr_t slot = dirty_slots[index].address;
			int protection;

			if (dirty_slots[index].generation != active_generation ||
			    !slot_writable_exposed(slot))
				continue;
			protection = (slot_protection(slot) | PROT_READ) &
				~PROT_WRITE;
			if (mprotect((void *)slot, darwin_page_size,
				     protection) < 0)
				return -1;
			set_slot_write_validated(slot, 0);
			clear_slot_writable_exposed(slot);
		}
		discard_dirty_generation(active_generation);
		return 0;
	}

	for (mapping = mappings; mapping != NULL; mapping = mapping->next) {
		if (mapping->generation != 0 &&
		    mapping->generation == active_generation &&
		    mapping->active && mapping->loaded &&
		    mapping->writable_exposed && flush_mapping(mapping) < 0) {
			fprintf(stderr,
				"Darwin VM: active flush failed guest=%p "
				"generation=%lu fd=%d off=%#llx prot=%x\n",
				(void *)mapping->address, mapping->generation,
				mapping->fd, mapping->offset, mapping->protection);
			return -1;
		}
	}
	for (mapping = mappings; mapping != NULL; mapping = mapping->next) {
		uintptr_t slot;
		int protection;

		if (mapping->generation == 0 ||
		    mapping->generation != active_generation ||
		    !mapping->active || !mapping->loaded ||
		    !mapping->writable_exposed)
			continue;
		slot = align_down(mapping->address, darwin_page_size);
		protection = (slot_protection(slot) | PROT_READ) & ~PROT_WRITE;
		if (mprotect((void *)slot, darwin_page_size, protection) < 0) {
			fprintf(stderr,
				"Darwin VM: clone guard failed slot=%p "
				"generation=%lu prot=%x\n",
				(void *)slot, mapping->generation, protection);
			return -1;
		}
		set_slot_write_validated(slot, 0);
		clear_slot_writable_exposed(slot);
	}
	discard_dirty_generation(active_generation);
	dirty_slot_overflow = 0;
	return 0;
}

int darwin_uml_vm_refresh_active(void)
{
	struct page_mapping *mapping;
	struct page_mapping *item;
	unsigned long epoch = ++current_refresh_epoch;

	if (epoch == 0)
		epoch = ++current_refresh_epoch;
	for (mapping = mappings; mapping != NULL; mapping = mapping->next) {
		uintptr_t slot;
		unsigned long position;
		int changed = 0;

		if (mapping->generation == 0 ||
		    mapping->generation != active_generation ||
		    !mapping->active || !mapping->loaded ||
		    mapping->refresh_epoch == epoch)
			continue;
		slot = align_down(mapping->address, darwin_page_size);
		/* The kernel may have changed a user page through its physical-memory
		 * alias while servicing the syscall.  The old implementation rebuilt
		 * every active 16 KiB slot unconditionally, even though almost all
		 * syscalls leave user memory untouched.  Compare the authoritative
		 * backing pages with the snapshots recorded by flush_mapping() and
		 * preserve the existing slot, including its code instrumentation, when
		 * all four quarters are unchanged.
		 */
		for (position = 0; position < darwin_page_size;
		     position += GUEST_PAGE_SIZE) {
			unsigned char backing[GUEST_PAGE_SIZE];
			uintptr_t address = slot + position;

			item = find_mapping(address, mapping_generation(address));
			if (item == NULL || !item->active || !item->loaded)
				continue;
			if (item->original == NULL ||
			    read_guest_page(item->fd, backing, item->offset) < 0)
				return -1;
			if (memcmp(backing, item->original, sizeof(backing)) != 0) {
				changed = 1;
				break;
			}
		}
		if (changed && load_mapping(mapping) < 0)
			return -1;
		for (position = 0; position < darwin_page_size;
		     position += GUEST_PAGE_SIZE) {
			uintptr_t address = slot + position;

			item = find_mapping(address, mapping_generation(address));
			if (item != NULL && item->active && item->loaded)
				item->refresh_epoch = epoch;
		}
	}
	return 0;
}

int darwin_uml_vm_sync_to_backing(unsigned long address_value)
{
	uintptr_t address = align_down((uintptr_t)address_value,
				       GUEST_PAGE_SIZE);
	struct page_mapping *mapping = find_mapping(
		address, mapping_generation(address));

	if (mapping == NULL || !mapping->active || !mapping->loaded)
		return 0;
	return flush_mapping(mapping);
}

int darwin_uml_vm_sync_from_backing(unsigned long address_value)
{
	uintptr_t address = align_down((uintptr_t)address_value,
				       GUEST_PAGE_SIZE);
	struct page_mapping *mapping = find_mapping(
		address, mapping_generation(address));
	uintptr_t slot;
	int protection;
	int executable;
	int result = -1;

	if (mapping == NULL || !mapping->active || !mapping->loaded)
		return 0;
	slot = align_down(address, darwin_page_size);
	protection = runtime_slot_protection(slot);
	executable = mapping->guest_code;
	if (mapping->original == NULL)
		mapping->original = malloc(GUEST_PAGE_SIZE);
	if (mapping->original == NULL)
		return -1;
	if (mprotect((void *)slot, darwin_page_size,
		     PROT_READ | PROT_WRITE) < 0)
		return -1;
	if (executable)
		reset_mapping_instrumentation(mapping);
	if (read_guest_page(mapping->fd, (void *)address, mapping->offset) < 0)
		goto restore;
	memcpy(mapping->original, (const void *)address, GUEST_PAGE_SIZE);
	mapping->discard_changes = 0;
	if (executable) {
		if (instrument_mandatory_exec_page(mapping))
			__builtin___clear_cache((char *)mapping->address,
				(char *)(mapping->address + GUEST_PAGE_SIZE));
		mapping->instrumentation_valid = 1;
	}
	result = 0;
restore:
	if (mprotect((void *)slot, darwin_page_size, protection) < 0)
		return -1;
	if (protection & PROT_WRITE)
		mark_slot_writable_exposed(slot);
	if (result == 0 && executable &&
	    instrument_known_code_in_slot(slot) < 0)
		return -1;
	return result;
}

int darwin_uml_vm_map(void *address_pointer, int fd,
		      unsigned long long offset, unsigned long length,
		      int protection, int guest_code, int guest_writable)
{
	uintptr_t address = (uintptr_t)address_pointer;
	unsigned long generation = mapping_generation(address);
	unsigned long position;

	/*
	 * Upstream UML historically asks the host for executable kernel
	 * mappings even when the PTE is writable.  Darwin enforces W^X, and
	 * executable guest pages are never written in place: syscall patching
	 * is done while their 16 KiB shadow slot is writable in load_mapping().
	 */
	if (address >= GUEST_ADDRESS_LIMIT &&
	    (protection & (PROT_WRITE | PROT_EXEC)) ==
	    (PROT_WRITE | PROT_EXEC))
		protection &= ~PROT_EXEC;

	if (!is_shadow_mapping(address, offset, length)) {
		void *result = mmap(address_pointer, length, protection,
				    MAP_FIXED | MAP_SHARED, fd, (off_t)offset);
		return result == MAP_FAILED ? -1 : 0;
	}
	for (position = 0; position < length; position += GUEST_PAGE_SIZE) {
		struct page_mapping *mapping =
			find_mapping(address + position, generation);
		int new_mapping = 0;
		int backing_changed;
		int protection_changed;

		if (mapping == NULL) {
			mapping = calloc(1, sizeof(*mapping));
			if (mapping == NULL)
				return -1;
			mapping->address = address + position;
			mapping->generation = generation;
			insert_mapping(mapping);
			new_mapping = 1;
		} else if (mapping->writable_exposed &&
			   flush_mapping(mapping) < 0) {
			return -1;
		}
		if (mapping->fd != fd || mapping->offset != offset + position ||
		    mapping->guest_code != !!guest_code) {
			if (store_code_cache(mapping) < 0)
				return -1;
			reset_mapping_instrumentation(mapping);
		}
		backing_changed = new_mapping || mapping->fd != fd ||
			mapping->offset != offset + position;
		protection_changed = new_mapping ||
			mapping->protection != protection ||
			mapping->guest_code != !!guest_code ||
			mapping->guest_writable != !!guest_writable;
		if (!new_mapping && backing_changed)
			remove_backing_hash(mapping);
		mapping->fd = fd;
		mapping->offset = offset + position;
		if (backing_changed)
			insert_backing_hash(mapping);
		mapping->protection = protection;
		mapping->guest_code = !!guest_code;
		mapping->guest_writable = !!guest_writable;
		mapping->active = generation == 0 || generation == active_generation;
		if (backing_changed || protection_changed)
			set_slot_write_validated(
				align_down(mapping->address, darwin_page_size), 0);
		if (mapping->active) {
			uintptr_t slot = align_down(mapping->address,
						    darwin_page_size);

			/*
			 * Darwin walks every present PTE during a userspace TLB sync so
			 * it can discover newly prefaulted 4 KiB siblings.  Most records
			 * are already materialized and byte-for-byte identical.  Reusing
			 * those shadows avoids replacing and rereading an entire 16 KiB
			 * slot on every syscall without weakening change detection.
			 */
			if (mapping->loaded && !backing_changed &&
			    !protection_changed)
				continue;
			/* A descheduled mm retains PTE metadata but none of its host
			 * shadows.  Broad fork/COW TLB ranges can revisit every one of
			 * those PTEs; recording their new metadata must not eagerly rebuild
			 * the complete address space.  retry_fault() materializes the exact
			 * slot the guest subsequently touches.  If a sibling quarter is
			 * already exposed, rebuild now because Darwin's 16 KiB granule would
			 * otherwise hide the new 4 KiB mapping behind anonymous zero-fill.
			 */
			if (mapping->generation != 0 && !mapping->loaded &&
			    !slot_has_loaded_mapping(slot))
				continue;

			if (mapping_batch_depth != 0) {
				if (queue_slot_rebuild(slot) < 0)
					return -1;
			} else if (load_mapping(mapping) < 0) {
				return -1;
			}
		}
	}
	return 0;
}

static int change_mixed_slot(uintptr_t address, int execute, int write)
{
	unsigned long generation = mapping_generation(address);
	uintptr_t page = align_down(address, GUEST_PAGE_SIZE);
	struct page_mapping *mapping = find_mapping(page, generation);
	uintptr_t slot;
	int protection;

	if (mapping == NULL || !mapping->active || !mapping->loaded)
		return 0;
	if (execute && !(mapping->protection & PROT_EXEC))
		return 0;
	if (write && !(mapping->protection & PROT_WRITE))
		return 0;
	if (!execute && !write)
		return 0;
	slot = align_down(address, darwin_page_size);
	/* Keep a mixed read/write host slot protected.  The kernel fault path
	 * either resolves COW for all writable 4 KiB siblings or requests a
	 * hardware post-store breakpoint for a genuine permission boundary.
	 */
	if (write && slot_has_mixed_write_permissions(slot))
		return 0;
	protection = slot_protection(slot);
	if ((protection & (PROT_WRITE | PROT_EXEC)) !=
	    (PROT_WRITE | PROT_EXEC))
		return 0;
	if (execute)
		protection = (protection | PROT_READ | PROT_EXEC) & ~PROT_WRITE;
	else
		protection = (protection | PROT_READ | PROT_WRITE) & ~PROT_EXEC;
	if (mprotect((void *)slot, darwin_page_size, protection) < 0)
		return -1;
	if (write)
		mark_slot_writable_exposed(slot);
	return 1;
}

static int queue_code_target(uintptr_t **targets, size_t *count,
			     size_t *capacity, uintptr_t target)
{
	uintptr_t *grown;

	if (*count < *capacity) {
		(*targets)[(*count)++] = target;
		return 0;
	}
	if (*capacity >= 65536)
		return -1;
	*capacity *= 2;
	grown = realloc(*targets, *capacity * sizeof(**targets));
	if (grown == NULL)
		return -1;
	*targets = grown;
	(*targets)[(*count)++] = target;
	return 0;
}

static int instrument_guest_code(uintptr_t entry)
{
	size_t capacity = 1024;
	size_t count = 0;
	size_t decoded_count = 0;
	uintptr_t *targets = malloc(capacity * sizeof(*targets));
	int result = 0;

	if (targets == NULL)
		return -1;
	targets[count++] = entry;
	while (count != 0 && decoded_count < 1000000) {
		uintptr_t address = targets[--count];
		unsigned long generation = mapping_generation(address);

		if (remember_code_target(address, generation) < 0) {
			result = -1;
			break;
		}

		for (;;) {
			uintptr_t page = align_down(address, GUEST_PAGE_SIZE);
			struct page_mapping *mapping = find_mapping(
				page, mapping_generation(page));
			unsigned int index;
			uint32_t instruction;
			int stop_block = 0;

			if (mapping == NULL || !mapping->active || !mapping->loaded) {
				if (remember_code_target(address, generation) < 0)
					result = -1;
				break;
			}
			if (
			    !mapping->guest_code ||
			    mapping->original == NULL)
				break;
			mark_code_target_resolved(address, generation);
			if (!mapping->special_scanned &&
			    instrument_special_code_page(mapping) < 0) {
				result = -1;
				break;
			}
			index = (unsigned int)((address - page) /
					       sizeof(uint32_t));
			if (was_patched(mapping->code_known, index))
				break;
			memcpy(&instruction,
			       mapping->original + index * sizeof(uint32_t),
			       sizeof(instruction));
			mark_patch(mapping->code_known, index);
			decoded_count++;

			if ((instruction & 0xfffffc1fU) == 0xd61f0000U ||
			    (instruction & 0xfffffc1fU) == 0xd63f0000U ||
			    (instruction & 0xfffffc1fU) == 0xd65f0000U) {
				int is_return =
					(instruction & 0xfffffc1fU) == 0xd65f0000U;
				int is_call =
					(instruction & 0xfffffc1fU) == 0xd63f0000U;

				if (!is_return &&
				    patch_known_instruction(mapping, index,
						indirect_replacement(address,
							mapping->generation),
							    mapping->indirect_patched) < 0)
					result = -1;
				if (is_call &&
				    queue_code_target(&targets, &count, &capacity,
							      address + sizeof(uint32_t)) < 0)
					result = -1;
				stop_block = 1;
			} else if (instruction == LINUX_SVC_0) {
				if (patch_known_instruction(mapping, index,
							UML_SYSCALL_BRK,
							mapping->syscall_patched) < 0)
					result = -1;
			} else if ((instruction & ~31U) ==
				   AARCH64_MSR_TPIDR_EL0) {
				unsigned int reg = instruction & 31U;

				if (patch_known_instruction(mapping, index,
						tls_write_replacement(reg),
						mapping->tls_write_patched) < 0)
					result = -1;
			} else if ((instruction & ~31U) ==
				   AARCH64_MRS_TPIDR_EL0) {
				unsigned int reg = instruction & 31U;

				if (patch_known_instruction(mapping, index,
						tls_read_replacement(address, reg),
						mapping->tls_read_patched) < 0)
					result = -1;
			} else if (instruction_uses_x18(instruction, address)) {
				if (instrument_x18_code_page(mapping) < 0 ||
				    patch_known_instruction(mapping, index,
					x18_replacement(address,
						mapping->generation, instruction),
							    mapping->x18_patched) < 0)
					result = -1;
			}

			if ((instruction & 0x7c000000U) == 0x14000000U) {
				if (queue_code_target(&targets, &count, &capacity,
						direct_branch_target(address,
							instruction & 0x03ffffffU, 26)) < 0)
					result = -1;
				/* B has no fallthrough; BL returns to the next word. */
				if (!(instruction & 0x80000000U))
					stop_block = 1;
			} else if ((instruction & 0xff000010U) == 0x54000000U ||
				   (instruction & 0x7e000000U) == 0x34000000U) {
				if (queue_code_target(&targets, &count, &capacity,
						direct_branch_target(address,
							(instruction >> 5) & 0x7ffffU,
							19)) < 0)
					result = -1;
			} else if ((instruction & 0x7e000000U) == 0x36000000U) {
				if (queue_code_target(&targets, &count, &capacity,
						direct_branch_target(address,
							(instruction >> 5) & 0x3fffU,
							14)) < 0)
					result = -1;
			}
			if (result < 0 || stop_block)
				break;
			address += sizeof(uint32_t);
		}
		if (result < 0)
			break;
	}
	free(targets);
	return result;
}

static int instrument_known_code_in_slot(uintptr_t slot)
{
	struct known_code_target *target;
	unsigned long generation = mapping_generation(slot);
	unsigned int bucket;

	if (darwin_page_size == 0)
		darwin_page_size = (size_t)getpagesize();
	bucket = known_code_region_hash_index(slot, generation, darwin_page_size);

	for (target = known_code_slot_hash[bucket]; target != NULL;
	     target = target->slot_hash_next) {
		struct page_mapping *mapping;

		if (target->resolved ||
		    target->generation != generation ||
		    align_down(target->address, darwin_page_size) != slot)
			continue;
		mapping = find_mapping(align_down(target->address, GUEST_PAGE_SIZE),
				       target->generation);
		if (mapping == NULL || !mapping->active || !mapping->loaded ||
		    !mapping->guest_code)
			continue;
		if (instrument_guest_code(target->address) < 0)
			return -1;
	}
	return 0;
}

int darwin_uml_vm_prepare_execute(unsigned long address)
{
	int result = instrument_guest_code((uintptr_t)address);

	if (result >= 0 && change_mixed_slot((uintptr_t)address, 1, 0) < 0)
		result = -1;
	if (seal_gateway_pages() < 0)
		result = -1;
	return result < 0 ? -1 : 0;
}

int darwin_uml_vm_promote_syscall(unsigned long address_value)
{
	uintptr_t address;
	uintptr_t page;
	struct page_mapping *mapping;
	unsigned int index;
	uint32_t original;
	uint32_t replacement;
	int result;

	if (!direct_gateway_enabled())
		return 0;
	address = (uintptr_t)address_value;
	page = align_down(address, GUEST_PAGE_SIZE);
	mapping = find_mapping(page, mapping_generation(page));
	index = (unsigned int)((address - page) / sizeof(uint32_t));
	if (mapping == NULL || !mapping->active || !mapping->loaded ||
	    mapping->original == NULL ||
	    !was_patched(mapping->syscall_patched, index) ||
	    was_patched(mapping->syscall_direct_patched, index))
		return 0;
	if (mapping->syscall_trap_count[index] <
	    DIRECT_SYSCALL_PROMOTION_THRESHOLD) {
		mapping->syscall_trap_count[index]++;
		if (mapping->syscall_trap_count[index] <
		    DIRECT_SYSCALL_PROMOTION_THRESHOLD)
			return 0;
	}
	memcpy(&original, mapping->original + index * sizeof(original),
	       sizeof(original));
	if (original != LINUX_SVC_0)
		return 0;
	replacement = syscall_replacement(address);
	if (replacement == UML_SYSCALL_BRK)
		return 0;
	result = patch_known_instruction(mapping, index, replacement,
		mapping->syscall_direct_patched);
	if (seal_gateway_pages() < 0)
		return -1;
	return result < 0 ? -1 : 1;
}

int darwin_uml_vm_validate_syscall(unsigned long address_value)
{
	uintptr_t address = (uintptr_t)address_value;
	uintptr_t page = align_down(address, GUEST_PAGE_SIZE);
	struct page_mapping *mapping = find_mapping(
		page, mapping_generation(page));
	unsigned int index = (unsigned int)((address - page) / sizeof(uint32_t));
	uintptr_t slot;
	uint32_t original;
	int protection;

	if (mapping == NULL || !mapping->active || !mapping->loaded ||
	    mapping->original == NULL)
		return -1;
	memcpy(&original, mapping->original + index * sizeof(original),
	       sizeof(original));
	if (original == LINUX_SVC_0) {
		mark_patch(mapping->syscall_patched, index);
		return 1;
	}

	/* The live BRK did not originate from a Linux SVC.  Repair the pristine
	 * instruction and retry it instead of fabricating a syscall from the
	 * register contents at an arbitrary program point.
	 */
	clear_patch(mapping->syscall_patched, index);
	clear_patch(mapping->syscall_direct_patched, index);
	mapping->syscall_trap_count[index] = 0;
	slot = align_down(address, darwin_page_size);
	protection = runtime_slot_protection(slot);
	if (mprotect((void *)slot, darwin_page_size,
		     PROT_READ | PROT_WRITE) < 0)
		return -1;
	memcpy((void *)address, &original, sizeof(original));
	__builtin___clear_cache((char *)address,
				  (char *)(address + sizeof(original)));
	if (mprotect((void *)slot, darwin_page_size, protection) < 0)
		return -1;
	return 0;
}

int darwin_uml_vm_begin_write_step(unsigned long address_value)
{
	uintptr_t address = (uintptr_t)address_value;
	uintptr_t page = align_down(address, GUEST_PAGE_SIZE);
	uintptr_t slot = align_down(address, darwin_page_size);
	struct page_mapping *mapping = find_mapping(
		page, mapping_generation(page));
	int protection;

	if (mapping == NULL || !mapping->active || !mapping->loaded ||
	    !(mapping->protection & PROT_WRITE) ||
	    !slot_has_mixed_write_permissions(slot))
		return 0;
	protection = (slot_protection(slot) | PROT_READ | PROT_WRITE) &
		~PROT_EXEC;
	if (mprotect((void *)slot, darwin_page_size, protection) < 0)
		return -1;
	mark_slot_writable_exposed(slot);
	return 1;
}

int darwin_uml_vm_finish_write_step(unsigned long address_value)
{
	uintptr_t slot = align_down((uintptr_t)address_value, darwin_page_size);

	return mprotect((void *)slot, darwin_page_size,
		runtime_slot_protection(slot));
}

static int enable_clean_slot_write(uintptr_t address)
{
	uintptr_t page = align_down(address, GUEST_PAGE_SIZE);
	uintptr_t slot = align_down(address, darwin_page_size);
	struct page_mapping *mapping = find_mapping(
		page, mapping_generation(page));
	int protection;

	if (mapping == NULL || !mapping->active || !mapping->loaded ||
	    !(mapping->protection & PROT_WRITE) ||
	    slot_has_mixed_write_permissions(slot))
		return 0;
	set_slot_write_validated(slot, 1);
	protection = runtime_slot_protection(slot);
	if (!(protection & PROT_WRITE) ||
	    mprotect((void *)slot, darwin_page_size, protection) < 0) {
		set_slot_write_validated(slot, 0);
		return -1;
	}
	mark_slot_writable_exposed(slot);
	return 1;
}

int darwin_uml_vm_resolve_fault(unsigned long address, int execute, int write)
{
	uintptr_t slot = align_down((uintptr_t)address, darwin_page_size);
	int result;

	if (write && !slot_write_validated(slot)) {
		result = enable_clean_slot_write((uintptr_t)address);
		(void)seal_gateway_pages();
		return result;
	}
	result = change_mixed_slot((uintptr_t)address, execute, write);
	if (seal_gateway_pages() < 0)
		return -1;
	return result;
}

int darwin_uml_vm_retry_fault(unsigned long address, int execute, int write)
{
	unsigned long generation = mapping_generation((uintptr_t)address);
	uintptr_t slot = align_down((uintptr_t)address, darwin_page_size);
	struct page_mapping *mapping = find_mapping(
		align_down((uintptr_t)address, GUEST_PAGE_SIZE), generation);
	int resolved;

	if (mapping == NULL || !mapping->active) {
		(void)seal_gateway_pages();
		return 0;
	}
	if (!mapping->loaded && load_mapping(mapping) < 0) {
		(void)seal_gateway_pages();
		return -1;
	}
	if (execute && !(mapping->protection & PROT_EXEC)) {
		(void)seal_gateway_pages();
		return 0;
	}
	if (write && !(mapping->protection & PROT_WRITE)) {
		(void)seal_gateway_pages();
		return 0;
	}
	if (!execute && !write && !(mapping->protection & PROT_READ)) {
		(void)seal_gateway_pages();
		return 0;
	}
	if (write && slot_has_mixed_write_permissions(slot)) {
		(void)seal_gateway_pages();
		return 2;
	}
	if (write && !slot_write_validated(slot)) {
		resolved = enable_clean_slot_write((uintptr_t)address);
		if (seal_gateway_pages() < 0)
			return -1;
		return resolved;
	}
	resolved = change_mixed_slot((uintptr_t)address, execute, write);
	if (resolved > 0 && write)
		set_slot_write_validated(slot, 1);
	if (seal_gateway_pages() < 0)
		return -1;
	return resolved < 0 ? -1 : 1;
}

int darwin_uml_vm_x18_instruction(unsigned long address_value,
				  unsigned int *instruction)
{
	uintptr_t address = (uintptr_t)address_value;
	uintptr_t page = align_down(address, GUEST_PAGE_SIZE);
	struct page_mapping *mapping = find_mapping(
		page, mapping_generation(page));
	unsigned int index = (unsigned int)((address - page) / sizeof(uint32_t));

	if (mapping == NULL || !mapping->active || !mapping->loaded ||
	    mapping->original == NULL ||
	    !was_patched(mapping->x18_patched, index))
		return 0;
	memcpy(instruction, mapping->original + index * sizeof(uint32_t),
	       sizeof(*instruction));
	return 1;
}

int darwin_uml_vm_patch_x18_fault(unsigned long address_value)
{
	uintptr_t address = (uintptr_t)address_value;
	uintptr_t page = align_down(address, GUEST_PAGE_SIZE);
	struct page_mapping *mapping = find_mapping(
		page, mapping_generation(page));
	unsigned int index = (unsigned int)((address - page) / sizeof(uint32_t));
	uint32_t original;
	uint32_t current;
	uint32_t replacement;
	int result;

	if (mapping == NULL || !mapping->active || !mapping->loaded ||
	    mapping->original == NULL)
		return 0;
	memcpy(&current, (const void *)address, sizeof(current));
	if (!instruction_uses_x18(current, address))
		return 0;
	/* The live word is authoritative on this fault path.  A recycled host
	 * slot can leave both its bitmap and saved pristine copy stale; keeping
	 * the observed guest instruction here also makes later writeback and
	 * code-cache replay restore the correct bytes.
	 */
	original = current;
	memcpy(mapping->original + index * sizeof(original), &original,
	       sizeof(original));
	if (was_patched(mapping->x18_patched, index)) {
		uintptr_t slot;
		int protection;

		/* A 4 KiB executable quarter can be recopied while Darwin applies
		 * permissions to its shared 16 KiB slot.  Repair a live original
		 * word even when the persistent instrumentation bitmap is current.
		 */
		slot = align_down(address, darwin_page_size);
		protection = runtime_slot_protection(slot);
		replacement = x18_replacement(address, mapping->generation,
					      original);
		if (mprotect((void *)slot, darwin_page_size,
			     PROT_READ | PROT_WRITE) < 0)
			return -1;
		memcpy((void *)address, &replacement, sizeof(replacement));
		__builtin___clear_cache((char *)address,
					  (char *)(address + sizeof(replacement)));
		result = mprotect((void *)slot, darwin_page_size, protection);
	} else {
		result = patch_known_instruction(mapping, index,
			x18_replacement(address, mapping->generation, original),
			mapping->x18_patched);
	}
	if (seal_gateway_pages() < 0)
		return -1;
	return result < 0 ? -1 : 1;
}

int darwin_uml_vm_x18_stub_fault(unsigned long native_pc_value,
				 unsigned long *site_value,
				 unsigned int *scratch)
{
	uintptr_t native_pc = (uintptr_t)native_pc_value;
	struct x18_stub *record;
	unsigned int bucket = x18_stub_fault_hash_index(native_pc);

	for (record = x18_stub_fault_hash[bucket]; record != NULL;
	     record = record->fault_hash_next) {
		if (record->native_pc != native_pc)
			continue;
		*site_value = (unsigned long)record->site;
		*scratch = record->scratch;
		return 1;
	}
	return 0;
}

int darwin_uml_vm_gateway_x18_fault(unsigned long native_pc_value,
				    unsigned int *instruction_value)
{
	uintptr_t native_pc = (uintptr_t)native_pc_value;
	struct x18_stub *x18_record;
	unsigned long guest_site;
	unsigned int scratch;
	unsigned int base;
	uint32_t instruction;
	uint32_t operation;

	if (find_gateway_region_address(native_pc) == NULL)
		return 0;
	/* A relocated guest instruction may legitimately fault through guest
	 * x18.  Only repair the runtime's surrounding context accesses.
	 */
	if (darwin_uml_vm_x18_stub_fault(native_pc, &guest_site, &scratch) > 0)
		return 0;
	for (x18_record = x18_stubs; x18_record != NULL;
	     x18_record = x18_record->all_next)
		if (native_pc >= x18_record->address &&
		    native_pc < x18_record->address + X18_STUB_SIZE)
			break;
	memcpy(&instruction, (const void *)native_pc, sizeof(instruction));
	base = (instruction >> 5) & 31U;
	if (x18_record != NULL) {
		if (base != 18 && base != x18_record->scratch)
			return 0;
	} else if (base != 18) {
		return 0;
	}
	operation = instruction & 0xffc00000U;
	if (operation != 0xf9400000U && operation != 0xf9000000U)
		return 0;
	*instruction_value = instruction;
	return x18_record != NULL ? 2 : 1;
}

int darwin_uml_vm_indirect_instruction(unsigned long address_value,
				       unsigned int *instruction,
				       unsigned long *site_value)
{
	uintptr_t address = (uintptr_t)address_value;
	uintptr_t page = align_down(address, GUEST_PAGE_SIZE);
	struct indirect_stub *record = find_indirect_slow_stub(address);
	struct page_mapping *mapping = find_mapping(
		page, mapping_generation(page));
	unsigned int index = (unsigned int)((address - page) / sizeof(uint32_t));

	if (record != NULL && record->generation == active_generation) {
		mapping = find_mapping(align_down(record->site, GUEST_PAGE_SIZE),
				       record->generation);
		if (mapping != NULL && mapping->active && mapping->loaded) {
			*instruction = record->original;
			*site_value = (unsigned long)record->site;
			return 1;
		}
	}
	if (mapping == NULL || !mapping->active || !mapping->loaded ||
	    mapping->original == NULL ||
	    !was_patched(mapping->indirect_patched, index))
		return 0;
	memcpy(instruction, mapping->original + index * sizeof(uint32_t),
	       sizeof(*instruction));
	*site_value = address_value;
	return 1;
}

static struct indirect_history_site *find_indirect_history(
		struct page_mapping *mapping, unsigned int index,
		uint32_t original, int create)
{
	struct code_cache_entry *page;
	struct indirect_history_site *history;

	if (mapping == NULL || mapping->original == NULL)
		return NULL;
	page = ensure_code_cache(mapping->original);
	if (page == NULL)
		return NULL;
	for (history = page->indirect_history; history != NULL;
	     history = history->next)
		if (history->index == index && history->original == original)
			return history;
	if (!create)
		return NULL;
	history = calloc(1, sizeof(*history));
	if (history == NULL)
		return NULL;
	history->index = index;
	history->original = original;
	history->next = page->indirect_history;
	page->indirect_history = history;
	return history;
}

static uintptr_t warm_indirect_gateway(uintptr_t site,
				       unsigned long generation)
{
	uintptr_t page = align_down(site, GUEST_PAGE_SIZE);
	struct page_mapping *mapping = find_mapping(page, generation);
	struct code_cache_entry *source_page;
	unsigned int index =
		(unsigned int)((site - page) / sizeof(uint32_t));
	uint32_t original;

	if (mapping == NULL || !mapping->active || !mapping->loaded ||
	    mapping->original == NULL || !mapping->guest_code)
		return 0;
	memcpy(&original, mapping->original + index * sizeof(original),
	       sizeof(original));
	if (find_indirect_history(mapping, index, original, 0) == NULL)
		return 0;
	source_page = ensure_code_cache(mapping->original);
	if (source_page == NULL)
		return 0;
	/* A history entry proves that this site executed in a prior address
	 * space.  Start with an empty target table when the old targets are not
	 * mapped yet; the direct miss coroutine fills it before the branch is
	 * resumed, without paying an initial Mach exception.
	 */
	return indirect_gateway_stub(site, generation, original, source_page,
		NULL, 0);
}

static void record_indirect_history(struct page_mapping *mapping,
		unsigned int index, uint32_t original, uintptr_t site,
		uintptr_t target)
{
	struct indirect_history_site *history;
	struct page_mapping *target_mapping;
	struct code_cache_entry *target_page;
	int64_t delta;
	unsigned int page_offset;
	unsigned int position;

	history = find_indirect_history(mapping, index, original, 1);
	if (history == NULL)
		return;
	target_mapping = find_mapping(align_down(target, GUEST_PAGE_SIZE),
		mapping_generation(target));
	if (target_mapping == NULL || !target_mapping->active ||
	    !target_mapping->loaded || target_mapping->original == NULL ||
	    !target_mapping->guest_code)
		return;
	target_page = ensure_code_cache(target_mapping->original);
	if (target_page == NULL)
		return;
	delta = (int64_t)target - (int64_t)site;
	page_offset = (unsigned int)(target & (GUEST_PAGE_SIZE - 1));
	for (position = 0; position < history->target_count; position++)
		if (history->targets[position].delta == delta &&
		    history->targets[position].page_offset == page_offset &&
		    history->targets[position].target_page == target_page)
			return;
	if (history->target_count >= INDIRECT_STUB_TARGETS)
		return;
	history->targets[history->target_count].delta = delta;
	history->targets[history->target_count].page_offset = page_offset;
	history->targets[history->target_count].target_page = target_page;
	history->target_count++;
}

static unsigned int collect_indirect_history(struct page_mapping *mapping,
		unsigned int index, uint32_t original, uintptr_t site,
		uintptr_t current_target, uint64_t *targets)
{
	struct indirect_history_site *history = find_indirect_history(
		mapping, index, original, 0);
	unsigned int count = 1;
	unsigned int position;

	targets[0] = current_target;
	if (history == NULL)
		return count;
	for (position = 0; position < history->target_count &&
	     count < INDIRECT_STUB_TARGETS; position++) {
		const struct indirect_history_target *saved =
			&history->targets[position];
		uintptr_t candidate = (uintptr_t)((int64_t)site + saved->delta);
		struct page_mapping *target_mapping;
		unsigned int duplicate;

		if ((candidate & 3U) != 0 ||
		    (candidate & (GUEST_PAGE_SIZE - 1)) != saved->page_offset)
			continue;
		for (duplicate = 0; duplicate < count; duplicate++)
			if (targets[duplicate] == candidate)
				break;
		if (duplicate != count)
			continue;
		target_mapping = find_mapping(
			align_down(candidate, GUEST_PAGE_SIZE),
			mapping_generation(candidate));
		if (target_mapping == NULL || !target_mapping->active ||
		    !target_mapping->guest_code)
			continue;
		if (!target_mapping->loaded && load_mapping(target_mapping) < 0)
			continue;
		if (target_mapping->original == NULL ||
		    memcmp(target_mapping->original, saved->target_page->original,
			   GUEST_PAGE_SIZE) != 0)
			continue;
		if (instrument_guest_code(candidate) < 0 ||
		    change_mixed_slot(candidate, 1, 0) < 0)
			continue;
		targets[count++] = candidate;
	}
	return count;
}

int darwin_uml_vm_cache_indirect(unsigned long site_value,
				 unsigned int original,
				 unsigned long target_value)
{
	uintptr_t site = (uintptr_t)site_value;
	uintptr_t page = align_down(site, GUEST_PAGE_SIZE);
	unsigned long generation = mapping_generation(page);
	struct page_mapping *mapping = find_mapping(page, generation);
	struct indirect_stub *record;
	struct code_cache_entry *source_page;
	unsigned int index = (unsigned int)((site - page) / sizeof(uint32_t));
	uintptr_t stub;
	uintptr_t slot;
	uint32_t replacement;
	uint64_t initial_targets[INDIRECT_STUB_TARGETS];
	unsigned int initial_target_count;
	int protection;

	if (mapping == NULL || !mapping->active || !mapping->loaded ||
	    mapping->original == NULL ||
	    !was_patched(mapping->indirect_patched, index) ||
	    memcmp(mapping->original + index * sizeof(uint32_t), &original,
		   sizeof(original)) != 0)
		return 0;
	source_page = ensure_code_cache(mapping->original);
	if (source_page == NULL)
		return 0;
	record = find_indirect_stub(site, generation);
	if (record != NULL) {
		struct syscall_gateway_region *region;
		uint64_t target = (uint64_t)target_value;
		uintptr_t stub_page;
		unsigned int target_index;

		/* A warmed site can reach its slow coroutine before this generation
		 * has executed the dynamic target.  Apply all mandatory guest-code
		 * instrumentation before the coroutine resumes and branches there.
		 */
		if (darwin_uml_vm_prepare_execute(target_value) < 0)
			return -1;
		record_indirect_history(mapping, index, original, site,
					(uintptr_t)target_value);
		for (target_index = 0; target_index < record->target_count;
		     target_index++)
			if (target == record->cached_targets[target_index])
				return 0;
		if (record->target_count >= INDIRECT_STUB_TARGETS)
			return 0;
		stub_page = align_down(record->address, darwin_page_size);
		region = find_syscall_gateway_region(site);
		if (region == NULL ||
		    gateway_make_writable(region, stub_page) < 0)
			return -1;
		memcpy((void *)(record->address + INDIRECT_STUB_TARGET_BASE +
			record->target_count * sizeof(target)),
		       &target, sizeof(target));
		if (seal_gateway_pages() < 0)
			return -1;
		record->cached_targets[record->target_count++] = target;
		return 1;
	}
	initial_target_count = collect_indirect_history(mapping, index, original,
		site, (uintptr_t)target_value, initial_targets);
	stub = indirect_gateway_stub(site, generation, original, source_page,
		initial_targets, initial_target_count);
	if (stub == 0 || aarch64_branch(site, stub, &replacement) < 0) {
		(void)seal_gateway_pages();
		return 0;
	}
	slot = align_down(site, darwin_page_size);
	protection = runtime_slot_protection(slot);
	if (mprotect((void *)slot, darwin_page_size,
		     PROT_READ | PROT_WRITE) < 0) {
		(void)seal_gateway_pages();
		return -1;
	}
	memcpy((void *)site, &replacement, sizeof(replacement));
	__builtin___clear_cache((char *)site,
				  (char *)(site + sizeof(replacement)));
	if (mprotect((void *)slot, darwin_page_size, protection) < 0) {
		(void)seal_gateway_pages();
		return -1;
	}
	if (seal_gateway_pages() < 0)
		return -1;
	record_indirect_history(mapping, index, original, site,
				(uintptr_t)target_value);
	return 1;
}

int darwin_uml_vm_cache_direct_indirect(unsigned long resume_stub_value,
					unsigned long target_value)
{
	uintptr_t resume_stub = (uintptr_t)resume_stub_value;
	struct indirect_stub *record;

	if (resume_stub < INDIRECT_DIRECT_RESUME_OFFSET)
		return 0;
	record = find_indirect_slow_stub(
		resume_stub - INDIRECT_DIRECT_RESUME_OFFSET);
	if (record == NULL || record->generation != active_generation)
		return 0;
	return darwin_uml_vm_cache_indirect(record->site, record->original,
						 target_value);
}

int darwin_uml_vm_protect(void *address_pointer, unsigned long length,
			  int protection)
{
	uintptr_t address = (uintptr_t)address_pointer;
	unsigned long generation = mapping_generation(address);
	unsigned long position;
	int found = 0;

	for (position = 0; position < length; position += GUEST_PAGE_SIZE) {
		struct page_mapping *mapping =
			find_mapping(address + position, generation);

		if (mapping != NULL) {
			int protection_changed =
				mapping->protection != protection;

			if (mapping->writable_exposed &&
			    flush_mapping(mapping) < 0)
				return -1;
			if (!(mapping->protection & PROT_EXEC) &&
			    (protection & PROT_EXEC))
				reset_mapping_instrumentation(mapping);
			mapping->protection = protection;
			if (protection_changed)
				set_slot_write_validated(
					align_down(mapping->address,
						   darwin_page_size), 0);
			found = 1;
			if (mapping->active && load_mapping(mapping) < 0)
				return -1;
		}
	}
	if (found)
		return 0;
	if (darwin_page_size == 0)
		darwin_page_size = (size_t)getpagesize();
	address = align_down(address, darwin_page_size);
	length = (unsigned long)((((uintptr_t)address_pointer + length +
			darwin_page_size - 1) & ~(darwin_page_size - 1)) - address);
	if (mprotect((void *)address, length, protection) < 0) {
		fprintf(stderr,
			"Darwin VM: direct protect failed address=%p length=0x%lx prot=%x gen=%lu errno=%d\n",
			(void *)address, length, protection, generation, errno);
		return -1;
	}
	return 0;
}

int darwin_uml_vm_unmap(void *address_pointer, unsigned long length)
{
	uintptr_t address = (uintptr_t)address_pointer;
	unsigned long generation = mapping_generation(address);
	uintptr_t end = address + length;
	uintptr_t position;

	if (length == 0)
		return 0;
	if (end < address)
		return -1;
	position = align_down(address, GUEST_PAGE_SIZE);
	for (; position < end; position += GUEST_PAGE_SIZE) {
		struct page_mapping *mapping = find_mapping(position, generation);
		uintptr_t slot;

		if (mapping == NULL)
			continue;
		slot = align_down(mapping->address, darwin_page_size);
		if (mapping->writable_exposed && flush_mapping(mapping) < 0)
			return -1;
		if (store_code_cache(mapping) < 0)
			return -1;
		remove_mapping_list(mapping);
		remove_mapping_hash(mapping);
		remove_backing_hash(mapping);
		free(mapping->original);
		free(mapping);
		set_slot_write_validated(slot, 0);
		if (mapping_batch_depth != 0) {
			if (queue_slot_rebuild(slot) < 0)
				return -1;
		} else if (rebuild_slot(slot) < 0 && errno != EINVAL) {
			return -1;
		}
	}
	return 0;
}

int darwin_uml_vm_drop(void *address, unsigned long length)
{
	uintptr_t start = (uintptr_t)address;
	unsigned long generation = mapping_generation(start);
	struct page_mapping *mapping;

	for (mapping = mappings; mapping != NULL; mapping = mapping->next)
		if (mapping->generation == generation && mapping->active &&
		    mapping->address >= start && mapping->address < start + length &&
		    mapping->writable_exposed &&
		    flush_mapping(mapping) < 0)
			return -1;
	return 0;
}
