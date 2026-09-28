// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding x86-64 and i386 Linux guest probe: the RFLAGS a user task can
 * change, and the value it starts with, match Linux x86.
 *
 * Linux restores only FIX_EFLAGS (CF, PF, AF, ZF, SF, TF, DF, OF, RF and AC)
 * from a signal frame, lets a tracer change NT as well (ptrace's FLAG_MASK),
 * and starts a task from execve() with IF and reserved bit 1, 0x202. IOPL,
 * VM, VIF, VIP and ID are never a task's to set that way, and IF stays set.
 *
 * The x86-64 build traces an x86-64 and an i386 child stopped after execve(),
 * and raises a signal whose handler sets privileged bits in its frame. The
 * i386 build, whose path is the first argument, is the 32-bit child and runs
 * the same signal case under the i386 ABI. Prints one line per case and exits
 * with the number of failures.
 */

namespace {

using u32 = unsigned int;
using u64 = unsigned long long;
using word = unsigned long;

#if defined(__x86_64__)
constexpr long nr_write = 1;
constexpr long nr_rt_sigaction = 13;
constexpr long nr_getpid = 39;
constexpr long nr_fork = 57;
constexpr long nr_execve = 59;
constexpr long nr_wait4 = 61;
constexpr long nr_kill = 62;
constexpr long nr_ptrace = 101;
constexpr long nr_exit_group = 231;
/* uc_mcontext.eflags: uc_flags, uc_link and a 24-byte stack_t, then
 * r8-r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp and rip. */
constexpr unsigned frame_flags_offset = 40 + 17 * 8;

inline long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
			long a3 = 0)
{
	long result;
	register long r10 __asm__("r10") = a3;
	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
			 : "rcx", "r11", "memory", "cc");
	return result;
}

word read_flags()
{
	word flags;
	__asm__ volatile("pushfq\n\tpopq %0" : "=r"(flags) : : "memory");
	return flags;
}

/* kill(pid, SIGUSR1), whose handler runs before the syscall returns, then
 * the flags sigreturn restored, read before any other instruction can change
 * them. A popf then returns the flags a popf can change to their start. */
word raise_and_read_flags(long pid)
{
	long result;
	word flags;
	__asm__ volatile("syscall\n\t"
			 "pushfq\n\t"
			 "popq %1\n\t"
			 "pushq $0x202\n\t"
			 "popfq"
			 : "=a"(result), "=&r"(flags)
			 : "a"(nr_kill), "D"(pid), "S"(10L)
			 : "rcx", "r11", "memory", "cc");
	return flags;
}

__asm__(".text\n"
	".global probe_restorer\n"
	".type probe_restorer,@function\n"
	"probe_restorer:\n"
	"	mov $15, %eax\n"
	"	syscall\n"
	"	hlt\n"
	".size probe_restorer,.-probe_restorer\n"
	".global _start\n"
	".type _start,@function\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	mov %rsp, %rdi\n"
	"	and $-16, %rsp\n"
	"	call probe_main\n"
	"	mov %eax, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n"
	"	hlt\n"
	".size _start,.-_start\n");
#elif defined(__i386__)
constexpr long nr_write = 4;
constexpr long nr_getpid = 20;
constexpr long nr_kill = 37;
constexpr long nr_rt_sigaction = 174;
/* uc_mcontext.eflags: uc_flags, uc_link and a 12-byte stack_t, then gs, fs,
 * es and ds, edi-eax, trapno, err, eip and cs. */
constexpr unsigned frame_flags_offset = 20 + 64;

inline long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
			long a3 = 0)
{
	long result;
	__asm__ volatile("int $0x80"
			 : "=a"(result)
			 : "a"(number), "b"(a0), "c"(a1), "d"(a2), "S"(a3)
			 : "memory", "cc");
	return result;
}

word read_flags()
{
	word flags;
	__asm__ volatile("pushfl\n\tpopl %0" : "=r"(flags) : : "memory");
	return flags;
}

word raise_and_read_flags(long pid)
{
	long result;
	word flags;
	__asm__ volatile("int $0x80\n\t"
			 "pushfl\n\t"
			 "popl %1\n\t"
			 "pushl $0x202\n\t"
			 "popfl"
			 : "=a"(result), "=&r"(flags)
			 : "a"(nr_kill), "b"(pid), "c"(10L)
			 : "memory", "cc");
	return flags;
}

