// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Freestanding x86-64 guest probe for the "mm is shared" dispatch flag
 * (DARWIN_UML_DISPATCH_MM_SHARED). It prints its own code range, then runs
 * the mode its first argument names:
 *
 *   single        only system calls on its own;
 *   fork          fork() a child, which exits;
 *   vfork         vfork() a child, which exits;
 *   private-file  map a memfd MAP_PRIVATE and write it;
 *   thread        clone() a thread that shares the address space;
 *   shared-anon   map shared anonymous memory and write it;
 *   shared-ro     map a memfd MAP_SHARED, read-only, and read it;
 *   shm           attach a SysV shm segment and write it;
 *   uring-nommap  set up an io_uring whose rings live in the probe's own
 *                 memory (IORING_SETUP_NO_MMAP);
 *   fork-shared   map shared anonymous memory, then fork() a child, which
 *                 inherits the mapping and the mark;
 *   thread-fork   clone() a thread, then fork() a child, whose mm has one
 *                 task and no shared mapping.
 *
 * The first four leave the mm unshared; each of the others shares the
 * probe's mm, and fork-shared its child's too.
 */

namespace {

using u64 = unsigned long;

constexpr long nr_write = 1;
constexpr long nr_close = 3;
constexpr long nr_mmap = 9;
constexpr long nr_sched_yield = 24;
constexpr long nr_shmget = 29;
constexpr long nr_shmat = 30;
constexpr long nr_shmctl = 31;
constexpr long nr_getpid = 39;
constexpr long nr_fork = 57;
constexpr long nr_vfork = 58;
constexpr long nr_exit = 60;
constexpr long nr_wait4 = 61;
constexpr long nr_ftruncate = 77;
constexpr long nr_memfd_create = 319;
constexpr long nr_io_uring_setup = 425;

constexpr long prot_read = 1;
constexpr long prot_read_write = 3;
constexpr long map_shared = 0x01;
constexpr long map_private = 0x02;
constexpr long map_anonymous = 0x20;
constexpr long page_size = 4096;

constexpr u64 clone_thread_flags = 0x00000100 | 0x00000200 | 0x00000400 |
	0x00000800 | 0x00010000 | 0x00040000; /* VM FS FILES SIGHAND THREAD SYSVSEM */

inline long sys(long number, long a0 = 0, long a1 = 0, long a2 = 0, long a3 = 0,
		long a4 = 0, long a5 = 0)
{
	long result;
	register long r10 __asm__("r10") = a3;
	register long r8 __asm__("r8") = a4;
	register long r9 __asm__("r9") = a5;
	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return result;
}

bool failed(long result)
{
	return result < 0 && result > -4096;
}

void write_text(const char *text)
{
	long length = 0;
	while (text[length] != '\0')
		length++;
	sys(nr_write, 1, reinterpret_cast<long>(text), length);
}

void write_hex(u64 value)
{
	char text[19] = "0x";
	for (int index = 0; index < 16; index++)
		text[2 + index] = "0123456789abcdef"[(value >> (60 - 4 * index)) & 15];
	text[18] = '\0';
	write_text(text);
}

bool same(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}
	return *left == *right;
}

volatile int thread_done;

alignas(16) unsigned char thread_stack[64 * 1024];

/* io_uring rings in the probe's own memory: the rings in one page, the
 * submission entries in another. */
alignas(4096) unsigned char uring_rings[page_size];
alignas(4096) unsigned char uring_entries[page_size];

/* struct io_uring_params, with only what the probe sets named. */
struct UringParams {
	unsigned sq_entries;
	unsigned cq_entries;
	unsigned flags;
	unsigned rest[7];
	unsigned sq_off[8];
	unsigned long sq_user_addr;
	unsigned cq_off[8];
	unsigned long cq_user_addr;
};
static_assert(sizeof(UringParams) == 120);
constexpr unsigned uring_setup_no_mmap = 1U << 14;

/* Maps a fresh memfd of one page and returns the mapping, or 0. */
long map_memfd(long protection, long flags)
{
	const long descriptor = sys(nr_memfd_create, reinterpret_cast<long>("mm-shared"), 0);
	if (failed(descriptor) || sys(nr_ftruncate, descriptor, page_size) != 0)
		return 0;
	const long address = sys(nr_mmap, 0, page_size, protection, flags, descriptor, 0);
	sys(nr_close, descriptor);
	return failed(address) ? 0 : address;
}

} // namespace

