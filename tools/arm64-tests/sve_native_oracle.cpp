// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native oracle for the non-streaming SVE corpus cases that
// tools/arm64-tests/sve-native-corpus.py authors. It runs each case of a
// request file on this host's own CPU, outside streaming mode, and writes
// what the hardware did: the terminal event, the PC, the fault address and
// syndrome, the general and SIMD&FP registers, P0-P15, FFR, whether the SVE
// state is live, the size of the signal frame's sve_context, and the bytes
// of every data region. sve-native-corpus.py owns the file formats; its
// module docstring describes them.
//
// The records come from an arm64 Linux host with FEAT_SVE at a 16-byte
// vector length (arm64-builder's Neoverse V2). There every register a case can
// name, Z (which is V at that length), P, and FFR included, is in the signal
// frame, so a case runs on the calling thread with signal handlers on an
// alternate stack:
//
//   oracle_enter   saves the caller's registers and stops at BRK #0x10;
//   the handler    writes the case's X, SP, PC, NZCV, V, FPCR, and FPSR
//                  into the signal frame, and makes the frame's sve_context
//                  say whether the SVE state is live: with Z, P, and FFR
//                  for a case whose state is live, as a bare header for one
//                  whose state is not. The return from the signal sets or
//                  clears TIF_SVE from that record (restore_sigframe), so
//                  the program starts in exactly the case's state, whatever
//                  the kernel did with the state at earlier system calls;
//   the program    runs until it traps; its terminal SVC is replaced with
//                  BRK #7, so a completed case also traps, before any
//                  system call could discard its SVE state;
//   the handler    reads the state from that frame, and returns to
//                  oracle_restore, which returns to the caller.
//
// The same tool builds on an arm64 Mac, without SVE: there it runs requests
// without SVE state, through the Darwin signal frame, which the
// arm64-sve-native-oracle tooling test uses to check the request, region,
// and response paths with base A64 cases.

#if !defined(__aarch64__) && !defined(__arm64__)
#error "sve_native_oracle runs only on arm64 hosts"
#endif

#if defined(__linux__)
#include <asm/hwcap.h>
#include <asm/sigcontext.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <ucontext.h>
#elif defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/ucontext.h>
#else
#error "sve_native_oracle supports arm64 Linux and Darwin"
#endif

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t request_version = 1;
constexpr std::uint32_t response_version = 1;
constexpr std::size_t code_bytes = 16384;
constexpr std::uint32_t sentinel = 0xd42000e0;  // BRK #7, in place of SVC #0
constexpr std::uint32_t supervisor_call = 0xd4000001;
constexpr std::uint32_t fill_word = 0xd4200240;  // BRK #0x12
constexpr std::uint32_t sve_vl = 16;              // the one VL the corpus has
constexpr std::uint32_t p_bytes = sve_vl / 8;

// Terminal events, numbered as the corpus pack numbers them.
enum Event : std::uint32_t {
    system_call = 0,
    memory_fault = 1,
    illegal_instruction = 3,
    breakpoint = 4,
    floating_point_trap = 5,
    pc_alignment = 6,
    fatal = 8,
};

// Region flags.
constexpr std::uint8_t region_readable = 1;
constexpr std::uint8_t region_writable = 2;
constexpr std::uint8_t region_explicit = 0x80;

// The template's view of the caller; the offsets are fixed by the assembly
// below.
struct Context {
    std::uint64_t saved[22];  // x19-x30, sp, d8-d15, fpcr
};
static_assert(sizeof(Context) == 176);

}  // namespace

extern "C" {
void oracle_enter(Context *context);
extern char oracle_ready[], oracle_restore[];
}

#if defined(__APPLE__)
#define UML_ORACLE_SYMBOL(name) "_" #name
#define UML_ORACLE_TYPE(name)
#else
#define UML_ORACLE_SYMBOL(name) #name
#define UML_ORACLE_TYPE(name) ".type " #name ", %function\n"
#endif

