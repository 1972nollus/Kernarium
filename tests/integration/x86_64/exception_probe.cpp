// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding x86-64 and i386 Linux guest probe: what the x86_64-userspace
 * slice delivers for x86 exceptions and interrupts, against native Linux x86.
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
	"	ret\n");
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
	"	ret\n");
#else
#error "the exception probe is an x86-64 or i386 program"
#endif

constexpr word sa_siginfo = 4;
constexpr word sa_restorer = 0x04000000;
constexpr long sigalrm = 14;

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

bool equal(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
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
	if (count == 2 && equal(arguments[1], "timer-stos"))
		return timer_ticks_during("REP STOSB", repeat_stos) ? 0 : 1;
	if (count == 2 && equal(arguments[1], "timer-cmps"))
		return timer_ticks_during("REPE CMPSB", repeat_cmps) ? 0 : 1;
#if defined(__x86_64__)
	if (count == 2)
		return run_all(arguments[0], arguments[1]);
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
