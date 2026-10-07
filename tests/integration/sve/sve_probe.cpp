// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Freestanding arm64/Linux probe of the guest kernel's non-streaming SVE task
 * state. It runs as the guest's /init with the Darwin host's SVE policy on,
 * or as an ordinary program on an arm64 Linux host with SVE at a 16-byte
 * vector length, which supplied its expected values: every check holds on
 * both. The guest runs Linux 6.18, and where native Linux 6.12 differs from
 * it, the guest is held to 6.18 and only a native run accepts 6.12.
 *
 * "kernel" mode covers each duty of the kernel: the vector length, prctl and
 * abi.sve_default_vector_length; the live state, which executing SVE starts
 * and a system call, a sigreturn without SVE data, fork and exec end; signal
 * frames and sigreturn, including invalid frames; the task's P and FFR across
 * preemption; NT_ARM_SVE at signal and syscall-entry stops, and what a
 * tracer's write leaves after a system call and in a fork child; and, where
 * the run implements SME, how streaming mode and NT_ARM_SVE interact. Where
 * native Linux may either keep a discarded state live with zero registers or
 * stop tracking it, depending on whether the task was switched out during a
 * system call, a check accepts both. The only SVE instructions are PTRUE,
 * SETFFR, RDFFR and predicate EOR and SEL; register data goes in and out
 * through frames and ptrace. Everything else is compiled with
 * -mgeneral-regs-only.
 *
 * "off" mode runs with the policy off and checks that a run without SVE shows
 * none of it: no HWCAP, prctl, sysctl, regset or sve_context, and an SVE
 * instruction is undefined.
 *
 * In the guest, SVE_PROBE_MODE=<mode> on the kernel command line selects the
 * mode, "kernel" by default, and the probe powers the guest off at the end.
 * Natively the first argument selects it, and the exit status is 0 when every
 * check passed. Both print PASS or FAIL lines, then "sve-probe: <mode>: <n>
 * failure(s) in <m> check(s)".
 */

namespace {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long;

constexpr long nr_mkdirat = 34;
constexpr long nr_mount = 40;
constexpr long nr_openat = 56;
constexpr long nr_close = 57;
constexpr long nr_read = 63;
constexpr long nr_write = 64;
constexpr long nr_exit = 93;
constexpr long nr_exit_group = 94;
constexpr long nr_nanosleep = 101;
constexpr long nr_ptrace = 117;
constexpr long nr_kill = 129;
constexpr long nr_sigaltstack = 132;
constexpr long nr_rt_sigaction = 134;
constexpr long nr_reboot = 142;
constexpr long nr_prctl = 167;
constexpr long nr_getpid = 172;
constexpr long nr_clone = 220;
constexpr long nr_execve = 221;
constexpr long nr_wait4 = 260;

constexpr long at_fdcwd = -100;
constexpr long o_rdwr = 2;
constexpr long o_wronly = 1;
constexpr long enoent = 2;
constexpr long eio = 5;
constexpr long ebadf = 9;
constexpr long eacces = 13;
constexpr long erofs = 30;
constexpr long einval = 22;

constexpr int sigill = 4;
constexpr int sigtrap = 5;
constexpr int sigusr1 = 10;
constexpr int sigsegv = 11;
constexpr int sigchld = 17;
constexpr int sigstop = 19;
constexpr int ill_illopc = 1;

constexpr u64 sa_siginfo = 4;
constexpr u64 sa_onstack = 0x08000000;
constexpr u64 sa_nodefer = 0x40000000;

constexpr u64 clone_vm = 0x100;
constexpr u64 clone_vfork = 0x4000;

constexpr long pr_sve_set_vl = 50;
constexpr long pr_sve_get_vl = 51;
constexpr long pr_sme_get_vl = 64;
constexpr long pr_set_vl_onexec = 1L << 18;
constexpr long pr_vl_inherit = 1L << 17;
constexpr long pr_vl_len_mask = 0xffff;

constexpr long ptrace_traceme = 0;
constexpr long ptrace_cont = 7;
constexpr long ptrace_syscall = 24;
constexpr long ptrace_setoptions = 0x4200;
constexpr long ptrace_getregset = 0x4204;
constexpr long ptrace_setregset = 0x4205;
constexpr long ptrace_o_tracesysgood = 1;

constexpr long nt_prstatus = 1;
constexpr long nt_prfpreg = 2;
constexpr long nt_arm_sve = 0x405;
constexpr long nt_arm_ssve = 0x40b;

constexpr u64 at_hwcap = 16;
constexpr u64 at_hwcap2 = 26;
constexpr u64 at_minsigstksz = 51;
constexpr u64 hwcap_sve = 1UL << 22;
constexpr u64 hwcap2_sve_bits = 0x3fUL << 1;  /* SVE2, SVEAES ... SVESM4 */
constexpr u64 hwcap2_sme = 1UL << 23;

/* Signal frame records (arch/arm64/include/uapi/asm/sigcontext.h). */
constexpr u32 fpsimd_magic = 0x46508001;
constexpr u32 esr_magic = 0x45535201;
constexpr u32 extra_magic = 0x45585401;
constexpr u32 sve_magic = 0x53564501;
constexpr u32 tpidr2_magic = 0x54504902;
constexpr u32 za_magic = 0x54366345;
constexpr u32 zt_magic = 0x5a544e01;
constexpr u16 sve_sig_flag_sm = 1;

constexpr u64 fpsimd_size = 528;
constexpr u64 terminator_size = 16;
constexpr u64 extra_size = 32;
constexpr u64 sigframe_maxsz = 256 * 1024;
constexpr u64 reserved_size = 4096;

/* struct rt_sigframe: siginfo, then the arm64 ucontext. */
constexpr u64 uc_offset = 128;
constexpr u64 uc_stack_offset = uc_offset + 16;
constexpr u64 uc_sigmask_offset = uc_offset + 40;
constexpr u64 mcontext_offset = uc_offset + 176;
constexpr u64 mc_regs_offset = mcontext_offset + 8;
constexpr u64 mc_sp_offset = mcontext_offset + 256;
constexpr u64 mc_pc_offset = mcontext_offset + 264;
constexpr u64 mc_pstate_offset = mcontext_offset + 272;
constexpr u64 reserved_offset = mcontext_offset + 288;
constexpr u64 rt_sigframe_size = reserved_offset + reserved_size;
/* sigreturn_into() and the native frame use these offsets. */
static_assert(mc_regs_offset == 312 && mc_sp_offset == 560 &&
	      mc_pc_offset == 568 && reserved_offset == 592 &&
	      rt_sigframe_size == 4688);

/* The vector length, and the sve_context and NT_ARM_SVE layouts. */
constexpr u64 vl = 16;
constexpr u64 round16(u64 value) { return (value + 15) & ~15UL; }
constexpr u64 sve_zreg_offset(u64 vq, u64 n) { return 16 + n * vq * 16; }
constexpr u64 sve_preg_offset(u64 vq, u64 n) { return 16 + 32 * vq * 16 + n * vq * 2; }
constexpr u64 sve_ffr_offset(u64 vq) { return sve_preg_offset(vq, 16); }
constexpr u64 sve_context_size(u64 vq) { return vq ? sve_ffr_offset(vq) + vq * 2 : 16; }
constexpr u64 sve_pt_fpsr_offset(u64 vq) { return round16(sve_ffr_offset(vq) + vq * 2); }
constexpr u64 sve_pt_size_sve(u64 vq) { return round16(sve_pt_fpsr_offset(vq) + 8); }
constexpr u64 sve_pt_size_fpsimd = 16 + fpsimd_size;
constexpr u16 sve_pt_regs_fpsimd = 0;
constexpr u16 sve_pt_regs_sve = 1;
constexpr u16 sve_pt_vl_inherit = 2;
/* Natively measured at this VL: a 576-byte record, 592 bytes of NT_ARM_SVE
 * and, without SME, an AT_MINSIGSTKSZ of 4720. */
static_assert(round16(sve_context_size(1)) == 576 && sve_pt_size_sve(1) == 592 &&
	      sve_pt_size_fpsimd == 544);
constexpr u64 minsigstksz_without_sme = 4720;

constexpr u64 max_vl = 256;
constexpr u64 max_sve_record = 16 + 34 * max_vl + max_vl / 8;

/* ------------------------------------------------------------------ */
/* System calls, memory and output.                                    */

inline long sys(long number, long a0 = 0, long a1 = 0, long a2 = 0,
		long a3 = 0, long a4 = 0, long a5 = 0)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x5 __asm__("x5") = a5;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory", "cc");
	return x0;
}

template <typename T> long ptr(T *value) { return reinterpret_cast<long>(value); }

} // namespace