// x0 is the Context throughout.
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl " UML_ORACLE_SYMBOL(oracle_enter) "\n"
    UML_ORACLE_TYPE(oracle_enter)
    UML_ORACLE_SYMBOL(oracle_enter) ":\n"
    "\tstp x19, x20, [x0, #0]\n\tstp x21, x22, [x0, #16]\n"
    "\tstp x23, x24, [x0, #32]\n\tstp x25, x26, [x0, #48]\n"
    "\tstp x27, x28, [x0, #64]\n\tstp x29, x30, [x0, #80]\n"
    "\tmov x1, sp\n\tstr x1, [x0, #96]\n"
    "\tstp d8, d9, [x0, #104]\n\tstp d10, d11, [x0, #120]\n"
    "\tstp d12, d13, [x0, #136]\n\tstp d14, d15, [x0, #152]\n"
    "\tmrs x1, fpcr\n\tstr x1, [x0, #168]\n"
    ".globl " UML_ORACLE_SYMBOL(oracle_ready) "\n"
    UML_ORACLE_SYMBOL(oracle_ready) ":\n"
    "\tbrk #0x10\n"
    "\tbrk #0x10\n"
    ".globl " UML_ORACLE_SYMBOL(oracle_restore) "\n"
    UML_ORACLE_SYMBOL(oracle_restore) ":\n"
    "\tldr x1, [x0, #168]\n\tmsr fpcr, x1\n"
    "\tmsr fpsr, xzr\n"
    "\tldp x19, x20, [x0, #0]\n\tldp x21, x22, [x0, #16]\n"
    "\tldp x23, x24, [x0, #32]\n\tldp x25, x26, [x0, #48]\n"
    "\tldp x27, x28, [x0, #64]\n\tldp x29, x30, [x0, #80]\n"
    "\tldr x1, [x0, #96]\n\tmov sp, x1\n"
    "\tldp d8, d9, [x0, #104]\n\tldp d10, d11, [x0, #120]\n"
    "\tldp d12, d13, [x0, #136]\n\tldp d14, d15, [x0, #152]\n"
    "\tret\n");

namespace {

[[noreturn]] void fail(const std::string &message)
{
    std::fprintf(stderr, "sve_native_oracle: %s\n", message.c_str());
    std::exit(1);
}

// Async-signal-safe: a fault the oracle cannot attribute.
[[noreturn]] void internal(const char *what, std::uint64_t pc)
{
    char text[128];
    const int length = std::snprintf(text, sizeof(text),
                                     "sve_native_oracle: %s at pc %#llx\n",
                                     what,
                                     static_cast<unsigned long long>(pc));
    if (length > 0)
        (void)!write(2, text, static_cast<std::size_t>(length));
    _exit(4);
}

// Base fills, as the corpus pack defines them (tests/unit/arm64/corpus/
// README.md), so a request names only the registers a case sets.
std::uint64_t base_x(unsigned index)
{
    return 0x5eed000000000000ULL | static_cast<std::uint64_t>(index) << 40 |
           0x01010101ULL * (index + 1);
}

std::uint8_t base_v(unsigned index, unsigned byte)
{
    return static_cast<std::uint8_t>(((16 * index + byte) * 0x9d + 0x3b) & 0xff);
}

std::uint8_t region_fill(std::uint32_t seed, std::uint32_t offset)
{
    return static_cast<std::uint8_t>(
        ((seed * 0x01000193U + offset) * 0x9e3779b1U) >> 24);
}

struct State {
    std::array<std::uint64_t, 32> x{};  // X0-X30, then SP
    std::uint32_t nzcv = 0;
    std::uint32_t fpcr = 0;
    std::uint32_t fpsr = 0;
    std::uint8_t live = 0;
    std::array<std::array<std::uint8_t, 16>, 32> v{};
    std::array<std::array<std::uint8_t, p_bytes>, 16> p{};
    std::array<std::uint8_t, p_bytes> ffr{};

    void base()
    {
        for (unsigned i = 0; i < 32; ++i)
            x[i] = base_x(i);
        for (unsigned i = 0; i < 32; ++i)
            for (unsigned b = 0; b < 16; ++b)
                v[i][b] = base_v(i, b);
    }
};

class Reader {
public:
    explicit Reader(std::vector<std::uint8_t> data) : data_(std::move(data)) {}

