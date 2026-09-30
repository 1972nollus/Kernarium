// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding x86-64 and i386 Linux guest probe: what the x86_64-userspace
 * slice delivers for x86 exceptions and interrupts, against native Linux
 * x86.
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

using u32 = unsigned int;
using u64 = unsigned long long;
using word = unsigned long;

#if defined(__x86_64__)
constexpr long nr_write = 1;
constexpr long nr_rt_sigaction = 13;
constexpr long nr_setitimer = 38;
constexpr long nr_fork = 57;
constexpr long nr_execve = 59;
constexpr long nr_wait4 = 61;
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
	"	ret\n");

/* uc_mcontext: uc_flags, uc_link and a 24-byte stack_t, then r8-r15, rdi,
 * rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags, csgsfs, err and trapno. */
constexpr unsigned frame_ip_offset = 40 + 16 * 8;
constexpr unsigned frame_flags_offset = 40 + 17 * 8;
constexpr unsigned frame_error_offset = 40 + 19 * 8;
constexpr unsigned frame_trapno_offset = 40 + 20 * 8;
constexpr unsigned siginfo_address_offset = 16;

#elif defined(__i386__)
constexpr long nr_write = 4;
constexpr long nr_setitimer = 104;
constexpr long nr_rt_sigaction = 174;
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
	"	ret\n");

/* uc_mcontext: uc_flags, uc_link and a 12-byte stack_t, then gs, fs, es, ds,
 * edi, esi, ebp, esp, ebx, edx, ecx, eax, trapno, err, eip, cs and eflags. */
constexpr unsigned frame_trapno_offset = 20 + 12 * 4;
constexpr unsigned frame_error_offset = 20 + 13 * 4;
constexpr unsigned frame_ip_offset = 20 + 14 * 4;
constexpr unsigned frame_flags_offset = 20 + 16 * 4;
constexpr unsigned siginfo_address_offset = 12;

#else
#error "the exception probe is an x86-64 or i386 program"
#endif

constexpr word sa_siginfo = 4;
constexpr word sa_restorer = 0x04000000;
constexpr long sigill = 4;
constexpr long sigtrap = 5;
constexpr long sigsegv = 11;
constexpr long sigalrm = 14;
constexpr long trap_brkpt = 1;
constexpr long si_kernel = 0x80;
constexpr word flag_rf = 1UL << 16;

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

#if defined(__x86_64__)
long wait_for(long pid, int &status)
{
	return raw_syscall(nr_wait4, pid, reinterpret_cast<long>(&status), 0, 0);
}

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
