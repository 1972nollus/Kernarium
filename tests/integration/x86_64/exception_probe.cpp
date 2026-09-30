// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding x86-64 and i386 Linux guest probe: what the x86_64-userspace
 * slice delivers for x86 exceptions and interrupts, and what a tracer of the
 * same ABI reads and writes, against native Linux x86.
 *
 * Only a system call's own return can be restarted: a signal taken between
 * two instructions leaves the accumulator alone even when it holds an
 * -ERESTART* code, and a tracer that rewrites orig_ax at a system call's entry
 * changes or skips that call.
 *
 * A repeated string instruction retires once however long it runs, and the
 * kernel services its timer only when the engine returns to it, so a loop of
 * long REP STOSB or REPE CMPSB must still see a 1 ms interval timer about as
 * often as ordinary code does.
 *
 * The x86-64 build, whose second argument is the path of the i386 build, runs
 * every case in a child of the matching ABI. Prints one line per case and
 * exits with the number of failures.
 */

namespace {

using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using word = unsigned long;

#if defined(__x86_64__)
constexpr long nr_write = 1;
constexpr long nr_rt_sigaction = 13;
constexpr long nr_setitimer = 38;
constexpr long nr_getpid = 39;
constexpr long nr_fork = 57;
constexpr long nr_execve = 59;
constexpr long nr_wait4 = 61;
constexpr long nr_kill = 62;
constexpr long nr_ptrace = 101;
constexpr long nr_getppid = 110;
constexpr long nr_clock_gettime = 228;
constexpr long nr_exit_group = 231;

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
	".size _start,.-_start\n"
	/* REP STOSB of zero: (buffer, count). */
	".global probe_stos\n"
	"probe_stos:\n"
	"	movq %rsi, %rcx\n"
	"	xorl %eax, %eax\n"
	"	rep stosb\n"
	"	ret\n"
	/* REPE CMPSB: (left, right, count), returns what RCX kept. */
	".global probe_cmps\n"
	"probe_cmps:\n"
	"	movq %rdx, %rcx\n"
	"	repe cmpsb\n"
	"	movq %rcx, %rax\n"
	"	ret\n"
	/* ICEBP, INT 4: each traps after itself. */
	".global probe_icebp\n"
	"probe_icebp:\n"
	"	.byte 0xf1\n"
	".global probe_icebp_end\n"
	"probe_icebp_end:\n"
	"	ret\n"
	".global probe_int4\n"
	"probe_int4:\n"
	"	int $4\n"
	".global probe_int4_end\n"
	"probe_int4_end:\n"
	"	ret\n"
	/* Spins (loops) times in 2-byte instructions with (value) in RAX and
	 * returns RAX. A signal that wrongly restarted a system call here would
	 * move RIP back one whole instruction. */
	".global probe_hold_ax\n"
	"probe_hold_ax:\n"
	"	movq %rdi, %rax\n"
	"	movq %rsi, %rcx\n"
	".Lprobe_hold_ax_loop:\n"
	"	.byte 0x66, 0x90\n"
	"	.byte 0xff, 0xc9\n"
	"	jnz .Lprobe_hold_ax_loop\n"
	"	ret\n"
	/* Stores pi through a SIGTRAP taken with it on the x87 stack: (out). */
	".global probe_x87_signal\n"
	"probe_x87_signal:\n"
	"	fninit\n"
	"	fldpi\n"
	"	int3\n"
	"	fstpl (%rdi)\n"
	"	ret\n"
	/* The traced child: known x87, SSE and general register state, then
	 * #UD for the tracer's stop. The tracer resumes it at
	 * probe_traced_resume, which exits with RBX's low byte. */
	".global probe_traced\n"
	"probe_traced:\n"
	"	fninit\n"
	"	fldcw probe_control_word(%rip)\n"
	"	ldmxcsr probe_mxcsr_value(%rip)\n"
	"	fld1\n"
	"	movabs $0x1122334455667788, %rbx\n"
	".global probe_traced_stop\n"
	"probe_traced_stop:\n"
	"	ud2\n"
	".global probe_traced_resume\n"
	"probe_traced_resume:\n"
	"	mov %ebx, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n"
	"	hlt\n"
	".section .rodata\n"
	"probe_control_word:\n"
	"	.short 0x027f\n"
	"	.balign 4\n"
	"probe_mxcsr_value:\n"
	"	.long 0x3f80\n"
	".text\n");

/* uc_mcontext: uc_flags, uc_link and a 24-byte stack_t, then r8-r15, rdi,
 * rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags, csgsfs, err and trapno. */
constexpr unsigned frame_ip_offset = 40 + 16 * 8;
constexpr unsigned frame_flags_offset = 40 + 17 * 8;
constexpr unsigned frame_error_offset = 40 + 19 * 8;
constexpr unsigned frame_trapno_offset = 40 + 20 * 8;
constexpr unsigned siginfo_address_offset = 16;