    const std::uint8_t *bytes(std::size_t count)
    {
        if (count > data_.size() - offset_)
            fail("truncated request");
        const std::uint8_t *result = data_.data() + offset_;
        offset_ += count;
        return result;
    }

    template <typename T> T get()
    {
        T value;
        std::memcpy(&value, bytes(sizeof(T)), sizeof(T));
        return value;
    }

    void copy(std::uint8_t *target, std::size_t count)
    {
        std::memcpy(target, bytes(count), count);
    }

    bool done() const { return offset_ == data_.size(); }

private:
    std::vector<std::uint8_t> data_;
    std::size_t offset_ = 0;
};

class Writer {
public:
    template <typename T> void put(T value)
    {
        const auto *raw = reinterpret_cast<const std::uint8_t *>(&value);
        data_.insert(data_.end(), raw, raw + sizeof(T));
    }

    void bytes(const std::uint8_t *source, std::size_t count)
    {
        data_.insert(data_.end(), source, source + count);
    }

    const std::vector<std::uint8_t> &data() const { return data_; }

private:
    std::vector<std::uint8_t> data_;
};

// A state is encoded against a reference: masks select the X, V, and P
// entries that differ, and the scalar fields are always present. The SVE
// part is present when the request has a VL.
void read_state(Reader &reader, State &state, bool sve)
{
    const auto x_mask = reader.get<std::uint32_t>();
    for (unsigned i = 0; i < 32; ++i)
        if (x_mask >> i & 1)
            state.x[i] = reader.get<std::uint64_t>();
    state.nzcv = reader.get<std::uint32_t>();
    state.fpcr = reader.get<std::uint32_t>();
    state.fpsr = reader.get<std::uint32_t>();
    const auto v_mask = reader.get<std::uint32_t>();
    for (unsigned i = 0; i < 32; ++i)
        if (v_mask >> i & 1)
            reader.copy(state.v[i].data(), 16);
    if (!sve)
        return;
    state.live = reader.get<std::uint8_t>();
    const auto p_mask = reader.get<std::uint16_t>();
    for (unsigned i = 0; i < 16; ++i)
        if (p_mask >> i & 1)
            reader.copy(state.p[i].data(), p_bytes);
    if (reader.get<std::uint8_t>())
        reader.copy(state.ffr.data(), p_bytes);
    if (state.live > 1)
        fail("bad live flag");
    bool any = false;
    for (const auto &p : state.p)
        for (const auto byte : p)
            any |= byte != 0;
    for (const auto byte : state.ffr)
        any |= byte != 0;
    if (!state.live && any)
        fail("P or FFR in a case whose SVE state is not live");
}

void write_state(Writer &writer, const State &state, const State &reference,
                 bool sve)
{
    std::uint32_t x_mask = 0;
    for (unsigned i = 0; i < 32; ++i)
        if (state.x[i] != reference.x[i])
            x_mask |= 1U << i;
    writer.put(x_mask);
    for (unsigned i = 0; i < 32; ++i)
        if (x_mask >> i & 1)
            writer.put(state.x[i]);
    writer.put(state.nzcv);
    writer.put(state.fpcr);
    writer.put(state.fpsr);
    std::uint32_t v_mask = 0;
    for (unsigned i = 0; i < 32; ++i)
        if (state.v[i] != reference.v[i])
            v_mask |= 1U << i;
    writer.put(v_mask);
    for (unsigned i = 0; i < 32; ++i)
        if (v_mask >> i & 1)
            writer.bytes(state.v[i].data(), 16);
    if (!sve)
        return;
    writer.put(state.live);
    std::uint16_t p_mask = 0;
    for (unsigned i = 0; i < 16; ++i)
        if (state.p[i] != reference.p[i])
            p_mask = static_cast<std::uint16_t>(p_mask | 1U << i);
    writer.put(p_mask);
    for (unsigned i = 0; i < 16; ++i)
        if (p_mask >> i & 1)
            writer.bytes(state.p[i].data(), p_bytes);
    const bool ffr = state.ffr != reference.ffr;
    writer.put<std::uint8_t>(ffr);
    if (ffr)
        writer.bytes(state.ffr.data(), p_bytes);
}

struct Region {
    std::uint64_t address = 0;
    std::uint32_t size = 0;
    std::uint8_t flags = 0;
    std::vector<std::uint8_t> initial;
};

struct Case {
    std::uint32_t entry = 0;
    std::vector<std::uint32_t> words;
    State initial;
    std::vector<Region> regions;
};

struct Host {
    std::uint32_t vl = 0;  // 0 without FEAT_SVE
    std::uint32_t page = 0;
    std::uint64_t hwcap = 0;
    std::uint64_t hwcap2 = 0;
};

Host probe_host()
{
    Host host;
    host.page = static_cast<std::uint32_t>(sysconf(_SC_PAGESIZE));
#if defined(__linux__)
    host.hwcap = getauxval(AT_HWCAP);
    host.hwcap2 = getauxval(AT_HWCAP2);
    if (host.hwcap & HWCAP_SVE) {
        const int vl = prctl(PR_SVE_GET_VL);
        if (vl < 0)
            fail("cannot read the SVE vector length");
        host.vl = static_cast<std::uint32_t>(vl & PR_SVE_VL_LEN_MASK);
    }
#endif
    return host;
}

// The signal frame of a trap, as this host lays it out.
class Frame {
public:
#if defined(__linux__)
    explicit Frame(void *raw) : context_(static_cast<ucontext_t *>(raw))
    {
        auto *head = reinterpret_cast<_aarch64_ctx *>(
            context_->uc_mcontext.__reserved);
        const auto *end = reinterpret_cast<const char *>(head) +
                          sizeof(context_->uc_mcontext.__reserved);
        while (reinterpret_cast<const char *>(head) + sizeof(*head) <= end &&
               head->magic != 0) {
            if (head->magic == FPSIMD_MAGIC)
                fpsimd_ = reinterpret_cast<fpsimd_context *>(head);
            else if (head->magic == ESR_MAGIC)
                esr_ = reinterpret_cast<esr_context *>(head);
            else if (head->magic == SVE_MAGIC)
                sve_ = reinterpret_cast<sve_context *>(head);
            else if (head->magic == EXTRA_MAGIC)
                internal("signal frame overflows __reserved", pc());
            if (head->size == 0)
                break;
            head = reinterpret_cast<_aarch64_ctx *>(
                reinterpret_cast<char *>(head) + head->size);
        }
        if (fpsimd_ == nullptr)
            internal("signal frame without FP state", pc());
    }