__asm__(".text\n"
	".global probe_restorer\n"
	".type probe_restorer,@function\n"
	"probe_restorer:\n"
	"	mov $173, %eax\n"
	"	int $0x80\n"
	"	hlt\n"
	".size probe_restorer,.-probe_restorer\n"
	".global _start\n"
	".type _start,@function\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	mov %esp, %eax\n"
	"	and $-16, %esp\n"
	"	sub $12, %esp\n"
	"	push %eax\n"
	"	call probe_main\n"
	"	mov %eax, %ebx\n"
	"	mov $252, %eax\n"
	"	int $0x80\n"
	"	hlt\n"
	".size _start,.-_start\n");
#else
#error "the RFLAGS probe is an x86-64 or i386 program"
#endif

constexpr word flag_cf = 1UL << 0;
constexpr word flag_reserved = 1UL << 1;
constexpr word flag_if = 1UL << 9;
constexpr word flag_iopl = 3UL << 12;
constexpr word flag_nt = 1UL << 14;
constexpr word flag_ac = 1UL << 18;
constexpr word flag_vif = 1UL << 19;
constexpr word flag_vip = 1UL << 20;
constexpr word flag_id = 1UL << 21;
constexpr word initial_flags = flag_if | flag_reserved;
/* A pushed image never shows VM, so the signal case leaves it out. */
constexpr word frame_privileged = flag_iopl | flag_vif | flag_vip | flag_id;

constexpr long sigusr1 = 10;
constexpr word sa_siginfo = 4;
constexpr word sa_restorer = 0x04000000;

struct KernelSigaction {
	word handler;
	word flags;
	word restorer;
	u32 mask[2];
};

void put(const char *text)
{
	long length = 0;
	while (text[length] != '\0')
		++length;
	raw_syscall(nr_write, 1, reinterpret_cast<long>(text), length);
}

void put_hex(u64 value)
{
	char text[19];
	int position = 18;
	text[position] = '\0';
	do {
		text[--position] = "0123456789abcdef"[value & 0xf];
		value >>= 4;
	} while (value != 0);
	text[--position] = 'x';
	text[--position] = '0';
	put(text + position);
}