/* struct user_regs_struct and struct user. */
constexpr unsigned user_regs_words = 27;
constexpr unsigned user_bx = 5;
constexpr unsigned user_ax = 10;
constexpr unsigned user_orig_ax = 15;
constexpr unsigned user_ip = 16;
constexpr unsigned user_cs = 17;
constexpr unsigned user_flags = 18;
constexpr unsigned user_ss = 20;
constexpr word user_debugreg7 = 848 + 7 * 8;
/* sizeof(struct user): PTRACE_PEEKUSER rejects it and beyond. */
constexpr word user_past = 928;
constexpr word expected_bx = 0x1122334455667788UL;
constexpr word expected_cs = 0x33;
#elif defined(__i386__)
constexpr long nr_fork = 2;
constexpr long nr_write = 4;
constexpr long nr_getpid = 20;
constexpr long nr_ptrace = 26;
constexpr long nr_kill = 37;
constexpr long nr_getppid = 64;
constexpr long nr_setitimer = 104;
constexpr long nr_wait4 = 114;
constexpr long nr_rt_sigaction = 174;
constexpr long nr_set_thread_area = 243;
constexpr long nr_exit_group = 252;
constexpr long nr_clock_gettime = 265;

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
	".size _start,.-_start\n"
	".global probe_stos\n"
	"probe_stos:\n"
	"	pushl %edi\n"
	"	movl 8(%esp), %edi\n"
	"	movl 12(%esp), %ecx\n"
	"	xorl %eax, %eax\n"
	"	rep stosb\n"
	"	popl %edi\n"
	"	ret\n"
	".global probe_cmps\n"
	"probe_cmps:\n"
	"	pushl %esi\n"
	"	pushl %edi\n"
	"	movl 12(%esp), %esi\n"
	"	movl 16(%esp), %edi\n"
	"	movl 20(%esp), %ecx\n"
	"	repe cmpsb\n"
	"	movl %ecx, %eax\n"
	"	popl %edi\n"
	"	popl %esi\n"
	"	ret\n"
	".global probe_icebp\n"
	"probe_icebp:\n"
	"	.byte 0xf1\n"
	".global probe_icebp_end\n"
	"probe_icebp_end:\n"
	"	ret\n"
	".global probe_int4\n"
	"probe_int4:\n"
	"	int $4\n"
	".global probe_int4_end\n"
	"probe_int4_end:\n"
	"	ret\n"
	".global probe_hold_ax\n"
	"probe_hold_ax:\n"
	"	movl 4(%esp), %eax\n"
	"	movl 8(%esp), %ecx\n"
	".Lprobe_hold_ax_loop:\n"
	"	.byte 0x66, 0x90\n"
	"	.byte 0xff, 0xc9\n"
	"	jnz .Lprobe_hold_ax_loop\n"
	"	ret\n"
	/* INTO after adding (value) to 0x7fffffff: overflows for 1, not 0. */
	".global probe_into\n"
	"probe_into:\n"
	"	movl 4(%esp), %eax\n"
	"	addl $0x7fffffff, %eax\n"
	".global probe_into_insn\n"
	"probe_into_insn:\n"
	"	into\n"
	".global probe_into_end\n"
	"probe_into_end:\n"
	"	ret\n"
	/* BOUND EAX, [EDX] and BOUND AX, [EDX]: (index, bounds). */
	".global probe_bound32\n"
	"probe_bound32:\n"
	"	movl 4(%esp), %eax\n"
	"	movl 8(%esp), %edx\n"
	".global probe_bound32_insn\n"
	"probe_bound32_insn:\n"
	"	.byte 0x62, 0x02\n"
	".global probe_bound32_end\n"
	"probe_bound32_end:\n"
	"	ret\n"
	".global probe_bound16\n"
	"probe_bound16:\n"
	"	movl 4(%esp), %eax\n"
	"	movl 8(%esp), %edx\n"
	".global probe_bound16_insn\n"
	"probe_bound16_insn:\n"
	"	.byte 0x66, 0x62, 0x02\n"
	".global probe_bound16_end\n"
	"probe_bound16_end:\n"
	"	ret\n"
	".global probe_x87_signal\n"
	"probe_x87_signal:\n"
	"	movl 4(%esp), %eax\n"
	"	fninit\n"
	"	fldpi\n"
	"	int3\n"
	"	fstpl (%eax)\n"
	"	ret\n"
	/* (tls_entry), which the tracer reads from ESI. */
	".global probe_traced\n"
	"probe_traced:\n"
	"	movl 4(%esp), %esi\n"
	"	fninit\n"
	"	fldcw probe_control_word\n"
	"	ldmxcsr probe_mxcsr_value\n"
	"	fld1\n"
	"	movl $0x11223344, %ebx\n"
	".global probe_traced_stop\n"
	"probe_traced_stop:\n"
	"	ud2\n"
	".global probe_traced_resume\n"
	"probe_traced_resume:\n"
	"	movl $252, %eax\n"
	"	int $0x80\n"
	"	hlt\n"
	".section .rodata\n"
	"probe_control_word:\n"
	"	.short 0x027f\n"
	"	.balign 4\n"
	"probe_mxcsr_value:\n"
	"	.long 0x3f80\n"
	".text\n");

/* uc_mcontext: uc_flags, uc_link and a 12-byte stack_t, then gs, fs, es, ds,
 * edi, esi, ebp, esp, ebx, edx, ecx, eax, trapno, err, eip, cs and eflags. */
constexpr unsigned frame_trapno_offset = 20 + 12 * 4;
constexpr unsigned frame_error_offset = 20 + 13 * 4;
constexpr unsigned frame_ip_offset = 20 + 14 * 4;
constexpr unsigned frame_flags_offset = 20 + 16 * 4;
constexpr unsigned siginfo_address_offset = 12;

/* struct user_regs_struct and struct user of i386. */
constexpr unsigned user_regs_words = 17;
constexpr unsigned user_bx = 0;
constexpr unsigned user_si = 3;
constexpr unsigned user_ax = 6;
constexpr unsigned user_orig_ax = 11;
constexpr unsigned user_ip = 12;
constexpr unsigned user_cs = 13;
constexpr unsigned user_flags = 14;
constexpr unsigned user_ss = 16;
constexpr word user_debugreg7 = 252 + 7 * 4;
/* Linux accepts sizeof(struct user32), 284, itself and rejects beyond. */
constexpr word user_past = 288;
constexpr word expected_bx = 0x11223344UL;
constexpr word expected_cs = 0x23;
#else
#error "the exception probe is an x86-64 or i386 program"
#endif