    std::uint64_t x(unsigned i) const
    {
        return i < 31 ? context_->uc_mcontext.regs[i] : context_->uc_mcontext.sp;
    }
    void set_x(unsigned i, std::uint64_t value)
    {
        (i < 31 ? context_->uc_mcontext.regs[i] : context_->uc_mcontext.sp) =
            value;
    }
    std::uint64_t pc() const { return context_->uc_mcontext.pc; }
    void set_pc(std::uint64_t value) { context_->uc_mcontext.pc = value; }
    std::uint64_t pstate() const { return context_->uc_mcontext.pstate; }
    void set_pstate(std::uint64_t value) { context_->uc_mcontext.pstate = value; }
    std::uint8_t *v(unsigned i)
    {
        return reinterpret_cast<std::uint8_t *>(&fpsimd_->vregs[i]);
    }
    std::uint32_t fpsr() const { return fpsimd_->fpsr; }
    std::uint32_t fpcr() const { return fpsimd_->fpcr; }
    void set_fp_controls(std::uint32_t fpsr, std::uint32_t fpcr)
    {
        fpsimd_->fpsr = fpsr;
        fpsimd_->fpcr = fpcr;
    }
    std::uint32_t esr() const { return esr_ != nullptr ? esr_->esr : 0U; }
    std::uint32_t sve_size() const { return sve_ != nullptr ? sve_->head.size : 0U; }
    // Whether the frame holds the SVE registers: the task's SVE state was
    // live when the signal arrived.
    bool live() const
    {
        return sve_ != nullptr && sve_->vl == sve_vl &&
               sve_->head.size >= SVE_SIG_CONTEXT_SIZE(sve_vq_from_vl(sve_vl));
    }
    std::uint8_t *z(unsigned i)
    {
        return sve_bytes(SVE_SIG_ZREG_OFFSET(sve_vq_from_vl(sve_vl), i));
    }
    std::uint8_t *p(unsigned i)
    {
        return sve_bytes(SVE_SIG_PREG_OFFSET(sve_vq_from_vl(sve_vl), i));
    }
    std::uint8_t *ffr()
    {
        return sve_bytes(SVE_SIG_FFR_OFFSET(sve_vq_from_vl(sve_vl)));
    }
    // Make the frame's sve_context the record of a live SVE state, with
    // room for Z, P, and FFR, or a bare header. The record is the frame's
    // last before its terminator, which moves with it; __reserved has room
    // for either form.
    void set_live(bool live)
    {
        if (sve_ == nullptr || sve_->vl != sve_vl || sve_->flags != 0)
            internal("signal frame without a usable sve_context", pc());
        auto *after = reinterpret_cast<_aarch64_ctx *>(
            sve_bytes(sve_->head.size));
        if (after->magic != 0 || after->size != 0)
            internal("a record follows the sve_context", pc());
        const std::uint32_t size =
            live ? (SVE_SIG_CONTEXT_SIZE(sve_vq_from_vl(sve_vl)) + 15U) & ~15U
                 : static_cast<std::uint32_t>(sizeof(sve_context));
        const auto *end = reinterpret_cast<const std::uint8_t *>(
                              context_->uc_mcontext.__reserved) +
                          sizeof(context_->uc_mcontext.__reserved);
        if (sve_bytes(size) + sizeof(_aarch64_ctx) > end)
            internal("no room for the sve_context", pc());
        sve_->head.size = size;
        after = reinterpret_cast<_aarch64_ctx *>(sve_bytes(size));
        after->magic = 0;
        after->size = 0;
    }

private:
    std::uint8_t *sve_bytes(std::size_t offset) const
    {
        return reinterpret_cast<std::uint8_t *>(sve_) + offset;
    }