extern "C" {
extern const char __ehdr_start[];
extern const char etext[];
long fork_exit(long number);
long clone_thread(u64 flags, void *stack, int (*function)());
int thread_main();
[[noreturn]] void probe_main(long argc, char **argv);
}

int thread_main()
{
	for (int round = 0; round < 8; round++)
		sys(nr_getpid);
	thread_done = 1;
	return 0;
}

__asm__(R"(
	.text
	.globl fork_exit
fork_exit:			# fork or vfork; the child exits at once
	mov	%rdi, %rax
	syscall
	test	%rax, %rax
	jnz	1f
	mov	$60, %eax
	xor	%edi, %edi
	syscall
1:	ret

	.globl clone_thread
clone_thread:			# rdi flags, rsi stack, rdx function
	mov	%rdx, %r9
	xor	%edx, %edx
	xor	%r10d, %r10d
	xor	%r8d, %r8d
	mov	$56, %eax
	syscall
	test	%rax, %rax
	jnz	1f
	call	*%r9
	mov	%eax, %edi
	mov	$60, %eax
	syscall
1:	ret

	.globl _start
_start:
	mov	(%rsp), %rdi
	lea	8(%rsp), %rsi
	and	$-16, %rsp
	call	probe_main
	hlt
)");

namespace {

/* Starts a thread on the probe's mm and waits until it is done. */
bool run_thread()
{
	if (clone_thread(clone_thread_flags, thread_stack + sizeof(thread_stack), thread_main) <= 0)
		return false;
	while (!thread_done)
		sys(nr_sched_yield);
	return true;
}

/* fork() or vfork() a child that exits at once, and reap it. */
bool fork_child(long number)
{
	const long pid = fork_exit(number);
	int wait_status = 0;
	return pid > 0 && sys(nr_wait4, pid, reinterpret_cast<long>(&wait_status)) == pid &&
	       wait_status == 0;
}

} // namespace

void probe_main(long argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "single";
	write_text("mm-shared-probe: mode=");
	write_text(mode);
	write_text(" range=");
	write_hex(reinterpret_cast<u64>(__ehdr_start));
	write_text("-");
	write_hex(reinterpret_cast<u64>(etext));
	write_text("\n");
	long status = 0;
	if (same(mode, "fork") || same(mode, "vfork")) {
		status = !fork_child(same(mode, "fork") ? nr_fork : nr_vfork);
	} else if (same(mode, "thread")) {
		status = !run_thread();
	} else if (same(mode, "private-file") || same(mode, "shared-ro")) {
		const bool shared = same(mode, "shared-ro");
		const long address = map_memfd(shared ? prot_read : prot_read_write,
					       shared ? map_shared : map_private);
		if (address == 0)
			status = 1;
		else if (shared)
			status = *reinterpret_cast<volatile long *>(address) != 0;
		else
			*reinterpret_cast<volatile long *>(address) = 1;
	} else if (same(mode, "shared-anon") || same(mode, "fork-shared")) {
		const long address = sys(nr_mmap, 0, page_size, prot_read_write,
					 map_shared | map_anonymous, -1, 0);
		if (failed(address))
			status = 1;
		else
			*reinterpret_cast<volatile long *>(address) = 1;
		if (status == 0 && same(mode, "fork-shared"))
			status = !fork_child(nr_fork);
	} else if (same(mode, "shm")) {
		const long segment = sys(nr_shmget, 0, page_size, 01000 | 0600);
		const long address = failed(segment) ? segment : sys(nr_shmat, segment, 0, 0);
		if (failed(address))
			status = 1;
		else
			*reinterpret_cast<volatile long *>(address) = 1;
		if (!failed(segment))
			sys(nr_shmctl, segment, 0, 0);
	} else if (same(mode, "uring-nommap")) {
		UringParams params{};
		params.flags = uring_setup_no_mmap;
		params.sq_user_addr = reinterpret_cast<unsigned long>(uring_entries);
		params.cq_user_addr = reinterpret_cast<unsigned long>(uring_rings);
		const long ring = sys(nr_io_uring_setup, 1, reinterpret_cast<long>(&params));
		if (failed(ring))
			status = 1;
		else
			sys(nr_close, ring);
	} else if (same(mode, "thread-fork")) {
		status = !run_thread() || !fork_child(nr_fork);
	} else if (!same(mode, "single")) {
		status = 2;
	}
	for (int round = 0; round < 8; round++)
		sys(nr_getpid);
	for (;;)
		sys(nr_exit, status);
}