extern "C" void *memset(void *destination, int value, u64 count)
{
	auto *bytes = static_cast<volatile u8 *>(destination);
	for (u64 index = 0; index < count; index++)
		bytes[index] = static_cast<u8>(value);
	return destination;
}

extern "C" void *memcpy(void *destination, const void *source, u64 count)
{
	auto *to = static_cast<volatile u8 *>(destination);
	const auto *from = static_cast<const volatile u8 *>(source);
	for (u64 index = 0; index < count; index++)
		to[index] = from[index];
	return destination;
}

extern "C" int memcmp(const void *left, const void *right, u64 count)
{
	const auto *a = static_cast<const volatile u8 *>(left);
	const auto *b = static_cast<const volatile u8 *>(right);
	for (u64 index = 0; index < count; index++)
		if (a[index] != b[index])
			return a[index] < b[index] ? -1 : 1;
	return 0;
}

namespace {

u64 string_length(const char *text)
{
	u64 length = 0;
	while (text[length] != '\0')
		length++;
	return length;
}

bool same_string(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}
	return *left == *right;
}

void write_text(const char *text)
{
	u64 length = string_length(text);
	while (length > 0) {
		long written = sys(nr_write, 1, ptr(text), static_cast<long>(length));
		if (written <= 0)
			return;
		text += written;
		length -= static_cast<u64>(written);
	}
}

/* One line, assembled first so that a child's lines do not interleave. */
struct Line {
	char text[256];
	u64 length = 0;

	Line &add(const char *part)
	{
		while (*part != '\0' && length + 2 < sizeof(text))
			text[length++] = *part++;
		text[length] = '\0';
		return *this;
	}
	Line &number(long value)
	{
		char digits[24];
		int count = 0;
		bool negative = value < 0;
		u64 magnitude = negative ? static_cast<u64>(-value) : static_cast<u64>(value);
		do {
			digits[count++] = static_cast<char>('0' + magnitude % 10);
			magnitude /= 10;
		} while (magnitude != 0);
		if (negative)
			add("-");
		while (count > 0) {
			char digit[2] = {digits[--count], '\0'};
			add(digit);
		}
		return *this;
	}
	Line &hex(u64 value)
	{
		static const char nibbles[] = "0123456789abcdef";
		add("0x");
		bool started = false;
		for (int shift = 60; shift >= 0; shift -= 4) {
			unsigned nibble = (value >> shift) & 15;
			if (nibble != 0 || started || shift == 0) {
				char digit[2] = {nibbles[nibble], '\0'};
				add(digit);
				started = true;
			}
		}
		return *this;
	}
	void print()
	{
		text[length++] = '\n';
		text[length] = '\0';
		write_text(text);
		length = 0;
	}
};

int failures;
int checks;
const char *group = "";
bool native;

void check(bool passed, const char *name)
{
	checks++;
	if (!passed)
		failures++;
	Line().add(passed ? "PASS " : "FAIL ").add(group).add(": ").add(name).print();
}

void check_value(u64 actual, u64 expected, const char *name)
{
	checks++;
	if (actual == expected) {
		Line().add("PASS ").add(group).add(": ").add(name).print();
		return;
	}
	failures++;
	Line().add("FAIL ").add(group).add(": ").add(name).add(": ")
		.hex(actual).add(" != ").hex(expected).print();
}

void check_error(long result, long error, const char *name)
{
	check_value(static_cast<u64>(result), static_cast<u64>(-error), name);
}

void note(const char *text, long value)
{
	Line().add("sve-probe: ").add(text).add(" ").number(value).print();
}

[[noreturn]] void exit_task(long code)
{
	for (;;)
		sys(nr_exit, code);
}

/* ------------------------------------------------------------------ */
/* The run's parameters and deterministic register contents.          */

u64 auxv_hwcap;
u64 auxv_hwcap2;
u64 auxv_minsigstksz;
bool sme;       /* the run implements SME */
u64 svl;        /* its streaming vector length, bytes */
u64 svq;
const char *self_path = "/init";

u8 v_byte(u64 seed, u64 reg, u64 byte) { return static_cast<u8>(0xf0 ^ (reg + byte * 5 + seed * 29)); }
u8 z_byte(u64 reg, u64 byte) { return static_cast<u8>(reg * 7 + byte * 13 + 1); }
u16 p_value(u64 seed, u64 reg) { return static_cast<u16>(0x9e37 * (reg + 1) + 0x7f4a * seed + (reg << 12)); }
/* WRFFR takes only monotonic values. */
u16 ffr_value(u64 seed) { return static_cast<u16>((2UL << (seed % 16)) - 1); }

/* P0-P15 and FFR as a frame or NT_ARM_SVE holds them. */
struct Predicates {
	u16 p[16];
	u16 ffr;
};

Predicates pattern(u64 seed)
{
	Predicates value{};
	for (u64 reg = 0; reg < 16; reg++)
		value.p[reg] = p_value(seed, reg);
	value.ffr = ffr_value(seed);
	return value;
}

/* After SETFFR and PTRUE P0.B in a task that is not live. */
Predicates marked()
{
	Predicates value{};
	value.p[0] = 0xffff;
	value.ffr = 0xffff;
	return value;
}

/* ------------------------------------------------------------------ */
/* Assembly: sigreturn into a frame, continuations, clone, faults.     */

extern "C" {
/* Saves the callee-saved registers, SP and the return address into the
 * frame's mcontext, sets its PC to `continuation`, and runs rt_sigreturn on
 * it. The continuation runs with the frame's state and returns to this
 * function's caller. */
void sigreturn_into(u8 *frame, void (*continuation)());
/* Continuations. The ones a streaming frame can reach use only GPRs and
 * leave streaming mode through a system call before returning to C++. */
void continue_return();
void continue_fault();
void continue_fault_then_syscall();
void continue_syscall();
void continue_exit_zero();
/* P1 = (P1 EOR P2) AND P0, P3 = FFR, then a fault. */
void continue_predicate_ops();
/* SETFFR and PTRUE P0.B, then a fault; after a system call first. */
void continue_mark_fault();
void continue_syscall_mark_fault();
/* A loop of SEL P1.B, P1, P1.B, P1.B until CNTVCT_EL0 reaches spin_until,
 * then a fault. */
void continue_spin();
/* A load from address 0, which the SIGSEGV handler steps over. */
void fault();
/* SETFFR and PTRUE P0.B: the task executes SVE. */
void sve_mark();
/* PTRUE P0.B alone, for off mode. */
void sve_ptrue();
/* kill(pid, SIGSTOP), then a fault, then a system call. */
void stop_fault_syscall(long pid);
/* clone(flags, stack); the child runs function(argument) on `stack` and
 * exits with its result. Returns the child's PID (or -errno) in the parent. */
long clone_run(u64 flags, void *stack, int (*function)(void *), void *argument);
}