/* Both ABIs assemble these alike. UD2 raises #UD. */
__asm__(".text\n"
	".global probe_ud2\n"
	"probe_ud2:\n"
	"	ud2\n"
	".global probe_ud2_end\n"
	"probe_ud2_end:\n"
	"	ret\n");

constexpr word sa_siginfo = 4;
constexpr word sa_restorer = 0x04000000;
constexpr word sa_restart = 0x10000000;
constexpr long sigill = 4;
constexpr long sigtrap = 5;
constexpr long sigkill = 9;
constexpr long sigsegv = 11;
constexpr long sigalrm = 14;
constexpr long sigstop = 19;
constexpr long trap_brkpt = 1;
constexpr long si_kernel = 0x80;
constexpr word flag_reserved = 1UL << 1;
constexpr word flag_rf = 1UL << 16;
constexpr long eio = 5;
constexpr long ill_illopn = 2;

struct KernelSigaction {
	word handler;
	word flags;
	word restorer;
	u32 mask[2];
};

struct Timespec {
	long seconds;
	long nanoseconds;
};

struct Itimerval {
	long interval_seconds;
	long interval_microseconds;
	long value_seconds;
	long value_microseconds;
};

void put(const char *text)
{
	long length = 0;
	while (text[length] != '\0')
		++length;
	raw_syscall(nr_write, 1, reinterpret_cast<long>(text), length);
}

void put_decimal(u64 value)
{
	char text[21];
	int position = 20;
	text[position] = '\0';
	do {
		text[--position] = static_cast<char>('0' + value % 10);
		value /= 10;
	} while (value != 0);
	put(text + position);
}