    ucontext_t *context_;
    fpsimd_context *fpsimd_ = nullptr;
    esr_context *esr_ = nullptr;
    sve_context *sve_ = nullptr;
#else
    explicit Frame(void *raw)
        : machine_(static_cast<ucontext_t *>(raw)->uc_mcontext)
    {
    }

    std::uint64_t x(unsigned i) const
    {
        const auto &gpr = machine_->__ss;
        return i < 29 ? gpr.__x[i] : i == 29 ? gpr.__fp : i == 30 ? gpr.__lr
                                                                  : gpr.__sp;
    }
    void set_x(unsigned i, std::uint64_t value)
    {
        auto &gpr = machine_->__ss;
        (i < 29 ? gpr.__x[i] : i == 29 ? gpr.__fp : i == 30 ? gpr.__lr
                                                            : gpr.__sp) = value;
    }
    std::uint64_t pc() const { return machine_->__ss.__pc; }
    void set_pc(std::uint64_t value) { machine_->__ss.__pc = value; }
    std::uint64_t pstate() const { return machine_->__ss.__cpsr; }
    void set_pstate(std::uint64_t value)
    {
        machine_->__ss.__cpsr = static_cast<std::uint32_t>(value);
    }
    std::uint8_t *v(unsigned i)
    {
        return reinterpret_cast<std::uint8_t *>(&machine_->__ns.__v[i]);
    }
    std::uint32_t fpsr() const { return machine_->__ns.__fpsr; }
    std::uint32_t fpcr() const { return machine_->__ns.__fpcr; }
    void set_fp_controls(std::uint32_t fpsr, std::uint32_t fpcr)
    {
        machine_->__ns.__fpsr = fpsr;
        machine_->__ns.__fpcr = fpcr;
    }
    std::uint32_t esr() const { return machine_->__es.__esr; }
    std::uint32_t sve_size() const { return 0; }
    bool live() const { return false; }
    std::uint8_t *z(unsigned) { return nullptr; }
    std::uint8_t *p(unsigned) { return nullptr; }
    std::uint8_t *ffr() { return nullptr; }
    void set_live(bool live)
    {
        if (live)
            internal("a live SVE state on a host without SVE", pc());
    }

private:
    mcontext_t machine_;
#endif
};

// What the handler hands over between the stages of a case.
enum class Stage { ready, run, done };

struct Run {
    Context context{};
    const Case *current = nullptr;
    std::uint32_t *code = nullptr;
    volatile Stage stage = Stage::done;
    // Results
    std::uint32_t event = fatal;
    std::uint32_t pc_offset = 0;
    std::uint64_t far = 0;
    std::uint32_t esr = 0;
    std::uint32_t sve_size = 0;
    State final_state;
};

Run run;

constexpr std::uint64_t nzcv_bits = 0xf0000000ULL;
// PSTATE.BTYPE: the handler returns to oracle_restore with it clear.
constexpr std::uint64_t btype_bits = 0xc00ULL;

void install(Frame &frame, const State &state, std::uint64_t entry)
{
    for (unsigned i = 0; i < 32; ++i)
        frame.set_x(i, state.x[i]);
    frame.set_pc(entry);
    frame.set_pstate((frame.pstate() & ~nzcv_bits & ~btype_bits) |
                     (state.nzcv & nzcv_bits));
    for (unsigned i = 0; i < 32; ++i)
        std::memcpy(frame.v(i), state.v[i].data(), 16);
    frame.set_fp_controls(state.fpsr, state.fpcr);
    frame.set_live(state.live != 0);
    if (!state.live)
        return;
    // A live frame's Z registers are the V registers at this VL; the
    // return from the signal takes Z, P, and FFR from it.
    for (unsigned i = 0; i < 32; ++i)
        std::memcpy(frame.z(i), state.v[i].data(), 16);
    for (unsigned i = 0; i < 16; ++i)
        std::memcpy(frame.p(i), state.p[i].data(), p_bytes);
    std::memcpy(frame.ffr(), state.ffr.data(), p_bytes);
}

void capture(Frame &frame, int signo, const siginfo_t *info)
{
    const std::uint64_t pc = frame.pc();
    const auto code = reinterpret_cast<std::uint64_t>(run.code);
    const bool inside = pc >= code && pc < code + code_bytes;
    run.pc_offset = inside ? static_cast<std::uint32_t>(pc - code) : 0xffffffffU;
    run.esr = frame.esr();
    run.sve_size = frame.sve_size();
    run.far = 0;
    const std::uint32_t ec = run.esr >> 26;
    if (!inside)
        run.event = fatal;
    else if (signo == SIGTRAP)
        run.event = run.code[(pc - code) / 4] == sentinel ? system_call
                                                           : breakpoint;
    else if (signo == SIGILL)
        run.event = illegal_instruction;
    else if (signo == SIGBUS && ec == 0x22)
        run.event = pc_alignment;
    else if (signo == SIGSEGV || signo == SIGBUS)
        run.event = memory_fault;
    else if (signo == SIGFPE)
        run.event = floating_point_trap;
    else
        run.event = fatal;
    if (run.event == memory_fault)
        run.far = reinterpret_cast<std::uint64_t>(info->si_addr);
    State &state = run.final_state;
    for (unsigned i = 0; i < 32; ++i)
        state.x[i] = frame.x(i);
    state.nzcv = static_cast<std::uint32_t>(frame.pstate() & nzcv_bits);
    state.fpsr = frame.fpsr();
    state.fpcr = frame.fpcr();
    for (unsigned i = 0; i < 32; ++i)
        std::memcpy(state.v[i].data(), frame.v(i), 16);
    state.live = frame.live() ? 1 : 0;
    state.p = {};
    state.ffr = {};
    if (state.live) {
        for (unsigned i = 0; i < 16; ++i)
            std::memcpy(state.p[i].data(), frame.p(i), p_bytes);
        std::memcpy(state.ffr.data(), frame.ffr(), p_bytes);
        // Z is V at this VL; a frame that disagrees is not one this
        // oracle can describe.
        for (unsigned i = 0; i < 32; ++i)
            if (std::memcmp(frame.z(i), state.v[i].data(), 16) != 0)
                run.event = fatal;
    }
}

void handler(int signo, siginfo_t *info, void *raw)
{
    Frame frame(raw);
    switch (run.stage) {
    case Stage::ready:
        if (signo != SIGTRAP ||
            frame.pc() != reinterpret_cast<std::uint64_t>(oracle_ready))
            internal("signal while loading a case", frame.pc());
        install(frame, run.current->initial,
                reinterpret_cast<std::uint64_t>(run.code) + run.current->entry);
        run.stage = Stage::run;
        return;
    case Stage::run:
        capture(frame, signo, info);
        frame.set_x(0, reinterpret_cast<std::uint64_t>(&run.context));
        frame.set_pc(reinterpret_cast<std::uint64_t>(oracle_restore));
        frame.set_pstate(frame.pstate() & ~btype_bits);
        run.stage = Stage::done;
        return;
    case Stage::done:
        break;
    }
    internal("signal outside a case", frame.pc());
}

void watchdog(int, siginfo_t *, void *)
{
    internal("a case ran for ten seconds", 0);
}

// A case runs on its own SP, so the handlers run on an alternate stack.
void install_handlers()
{
    static std::vector<std::uint8_t> alternate(1 << 18);
    stack_t stack{};
    stack.ss_sp = alternate.data();
    stack.ss_size = alternate.size();
    if (sigaltstack(&stack, nullptr) != 0)
        fail("cannot install the signal stack");
    struct sigaction action {};
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int signo : {SIGILL, SIGSEGV, SIGBUS, SIGTRAP, SIGFPE})
        if (sigaction(signo, &action, nullptr) != 0)
            fail("cannot install a signal handler");
    action.sa_sigaction = watchdog;
    if (sigaction(SIGALRM, &action, nullptr) != 0)
        fail("cannot install the watchdog");
}