__asm__(R"(
	.text
	.arch	armv8.2-a+sve
	.p2align 2
	.globl sigreturn_into
sigreturn_into:
	add	x9, x0, #312
	stp	x19, x20, [x9, #152]
	stp	x21, x22, [x9, #168]
	stp	x23, x24, [x9, #184]
	stp	x25, x26, [x9, #200]
	stp	x27, x28, [x9, #216]
	stp	x29, x30, [x9, #232]
	mov	x10, sp
	str	x10, [x0, #560]
	str	x1, [x0, #568]
	mov	sp, x0
	mov	x8, #139
	svc	#0
	brk	#0x51

	.globl continue_return
continue_return:
	ret

	.globl continue_fault
continue_fault:
	mov	x9, #0
	ldr	x9, [x9]
	ret

	.globl continue_fault_then_syscall
continue_fault_then_syscall:
	mov	x9, #0
	ldr	x9, [x9]
	mov	x8, #172
	svc	#0
	ret

	.globl continue_syscall
continue_syscall:
	mov	x8, #172
	svc	#0
	ret

	.globl continue_exit_zero
continue_exit_zero:
	mov	x0, #0
	mov	x8, #93
	svc	#0
	brk	#0x52

	.globl continue_predicate_ops
continue_predicate_ops:
	eor	p1.b, p0/z, p1.b, p2.b
	rdffr	p3.b
	mov	x9, #0
	ldr	x9, [x9]
	ret

	.globl continue_syscall_mark_fault
continue_syscall_mark_fault:
	mov	x8, #172
	svc	#0
	.globl continue_mark_fault
continue_mark_fault:
	setffr
	ptrue	p0.b
	mov	x9, #0
	ldr	x9, [x9]
	ret

	.globl continue_spin
continue_spin:
	adrp	x9, spin_until
	ldr	x9, [x9, :lo12:spin_until]
1:	sel	p1.b, p1, p1.b, p1.b
	mrs	x10, cntvct_el0
	cmp	x10, x9
	b.lo	1b
	mov	x9, #0
	ldr	x9, [x9]
	ret

	.globl fault
fault:
	mov	x9, #0
	ldr	x9, [x9]
	ret

	.globl sve_mark
sve_mark:
	setffr
	ptrue	p0.b
	ret

	.globl sve_ptrue
sve_ptrue:
	ptrue	p0.b
	ret

	.globl stop_fault_syscall
stop_fault_syscall:
	mov	x1, #19
	mov	x8, #129
	svc	#0
	mov	x9, #0
	ldr	x9, [x9]
	mov	x8, #172
	svc	#0
	ret

	.globl clone_run
clone_run:
	mov	x10, x2
	mov	x11, x3
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x8, #220
	svc	#0
	cbnz	x0, 1f
	mov	x0, x11
	blr	x10
	mov	x8, #93
	svc	#0
	brk	#0x53
1:	ret
)");

} // namespace

extern "C" u64 spin_until;
u64 spin_until;

namespace {

u64 counter()
{
	u64 value;
	__asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value));
	return value;
}

u64 counter_frequency()
{
	u64 value;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
}

/* ------------------------------------------------------------------ */
/* Signal handlers and what they capture.                              */

struct Capture {
	bool valid;
	const char *error;      /* the first layout error, or nullptr */
	u64 record_count;
	u32 magics[12];
	u32 sizes[12];
	u32 fpsr, fpcr;
	u8 vregs[512];
	int sve_records;
	u16 sve_vl, sve_flags;
	u32 sve_size;
	u8 sve_data[max_sve_record];
	bool sme_records;       /* tpidr2_context, za_context or zt_context */
};

Capture captures[3];
volatile int capture_count;
volatile int nested_request;
volatile int ill_code;

template <typename T> T load(const u8 *at)
{
	T value;
	memcpy(&value, at, sizeof(value));
	return value;
}

template <typename T> void store(u8 *at, T value) { memcpy(at, &value, sizeof(value)); }

void parse_frame(Capture &capture, u8 *frame)
{
	memset(&capture, 0, sizeof(capture));
	capture.valid = true;
	u8 *base = frame + reserved_offset;
	u64 limit = reserved_size;
	u64 offset = 0;
	bool extra_seen = false;
	for (;;) {
		if (capture.record_count == 12 || limit - offset < 8) {
			capture.error = "records run past their space";
			return;
		}
		u8 *head = base + offset;
		u32 magic = load<u32>(head);
		u32 size = load<u32>(head + 4);
		if (magic == 0) {
			if (size != 0)
				capture.error = "terminator with a size";
			return;
		}
		if (size < 16 || (size & 15) != 0 || size > limit - offset) {
			capture.error = "record size";
			return;
		}
		u64 index = capture.record_count++;
		capture.magics[index] = magic;
		capture.sizes[index] = size;
		switch (magic) {
		case fpsimd_magic:
			memcpy(capture.vregs, head + 16, sizeof(capture.vregs));
			capture.fpsr = load<u32>(head + 8);
			capture.fpcr = load<u32>(head + 12);
			break;
		case esr_magic:
			break;
		case sve_magic:
			capture.sve_records++;
			capture.sve_vl = load<u16>(head + 8);
			capture.sve_flags = load<u16>(head + 10);
			capture.sve_size = size;
			memcpy(capture.sve_data, head, size < max_sve_record ? size : max_sve_record);
			break;
		case tpidr2_magic:
		case za_magic:
		case zt_magic:
			capture.sme_records = true;
			break;
		case extra_magic: {
			u64 datap = load<u64>(head + 8);
			u32 data_size = load<u32>(head + 16);
			u8 *terminator = head + size;
			if (extra_seen || load<u64>(terminator) != 0 ||
			    datap != reinterpret_cast<u64>(terminator + 16)) {
				capture.error = "extra_context";
				return;
			}
			extra_seen = true;
			base = reinterpret_cast<u8 *>(datap);
			limit = data_size;
			offset = 0;
			continue;
		}
		default:
			capture.error = "unexpected record";
			return;
		}
		offset += size;
	}
}

void capture_frame(void *context)
{
	u8 *frame = static_cast<u8 *>(context) - uc_offset;
	int index = capture_count;
	if (index < 3) {
		parse_frame(captures[index], frame);
		capture_count = index + 1;
	}
}

void segv_handler(int, void *, void *context)
{
	capture_frame(context);
	if (nested_request == 1) {
		/* The handler's own state, seen through a fault inside it. */
		nested_request = 2;
		fault();
		nested_request = 0;
	}
	u8 *frame = static_cast<u8 *>(context) - uc_offset;
	store<u64>(frame + mc_pc_offset, load<u64>(frame + mc_pc_offset) + 4);
}

/* A signal sent by kill(): the frame is taken, the PC stays. */
void usr1_handler(int, void *, void *context)
{
	capture_frame(context);
}

void ill_handler(int, void *info, void *context)
{
	ill_code = load<int>(static_cast<u8 *>(info) + 8);
	u8 *frame = static_cast<u8 *>(context) - uc_offset;
	store<u64>(frame + mc_pc_offset, load<u64>(frame + mc_pc_offset) + 4);
}

struct KernelSigaction {
	void (*handler)(int, void *, void *);
	u64 flags;
	void (*restorer)();
	u64 mask;
};

alignas(16) u8 altstack[512 * 1024];

void set_handler(int signal, void (*handler)(int, void *, void *))
{
	KernelSigaction action = {handler, handler != nullptr ?
		sa_siginfo | sa_onstack | sa_nodefer : 0, nullptr, 0};
	sys(nr_rt_sigaction, signal, ptr(&action), 0, 8);
}

void install_handlers()
{
	struct {
		void *sp;
		int flags;
		u64 size;
	} stack = {altstack, 0, sizeof(altstack)};
	sys(nr_sigaltstack, ptr(&stack), 0);
	set_handler(sigsegv, segv_handler);
	set_handler(sigusr1, usr1_handler);
	set_handler(sigill, ill_handler);
}

void reset_captures()
{
	capture_count = 0;
	nested_request = 0;
}

/* A fault now, captured. */
Capture &capture_now()
{
	reset_captures();
	fault();
	return captures[0];
}

/* ------------------------------------------------------------------ */
/* Frames for rt_sigreturn.                                            */

struct Layout {
	u64 size;
	u64 limit;
	bool extra;
	u64 count;
	u64 offsets[12];
};

bool layout_alloc(Layout &layout, u64 size, bool extend = true)
{
	u64 padded = round16(size);
	if (padded > layout.limit - layout.size && !layout.extra && extend) {
		layout.limit += extra_size;
		if (!layout_alloc(layout, extra_size, false))
			return false;
		layout.extra = true;
		layout.size += terminator_size;
		layout.limit = sigframe_maxsz - terminator_size;
	}
	if (padded > layout.limit - layout.size)
		return false;
	layout.offsets[layout.count++] = layout.size;
	layout.size += padded;
	return true;
}

alignas(16) u8 frame_buffer[sigframe_maxsz + 4096];

struct FrameBuilder {
	u8 *frame = frame_buffer;
	Layout layout{};
	u64 extra_offset = 0;

	explicit FrameBuilder()
	{
		memset(frame_buffer, 0, sizeof(frame_buffer));
		/* Keep the alternate signal stack: sigreturn restores it. */
		store<u64>(frame + uc_stack_offset, reinterpret_cast<u64>(altstack));
		store<int>(frame + uc_stack_offset + 8, 0);
		store<u64>(frame + uc_stack_offset + 16, sizeof(altstack));
		store<u64>(frame + uc_sigmask_offset, 0);
		store<u64>(frame + mc_pstate_offset, 0);
		layout.size = reserved_offset;
		layout.limit = reserved_offset + reserved_size - terminator_size - extra_size;
	}

	u8 *add(u32 magic, u64 size)
	{
		u64 before = layout.count;
		if (!layout_alloc(layout, size))
			return nullptr;
		if (layout.count == before + 2)
			extra_offset = layout.offsets[before];
		u8 *head = frame + layout.offsets[layout.count - 1];
		store<u32>(head, magic);
		store<u32>(head + 4, static_cast<u32>(round16(size)));
		return head;
	}

	u8 *fpsimd(u64 seed)
	{
		u8 *head = add(fpsimd_magic, fpsimd_size);
		for (u64 reg = 0; reg < 32; reg++)
			for (u64 byte = 0; byte < 16; byte++)
				head[16 + reg * 16 + byte] = v_byte(seed, reg, byte);
		return head;
	}

	/* Live non-streaming registers: Z data that sigreturn ignores, since
	 * fpsimd_context supplies all of each Z at this VL, then P and FFR. */
	u8 *sve(const Predicates &value, u16 record_vl = vl)
	{
		u64 vq = record_vl / 16;
		u8 *head = add(sve_magic, sve_context_size(vq));
		store<u16>(head + 8, record_vl);
		for (u64 reg = 0; reg < 32; reg++)
			for (u64 byte = 0; byte < 16 * vq; byte++)
				head[sve_zreg_offset(vq, reg) + byte] = z_byte(reg, byte);
		for (u64 reg = 0; reg < 16; reg++)
			store<u16>(head + sve_preg_offset(vq, reg), value.p[reg]);
		store<u16>(head + sve_ffr_offset(vq), value.ffr);
		return head;
	}

	u8 *sve_header(u16 record_vl = vl)
	{
		u8 *head = add(sve_magic, 16);
		store<u16>(head + 8, record_vl);
		return head;
	}

	/* Streaming registers at the SVL, for a run with SME. */
	u8 *sve_streaming()
	{
		u8 *head = add(sve_magic, sve_context_size(svq));
		store<u16>(head + 8, static_cast<u16>(svl));
		store<u16>(head + 10, sve_sig_flag_sm);
		for (u64 reg = 0; reg < 32; reg++)
			for (u64 byte = 0; byte < svl; byte++)
				head[sve_zreg_offset(svq, reg) + byte] = z_byte(reg, byte);
		return head;
	}

	u8 *finish()
	{
		layout.limit += terminator_size;
		layout.size += terminator_size;
		if (layout.extra) {
			u8 *extra = frame + extra_offset;
			u8 *data = extra + extra_size + terminator_size;
			store<u32>(extra, extra_magic);
			store<u32>(extra + 4, static_cast<u32>(extra_size));
			store<u64>(extra + 8, reinterpret_cast<u64>(data));
			store<u32>(extra + 16, static_cast<u32>(frame + round16(layout.size) - data));
		}
		return frame;
	}
};

/* fpsimd_context with V from `seed`, then sve_context with its P and FFR. */
u8 *live_frame(u64 seed)
{
	FrameBuilder builder;
	builder.fpsimd(seed);
	builder.sve(pattern(seed));
	return builder.finish();
}

u8 *header_frame(u64 seed)
{
	FrameBuilder builder;
	builder.fpsimd(seed);
	builder.sve_header();
	return builder.finish();
}

/* ------------------------------------------------------------------ */
/* Checks of a captured frame.                                         */

bool well_formed(const Capture &capture)
{
	return capture.valid && capture.error == nullptr && capture.record_count > 0 &&
		capture.magics[0] == fpsimd_magic && capture.sve_records == 1 &&
		capture.sme_records == sme;
}

/* Not live: the record is a header with the VL. */
bool header_only(const Capture &capture)
{
	return well_formed(capture) && capture.sve_vl == vl && capture.sve_flags == 0 &&
		capture.sve_size == 16;
}

/* Live with these P and FFR; at this VL each Z is its V. */
bool live_with(const Capture &capture, const Predicates &value)
{
	if (!well_formed(capture) || capture.sve_vl != vl || capture.sve_flags != 0 ||
	    capture.sve_size != round16(sve_context_size(1)))
		return false;
	for (u64 reg = 0; reg < 16; reg++)
		if (load<u16>(capture.sve_data + sve_preg_offset(1, reg)) != value.p[reg])
			return false;
	if (load<u16>(capture.sve_data + sve_ffr_offset(1)) != value.ffr)
		return false;
	return memcmp(capture.sve_data + sve_zreg_offset(1, 0), capture.vregs, 512) == 0;
}

/* After a system call: native Linux stops tracking the state, or keeps it
 * live with P and FFR zero when the task was not switched out. */
bool discarded(const Capture &capture)
{
	return header_only(capture) || live_with(capture, Predicates{});
}

bool vregs_are(const Capture &capture, u64 seed)
{
	for (u64 reg = 0; reg < 32; reg++)
		for (u64 byte = 0; byte < 16; byte++)
			if (capture.vregs[reg * 16 + byte] != v_byte(seed, reg, byte))
				return false;
	return true;
}

/* ------------------------------------------------------------------ */
/* Parameters, frames and sigreturn.                                   */

void test_parameters()
{
	group = "parameters";
	check((auxv_hwcap & hwcap_sve) != 0, "AT_HWCAP has HWCAP_SVE");
	note("HWCAP2 SVE bits", static_cast<long>(auxv_hwcap2 & hwcap2_sve_bits));
	long status = sys(nr_prctl, pr_sve_get_vl);
	check_value(static_cast<u64>(status), vl, "PR_SVE_GET_VL reports 16 bytes");
	sme = (auxv_hwcap2 & hwcap2_sme) != 0;
	if (sme) {
		svl = static_cast<u64>(sys(nr_prctl, pr_sme_get_vl)) & pr_vl_len_mask;
		svq = svl / 16;
	}
	note("SME streaming vector length", static_cast<long>(svl));
	note("AT_MINSIGSTKSZ", static_cast<long>(auxv_minsigstksz));
	if (sme)
		check(auxv_minsigstksz >= minsigstksz_without_sme,
		      "AT_MINSIGSTKSZ covers the SVE frame");
	else
		check_value(auxv_minsigstksz, minsigstksz_without_sme,
			    "AT_MINSIGSTKSZ is the native value with SVE alone");
}

void test_frames()
{
	group = "frame";
	sys(nr_getpid);
	Capture &plain = capture_now();
	check(header_only(plain),
	      "a task that has not executed SVE since a system call has a header-only sve_context");
	sve_mark();
	check(live_with(capture_now(), marked()),
	      "SETFFR and PTRUE make the task live: P0 and FFR all true, the rest zero");
	sve_mark();
	sys(nr_getpid);
	check(discarded(capture_now()), "a system call discards P and FFR");
	long self = sys(nr_getpid);
	sve_mark();
	reset_captures();
	sys(nr_kill, self, sigusr1);
	check(capture_count == 1 && header_only(captures[0]),
	      "a signal a system call raises has a header-only sve_context");

	/* The handler starts with the interrupted task's state. */
	reset_captures();
	nested_request = 1;
	sigreturn_into(live_frame(1), continue_fault);
	check(capture_count == 2 && live_with(captures[0], pattern(1)),
	      "a fault's frame holds the live P and FFR");
	check(capture_count == 2 && live_with(captures[1], pattern(1)),
	      "its handler starts with the same P and FFR");
}

void test_sigreturn()
{
	group = "sigreturn";
	reset_captures();
	sigreturn_into(live_frame(2), continue_fault);
	check(capture_count == 1 && live_with(captures[0], pattern(2)),
	      "a frame with SVE data restores P and FFR");
	check(capture_count == 1 && vregs_are(captures[0], 2),
	      "and each Z is the V of fpsimd_context, not the record's Z data");

	reset_captures();
	sigreturn_into(live_frame(3), continue_predicate_ops);
	Predicates expected = pattern(3);
	expected.p[1] = static_cast<u16>((expected.p[1] ^ expected.p[2]) & expected.p[0]);
	expected.p[3] = expected.ffr;
	check(capture_count == 1 && live_with(captures[0], expected),
	      "the CPU executes SVE on the P and FFR sigreturn restored");

	reset_captures();
	sigreturn_into(live_frame(4), continue_return);
	sigreturn_into(header_frame(4), continue_fault);
	check(capture_count == 1 && header_only(captures[0]) && vregs_are(captures[0], 4),
	      "a header-only sve_context leaves the task not live");
	reset_captures();
	sigreturn_into(live_frame(4), continue_return);
	sigreturn_into(header_frame(4), continue_mark_fault);
	check(capture_count == 1 && live_with(captures[0], marked()),
	      "and its other P read zero when it next executes SVE");

	reset_captures();
	sigreturn_into(live_frame(5), continue_return);
	FrameBuilder builder;
	builder.fpsimd(5);
	sigreturn_into(builder.finish(), continue_mark_fault);
	check(capture_count == 1 && live_with(captures[0], marked()),
	      "a frame without sve_context discards P and FFR too");

	reset_captures();
	sigreturn_into(live_frame(6), continue_syscall_mark_fault);
	check(capture_count == 1 && live_with(captures[0], marked()),
	      "a system call zeroes the P and FFR sigreturn restored");
}

/* ------------------------------------------------------------------ */
/* Invalid frames: each child dies of SIGSEGV; a valid one exits 0.    */

enum class Frame {
	live,
	header,
	no_sve,
	header_vl_32,
	header_vl_0,
	data_vl_32,
	short_record,
	two_records,
	streaming_without_sme,
	tpidr2_without_sme,
	za_without_sme,
};

Frame frame_kind;

int frame_child(void *)
{
	set_handler(sigsegv, nullptr);
	FrameBuilder builder;
	builder.fpsimd(7);
	switch (frame_kind) {
	case Frame::live:
		builder.sve(pattern(7));
		break;
	case Frame::header:
		builder.sve_header();
		break;
	case Frame::no_sve:
		break;
	case Frame::header_vl_32:
		builder.sve_header(32);
		break;
	case Frame::header_vl_0:
		builder.sve_header(0);
		break;
	case Frame::data_vl_32:
		builder.sve(pattern(7), 32);
		break;
	case Frame::short_record: {
		u8 *head = builder.add(sve_magic, 64);
		store<u16>(head + 8, static_cast<u16>(vl));
		break;
	}
	case Frame::two_records:
		builder.sve_header();
		builder.sve_header();
		break;
	case Frame::streaming_without_sme: {
		u8 *head = builder.sve(pattern(7));
		store<u16>(head + 10, sve_sig_flag_sm);
		break;
	}
	case Frame::tpidr2_without_sme:
		builder.sve_header();
		builder.add(tpidr2_magic, 16);
		break;
	case Frame::za_without_sme: {
		builder.sve_header();
		u8 *head = builder.add(za_magic, 16);
		store<u16>(head + 8, static_cast<u16>(vl));
		break;
	}
	}
	sigreturn_into(builder.finish(), continue_exit_zero);
	return 0;
}

/* Runs function in a forked child; returns its wait status. */
long run_child(int (*function)(void *), void *argument = nullptr, u64 flags = sigchld,
	       void *stack = nullptr)
{
	long pid = clone_run(flags, stack, function, argument);
	if (pid < 0)
		return pid;
	int status = 0;
	sys(nr_wait4, pid, ptr(&status), 0, 0);
	return status;
}

bool killed_by(long status, int signal) { return (status & 0x7f) == signal; }

void test_invalid_frames()
{
	group = "invalid-frames";
	struct Case {
		Frame kind;
		bool valid;
		bool without_sme;
		const char *name;
	};
	static const Case cases[] = {
		{Frame::live, true, false, "a frame with SVE data is accepted"},
		{Frame::header, true, false, "a frame with a header-only sve_context is accepted"},
		{Frame::no_sve, true, false, "a frame without sve_context is accepted"},
		{Frame::header_vl_32, false, false, "a header with VL 32 is rejected"},
		{Frame::header_vl_0, false, false, "a header with VL 0 is rejected"},
		{Frame::data_vl_32, false, false, "SVE data for VL 32 is rejected"},
		{Frame::short_record, false, false,
		 "an sve_context between a header and full is rejected"},
		{Frame::two_records, false, false, "two sve_context records are rejected"},
		{Frame::streaming_without_sme, false, true,
		 "a streaming sve_context is rejected without SME"},
		{Frame::tpidr2_without_sme, false, true,
		 "a tpidr2_context is rejected without SME"},
		{Frame::za_without_sme, false, true, "a za_context is rejected without SME"},
	};
	for (const Case &item : cases) {
		if (item.without_sme && sme)
			continue;
		frame_kind = item.kind;
		long status = run_child(frame_child);
		check(item.valid ? status == 0 : killed_by(status, sigsegv), item.name);
	}
}

/* ------------------------------------------------------------------ */
/* Preemption: tasks keep their P and FFR across switches.             */

int spin_child(void *argument)
{
	u64 seed = reinterpret_cast<u64>(argument);
	reset_captures();
	sigreturn_into(live_frame(seed), continue_spin);
	return capture_count == 1 && live_with(captures[0], pattern(seed)) ? 0 : 1;
}

void test_switches()
{
	group = "switch";
	constexpr int tasks = 3;
	spin_until = counter() + counter_frequency() * 3 / 2;
	long pids[tasks];
	for (int index = 0; index < tasks; index++)
		pids[index] = clone_run(sigchld, nullptr, spin_child,
					reinterpret_cast<void *>(10 + index));
	bool kept = true;
	for (long pid : pids) {
		int status = -1;
		kept = kept && pid > 0 && sys(nr_wait4, pid, ptr(&status), 0, 0) == pid &&
			status == 0;
	}
	check(kept, "three tasks on two CPUs keep their P and FFR for 1.5 seconds");
}

/* ------------------------------------------------------------------ */
/* fork, vfork, clone, exec.                                           */

volatile int shared_result;

/* A child starts not live and can execute SVE: 0 if so. */
int expect_fresh(void *)
{
	bool fresh = header_only(capture_now());
	sve_mark();
	bool marks = live_with(capture_now(), marked());
	int result = fresh && marks ? 0 : 1;
	shared_result = result;
	return result;
}

int expect_inherit_flag(void *)
{
	return sys(nr_prctl, pr_sve_get_vl) == static_cast<long>(vl | pr_vl_inherit) ? 0 : 1;
}

int exec_child(void *argument)
{
	const char *inherit = static_cast<const char *>(argument);
	if (inherit[0] == 'i')
		sys(nr_prctl, pr_sve_set_vl, static_cast<long>(vl) | pr_vl_inherit);
	sve_mark();
	const char *argv[] = {self_path, "exec-check", inherit, nullptr};
	const char *envp[] = {nullptr};
	sys(nr_execve, ptr(self_path), ptr(argv), ptr(envp));
	return 99;
}

alignas(16) u8 thread_stack[64 * 1024];

void test_task_creation()
{
	group = "task-creation";
	sigreturn_into(live_frame(8), continue_return);
	check(run_child(expect_fresh) == 0,
	      "fork: the child starts not live and executes SVE");
	shared_result = -1;
	sigreturn_into(live_frame(8), continue_return);
	long status = run_child(expect_fresh, nullptr, clone_vm | sigchld,
				thread_stack + sizeof(thread_stack));
	check(status == 0 && shared_result == 0,
	      "clone(CLONE_VM): the child starts not live and executes SVE");
	shared_result = -1;
	sigreturn_into(live_frame(8), continue_return);
	status = run_child(expect_fresh, nullptr, clone_vm | clone_vfork | sigchld,
			   thread_stack + sizeof(thread_stack));
	check(status == 0 && shared_result == 0,
	      "vfork: the child starts not live and executes SVE");
	check(run_child(exec_child, const_cast<char *>("plain")) == 0,
	      "exec: the image starts not live, executes SVE and does not inherit");
	check(run_child(exec_child, const_cast<char *>("inherit")) == 0,
	      "exec: PR_SVE_VL_INHERIT survives exec");
	sys(nr_prctl, pr_sve_set_vl, static_cast<long>(vl) | pr_vl_inherit);
	check(run_child(expect_inherit_flag) == 0, "fork: the child keeps PR_SVE_VL_INHERIT");
	sys(nr_prctl, pr_sve_set_vl, static_cast<long>(vl));
}

/* Run by the exec'd image; its exit status reports the result. */
[[noreturn]] void exec_check(const char *inherit)
{
	install_handlers();
	sme = (auxv_hwcap2 & hwcap2_sme) != 0;
	long status = sys(nr_prctl, pr_sve_get_vl);
	bool fresh = header_only(capture_now());
	sve_mark();
	bool marks = live_with(capture_now(), marked());
	bool flag = (status & pr_vl_inherit) != 0;
	bool length = (status & pr_vl_len_mask) == static_cast<long>(vl);
	exit_task(fresh && marks && length && flag == (inherit[0] == 'i') ? 0 : 1);
}

/* ------------------------------------------------------------------ */
/* prctl and the sysctl.                                               */

void test_prctl()
{
	group = "prctl";
	static const long lengths[] = {16, 32, 64, 128, 256, 512, 4096, 8192};
	for (long length : lengths) {
		Line name;
		name.add("PR_SVE_SET_VL ").number(length).add(" resolves to 16");
		check_value(static_cast<u64>(sys(nr_prctl, pr_sve_set_vl, length)), vl, name.text);
	}
	check_error(sys(nr_prctl, pr_sve_set_vl, 17), einval, "PR_SVE_SET_VL 17 is EINVAL");
	check_error(sys(nr_prctl, pr_sve_set_vl, 0), einval, "PR_SVE_SET_VL 0 is EINVAL");
	check_error(sys(nr_prctl, pr_sve_set_vl, static_cast<long>(vl) | (1L << 20)), einval,
		    "an unknown PR_SVE_SET_VL flag is EINVAL");
	check_value(static_cast<u64>(sys(nr_prctl, pr_sve_set_vl,
					 static_cast<long>(vl) | pr_vl_inherit)),
		    vl | pr_vl_inherit, "PR_SVE_VL_INHERIT is accepted");
	check_value(static_cast<u64>(sys(nr_prctl, pr_sve_get_vl)), vl | pr_vl_inherit,
		    "PR_SVE_GET_VL reports it");
	check_value(static_cast<u64>(sys(nr_prctl, pr_sve_set_vl,
					 static_cast<long>(vl) | pr_set_vl_onexec)),
		    vl, "PR_SVE_SET_VL_ONEXEC is accepted and clears the inherit flag");
	check_value(static_cast<u64>(sys(nr_prctl, pr_sve_get_vl)), vl,
		    "PR_SVE_GET_VL reports 16 alone again");
}

const char *const sysctl_path = "/proc/sys/abi/sve_default_vector_length";

bool read_sysctl(char *buffer, u64 size)
{
	long fd = sys(nr_openat, at_fdcwd, ptr(sysctl_path), 0);
	if (fd < 0)
		return false;
	long length = sys(nr_read, fd, ptr(buffer), static_cast<long>(size - 1));
	sys(nr_close, fd);
	if (length < 0)
		return false;
	buffer[length] = '\0';
	return true;
}

long write_sysctl(const char *text)
{
	long fd = sys(nr_openat, at_fdcwd, ptr(sysctl_path), o_wronly);
	if (fd < 0)
		return fd;
	long result = sys(nr_write, fd, ptr(text), static_cast<long>(string_length(text)));
	sys(nr_close, fd);
	return result;
}

void test_sysctl()
{
	group = "sysctl";
	char buffer[64];
	check(read_sysctl(buffer, sizeof(buffer)) && same_string(buffer, "16\n"),
	      "abi.sve_default_vector_length reads 16");
	long written = write_sysctl("32\n");
	if (native && (written == -eacces || written == -erofs)) {
		Line().add("sve-probe: sysctl writes skipped: /proc/sys is not writable").print();
		return;
	}
	check(written == 3, "writing a valid length succeeds");
	check(read_sysctl(buffer, sizeof(buffer)) && same_string(buffer, "16\n"),
	      "and the default stays 16");
	check(write_sysctl("-1\n") == 3, "writing -1 succeeds");
	check(write_sysctl("17\n") == -einval, "writing 17 is EINVAL");
	check(read_sysctl(buffer, sizeof(buffer)) && same_string(buffer, "16\n"),
	      "the default is still 16");
}

/* ------------------------------------------------------------------ */
/* ptrace: NT_ARM_SVE at signal and syscall stops, and fork.           */

struct Iovec {
	void *base;
	u64 length;
};

struct RegsetHeader {
	u32 size;
	u32 max_size;
	u16 vl;
	u16 max_vl;
	u16 flags;
	u16 reserved;
};

alignas(16) u8 regset_buffer[64 * 1024];

long get_regset(long pid, long type, u64 length = sizeof(regset_buffer))
{
	memset(regset_buffer, 0, length);
	Iovec iov = {regset_buffer, length};
	long result = sys(nr_ptrace, ptrace_getregset, pid, type, ptr(&iov));
	return result < 0 ? result : static_cast<long>(iov.length);
}

long set_regset(long pid, long type, u64 length)
{
	Iovec iov = {regset_buffer, length};
	return sys(nr_ptrace, ptrace_setregset, pid, type, ptr(&iov));
}

RegsetHeader regset_header()
{
	RegsetHeader header;
	memcpy(&header, regset_buffer, sizeof(header));
	return header;
}

int wait_stop(long pid)
{
	int status = 0;
	if (sys(nr_wait4, pid, ptr(&status), 0, 0) != pid)
		return -1;
	return status;
}

bool stopped_with(int status, int signal)
{
	return (status & 0xff) == 0x7f && ((status >> 8) & 0xff) == signal;
}

/* Continues a tracee, passing it every signal it stops with, until it exits;
 * returns its wait status. */
int finish_tracee(long pid, int signal = 0)
{
	sys(nr_ptrace, ptrace_cont, pid, 0, signal);
	int status = 0;
	for (int round = 0; round < 16; round++) {
		status = wait_stop(pid);
		if ((status & 0xff) != 0x7f)
			break;
		sys(nr_ptrace, ptrace_cont, pid, 0, (status >> 8) & 0xff);
	}
	return status;
}

constexpr u32 traced_fpsr = 0x00000011;
constexpr u32 traced_fpcr = 0x00800000;

/* NT_ARM_SVE in SVE form: Z from `seed`'s V, P and FFR from its pattern. */
u64 fill_sve(u64 seed, u16 header_vl = vl)
{
	u64 size = sve_pt_size_sve(1);
	memset(regset_buffer, 0, size);
	RegsetHeader header = {static_cast<u32>(size), 0, header_vl, 0, sve_pt_regs_sve, 0};
	memcpy(regset_buffer, &header, sizeof(header));
	for (u64 reg = 0; reg < 32; reg++)
		for (u64 byte = 0; byte < 16; byte++)
			regset_buffer[sve_zreg_offset(1, reg) + byte] = v_byte(seed, reg, byte);
	Predicates value = pattern(seed);
	for (u64 reg = 0; reg < 16; reg++)
		store<u16>(regset_buffer + sve_preg_offset(1, reg), value.p[reg]);
	store<u16>(regset_buffer + sve_ffr_offset(1), value.ffr);
	store<u32>(regset_buffer + sve_pt_fpsr_offset(1), traced_fpsr);
	store<u32>(regset_buffer + sve_pt_fpsr_offset(1) + 4, traced_fpcr);
	return size;
}

/* NT_ARM_SVE in FPSIMD form: V from `seed`. */
u64 fill_fpsimd(u64 seed)
{
	memset(regset_buffer, 0, sve_pt_size_fpsimd);
	RegsetHeader header = {static_cast<u32>(sve_pt_size_fpsimd), 0,
			       static_cast<u16>(vl), 0, sve_pt_regs_fpsimd, 0};
	memcpy(regset_buffer, &header, sizeof(header));
	for (u64 reg = 0; reg < 32; reg++)
		for (u64 byte = 0; byte < 16; byte++)
			regset_buffer[16 + reg * 16 + byte] = v_byte(seed, reg, byte);
	store<u32>(regset_buffer + 16 + 512, traced_fpsr);
	store<u32>(regset_buffer + 16 + 516, traced_fpcr);
	return sve_pt_size_fpsimd;
}

/* The SVE form just read holds `seed`'s registers. */
bool regset_holds(u64 seed, u32 fpsr)
{
	RegsetHeader header = regset_header();
	if (header.size != sve_pt_size_sve(1) || (header.flags & 1) != sve_pt_regs_sve)
		return false;
	for (u64 reg = 0; reg < 32; reg++)
		for (u64 byte = 0; byte < 16; byte++)
			if (regset_buffer[sve_zreg_offset(1, reg) + byte] != v_byte(seed, reg, byte))
				return false;
	Predicates value = pattern(seed);
	for (u64 reg = 0; reg < 16; reg++)
		if (load<u16>(regset_buffer + sve_preg_offset(1, reg)) != value.p[reg])
			return false;
	return load<u16>(regset_buffer + sve_ffr_offset(1)) == value.ffr &&
		load<u32>(regset_buffer + sve_pt_fpsr_offset(1)) == fpsr;
}

/* The tracee stops in kill(), and then reports through a fault what the
 * tracer wrote. */
int stop_tracee(void *)
{
	sys(nr_ptrace, ptrace_traceme, 0, 0, 0);
	reset_captures();
	stop_fault_syscall(sys(nr_getpid));
	const Capture &capture = captures[0];
	return live_with(capture, pattern(21)) && vregs_are(capture, 21) &&
		capture.fpsr == traced_fpsr && capture.fpcr == traced_fpcr ? 0 : 1;
}

void test_stop_regsets()
{
	group = "ptrace-stop";
	long pid = clone_run(sigchld, nullptr, stop_tracee, nullptr);
	if (!stopped_with(wait_stop(pid), sigstop)) {
		check(false, "the tracee stops");
		return;
	}
	long length = get_regset(pid, nt_arm_sve);
	RegsetHeader header = regset_header();
	check(length >= static_cast<long>(sizeof(header)), "NT_ARM_SVE can be read");
	check_value(header.vl, vl, "its vl is 16");
	check_value(header.max_vl, vl, "its max_vl is 16");
	check_value(header.max_size, sve_pt_size_sve(1), "its max_size is 592");
	check_value(header.flags, sve_pt_regs_fpsimd,
		    "a task stopped in a system call is in FPSIMD form");
	check_value(header.size, sve_pt_size_fpsimd, "of 544 bytes");
	u8 fpsimd[fpsimd_size];
	memcpy(fpsimd, regset_buffer + 16, fpsimd_size);
	length = get_regset(pid, nt_prfpreg, fpsimd_size);
	check(length == static_cast<long>(fpsimd_size) &&
		      memcmp(fpsimd, regset_buffer, fpsimd_size) == 0,
	      "the FPSIMD form is NT_PRFPREG's state");
	get_regset(pid, nt_arm_sve, 16);
	check_value(regset_header().size, sve_pt_size_fpsimd,
		    "a 16-byte read gives the header alone");

	check_value(static_cast<u64>(set_regset(pid, nt_arm_sve, fill_sve(20))), 0,
		    "NT_ARM_SVE can be written in SVE form");
	get_regset(pid, nt_arm_sve);
	check(regset_holds(20, traced_fpsr), "it reads back the Z, P, FFR and FPSR written");
	get_regset(pid, nt_prfpreg, fpsimd_size);
	bool same = true;
	for (u64 reg = 0; reg < 32; reg++)
		for (u64 byte = 0; byte < 16; byte++)
			same = same && regset_buffer[reg * 16 + byte] == v_byte(20, reg, byte);
	check(same, "NT_PRFPREG's V are the Z written");

	check_value(static_cast<u64>(set_regset(pid, nt_arm_sve, fill_fpsimd(22))), 0,
		    "NT_ARM_SVE can be written in FPSIMD form");
	get_regset(pid, nt_arm_sve);
	header = regset_header();
	same = header.flags == sve_pt_regs_fpsimd && header.size == sve_pt_size_fpsimd;
	for (u64 reg = 0; reg < 32; reg++)
		for (u64 byte = 0; byte < 16; byte++)
			same = same && regset_buffer[16 + reg * 16 + byte] == v_byte(22, reg, byte);
	check(same, "the task is then in FPSIMD form with the V written");

	check_error(set_regset(pid, nt_arm_sve, fill_sve(23, 32)), eio,
		    "SVE data for VL 32 is EIO");
	fill_sve(23);
	store<u16>(regset_buffer + 12, sve_pt_regs_sve | sve_pt_vl_inherit);
	check_value(static_cast<u64>(set_regset(pid, nt_arm_sve, 16)), 0,
		    "a header alone with SVE_PT_VL_INHERIT can be written");
	get_regset(pid, nt_arm_sve);
	header = regset_header();
	same = header.flags == (sve_pt_regs_sve | sve_pt_vl_inherit) &&
		header.size == sve_pt_size_sve(1);
	/* The guest's Linux 6.18 zeroes Z, P, FFR, FPSR and FPCR. Linux 6.12,
	 * which supplied the native values, keeps V, FPSR and FPCR. */
	const u64 zeroed = native ? sve_preg_offset(1, 0) : sve_zreg_offset(1, 0);
	const u64 end = native ? sve_ffr_offset(1) + 2 : sve_pt_fpsr_offset(1) + 8;
	for (u64 byte = zeroed; byte < end; byte++)
		same = same && regset_buffer[byte] == 0;
	check(same, native ? "the task is then live with P and FFR zero and inherits" :
			     "the task is then live with every register zero and inherits");

	check_value(static_cast<u64>(set_regset(pid, nt_arm_sve, fill_sve(21))), 0,
		    "NT_ARM_SVE can be written again");
	int status = finish_tracee(pid);
	check(status == 0, "the tracee runs live with the registers written");
}

/* Live, the tracee faults: the tracer sees the state at the SIGSEGV stop. */
int fault_tracee(void *)
{
	sys(nr_ptrace, ptrace_traceme, 0, 0, 0);
	sys(nr_kill, sys(nr_getpid), sigstop);
	reset_captures();
	sigreturn_into(live_frame(24), continue_fault);
	return capture_count == 1 && live_with(captures[0], pattern(24)) &&
		vregs_are(captures[0], 24) ? 0 : 1;
}

void test_fault_stop()
{
	group = "ptrace-fault";
	long pid = clone_run(sigchld, nullptr, fault_tracee, nullptr);
	if (!stopped_with(wait_stop(pid), sigstop)) {
		check(false, "the tracee stops");
		return;
	}
	sys(nr_ptrace, ptrace_cont, pid, 0, 0);
	if (!stopped_with(wait_stop(pid), sigsegv)) {
		check(false, "the tracee stops at its fault");
		return;
	}
	get_regset(pid, nt_arm_sve);
	check(regset_holds(24, 0), "NT_ARM_SVE at a fault holds the live Z, P and FFR");
	check(finish_tracee(pid, sigsegv) == 0, "the tracee's own frame holds them too");
}

/* The tracer writes NT_ARM_SVE at the entry of a getpid() made live. */
int syscall_tracee(void *)
{
	sys(nr_ptrace, ptrace_traceme, 0, 0, 0);
	sys(nr_kill, sys(nr_getpid), sigstop);
	sigreturn_into(live_frame(25), continue_syscall);
	Capture &capture = capture_now();
	return live_with(capture, pattern(26)) && vregs_are(capture, 26) ? 0 : 1;
}

/* Runs the tracee to the entry of system call `number` and returns true
 * there, or false if it exits first. */
bool run_to_entry(long pid, u64 number)
{
	sys(nr_ptrace, ptrace_setoptions, pid, 0, ptrace_o_tracesysgood);
	sys(nr_ptrace, ptrace_syscall, pid, 0, 0);
	bool entry = true;
	for (int round = 0; round < 64; round++) {
		int status = wait_stop(pid);
		if ((status & 0xff) != 0x7f)
			return false;
		int signal = (status >> 8) & 0xff;
		if (signal != (sigtrap | 0x80)) {
			sys(nr_ptrace, ptrace_syscall, pid, 0, signal == sigstop ? 0 : signal);
			continue;
		}
		get_regset(pid, nt_prstatus, 34 * 8);
		if (entry && load<u64>(regset_buffer + 8 * 8) == number)
			return true;
		entry = !entry;
		sys(nr_ptrace, ptrace_syscall, pid, 0, 0);
	}
	return false;
}

void test_syscall_stop()
{
	group = "ptrace-syscall";
	long pid = clone_run(sigchld, nullptr, syscall_tracee, nullptr);
	if (!stopped_with(wait_stop(pid), sigstop) || !run_to_entry(pid, nr_getpid)) {
		check(false, "the tracee stops at the getpid entry");
		return;
	}
	get_regset(pid, nt_arm_sve);
	RegsetHeader header = regset_header();
	check(header.flags == sve_pt_regs_fpsimd && header.size == sve_pt_size_fpsimd,
	      "at syscall entry the live state is discarded");
	check_value(static_cast<u64>(set_regset(pid, nt_arm_sve, fill_sve(26))), 0,
		    "NT_ARM_SVE can be written at syscall entry");
	check(finish_tracee(pid) == 0, "the state written survives the system call");
}

/* The tracer writes NT_ARM_SVE at the entry of the tracee's fork: the parent
 * returns live with it, and the child starts not live. */
int fork_grandchild(void *)
{
	return header_only(capture_now()) ? 0 : 1;
}

int fork_tracee(void *)
{
	sys(nr_ptrace, ptrace_traceme, 0, 0, 0);
	sys(nr_kill, sys(nr_getpid), sigstop);
	long pid = clone_run(sigchld, nullptr, fork_grandchild, nullptr);
	bool parent = live_with(capture_now(), pattern(27));
	int status = -1;
	sys(nr_wait4, pid, ptr(&status), 0, 0);
	return (parent ? 0 : 1) | (status == 0 ? 0 : 2);
}

void test_fork_stop()
{
	group = "ptrace-fork";
	long pid = clone_run(sigchld, nullptr, fork_tracee, nullptr);
	if (!stopped_with(wait_stop(pid), sigstop) || !run_to_entry(pid, nr_clone)) {
		check(false, "the tracee stops at the clone entry");
		return;
	}
	check_value(static_cast<u64>(set_regset(pid, nt_arm_sve, fill_sve(27))), 0,
		    "NT_ARM_SVE can be written at clone entry");
	int status = finish_tracee(pid);
	Line name;
	name.add("the parent returns live with it and the child starts not live (status ")
		.hex(static_cast<u64>(status)).add(")");
	check(status == 0, name.text);
}

/* ------------------------------------------------------------------ */
/* With SME: streaming mode and the non-streaming state.               */

u8 *streaming_frame()
{
	FrameBuilder builder;
	builder.fpsimd(30);
	builder.sve_streaming();
	return builder.finish();
}

int streaming_tracee(void *)
{
	sys(nr_ptrace, ptrace_traceme, 0, 0, 0);
	sys(nr_kill, sys(nr_getpid), sigstop);
	reset_captures();
	sigreturn_into(streaming_frame(), continue_fault_then_syscall);
	return capture_count == 1 && live_with(captures[0], pattern(31)) ? 0 : 1;
}

void test_streaming()
{
	group = "streaming";
	reset_captures();
	nested_request = 1;
	sigreturn_into(live_frame(29), continue_return);
	sigreturn_into(streaming_frame(), continue_fault_then_syscall);
	check(capture_count == 2 && captures[0].sve_flags == sve_sig_flag_sm &&
		      captures[0].sve_vl == svl,
	      "a streaming task's frame holds the streaming registers");
	check(capture_count == 2 && header_only(captures[1]),
	      "its handler starts outside streaming mode with no live SVE state");

	long pid = clone_run(sigchld, nullptr, streaming_tracee, nullptr);
	if (!stopped_with(wait_stop(pid), sigstop)) {
		check(false, "the tracee stops");
		return;
	}
	sys(nr_ptrace, ptrace_cont, pid, 0, 0);
	if (!stopped_with(wait_stop(pid), sigsegv)) {
		check(false, "the tracee stops at its fault in streaming mode");
		return;
	}
	get_regset(pid, nt_arm_sve);
	RegsetHeader header = regset_header();
	check(header.size == 16 && header.flags == sve_pt_regs_fpsimd && header.vl == vl,
	      "NT_ARM_SVE in streaming mode is a header with VL 16");
	get_regset(pid, nt_arm_ssve);
	check_value(regset_header().flags & 1, sve_pt_regs_sve,
		    "NT_ARM_SSVE holds the streaming registers");
	check_value(static_cast<u64>(set_regset(pid, nt_arm_sve, fill_sve(31))), 0,
		    "NT_ARM_SVE can be written in streaming mode");
	get_regset(pid, nt_arm_ssve);
	check(regset_header().size == 16, "the tracee has left streaming mode");
	check(finish_tracee(pid, sigsegv) == 0, "and runs live with the state written");
}

/* ------------------------------------------------------------------ */
/* Off mode: a run that does not implement SVE.                        */

int off_tracee(void *)
{
	sys(nr_ptrace, ptrace_traceme, 0, 0, 0);
	sys(nr_kill, sys(nr_getpid), sigstop);
	return 0;
}

void test_off()
{
	group = "off";
	check((auxv_hwcap & hwcap_sve) == 0, "AT_HWCAP has no HWCAP_SVE");
	check((auxv_hwcap2 & hwcap2_sve_bits) == 0, "AT_HWCAP2 has no SVE bit");
	check_error(sys(nr_prctl, pr_sve_get_vl), einval, "PR_SVE_GET_VL fails with EINVAL");
	check_error(sys(nr_prctl, pr_sve_set_vl, 16), einval, "PR_SVE_SET_VL fails with EINVAL");
	check_error(sys(nr_openat, at_fdcwd, ptr(sysctl_path), 0), enoent,
		    "abi.sve_default_vector_length does not exist");
	if ((auxv_hwcap2 & hwcap2_sme) == 0) {
		check_value(auxv_minsigstksz, 0, "the auxiliary vector has no AT_MINSIGSTKSZ");
		const Capture &capture = capture_now();
		check(capture.valid && capture.error == nullptr && capture.record_count == 1 &&
			      capture.magics[0] == fpsimd_magic,
		      "a signal frame holds fpsimd_context alone");
	}
	ill_code = 0;
	sve_ptrue();
	check_value(static_cast<u64>(ill_code), ill_illopc,
		    "PTRUE is an undefined instruction");
	long pid = clone_run(sigchld, nullptr, off_tracee, nullptr);
	if (!stopped_with(wait_stop(pid), sigstop)) {
		check(false, "the tracee stops");
		return;
	}
	check_error(get_regset(pid, nt_arm_sve), einval, "NT_ARM_SVE does not exist");
	sys(nr_ptrace, ptrace_cont, pid, 0, 0);
	check(wait_stop(pid) == 0, "the tracee exits");
}

/* ------------------------------------------------------------------ */

void setup_console()
{
	if (sys(nr_write, 1, ptr(""), 0) != -ebadf)
		return;
	sys(nr_mkdirat, at_fdcwd, ptr("/dev"), 0755);
	sys(nr_mount, ptr("devtmpfs"), ptr("/dev"), ptr("devtmpfs"), 0, 0);
	for (int fd = 0; fd < 3; fd++)
		sys(nr_openat, at_fdcwd, ptr("/dev/console"), o_rdwr);
}

void mount_proc()
{
	sys(nr_mkdirat, at_fdcwd, ptr("/proc"), 0555);
	sys(nr_mount, ptr("proc"), ptr("/proc"), ptr("proc"), 0, 0);
}

const char *find_argument(char **argv, int index)
{
	for (int position = 0; position <= index; position++)
		if (argv[position] == nullptr)
			return nullptr;
	return argv[index];
}

/* Only the guest's /init powers off; natively the probe just exits. */
[[noreturn]] void finish()
{
	if (native)
		for (;;)
			sys(nr_exit_group, failures == 0 ? 0 : 1);
	for (;;) {
		sys(nr_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0);
		struct {
			long seconds, nanoseconds;
		} delay = {1, 0};
		sys(nr_nanosleep, ptr(&delay), 0);
	}
}

const char *known_mode(const char *name)
{
	static const char *const modes[] = {"kernel", "off"};
	for (const char *mode : modes)
		if (same_string(name, mode))
			return mode;
	return "unknown";
}

/* In the guest the mode comes from SVE_PROBE_MODE=<mode> on the kernel
 * command line, which the kernel passes to init as an environment variable. */
const char *find_mode(char **envp)
{
	static const char prefix[] = "SVE_PROBE_MODE=";
	for (; *envp != nullptr; envp++) {
		const char *entry = *envp;
		u64 index = 0;
		while (prefix[index] != '\0' && entry[index] == prefix[index])
			index++;
		if (prefix[index] == '\0')
			return known_mode(entry + index);
	}
	return "kernel";
}

} // namespace

extern "C" [[noreturn]] void probe_main(u64 *initial)
{
	int argc = static_cast<int>(initial[0]);
	char **argv = reinterpret_cast<char **>(initial + 1);
	char **envp = argv + argc + 1;
	u64 *auxv = initial + 1 + argc + 1;
	while (*auxv != 0)
		auxv++;
	auxv++;
	for (; auxv[0] != 0; auxv += 2) {
		if (auxv[0] == at_hwcap)
			auxv_hwcap = auxv[1];
		else if (auxv[0] == at_hwcap2)
			auxv_hwcap2 = auxv[1];
		else if (auxv[0] == at_minsigstksz)
			auxv_minsigstksz = auxv[1];
	}
	if (argc > 0 && argv[0] != nullptr)
		self_path = argv[0];
	const char *first = find_argument(argv, 1);
	if (first != nullptr && same_string(first, "exec-check"))
		exec_check(find_argument(argv, 2) != nullptr ? argv[2] : "plain");

	native = sys(nr_getpid) != 1;
	const char *mode;
	if (native) {
		mode = first != nullptr ? known_mode(first) : "kernel";
	} else {
		setup_console();
		mount_proc();
		mode = find_mode(envp);
	}
	install_handlers();
	Line().add("sve-probe: mode ").add(mode).add(native ? " (native)" : "").print();
	if (same_string(mode, "off")) {
		test_off();
	} else if (same_string(mode, "kernel")) {
		test_parameters();
		if ((auxv_hwcap & hwcap_sve) != 0 && sys(nr_prctl, pr_sve_get_vl) == static_cast<long>(vl)) {
			test_frames();
			test_sigreturn();
			test_invalid_frames();
			test_switches();
			test_task_creation();
			test_prctl();
			test_sysctl();
			test_stop_regsets();
			test_fault_stop();
			test_syscall_stop();
			test_fork_stop();
			if (sme)
				test_streaming();
		}
	} else {
		check(false, "SVE_PROBE_MODE names a mode");
	}
	Line().add("sve-probe: ").add(mode).add(": ").number(failures).add(" failure(s) in ")
		.number(checks).add(" check(s)").print();
	finish();
}

__asm__(R"(
	.text
	.p2align 2
	.globl _start
_start:
	mov	x0, sp
	bl	probe_main
	brk	#0x50
)");