void put_hex(u64 value)
{
	char text[19];
	int position = 18;
	text[position] = '\0';
	do {
		text[--position] = "0123456789abcdef"[value & 15];
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

/* Prints a mismatch and returns whether there was none. */
bool expect(const char *what, u64 actual, u64 expected)
{
	if (actual == expected)
		return true;
	put("  ");
	put(what);
	put(" is ");
	put_hex(actual);
	put(", expected ");
	put_hex(expected);
	put("\n");
	return false;
}

extern "C" void probe_restorer();

bool install(long signal, void (*handler)(int, void *, void *))
{
	KernelSigaction action = {};
	action.handler = reinterpret_cast<word>(handler);
	action.flags = sa_siginfo | sa_restorer;
	action.restorer = reinterpret_cast<word>(&probe_restorer);
	return raw_syscall(nr_rt_sigaction, signal,
			   reinterpret_cast<long>(&action), 0, 8) == 0;
}

u64 now_nanoseconds()
{
	Timespec time = {};
	raw_syscall(nr_clock_gettime, 1, reinterpret_cast<long>(&time));
	return static_cast<u64>(time.seconds) * 1000000000ULL +
	       static_cast<u64>(time.nanoseconds);
}

bool set_interval_timer(long microseconds)
{
	const Itimerval timer = {0, microseconds, 0, microseconds};
	return raw_syscall(nr_setitimer, 0, reinterpret_cast<long>(&timer),
			   0) == 0;
}

extern "C" void probe_stos(void *buffer, word count);
extern "C" word probe_cmps(const void *left, const void *right, word count);

constexpr word stos_bytes = 1UL << 22;
constexpr word stos_rounds = 2000;
constexpr word cmps_bytes = 1UL << 16;
constexpr word cmps_rounds = 1000;
/* Native Linux delivers a 1 ms timer about once a millisecond; this asks for
 * one in every ten. */
constexpr u64 tick_allowance_nanoseconds = 10000000;

alignas(4096) unsigned char stos_buffer[stos_bytes];
alignas(4096) unsigned char cmps_left[cmps_bytes];
alignas(4096) unsigned char cmps_right[cmps_bytes];

volatile u64 ticks;
volatile u64 last_tick;
volatile u64 longest_gap;

void record_tick(int, void *, void *)
{
	const u64 now = now_nanoseconds();
	if (now - last_tick > longest_gap)
		longest_gap = now - last_tick;
	last_tick = now;
	ticks = ticks + 1;
}

/* A 1 ms interval timer keeps firing while `work` runs long repeated string
 * instructions and makes no system call. */
bool timer_ticks_during(const char *what, void (*work)())
{
	if (!install(sigalrm, record_tick)) {
		put("  rt_sigaction failed\n");
		return false;
	}
	ticks = 0;
	longest_gap = 0;
	const u64 started = now_nanoseconds();
	last_tick = started;
	if (!set_interval_timer(1000)) {
		put("  setitimer failed\n");
		return false;
	}
	work();
	set_interval_timer(0);
	const u64 finished = now_nanoseconds();
	if (finished - last_tick > longest_gap)
		longest_gap = finished - last_tick;
	const u64 elapsed = finished - started;
	const bool ok = ticks * tick_allowance_nanoseconds >= elapsed;
	put(ok ? "  " : "  too few: ");
	put(what);
	put(" ran ");
	put_decimal(elapsed / 1000000);
	put(" ms with ");
	put_decimal(ticks);
	put(" SIGALRM, longest gap ");
	put_decimal(longest_gap / 1000000);
	put(" ms\n");
	return ok;
}

void repeat_stos()
{
	for (word round = 0; round < stos_rounds; ++round)
		probe_stos(stos_buffer, stos_bytes);
}

volatile word cmps_left_over;

void repeat_cmps()
{
	for (word round = 0; round < cmps_rounds; ++round)
		cmps_left_over = probe_cmps(cmps_left, cmps_right, cmps_bytes);
}

/* What the last synchronous signal showed its handler. */
enum Observed {
	observed_count,
	observed_signal,
	observed_code,
	observed_address,
	observed_ip,
	observed_flags,
	observed_trapno,
	observed_error,
	observed_fields,
};

volatile word observed[observed_fields];
/* Where the handler resumes the task, so an unexpected fault cannot repeat. */
volatile word resume_ip;

word &frame_word(void *context, unsigned offset)
{
	return *reinterpret_cast<word *>(static_cast<unsigned char *>(context) +
					 offset);
}

void record_fault(int signal, void *info, void *context)
{
	const auto *details = static_cast<const unsigned char *>(info);
	observed[observed_count] = observed[observed_count] + 1;
	observed[observed_signal] = static_cast<word>(signal);
	observed[observed_code] =
		static_cast<word>(*reinterpret_cast<const int *>(details + 8));
	observed[observed_address] = *reinterpret_cast<const word *>(
		details + siginfo_address_offset);
	observed[observed_ip] = frame_word(context, frame_ip_offset);
	observed[observed_flags] = frame_word(context, frame_flags_offset);
	observed[observed_trapno] = frame_word(context, frame_trapno_offset);
	observed[observed_error] = frame_word(context, frame_error_offset);
	if (resume_ip != 0)
		frame_word(context, frame_ip_offset) = resume_ip;
}

bool install_fault_handlers()
{
	if (install(sigill, record_fault) && install(sigtrap, record_fault) &&
	    install(sigsegv, record_fault))
		return true;
	put("  rt_sigaction failed\n");
	return false;
}

struct Delivery {
	long signal;
	long code;
	word address;
	word ip;
	word trapno;
	bool resume_flag;
};

/* Runs `work`, resuming after `end` if it faults, and checks that it raised
 * exactly `expected`. */
bool delivers(const char *what, void (*work)(), word end,
	      const Delivery &expected)
{
	for (unsigned index = 0; index < observed_fields; ++index)
		observed[index] = 0;
	resume_ip = end;
	work();
	resume_ip = 0;
	if (observed[observed_count] != 1) {
		put("  ");
		put(what);
		put(" raised ");
		put_decimal(observed[observed_count]);
		put(" signals, the last ");
		put_decimal(observed[observed_signal]);
		put(" code ");
		put_hex(observed[observed_code]);
		put("\n");
		return false;
	}
	bool ok = expect("signal", observed[observed_signal],
			 static_cast<word>(expected.signal));
	ok &= expect("si_code", observed[observed_code],
		     static_cast<word>(expected.code));
	ok &= expect("si_addr", observed[observed_address], expected.address);
	ok &= expect("frame ip", observed[observed_ip], expected.ip);
	ok &= expect("trapno", observed[observed_trapno], expected.trapno);
	ok &= expect("err", observed[observed_error], 0);
	ok &= expect("RF", (observed[observed_flags] & flag_rf) != 0,
		     expected.resume_flag);
	if (!ok) {
		put("  in ");
		put(what);
		put("\n");
	}
	return ok;
}

extern "C" void probe_icebp();
extern "C" char probe_icebp_end[];
extern "C" void probe_int4();
extern "C" char probe_int4_end[];

word address_of(const char *label)
{
	return reinterpret_cast<word>(label);
}

/* ICEBP (INT1) raises #DB as a trap: SIGTRAP with TRAP_BRKPT, naming the
 * next instruction. */
bool icebp_traps()
{
	const word end = address_of(probe_icebp_end);
	return install_fault_handlers() &&
	       delivers("ICEBP", probe_icebp, end,
			{sigtrap, trap_brkpt, end, end, 1, false});
}

/* INT 4 raises #OF as a trap, which Linux delivers as SIGSEGV with
 * SI_KERNEL and no address. */
bool int4_traps()
{
	const word end = address_of(probe_int4_end);
	return install_fault_handlers() &&
	       delivers("INT 4", probe_int4, end,
			{sigsegv, si_kernel, 0, end, 4, false});
}

extern "C" void probe_x87_signal(double *out);

/* A signal taken with a value on the x87 stack returns to the same stack. */
bool x87_stack_survives_a_signal()
{
	if (!install_fault_handlers())
		return false;
	double result = 0;
	probe_x87_signal(&result);
	u64 bits;
	__builtin_memcpy(&bits, &result, sizeof(bits));
	return expect("pi stored after the SIGTRAP", bits,
		      0x400921fb54442d18ULL);
}

extern "C" word probe_hold_ax(word value, word loops);

volatile u64 alarms;

void count_alarm(int, void *, void *) { alarms = alarms + 1; }

/* One round spins for about a millisecond or more; the case waits for a few
 * timer signals. */
constexpr word hold_loops = 1UL << 22;
constexpr u64 hold_alarms = 3;
constexpr int hold_rounds = 10000;

/* A signal taken between two instructions, outside any system call, leaves
 * AX alone even when it holds one of the -ERESTART* codes an interrupted
 * system call returns, with SA_RESTART or without. */
bool signal_keeps_ax()
{
	const word restarts[] = {0, sa_restart};
	const long codes[] = {512, 513, 514, 516};
	bool ok = true;
	for (const word restart : restarts) {
		KernelSigaction action = {};
		action.handler = reinterpret_cast<word>(count_alarm);
		action.flags = sa_siginfo | sa_restorer | restart;
		action.restorer = reinterpret_cast<word>(&probe_restorer);
		if (raw_syscall(nr_rt_sigaction, sigalrm,
				reinterpret_cast<long>(&action), 0, 8) != 0) {
			put("  rt_sigaction failed\n");
			return false;
		}
		for (const long code : codes) {
			const word value = static_cast<word>(-code);
			word held = value;
			alarms = 0;
			if (!set_interval_timer(1000)) {
				put("  setitimer failed\n");
				return false;
			}
			for (int round = 0; round < hold_rounds && held == value &&
			     alarms < hold_alarms; ++round)
				held = probe_hold_ax(value, hold_loops);
			set_interval_timer(0);
			put(restart != 0 ? "  SA_RESTART" : "  no SA_RESTART");
			put(", AX -");
			put_decimal(static_cast<u64>(code));
			put(", ");
			put_decimal(alarms);
			put(" SIGALRM\n");
			ok &= expect("AX", held, value);
			if (held == value && alarms < hold_alarms) {
				put("  too few SIGALRM\n");
				ok = false;
			}
		}
	}
	return ok;
}

#if defined(__i386__)
/* Runs `work`, resuming after `end` if it faults, and checks that it raised
 * nothing. */
bool delivers_nothing(const char *what, void (*work)(), word end)
{
	observed[observed_count] = 0;
	resume_ip = end;
	work();
	resume_ip = 0;
	if (observed[observed_count] == 0)
		return true;
	put("  ");
	put(what);
	put(" raised signal ");
	put_decimal(observed[observed_signal]);
	put("\n");
	return false;
}

extern "C" void probe_into(word value);
extern "C" char probe_into_end[];
extern "C" void probe_bound32(word index, const void *bounds);
extern "C" char probe_bound32_insn[];
extern "C" char probe_bound32_end[];
extern "C" void probe_bound16(word index, const void *bounds);
extern "C" char probe_bound16_insn[];
extern "C" char probe_bound16_end[];

void into_overflowed() { probe_into(1); }
void into_clear() { probe_into(0); }

/* INTO raises #OF only when OF is set, as a trap. */
bool into_traps()
{
	const word end = address_of(probe_into_end);
	bool ok = install_fault_handlers();
	ok = ok && delivers_nothing("INTO without overflow", into_clear, end);
	ok = ok && delivers("INTO with overflow", into_overflowed, end,
			    {sigsegv, si_kernel, 0, end, 4, false});
	return ok;
}

const int bounds32[2] = {-2, 5};
const short bounds16[2] = {-2, 5};
volatile word bound_index;

void bound32() { probe_bound32(bound_index, bounds32); }
void bound16() { probe_bound16(bound_index, bounds16); }

/* BOUND compares a signed index with an inclusive pair of bounds and raises
 * #BR, a fault, outside them: SIGSEGV with SI_KERNEL at the instruction. */
bool bound_checks()
{
	if (!install_fault_handlers())
		return false;
	const word insn32 = address_of(probe_bound32_insn);
	const word end32 = address_of(probe_bound32_end);
	const word insn16 = address_of(probe_bound16_insn);
	const word end16 = address_of(probe_bound16_end);
	bool ok = true;
	const long inside[] = {-2, 0, 5};
	for (const long index : inside) {
		bound_index = static_cast<word>(index);
		ok &= delivers_nothing("BOUND r32 in range", bound32, end32);
		ok &= delivers_nothing("BOUND r16 in range", bound16, end16);
	}
	/* 0x7fff is 32767 to the 16-bit form, above its bounds; 0x10000 is
	 * zero to it, within them. */
	const long outside32[] = {-3, 6, 0x7fff};
	for (const long index : outside32) {
		bound_index = static_cast<word>(index);
		ok &= delivers("BOUND r32 out of range", bound32, end32,
			       {sigsegv, si_kernel, 0, insn32, 5, true});
	}
	bound_index = 0x7fff;
	ok &= delivers("BOUND r16 out of range", bound16, end16,
		       {sigsegv, si_kernel, 0, insn16, 5, true});
	bound_index = 0x10000;
	ok &= delivers_nothing("BOUND r16 of a truncated index", bound16,
			       end16);
	return ok;
}
#endif

extern "C" void probe_ud2();
extern "C" char probe_ud2_end[];

/* #UD, vector 6, is a fault: SIGILL with ILL_ILLOPN at the instruction. */
bool ud2_faults()
{
	const word insn = reinterpret_cast<word>(&probe_ud2);
	return install_fault_handlers() &&
	       delivers("UD2", probe_ud2, address_of(probe_ud2_end),
			{sigill, ill_illopn, insn, insn, 6, true});
}

constexpr long ptrace_traceme = 0;
constexpr long ptrace_peekuser = 3;
constexpr long ptrace_pokeuser = 6;
constexpr long ptrace_cont = 7;
constexpr long ptrace_getregs = 12;
constexpr long ptrace_syscall = 24;
constexpr long ptrace_setregs = 13;
constexpr long ptrace_getfpregs = 14;
constexpr long ptrace_setfpregs = 15;
#if defined(__i386__)
constexpr long ptrace_getfpxregs = 18;
constexpr long ptrace_get_thread_area = 25;
#endif

long ptrace(long request, long child, word address, const void *data)
{
	return raw_syscall(nr_ptrace, request, child, static_cast<long>(address),
			   reinterpret_cast<long>(data));
}

long wait_for(long pid, int &status)
{
	return raw_syscall(nr_wait4, pid, reinterpret_cast<long>(&status), 0, 0);
}

extern "C" [[noreturn]] void probe_traced(word tls_entry);
extern "C" char probe_traced_stop[];
extern "C" char probe_traced_resume[];

#if defined(__i386__)
constexpr u32 tls_base = 0x12345000;
#endif

struct UserDesc {
	u32 entry_number;
	u32 base_addr;
	u32 limit;
	u32 flags;
};

/* FXSAVE: cwd, swd, the abridged tag byte, MXCSR and ST(0). */
constexpr unsigned fxsave_swd = 2;
constexpr unsigned fxsave_twd = 4;
constexpr unsigned fxsave_mxcsr = 24;
constexpr unsigned fxsave_st0 = 32;
#if defined(__i386__)
/* The i387 FSAVE image: 32-bit cwd, swd and twd, then after the pointers
 * eight packed 10-byte registers. */
constexpr unsigned fsave_swd = 4;
constexpr unsigned fsave_twd = 8;
constexpr unsigned fsave_st0 = 28;
#endif

template <typename T> T field(const unsigned char *image, unsigned offset)
{
	T value;
	__builtin_memcpy(&value, image + offset, sizeof(value));
	return value;
}

/* ST(0) holds 1.0 and is the only register in use, at physical 7. */
bool holds_one(const unsigned char *image, unsigned st0)
{
	bool ok = expect("ST(0) significand", field<u64>(image, st0),
			 0x8000000000000000ULL);
	ok &= expect("ST(0) exponent", field<u16>(image, st0 + 8), 0x3fff);
	return ok;
}

bool fxsave_image(const unsigned char *image, u16 control)
{
	bool ok = expect("FXSAVE cwd", field<u16>(image, 0), control);
	ok &= expect("FXSAVE TOP", (field<u16>(image, fxsave_swd) >> 11) & 7, 7);
	ok &= expect("FXSAVE tag", image[fxsave_twd], 0x80);
	ok &= expect("FXSAVE mxcsr", field<u32>(image, fxsave_mxcsr), 0x3f80);
	ok &= holds_one(image, fxsave_st0);
	return ok;
}

#if defined(__i386__)
bool fsave_image(const unsigned char *image, u16 control)
{
	bool ok = expect("FSAVE cwd", field<u32>(image, 0) & 0xffff, control);
	ok &= expect("FSAVE TOP", (field<u32>(image, fsave_swd) >> 11) & 7, 7);
	ok &= expect("FSAVE tag", field<u32>(image, fsave_twd) & 0xffff, 0x3fff);
	ok &= holds_one(image, fsave_st0);
	return ok;
}
#endif

/* What the tracer checks at the child's #UD stop; the child still runs
 * afterwards, so a failure here is not fatal to it. */
bool check_stop(long child)
{
	word regs[user_regs_words] = {};
	bool ok = expect("PTRACE_GETREGS", static_cast<word>(ptrace(
		ptrace_getregs, child, 0, regs)), 0);
	ok &= expect("ip", regs[user_ip], address_of(probe_traced_stop));
	ok &= expect("bx", regs[user_bx], expected_bx);
	ok &= expect("cs", regs[user_cs], expected_cs);
	ok &= expect("ss", regs[user_ss], 0x2b);
	ok &= expect("flags reserved bit and RF",
		     regs[user_flags] & (flag_reserved | flag_rf),
		     flag_reserved | flag_rf);

	word value = 1;
	ok &= expect("PTRACE_PEEKUSER ip", static_cast<word>(ptrace(
		ptrace_peekuser, child, user_ip * sizeof(word), &value)), 0);
	ok &= expect("peeked ip", value, address_of(probe_traced_stop));
	value = 1;
	ok &= expect("PTRACE_PEEKUSER DR7", static_cast<word>(ptrace(
		ptrace_peekuser, child, user_debugreg7, &value)), 0);
	ok &= expect("peeked DR7", value, 0);
	ok &= expect("PTRACE_PEEKUSER past struct user", static_cast<word>(
		ptrace(ptrace_peekuser, child, user_past, &value)),
		static_cast<word>(-eio));

	alignas(16) unsigned char image[512] = {};
#if defined(__x86_64__)
	ok &= expect("PTRACE_GETFPREGS", static_cast<word>(ptrace(
		ptrace_getfpregs, child, 0, image)), 0);
	ok &= fxsave_image(image, 0x027f);
	image[0] = 0x7f;
	image[1] = 0x03;
	ok &= expect("PTRACE_SETFPREGS", static_cast<word>(ptrace(
		ptrace_setfpregs, child, 0, image)), 0);
	__builtin_memset(image, 0, sizeof(image));
	ptrace(ptrace_getfpregs, child, 0, image);
	ok &= fxsave_image(image, 0x037f);
#else
	ok &= expect("PTRACE_GETFPREGS", static_cast<word>(ptrace(
		ptrace_getfpregs, child, 0, image)), 0);
	ok &= fsave_image(image, 0x027f);
	ok &= expect("PTRACE_GETFPXREGS", static_cast<word>(ptrace(
		ptrace_getfpxregs, child, 0, image)), 0);
	ok &= fxsave_image(image, 0x027f);
	/* An FSAVE write changes x87 state and keeps SSE state. */
	ptrace(ptrace_getfpregs, child, 0, image);
	image[0] = 0x7f;
	image[1] = 0x03;
	ok &= expect("PTRACE_SETFPREGS", static_cast<word>(ptrace(
		ptrace_setfpregs, child, 0, image)), 0);
	__builtin_memset(image, 0, sizeof(image));
	ptrace(ptrace_getfpregs, child, 0, image);
	ok &= fsave_image(image, 0x037f);
	__builtin_memset(image, 0, sizeof(image));
	ptrace(ptrace_getfpxregs, child, 0, image);
	ok &= fxsave_image(image, 0x037f);

	UserDesc descriptor = {};
	ok &= expect("PTRACE_GET_THREAD_AREA", static_cast<word>(ptrace(
		ptrace_get_thread_area, child, regs[user_si], &descriptor)), 0);
	ok &= expect("thread area entry", descriptor.entry_number,
		     regs[user_si]);
	ok &= expect("thread area base", descriptor.base_addr, tls_base);
#endif

	regs[user_ip] = address_of(probe_traced_resume);
	ok &= expect("PTRACE_SETREGS", static_cast<word>(ptrace(
		ptrace_setregs, child, 0, regs)), 0);
	word reread[user_regs_words] = {};
	ptrace(ptrace_getregs, child, 0, reread);
	ok &= expect("ip after PTRACE_SETREGS", reread[user_ip],
		     address_of(probe_traced_resume));
	ok &= expect("PTRACE_POKEUSER bx", static_cast<word>(ptrace(
		ptrace_pokeuser, child, user_bx * sizeof(word),
		reinterpret_cast<const void *>(42))), 0);
	value = 0;
	ptrace(ptrace_peekuser, child, user_bx * sizeof(word), &value);
	ok &= expect("peeked bx after PTRACE_POKEUSER", value, 42);
	return ok;
}

/* A tracer of the same ABI reads the child's registers, FP state and TLS at
 * a signal stop, and its register writes take effect. */
bool tracer_sees_registers()
{
	const long child = raw_syscall(nr_fork);
	if (child == 0) {
		word tls_entry = 0;
#if defined(__i386__)
		/* 32-bit, 4 KiB-granular, usable: flags seg_32bit,
		 * limit_in_pages and useable. */
		UserDesc descriptor = {~0U, tls_base, 0xfffff,
				       1U | 1U << 4 | 1U << 6};
		if (raw_syscall(nr_set_thread_area,
				reinterpret_cast<long>(&descriptor)) != 0)
			raw_syscall(nr_exit_group, 3);
		tls_entry = descriptor.entry_number;
#endif
		raw_syscall(nr_ptrace, ptrace_traceme, 0, 0, 0);
		probe_traced(tls_entry);
	}
	int status = 0;
	if (child < 0 || wait_for(child, status) != child) {
		put("  fork or wait failed\n");
		return false;
	}
	if ((status & 0xff) != 0x7f || ((status >> 8) & 0xff) != sigill) {
		put("  the child did not stop with SIGILL: status ");
		put_hex(static_cast<u32>(status));
		put("\n");
		raw_syscall(nr_kill, child, sigkill);
		wait_for(child, status);
		return false;
	}
	bool ok = check_stop(child);
	if (ptrace(ptrace_cont, child, 0, nullptr) != 0 ||
	    wait_for(child, status) != child) {
		put("  PTRACE_CONT failed\n");
		raw_syscall(nr_kill, child, sigkill);
		wait_for(child, status);
		return false;
	}
	ok &= expect("child exit status", static_cast<u32>(status), 42 << 8);
	return ok;
}

/* Waits for the child's next stop and checks that `signal` caused it. */
bool stopped_with(long child, long signal, const char *what)
{
	int status = 0;
	if (wait_for(child, status) == child && (status & 0xff) == 0x7f &&
	    ((status >> 8) & 0xff) == signal)
		return true;
	put("  no ");
	put(what);
	put(" stop: status ");
	put_hex(static_cast<u32>(status));
	put("\n");
	return false;
}

/* Resumes the child to its next system call entry or exit. */
bool syscall_stop(long child, const char *what)
{
	return ptrace(ptrace_syscall, child, 0, nullptr) == 0 &&
	       stopped_with(child, sigtrap, what);
}

word peek_user(long child, unsigned index)
{
	word value = 0x5a5a5a5a;
	if (ptrace(ptrace_peekuser, child, index * sizeof(word), &value) != 0)
		put("  PTRACE_PEEKUSER failed\n");
	return value;
}

bool poke_user(long child, unsigned index, word value)
{
	return expect("PTRACE_POKEUSER", static_cast<word>(ptrace(
		ptrace_pokeuser, child, index * sizeof(word),
		reinterpret_cast<const void *>(value))), 0);
}

/* A tracer that rewrites orig_ax at a system call's entry stop runs another
 * call in its place, or with -1 skips it and leaves the AX it wrote; at a
 * stop outside any system call orig_ax reads -1. */
bool tracer_changes_syscalls()
{
	const word self = static_cast<word>(raw_syscall(nr_getpid));
	const long child = raw_syscall(nr_fork);
	if (child == 0) {
		const long parent = raw_syscall(nr_getppid);
		raw_syscall(nr_ptrace, ptrace_traceme, 0, 0, 0);
		raw_syscall(nr_kill, raw_syscall(nr_getpid), sigstop);
		const long replaced = raw_syscall(nr_getpid);
		const long skipped = raw_syscall(nr_getpid);
		__asm__ volatile("int3");
		raw_syscall(nr_exit_group, (replaced == parent ? 0 : 1) |
			    (skipped == 42 ? 0 : 2));
	}
	if (child < 0) {
		put("  fork failed\n");
		return false;
	}
	bool ok = stopped_with(child, sigstop, "SIGSTOP");
	ok = ok && syscall_stop(child, "getpid entry");
	ok = ok && expect("orig_ax at getpid's entry",
			  peek_user(child, user_orig_ax), nr_getpid);
	ok = ok && poke_user(child, user_orig_ax, nr_getppid);
	ok = ok && syscall_stop(child, "replaced call's exit");
	ok = ok && expect("AX of getppid run in getpid's place",
			  peek_user(child, user_ax), self);
	ok = ok && syscall_stop(child, "second getpid entry");
	ok = ok && poke_user(child, user_orig_ax, ~word{0});
	ok = ok && poke_user(child, user_ax, 42);
	ok = ok && syscall_stop(child, "skipped call's exit");
	ok = ok && expect("AX of a skipped call", peek_user(child, user_ax), 42);
	ok = ok && ptrace(ptrace_cont, child, 0, nullptr) == 0 &&
	     stopped_with(child, sigtrap, "INT3");
	ok = ok && expect("orig_ax at an INT3 stop",
			  peek_user(child, user_orig_ax), ~word{0});
	int status = 0;
	if (!ok || ptrace(ptrace_cont, child, 0, nullptr) != 0 ||
	    wait_for(child, status) != child) {
		raw_syscall(nr_kill, child, sigkill);
		wait_for(child, status);
		return false;
	}
	return expect("child exit status", static_cast<u32>(status), 0);
}

#if defined(__x86_64__)
/* Runs `path mode` and reports whether it exited 0. */
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
		put_decimal(static_cast<u32>(status & 0x7f));
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
	report("x86-64 a 1 ms timer fires during long REP STOSB",
	       in_child(self, "timer-stos"));
	report("x86-64 a 1 ms timer fires during long REPE CMPSB",
	       in_child(self, "timer-cmps"));
	report("i386 a 1 ms timer fires during long REP STOSB",
	       in_child(i386_probe, "timer-stos"));
	report("i386 a 1 ms timer fires during long REPE CMPSB",
	       in_child(i386_probe, "timer-cmps"));
	report("x86-64 ICEBP delivers SIGTRAP TRAP_BRKPT after itself",
	       in_child(self, "icebp"));
	report("i386 ICEBP delivers SIGTRAP TRAP_BRKPT after itself",
	       in_child(i386_probe, "icebp"));
	report("i386 BOUND passes in range and raises #BR outside it",
	       in_child(i386_probe, "bound"));
	report("x86-64 INT 4 delivers SIGSEGV SI_KERNEL",
	       in_child(self, "int4"));
	report("i386 INT 4 delivers SIGSEGV SI_KERNEL",
	       in_child(i386_probe, "int4"));
	report("i386 INTO delivers SIGSEGV SI_KERNEL on overflow",
	       in_child(i386_probe, "into"));
	report("x86-64 a signal keeps the x87 stack",
	       in_child(self, "x87-signal"));
	report("i386 a signal keeps the x87 stack",
	       in_child(i386_probe, "x87-signal"));
	report("x86-64 a signal between instructions keeps RAX",
	       in_child(self, "signal-ax"));
	report("i386 a signal between instructions keeps EAX",
	       in_child(i386_probe, "signal-ax"));
	report("x86-64 tracer reads and writes registers, FP state",
	       in_child(self, "tracer"));
	report("i386 tracer reads and writes registers, FP state and TLS",
	       in_child(i386_probe, "tracer"));
	report("x86-64 tracer changes and skips system calls by orig_ax",
	       in_child(self, "tracer-syscall"));
	report("i386 tracer changes and skips system calls by orig_eax",
	       in_child(i386_probe, "tracer-syscall"));
	report("x86-64 UD2 delivers SIGILL ILL_ILLOPN for #UD",
	       in_child(self, "ud2"));
	report("i386 UD2 delivers SIGILL ILL_ILLOPN for #UD",
	       in_child(i386_probe, "ud2"));
	return failures;
}
#endif

} // namespace