std::vector<std::uint8_t> read_file(const char *path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        fail(std::string("cannot read ") + path);
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

Case read_case(Reader &reader, bool sve, const Host &host)
{
    Case result;
    const auto word_count = reader.get<std::uint32_t>();
    result.entry = reader.get<std::uint32_t>();
    if (word_count == 0 || word_count > code_bytes / 4 ||
        result.entry % 4 != 0 || result.entry / 4 >= word_count)
        fail("bad program");
    result.words.resize(word_count);
    for (auto &word : result.words)
        word = reader.get<std::uint32_t>();
    result.initial.base();
    read_state(reader, result.initial, sve);
    const auto region_count = reader.get<std::uint8_t>();
    for (unsigned i = 0; i < region_count; ++i) {
        Region region;
        region.address = reader.get<std::uint64_t>();
        region.size = reader.get<std::uint32_t>();
        region.flags = reader.get<std::uint8_t>();
        if (region.address % host.page || region.size == 0 ||
            region.size % host.page || region.address >> 47 ||
            region.flags & ~(region_readable | region_writable | region_explicit))
            fail("bad region");
        region.initial.resize(region.size);
        if (region.flags & region_explicit) {
            reader.copy(region.initial.data(), region.size);
        } else {
            const auto seed = reader.get<std::uint32_t>();
            for (std::uint32_t b = 0; b < region.size; ++b)
                region.initial[b] = region_fill(seed, b);
        }
        result.regions.push_back(std::move(region));
    }
    return result;
}

void write_code(const Case &item)
{
#if defined(__APPLE__)
    pthread_jit_write_protect_np(0);
#endif
    for (std::size_t i = 0; i < code_bytes / 4; ++i)
        run.code[i] = fill_word;
    for (std::size_t i = 0; i < item.words.size(); ++i)
        run.code[i] = item.words[i] == supervisor_call ? sentinel : item.words[i];
#if defined(__APPLE__)
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(run.code, code_bytes);
#else
    __builtin___clear_cache(reinterpret_cast<char *>(run.code),
                            reinterpret_cast<char *>(run.code) + code_bytes);
#endif
}

void map_region(const Region &region)
{
    void *want = reinterpret_cast<void *>(region.address);
#if defined(__linux__)
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE;
#else
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#endif
    void *got = mmap(want, region.size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (got != want)
        fail("cannot map a region at its address");
    std::memcpy(got, region.initial.data(), region.size);
    int protection = PROT_NONE;
    if (region.flags & region_readable)
        protection |= PROT_READ;
    if (region.flags & region_writable)
        protection |= PROT_WRITE;
    if (mprotect(got, region.size, protection) != 0)
        fail("cannot protect a region");
}

void execute(const Case &item, bool sve, Writer &writer)
{
    write_code(item);
    for (const Region &region : item.regions)
        map_region(region);

    run.current = &item;
    run.event = fatal;
    run.stage = Stage::ready;
    alarm(10);
    oracle_enter(&run.context);
    alarm(0);
    if (run.stage != Stage::done)
        fail("the case returned without finishing");

    writer.put(run.event);
    writer.put(run.pc_offset);
    writer.put(run.far);
    writer.put(run.esr);
    writer.put(run.sve_size);
    write_state(writer, run.final_state, item.initial, sve);
    for (const Region &region : item.regions) {
        auto *bytes = reinterpret_cast<std::uint8_t *>(region.address);
        if (mprotect(bytes, region.size, PROT_READ) != 0)
            fail("cannot read back a region");
        std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
        for (std::uint32_t b = 0; b < region.size;) {
            if (bytes[b] == region.initial[b]) {
                ++b;
                continue;
            }
            std::uint32_t end = b;
            while (end < region.size && bytes[end] != region.initial[end])
                ++end;
            runs.emplace_back(b, end - b);
            b = end;
        }
        writer.put(static_cast<std::uint32_t>(runs.size()));
        for (const auto &[offset, length] : runs) {
            writer.put(offset);
            writer.put(length);
            writer.bytes(bytes + offset, length);
        }
        if (munmap(bytes, region.size) != 0)
            fail("cannot unmap a region");
    }
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s REQUEST RESPONSE\n", argv[0]);
        return 64;
    }
    const Host host = probe_host();
    Reader reader(read_file(argv[1]));
    if (std::memcmp(reader.bytes(8), "UMLSVEQ", 8) != 0 ||
        reader.get<std::uint32_t>() != request_version)
        fail("not a version 1 request");
    const auto vl = reader.get<std::uint32_t>();
    const auto count = reader.get<std::uint32_t>();
    if (vl != 0 && (vl != sve_vl || host.vl != sve_vl))
        fail("the request's VL is not this host's, or not 16 bytes");
    const bool sve = vl != 0;

#if defined(__APPLE__)
    void *page = mmap(nullptr, code_bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
#else
    void *page = mmap(nullptr, code_bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (page == MAP_FAILED)
        fail("cannot map the code page");
    run.code = static_cast<std::uint32_t *>(page);
    install_handlers();

    Writer writer;
    writer.bytes(reinterpret_cast<const std::uint8_t *>("UMLSVER"), 8);
    writer.put(response_version);
    writer.put(host.vl);
    writer.put(host.page);
    writer.put(host.hwcap);
    writer.put(host.hwcap2);
    writer.put(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const Case item = read_case(reader, sve, host);
        execute(item, sve, writer);
    }
    if (!reader.done())
        fail("trailing bytes in the request");
    std::ofstream stream(argv[2], std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(writer.data().data()),
                 static_cast<std::streamsize>(writer.data().size()));
    if (!stream)
        fail(std::string("cannot write ") + argv[2]);
    return 0;
}