bool equal(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

/* A written value keeps the bits no task may change, takes the ones it may,
 * and always has IF and bit 1. Prints why not. */
bool accepted_only(const char *what, u64 before, u64 requested, u64 after,
		   u64 privileged, u64 accepted)
{
	const bool ok = (after & privileged) == (before & privileged) &&
			(after & accepted) == (requested & accepted) &&
			(after & initial_flags) == initial_flags;
	if (!ok) {
		put("  ");
		put(what);
		put(": before ");
		put_hex(before);
		put(", requested ");
		put_hex(requested);
		put(", read back ");
		put_hex(after);
		put("\n");
	}
	return ok;
}

extern "C" void probe_restorer();

void set_privileged_frame_flags(int, void *, void *context)
{
	auto *flags = reinterpret_cast<word *>(static_cast<char *>(context) +
					       frame_flags_offset);
	*flags = (*flags | frame_privileged | flag_nt | flag_ac | flag_cf) &
		 ~flag_if;
}

/* A handler sets IOPL, NT, VIF, VIP, ID, AC and CF in its frame and clears
 * IF. sigreturn restores AC and CF only. */
bool sigreturn_restores_only_fix_eflags()
{
	KernelSigaction action = {};
	action.handler = reinterpret_cast<word>(&set_privileged_frame_flags);
	action.flags = sa_siginfo | sa_restorer;
	action.restorer = reinterpret_cast<word>(&probe_restorer);
	if (raw_syscall(nr_rt_sigaction, sigusr1,
			reinterpret_cast<long>(&action), 0, 8) != 0) {
		put("  rt_sigaction failed\n");
		return false;
	}
	const word before = read_flags();
	const word after = raise_and_read_flags(raw_syscall(nr_getpid));
	const word requested =
		(before | frame_privileged | flag_nt | flag_ac | flag_cf) &
		~flag_if;
	/* NT is not in FIX_EFLAGS, so the frame cannot change it either. */
	return accepted_only("frame flags", before, requested, after,
			     frame_privileged | flag_nt, flag_ac | flag_cf);
}

#if defined(__x86_64__)
constexpr word flag_vm = 1UL << 17;
constexpr word tracer_privileged = frame_privileged | flag_vm;
constexpr long nr_sigkill = 9;
constexpr long sigtrap = 5;
constexpr long ptrace_traceme = 0;
constexpr long ptrace_peekuser = 3;
constexpr long ptrace_pokeuser = 6;
constexpr long ptrace_getregs = 12;
constexpr long ptrace_setregs = 13;
constexpr long ptrace_getregset = 0x4204;
constexpr long ptrace_setregset = 0x4205;
constexpr long nt_prstatus = 1;
/* struct user_regs_struct: eflags follows r15-r8, rax-rdi, orig_rax, rip
 * and cs. The i386 layout has it after ebx-eax, ds-gs, orig_eax, eip, cs. */
constexpr unsigned user_flags_index = 18;
constexpr unsigned user_regs_words = 27;
constexpr unsigned user_regs32_flags_index = 14;
constexpr unsigned user_regs32_words = 17;

struct Iovec {
	void *base;
	word length;
};

long wait_for(long pid, int &status)
{
	return raw_syscall(nr_wait4, pid, reinterpret_cast<long>(&status), 0, 0);
}

/* Start a child that is traced from execve(): it stops with SIGTRAP after
 * the new program is loaded and before it runs an instruction. */
long start_traced(const char *path, const char *mode)
{
	const char *arguments[] = {path, mode, nullptr};
	const char *environment[] = {nullptr};
	const long child = raw_syscall(nr_fork);
	if (child == 0) {
		raw_syscall(nr_ptrace, ptrace_traceme, 0, 0, 0);
		raw_syscall(nr_execve, reinterpret_cast<long>(path),
			    reinterpret_cast<long>(arguments),
			    reinterpret_cast<long>(environment));
		raw_syscall(nr_exit_group, 127);
	}
	int status = 0;
	if (child < 0 || wait_for(child, status) != child ||
	    (status & 0xff) != 0x7f || ((status >> 8) & 0xff) != sigtrap) {
		put("  no stop after execve(): status ");
		put_hex(static_cast<u32>(status));
		put("\n");
		return -1;
	}
	return child;
}

void finish(long child)
{
	int status = 0;
	raw_syscall(nr_kill, child, nr_sigkill);
	wait_for(child, status);
}

bool get_regs(long child, u64 (&regs)[user_regs_words])
{
	return raw_syscall(nr_ptrace, ptrace_getregs, child, 0,
			   reinterpret_cast<long>(regs)) == 0;
}

bool set_regs(long child, u64 (&regs)[user_regs_words])
{
	return raw_syscall(nr_ptrace, ptrace_setregs, child, 0,
			   reinterpret_cast<long>(regs)) == 0;
}

bool exec_stop_flags(long child, u64 &flags)
{
	u64 regs[user_regs_words] = {};
	if (!get_regs(child, regs))
		return false;
	flags = regs[user_flags_index];
	if (flags == initial_flags)
		return true;
	put("  RFLAGS at the execve() stop ");
	put_hex(flags);
	put(", Linux starts a task with 0x202\n");
	return false;
}

u64 tracer_request(u64 flags)
{
	return (flags | tracer_privileged | flag_nt | flag_ac | flag_cf) &
	       ~flag_if;
}

bool setregs_changes_only_flag_mask(long child)
{
	u64 regs[user_regs_words] = {};
	if (!get_regs(child, regs))
		return false;
	const u64 before = regs[user_flags_index];
	regs[user_flags_index] = tracer_request(before);
	if (!set_regs(child, regs) || !get_regs(child, regs))
		return false;
	const u64 after = regs[user_flags_index];
	regs[user_flags_index] = before;
	const bool restored = set_regs(child, regs);
	return accepted_only("PTRACE_SETREGS", before, tracer_request(before),
			     after, tracer_privileged,
			     flag_nt | flag_ac | flag_cf) && restored;
}

bool pokeuser_changes_only_flag_mask(long child)
{
	const long offset = user_flags_index * sizeof(u64);
	u64 before = 0;
	u64 after = 0;
	if (raw_syscall(nr_ptrace, ptrace_peekuser, child, offset,
			reinterpret_cast<long>(&before)) != 0 ||
	    raw_syscall(nr_ptrace, ptrace_pokeuser, child, offset,
			static_cast<long>(tracer_request(before))) != 0 ||
	    raw_syscall(nr_ptrace, ptrace_peekuser, child, offset,
			reinterpret_cast<long>(&after)) != 0)
		return false;
	return accepted_only("PTRACE_POKEUSER", before, tracer_request(before),
			     after, tracer_privileged,
			     flag_nt | flag_ac | flag_cf);
}

/* An i386 tracee's general registers are its i386 regset, which 32-bit
 * tracers also write. */
bool setregset32_changes_only_flag_mask(long child)
{
	u32 regs[user_regs32_words] = {};
	Iovec vector = {regs, sizeof(regs)};
	if (raw_syscall(nr_ptrace, ptrace_getregset, child, nt_prstatus,
			reinterpret_cast<long>(&vector)) != 0 ||
	    vector.length != sizeof(regs))
		return false;
	const u32 before = regs[user_regs32_flags_index];
	const u32 requested = static_cast<u32>(tracer_request(before));
	regs[user_regs32_flags_index] = requested;
	if (raw_syscall(nr_ptrace, ptrace_setregset, child, nt_prstatus,
			reinterpret_cast<long>(&vector)) != 0 ||
	    raw_syscall(nr_ptrace, ptrace_getregset, child, nt_prstatus,
			reinterpret_cast<long>(&vector)) != 0)
		return false;
	return accepted_only("i386 NT_PRSTATUS", before, requested,
			     regs[user_regs32_flags_index], tracer_privileged,
			     flag_nt | flag_ac | flag_cf);
}

/* The signal case runs in a child, since no popf can clear an IOPL a
 * sigreturn wrongly installed. */
bool in_child(const char *path, const char *mode)
{
	const char *arguments[] = {path, mode, nullptr};
	const char *environment[] = {nullptr};
	const long child = raw_syscall(nr_fork);
	if (child == 0) {
		raw_syscall(nr_execve, reinterpret_cast<long>(path),
			    reinterpret_cast<long>(arguments),
			    reinterpret_cast<long>(environment));
		raw_syscall(nr_exit_group, 127);
	}
	int status = 0;
	if (child < 0 || wait_for(child, status) != child)
		return false;
	if ((status & 0x7f) != 0) {
		put("  killed by signal ");
		put_hex(static_cast<u32>(status & 0x7f));
		put("\n");
		return false;
	}
	return ((status >> 8) & 0xff) == 0;
}

int failures;

void report(const char *name, bool ok)
{
	put(ok ? "PASS " : "FAIL ");
	put(name);
	put("\n");
	failures += ok ? 0 : 1;
}

int run_all(const char *self, const char *i386_probe)
{
	u64 flags = 0;
	long child = start_traced(self, "idle");
	report("x86-64 execve() starts with RFLAGS 0x202",
	       child > 0 && exec_stop_flags(child, flags));
	report("x86-64 PTRACE_SETREGS changes only FLAG_MASK bits",
	       child > 0 && setregs_changes_only_flag_mask(child));
	report("x86-64 PTRACE_POKEUSER changes only FLAG_MASK bits",
	       child > 0 && pokeuser_changes_only_flag_mask(child));
	if (child > 0)
		finish(child);
	report("x86-64 sigreturn restores only FIX_EFLAGS bits",
	       in_child(self, "sigreturn"));

	child = start_traced(i386_probe, "idle");
	report("i386 execve() starts with EFLAGS 0x202",
	       child > 0 && exec_stop_flags(child, flags));
	report("i386 NT_PRSTATUS write changes only FLAG_MASK bits",
	       child > 0 && setregset32_changes_only_flag_mask(child));
	if (child > 0)
		finish(child);
	report("i386 sigreturn restores only FIX_EFLAGS bits",
	       in_child(i386_probe, "sigreturn"));
	return failures;
}
#endif

} // namespace

extern "C" int probe_main(word *stack)
{
	const long count = static_cast<long>(stack[0]);
	char **arguments = reinterpret_cast<char **>(stack + 1);
	if (count == 2 && equal(arguments[1], "idle"))
		return 0;
	if (count == 2 && equal(arguments[1], "sigreturn"))
		return sigreturn_restores_only_fix_eflags() ? 0 : 1;
#if defined(__x86_64__)
	if (count == 2)
		return run_all(arguments[0], arguments[1]);
	put("usage: rflags-probe I386_PROBE\n");
#endif
	return 100;
}

using size_type = decltype(sizeof(0));

extern "C" void *memset(void *destination, int value, size_type length)
{
	auto *bytes = static_cast<volatile unsigned char *>(destination);
	for (size_type index = 0; index < length; ++index)
		bytes[index] = static_cast<unsigned char>(value);
	return destination;
}

extern "C" void *memcpy(void *destination, const void *source,
			size_type length)
{
	auto *to = static_cast<volatile unsigned char *>(destination);
	const auto *from = static_cast<const unsigned char *>(source);
	for (size_type index = 0; index < length; ++index)
		to[index] = from[index];
	return destination;
}
