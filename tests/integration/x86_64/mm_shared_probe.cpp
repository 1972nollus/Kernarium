// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Freestanding x86-64 guest probe for the "mm is shared" dispatch flag
 * (DARWIN_UML_DISPATCH_MM_SHARED). It prints its own code range, then runs
 * the mode its first argument names:
 *
 *   single  only system calls on its own;
 *   fork    fork() a child, which exits;
 *   vfork   vfork() a child, which exits;
 *   thread  clone() a thread that shares the address space.
 *
 * Only "thread" shares the mm, so only it may make the host log the flag
 * for an address in the probe.
 */

namespace {

using u64 = unsigned long;

constexpr long nr_write = 1;
constexpr long nr_sched_yield = 24;
constexpr long nr_getpid = 39;
constexpr long nr_fork = 57;
constexpr long nr_vfork = 58;
constexpr long nr_exit = 60;
constexpr long nr_wait4 = 61;

constexpr u64 clone_thread_flags = 0x00000100 | 0x00000200 | 0x00000400 |
	0x00000800 | 0x00010000 | 0x00040000; /* VM FS FILES SIGHAND THREAD SYSVSEM */

inline long sys(long number, long a0 = 0, long a1 = 0, long a2 = 0, long a3 = 0)
{
	long result;
	register long r10 __asm__("r10") = a3;
	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
			 : "rcx", "r11", "memory");
	return result;
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
		long pid = fork_exit(same(mode, "fork") ? nr_fork : nr_vfork);
		int wait_status = 0;
		if (pid <= 0 || sys(nr_wait4, pid, reinterpret_cast<long>(&wait_status)) != pid ||
		    wait_status != 0)
			status = 1;
	} else if (same(mode, "thread")) {
		if (clone_thread(clone_thread_flags, thread_stack + sizeof(thread_stack),
				 thread_main) <= 0)
			status = 1;
		while (!thread_done)
			sys(nr_sched_yield);
	} else if (!same(mode, "single")) {
		status = 2;
	}
	for (int round = 0; round < 8; round++)
		sys(nr_getpid);
	for (;;)
		sys(nr_exit, status);
}