extern "C" int probe_main(word *stack)
{
	const long count = static_cast<long>(stack[0]);
	char **arguments = reinterpret_cast<char **>(stack + 1);
	if (count == 2) {
		const char *mode = arguments[1];
		if (equal(mode, "idle"))
			return 0;
		if (equal(mode, "timer-stos"))
			return timer_ticks_during("REP STOSB", repeat_stos) ? 0 : 1;
		if (equal(mode, "timer-cmps"))
			return timer_ticks_during("REPE CMPSB", repeat_cmps) ? 0 : 1;
		if (equal(mode, "icebp"))
			return icebp_traps() ? 0 : 1;
		if (equal(mode, "int4"))
			return int4_traps() ? 0 : 1;
		if (equal(mode, "x87-signal"))
			return x87_stack_survives_a_signal() ? 0 : 1;
		if (equal(mode, "signal-ax"))
			return signal_keeps_ax() ? 0 : 1;
		if (equal(mode, "tracer"))
			return tracer_sees_registers() ? 0 : 1;
		if (equal(mode, "tracer-syscall"))
			return tracer_changes_syscalls() ? 0 : 1;
		if (equal(mode, "ud2"))
			return ud2_faults() ? 0 : 1;
#if defined(__i386__)
		if (equal(mode, "into"))
			return into_traps() ? 0 : 1;
		if (equal(mode, "bound"))
			return bound_checks() ? 0 : 1;
#else
		return run_all(arguments[0], arguments[1]);
#endif
	}
#if defined(__x86_64__)
	put("usage: exception-probe I386_PROBE\n");
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
