// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/x86_64/jit/fex_engine.hpp"

#include "cpu/jit_memory/executable_memory.hpp"
#include "cpu/x86_64/code_page_registry.hpp"
#include "cpu/x86_64/cpuid.hpp"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LogManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <unordered_set>
#include <vector>

namespace uml::cpu::x86_64::jit {
namespace {

using ::uml::memory::GuestAddressSpace;
using ::uml::memory::MemoryAccess;
namespace Core = ::FEXCore::Core;
namespace X86State = ::FEXCore::X86State;

constexpr std::uint64_t guest_page_size = 4096;
constexpr std::uint64_t guest_page_mask = ~(guest_page_size - 1);
constexpr std::size_t tlb_entries = std::size_t{1} << Core::EMBEDDER_TLB_BITS;
/* A dispatch never starts more guest instructions than this, which bounds
 * how far unbalanced calls and returns can move the call-return stack from
 * its middle: one 16-byte record per instruction at most. */
constexpr std::int64_t maximum_dispatch_budget = 64 * 1024;
constexpr std::size_t callret_size = Core::InternalThreadState::CALLRET_STACK_SIZE;
static_assert(callret_size / 2 >= maximum_dispatch_budget * 16,
	"a dispatch cannot leave the call-return stack");
/* Address spaces one vCPU keeps translations for at once. */
constexpr std::size_t maximum_spaces = 8;

constexpr std::uint64_t user_rflags_mask = 0x0000'0000'003f'7fd5ULL;
constexpr std::uint64_t rflags_trap_flag = 1ULL << 8U;
constexpr std::uint64_t rflags_resume_flag = 1ULL << 16U;

constexpr std::uint16_t linux_user_code32 = 0x23;
constexpr std::uint16_t linux_user_code64 = 0x33;

/* x86 orders every store before every later one. FEXCore emulates that with
 * acquire loads and release stores, which costs about 30% on memory-bound
 * code. Lane 6 keys it per address space, on once the mm gains a second
 * thread, which bridge ABI 16 reports to the host; until then every address
 * space emulates it. */
[[nodiscard]] constexpr bool space_orders_stores(unsigned long) noexcept
{
	return true;
}

/* UML_X86_FATAL_TRACE, which the gadget runtime also reads, sends FEXCore's
 * own messages and every #UD to stderr. Diagnostic only: the guest console
 * shares stderr. */
[[nodiscard]] bool trace_enabled() noexcept
{
	static const bool enabled = [] {
		const auto *value = std::getenv("UML_X86_FATAL_TRACE");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}

void fex_message(LogMan::DebugLevels, const char *message)
{
	if (trace_enabled())
		std::fprintf(stderr, "Darwin x86 JIT: FEXCore: %s\n", message);
}

[[noreturn]] void fex_throw(const char *message)
{
	std::fprintf(stderr, "Darwin x86 JIT: fatal FEXCore assertion: %s\n", message);
	std::abort();
}

/* FEXCore reads its configuration from one process-wide layer when a context
 * is built; the execution mode and store ordering are among those values, so
 * building contexts is serialized. */
std::mutex &context_build_lock()
{
	static std::mutex lock;
	return lock;
}

void initialize_fex_once()
{
	static std::once_flag once;
	std::call_once(once, [] {
		LogMan::Msg::InstallHandler(fex_message);
		LogMan::Throw::InstallHandler(fex_throw);
		/* FEXCore maps no executable memory of its own on Apple: the one
		 * owner in this process serves its code buffers. */
		FEXCore::Allocator::SetExecutableMemoryHooks({
			.Allocate = &::uml::cpu::jit_memory::allocate,
			.Free = &::uml::cpu::jit_memory::release,
			.SetWritable = &::uml::cpu::jit_memory::set_writable,
		});
		FEXCore::Config::Initialize();
		using FEXCore::Config::Set;
		namespace C = FEXCore::Config;
		/* One block per guest basic block: the block entry is where the
		 * dispatch budget is spent. */
		Set(C::CONFIG_MULTIBLOCK, "0");
		Set(C::CONFIG_MAXINST, "500");
		/* Full 80-bit x87 is a product requirement. */
		Set(C::CONFIG_X87REDUCEDPRECISION, "0");
		Set(C::CONFIG_IS64BIT_MODE, "1");
	});
}

[[nodiscard]] FEXCore::HostFeatures fex_host_features(
	const ::uml::platform::HostCapabilities &host)
{
	using ::uml::platform::ArmFeature;
	FEXCore::HostFeatures features{};
	features.DCacheLineSize = 64;
	features.ICacheLineSize = 64;
	features.SupportsAES = host.has(ArmFeature::aes);
	features.SupportsCRC = host.has(ArmFeature::crc32);
	features.SupportsAtomics = host.has(ArmFeature::lse);
	features.SupportsRCPC = host.has(ArmFeature::lrcpc);
	features.SupportsTSOImm9 = host.has(ArmFeature::lrcpc2);
	/* FEAT_RNG: no Apple core reports it through sysctl. */
	features.SupportsRAND = false;
	/* AVX and AVX2 run as pairs of 128-bit operations on hosts without
	 * 256-bit SVE. */
	features.SupportsAVX = true;
	features.SupportsSHA = host.has(ArmFeature::sha1) && host.has(ArmFeature::sha256);
	features.SupportsPMULL_128Bit = host.has(ArmFeature::pmull);
	features.SupportsCSSC = host.has(ArmFeature::cssc);
	features.SupportsFCMA = host.has(ArmFeature::fcma);
	features.SupportsFlagM = host.has(ArmFeature::flag_m);
	features.SupportsFlagM2 = host.has(ArmFeature::flag_m2);
	features.SupportsRPRES = host.has(ArmFeature::rpres);
	features.SupportsFRINTTS = host.has(ArmFeature::frintts);
	features.SupportsECV = host.has(ArmFeature::ecv);
	features.SupportsWFXT = host.has(ArmFeature::wfxt);
	features.SupportsMOPS = host.has(ArmFeature::mops);
	/* The prefix builds FEXCore and its SoftFloat without preserve_all, so
	 * fallbacks use the ordinary call ABI. */
	features.SupportsPreserveAllABI = false;
	features.SupportsAFP = false;
	features.SupportsFloatExceptions = false;
	features.SupportsCacheMaintenanceOps = false;
	features.SupportsCPUIndexInTPIDRRO = false;
	features.HostType = FEXCore::HostFeatures::HostTypeEnum::Linux;
	features.CPUMIDRs.assign(8, 0x610f0000U);
	return features;
}

[[nodiscard]] __uint128_t guest_cpuid(void *, std::uint64_t function, std::uint64_t leaf)
{
	const auto result = cpuid(static_cast<std::uint32_t>(function), static_cast<std::uint32_t>(leaf));
	const std::uint64_t low = result.eax | (static_cast<std::uint64_t>(result.ebx) << 32U);
	const std::uint64_t high = result.ecx | (static_cast<std::uint64_t>(result.edx) << 32U);
	return (static_cast<__uint128_t>(high) << 64U) | low;
}

[[nodiscard]] std::uint64_t guest_xgetbv(void *, std::uint32_t function)
{
	/* XCR0 enables x87, SSE and AVX, as CPUID leaf 13 reports. */
	return function == 0 ? cpuid(13, 0).eax : 0;
}

/* A physical-order x87 register's full tag, from its contents: valid, zero,
 * or special (NaN, infinity, denormal, unnormal). */
[[nodiscard]] constexpr std::uint16_t x87_tag_for(const X87Register &value) noexcept
{
	const auto exponent = value.sign_exponent & 0x7fffU;
	if (exponent == 0x7fffU)
		return 2;
	if (exponent == 0)
		return value.significand == 0 ? 1 : 2;
	return (value.significand >> 63U) != 0 ? 0 : 2;
}

struct Space;

class EmbeddedSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
	explicit EmbeddedSyscallHandler(Space &space) noexcept
		: space_{space}
	{
		OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC;
	}

	uint64_t HandleSyscall(Core::CpuStateFrame *, FEXCore::HLE::SyscallArguments *) override
	{
		/* Embedded SYSCALL leaves the dispatch; this is never reached. */
		return static_cast<uint64_t>(-ENOSYS);
	}

	/* The decoder asks for each guest page before it reads its bytes. */
	FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(
		Core::InternalThreadState *, uint64_t address) override;

	std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(
		Core::InternalThreadState *, uint64_t) override
	{
		return std::nullopt;
	}

private:
	Space &space_;
};

/* FEXCore's call-return prediction stack, which generated code addresses
 * through x25 without a bounds check, between guard pages. */
class CallRetStack final {
public:
	CallRetStack() = default;
	CallRetStack(const CallRetStack &) = delete;
	CallRetStack &operator=(const CallRetStack &) = delete;
	~CallRetStack()
	{
		if (data_ != nullptr)
			::uml::cpu::jit_memory::release_guarded(data_, callret_size);
	}

	[[nodiscard]] bool reserve() noexcept
	{
		data_ = ::uml::cpu::jit_memory::reserve_guarded(callret_size);
		return data_ != nullptr;
	}

	[[nodiscard]] void *data() const noexcept { return data_; }

	[[nodiscard]] std::uint64_t middle() const noexcept
	{
		return reinterpret_cast<std::uint64_t>(data_) + callret_size / 2;
	}

private:
	void *data_{};
};

/* One guest address space in one execution mode, as this vCPU runs it. */
struct Space final {
	unsigned long generation{};
	ExecutionMode mode{};
	std::uint64_t last_use{};
	std::uint64_t mapping_sequence{};
	std::uint64_t code_sequence{};
	FexEngine::Impl *engine{};
	std::unique_ptr<EmbeddedSyscallHandler> syscalls;
	CallRetStack callret;
	std::unique_ptr<Core::EmbedderTLBEntry[]> tlb;
	std::array<Core::CPUState::gdt_segment, 32> gdt{};
	fextl::unique_ptr<FEXCore::Context::Context> context;
	Core::InternalThreadState *thread{};
	/* The pages of this address space that any translator holds code for.
	 * The guest runs READ_IMPLIES_EXEC, so "executable" says nothing about
	 * code: only a store to a page the registry holds publishes. Without a
	 * registry every store to an executable page does. */
	std::shared_ptr<CodePageRegistry> registry;
	CodePageRegistry::Participant participant;
	/* The pages this space counted in the registry. */
	std::unordered_set<std::uint64_t> code_pages;

	Space() = default;
	Space(const Space &) = delete;
	Space &operator=(const Space &) = delete;
	~Space()
	{
		release_code_pages();
		/* After leave() no other thread retires an entry of the TLB. */
		if (registry)
			registry->leave(participant);
		if (thread != nullptr)
			context->DestroyThread(thread);
		thread = nullptr;
		context.reset();
	}

	static void retire_write_translation(void *context, std::uint64_t page) noexcept
	{
		auto &space = *static_cast<Space *>(context);
		std::atomic_ref<std::uint64_t>{space.tlb[(page >> 12U) & (tlb_entries - 1)].WriteTag}
			.store(0, std::memory_order_relaxed);
	}

	void join_registry() noexcept
	{
		try {
			registry = CodePageRegistry::acquire(generation);
			registry->join(participant, &Space::retire_write_translation, this);
		} catch (...) {
			registry.reset();
		}
	}

	void flush_tlb() noexcept
	{
		std::memset(static_cast<void *>(tlb.get()), 0, sizeof(Core::EmbedderTLBEntry) * tlb_entries);
	}

	void retire_tlb_range(std::uint64_t address, std::uint64_t length) noexcept
	{
		if (length == 0)
			return;
		const auto first = address >> 12U;
		const auto last = (address + length - 1) >> 12U;
		if (last - first >= tlb_entries) {
			flush_tlb();
			return;
		}
		for (auto page = first; page <= last; ++page) {
			auto &entry = tlb[page & (tlb_entries - 1)];
			if (entry.ReadTag == ~page)
				entry.ReadTag = 0;
			if (std::atomic_ref<std::uint64_t>{entry.WriteTag}.load(std::memory_order_relaxed) == ~page)
				std::atomic_ref<std::uint64_t>{entry.WriteTag}.store(0, std::memory_order_relaxed);
		}
	}

	/* Counts `page` in before the decoder reads it, so a store another vCPU
	 * already translated lands first and every later one publishes. */
	void hold_code_page(std::uint64_t page) noexcept
	{
		if (!registry || code_pages.contains(page))
			return;
		try {
			code_pages.insert(page);
		} catch (...) {
			/* The count can never be returned: the page stays held,
			 * which costs publications, not correctness. */
		}
		/* A compile runs between two guest instructions, with no store
		 * translated and pending, so the registration need not wait for
		 * this participant to leave the engine. */
		const bool inside = (participant.engine_epoch.load(std::memory_order_relaxed) & 1U) != 0;
		if (inside)
			participant.engine_epoch.fetch_add(1, std::memory_order_release);
		registry->add(page, &participant);
		if (inside)
			participant.engine_epoch.fetch_add(1, std::memory_order_seq_cst);
	}

	void release_code_pages() noexcept
	{
		if (registry)
			for (const auto page : code_pages)
				registry->remove(page);
		code_pages.clear();
	}

	void invalidate_code(std::uint64_t address, std::uint64_t length) noexcept
	{
		if (length == 0)
			return;
		{
			std::unique_lock lock{context->GetCodeInvalidationMutex()};
			context->InvalidateCodeBuffersCodeRange(address, length);
			context->InvalidateThreadCachedCodeRange(thread, address, length);
		}
		/* FEXCore drops every translation of a page the range covers
		 * whole, and its decoder asks for the page again before it next
		 * translates it. */
		const auto first = (address + guest_page_size - 1) & guest_page_mask;
		const auto end = (address + length) & guest_page_mask;
		const auto release = [&](std::uint64_t page) {
			if (code_pages.erase(page) != 0 && registry)
				registry->remove(page);
		};
		if (end > first && (end - first) / guest_page_size >= code_pages.size()) {
			for (auto it = code_pages.begin(); it != code_pages.end();) {
				if (*it < first || *it >= end) {
					++it;
					continue;
				}
				if (registry)
					registry->remove(*it);
				it = code_pages.erase(it);
			}
		} else {
			for (auto page = first; page < end; page += guest_page_size)
				release(page);
		}
	}

	void clear_code() noexcept
	{
		context->ClearCodeCache(thread);
		release_code_pages();
	}

	[[nodiscard]] std::uint64_t code_changes() const noexcept
	{
		return registry ? registry->changes() : 0;
	}

	/* Whether a store to the guest page holding `address` may change a
	 * translation some translator of this address space holds. */
	[[nodiscard]] bool stores_publish(GuestAddressSpace &memory, std::uint64_t address) const
	{
		return memory.is_executable(address) && (!registry || registry->holds(address));
	}
};

} // namespace

struct FexEngine::Impl final {
	FEXCore::HostFeatures host_features;
	FEXCore::SignalDelegator signal_delegator;
	std::vector<std::unique_ptr<Space>> spaces;
	std::uint64_t clock{};
	/* Valid during run(). */
	GuestAddressSpace *memory{};
	void *execution_context{};
	const ::uml::cpu::InterruptToken *interrupt{};
	/* RFLAGS.TF was set on entry: a repeated string instruction steps one
	 * element at a time. */
	bool stepping{};

	explicit Impl(const ::uml::platform::HostCapabilities &host)
		: host_features{fex_host_features(host)}
	{}

	[[nodiscard]] Space *find(unsigned long generation, ExecutionMode mode) noexcept
	{
		for (auto &space : spaces)
			if (space->generation == generation && space->mode == mode)
				return space.get();
		return nullptr;
	}

	[[nodiscard]] Space *create(unsigned long generation, ExecutionMode mode) noexcept;
	void publish_code_write(Space &space, std::uint64_t address, std::size_t size) noexcept;
};

namespace {

FEXCore::HLE::ExecutableRangeInfo EmbeddedSyscallHandler::QueryGuestExecutableRange(
	Core::InternalThreadState *, uint64_t address)
{
	auto *memory = space_.engine->memory;
	const auto page = address & guest_page_mask;
	if (memory == nullptr || !memory->is_executable(page))
		return {};
	auto *host = memory->atomic_pointer(page, 1, MemoryAccess::execute);
	if (host == nullptr)
		return {};
	space_.hold_code_page(page);
	return {page, guest_page_size, false, reinterpret_cast<const uint8_t *>(host)};
}

void record_fault(Core::CpuStateFrame &frame, GuestAddressSpace &memory,
	std::uint64_t address, MemoryAccess access, std::uint64_t rip) noexcept
{
	if (memory.has_fault()) {
		const auto fault = memory.last_fault();
		address = fault.address;
		access = fault.access;
	}
	frame.Embedder.FaultAddress = address;
	frame.Embedder.FaultAccess = static_cast<std::uint32_t>(access);
	frame.Embedder.ExitReason = Core::EMBEDDER_EXIT_MEMORY_FAULT;
	frame.State.rip = rip;
}

void record_unsupported(Core::CpuStateFrame &frame, std::uint64_t address, std::uint64_t rip) noexcept
{
	frame.Embedder.FaultAddress = address;
	frame.Embedder.ExitReason = Core::EMBEDDER_EXIT_UNSUPPORTED;
	frame.State.rip = rip;
}

/* The TLB-miss helper the backend's shared stub calls. Returns the host
 * address of the access, or 0 after recording why the dispatch must end. */
uintptr_t embedder_tlb_miss(Core::CpuStateFrame *frame, std::uint64_t address, std::uint64_t info)
{
	auto &space = *reinterpret_cast<Space *>(frame->Embedder.HostContext);
	auto &engine = *space.engine;
	auto &memory = *engine.memory;
	const auto size = static_cast<std::size_t>(info & Core::EMBEDDER_ACCESS_SIZE_MASK);
	const bool store = (info & Core::EMBEDDER_ACCESS_WRITE) != 0;
	const bool writes = (info & (Core::EMBEDDER_ACCESS_WRITE | Core::EMBEDDER_ACCESS_ATOMIC |
		Core::EMBEDDER_ACCESS_READ_WRITE)) != 0;
	const bool atomic = (info & Core::EMBEDDER_ACCESS_ATOMIC) != 0;
	const bool ordered = (info & Core::EMBEDDER_ACCESS_ORDERED) != 0;
	const bool granule_split = (address & 15U) + size > 16U;
	const auto access = writes ? MemoryAccess::write : MemoryAccess::read;
	const auto rip = frame->Embedder.AccessRIP;

	if (atomic && granule_split) {
		/* A locked operand crossing a 16-byte granule has no
		 * single-copy-atomic host access. */
		record_unsupported(*frame, address, rip);
		return 0;
	}

	/* An ordered (TSO) access across a granule completes through the bounce
	 * buffer: x86 makes no single-copy promise for it, and LDAPR/STLR would
	 * raise an alignment fault. */
	const bool ordered_split = ordered && granule_split;
	if (!ordered_split && (address & ~guest_page_mask) + size <= guest_page_size) {
		auto *pointer = memory.atomic_pointer(address, size, access);
		if (pointer == nullptr) {
			record_fault(*frame, memory, address, access, rip);
			return 0;
		}
		const auto page = address >> 12U;
		const auto delta = reinterpret_cast<std::uint64_t>(pointer) - address;
		auto &entry = space.tlb[page & (tlb_entries - 1)];
		if (!writes) {
			entry.ReadDelta = delta;
			entry.ReadTag = ~page;
			return reinterpret_cast<uintptr_t>(pointer);
		}
		/* A page some translator holds code for never gets a write entry,
		 * so each store to it comes here and publishes before it lands. The
		 * registry is shared with the other vCPUs, one of which may be
		 * adding this very page, so the entry is settled like a sequence
		 * lock: fill, then re-read the change count, and take the entry
		 * back if it moved. The adder retires this entry after its own count
		 * bump, so either it sees the fill or the fill sees the bump. */
		for (;;) {
			const auto changes = space.code_changes();
			if (space.stores_publish(memory, address)) {
				engine.publish_code_write(space, address, size);
				return reinterpret_cast<uintptr_t>(pointer);
			}
			std::atomic_ref<std::uint64_t>{entry.WriteDelta}.store(delta, std::memory_order_relaxed);
			std::atomic_ref<std::uint64_t>{entry.WriteTag}.store(~page, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if (space.code_changes() == changes)
				return reinterpret_cast<uintptr_t>(pointer);
			std::atomic_ref<std::uint64_t>{entry.WriteTag}.store(0, std::memory_order_relaxed);
		}
	}

	/* A plain access across two guest pages completes through the bounce
	 * buffer, with the address space's own fault order. */
	if (atomic || (info & Core::EMBEDDER_ACCESS_READ_WRITE) != 0 || size > sizeof(frame->Embedder.Bounce)) {
		record_unsupported(*frame, address, rip);
		return 0;
	}
	auto *bounce = reinterpret_cast<std::byte *>(frame->Embedder.Bounce);
	if (ordered)
		std::atomic_thread_fence(std::memory_order_seq_cst);
	if (store) {
		if (!memory.write(address, std::span<const std::byte>{bounce, size})) {
			record_fault(*frame, memory, address, MemoryAccess::write, rip);
			return 0;
		}
		for (auto page = address & guest_page_mask; page < address + size; page += guest_page_size)
			if (space.stores_publish(memory, page))
				engine.publish_code_write(space, std::max(page, address),
					std::min<std::uint64_t>(page + guest_page_size, address + size) - std::max(page, address));
	} else if (!memory.read(address, std::span<std::byte>{bounce, size})) {
		record_fault(*frame, memory, address, MemoryAccess::read, rip);
		return 0;
	}
	if (ordered)
		std::atomic_thread_fence(std::memory_order_seq_cst);
	return reinterpret_cast<uintptr_t>(bounce);
}

/* REP MOVS and REP STOS: element by element in page-bounded chunks. On a
 * fault RCX, RSI and RDI hold the progress made and RIP the instruction, so
 * the guest restarts it where it stopped, as on x86. */
std::uint64_t embedder_string(Core::CpuStateFrame *frame, const Core::EmbedderStringOp *op_pointer)
{
	auto &space = *reinterpret_cast<Space *>(frame->Embedder.HostContext);
	auto &engine = *space.engine;
	auto &memory = *engine.memory;
	auto &op = *const_cast<Core::EmbedderStringOp *>(op_pointer);
	const std::size_t element = op.ElementSize;
	const bool backward = (static_cast<std::uint64_t>(op.Direction) & 2U) != 0;
	const bool copy = op.IsCopy != 0;

	const auto save_progress = [&] {
		frame->State.gregs[X86State::REG_RCX] = op.Count;
		frame->State.gregs[X86State::REG_RDI] = op.Dest;
		if (copy)
			frame->State.gregs[X86State::REG_RSI] = op.Src;
	};
	const auto fail = [&](std::uint64_t address, MemoryAccess access) {
		record_fault(*frame, memory, address, access, op.GuestRIP);
		save_progress();
		return std::uint64_t{1};
	};

	std::array<std::byte, 8> value{};
	std::memcpy(value.data(), &op.Value, sizeof(op.Value));
	/* Work between checks, in bytes. A long string spends the dispatch
	 * budget as it goes and stops between elements, with RF, when the budget
	 * is gone or the kernel has an interrupt for this vCPU. */
	const std::uint64_t check_bytes = engine.stepping ? element : 64 * 1024;
	std::uint64_t since_check = 0;
	while (op.Count != 0) {
		if (since_check >= check_bytes) {
			since_check = 0;
			frame->Embedder.Budget -= static_cast<std::int64_t>(std::max<std::uint64_t>(check_bytes / 64, 1));
			const bool interrupted = engine.interrupt != nullptr && engine.interrupt->state_pending();
			if (engine.stepping || interrupted || frame->Embedder.Budget <= 0) {
				frame->Embedder.ExitReason = Core::EMBEDDER_EXIT_YIELD;
				frame->State.rip = op.GuestRIP;
				save_progress();
				return 1;
			}
		}
		/* Elements that fit in the current page of each operand, walking
		 * in the string's direction. */
		const auto room = [&](std::uint64_t address) -> std::uint64_t {
			const auto offset = address & ~guest_page_mask;
			if (offset + element > guest_page_size)
				return 0;
			return backward ? offset / element + 1 : (guest_page_size - offset) / element;
		};
		std::uint64_t chunk = std::min<std::uint64_t>(op.Count, room(op.Dest));
		if (copy)
			chunk = std::min(chunk, room(op.Src));
		chunk = std::min<std::uint64_t>(chunk, std::max<std::uint64_t>(check_bytes / element, 1));
		if (chunk == 0) {
			/* An element straddling a page boundary moves on its own,
			 * through the address space's checked copy. */
			std::array<std::byte, 8> bytes{};
			if (copy && !memory.read(op.Src, std::span{bytes.data(), element}))
				return fail(op.Src, MemoryAccess::read);
			if (!memory.write(op.Dest, std::span<const std::byte>{copy ? bytes.data() : value.data(), element}))
				return fail(op.Dest, MemoryAccess::write);
			if (space.stores_publish(memory, op.Dest))
				engine.publish_code_write(space, op.Dest, element);
			const auto step = backward ? -static_cast<std::int64_t>(element) : static_cast<std::int64_t>(element);
			op.Dest += step;
			if (copy)
				op.Src += step;
			--op.Count;
			since_check += element;
			continue;
		}
		const auto bytes = chunk * element;
		const auto low_dest = backward ? op.Dest + element - bytes : op.Dest;
		auto *dest = memory.atomic_pointer(low_dest, bytes, MemoryAccess::write);
		if (dest == nullptr)
			return fail(backward ? op.Dest : low_dest, MemoryAccess::write);
		if (space.stores_publish(memory, low_dest))
			engine.publish_code_write(space, low_dest, bytes);
		if (copy) {
			const auto low_src = backward ? op.Src + element - bytes : op.Src;
			auto *src = memory.atomic_pointer(low_src, bytes, MemoryAccess::read);
			if (src == nullptr)
				return fail(backward ? op.Src : low_src, MemoryAccess::read);
			/* Overlap in the direction of travel repeats a pattern on x86;
			 * otherwise the copy is a plain move. */
			const bool repeats = backward ? (dest < src && dest + bytes > src) : (dest > src && dest < src + bytes);
			if (!repeats) {
				std::memmove(dest, src, bytes);
			} else if (!backward) {
				for (std::uint64_t index = 0; index < bytes; ++index)
					dest[index] = src[index];
			} else {
				for (std::uint64_t index = bytes; index-- > 0;)
					dest[index] = src[index];
			}
		} else if (element == 1) {
			std::memset(dest, std::to_integer<int>(value[0]), bytes);
		} else {
			for (std::uint64_t index = 0; index < chunk; ++index)
				std::memcpy(dest + index * element, value.data(), element);
		}
		const auto step = static_cast<std::int64_t>(bytes);
		op.Dest += backward ? -step : step;
		if (copy)
			op.Src += backward ? -step : step;
		op.Count -= chunk;
		since_check += bytes;
	}
	return 0;
}

/* The host FPCR for a guest MXCSR: RC as RMode and FTZ as FZ, as FEXCore's
 * LDMXCSR sets it. FEXCore applies MXCSR only when the guest loads it, so a
 * dispatch must start from the guest's own value, not from whatever the
 * thread's last guest left behind. Without FEAT_AFP, FZ also flushes
 * denormal inputs and DAZ alone has no effect. */
[[nodiscard]] std::uint64_t fpcr_for_mxcsr(std::uint32_t mxcsr) noexcept
{
	constexpr std::array<std::uint64_t, 4> rounding{0, 2, 1, 3};
	auto value = rounding[(mxcsr >> 13U) & 3U] << 22U;
	if ((mxcsr & (1U << 15U)) != 0)
		value |= 1ULL << 24U;
	return value;
}

[[nodiscard]] std::uint64_t read_fpcr() noexcept
{
	std::uint64_t value;
	asm volatile("mrs %0, fpcr" : "=r"(value));
	return value;
}

void write_fpcr(std::uint64_t value) noexcept
{
	asm volatile("msr fpcr, %0" : : "r"(value));
}

void load_state(Space &space, const CpuState &cpu) noexcept
{
	auto *thread = space.thread;
	auto &state = thread->CurrentFrame->State;
	std::copy(cpu.registers.begin(), cpu.registers.end(), std::begin(state.gregs));
	state.rip = cpu.rip;
	/* RF only suppresses instruction breakpoints, which this engine has
	 * none of, and is gone once the first instruction completes. */
	space.context->SetFlagsFromCompactedEFLAGS(thread,
		static_cast<std::uint32_t>(cpu.rflags & user_rflags_mask & ~rflags_resume_flag));

	std::array<__uint128_t, 16> low{};
	std::array<__uint128_t, 16> high{};
	for (std::size_t index = 0; index < cpu.vectors.size(); ++index) {
		std::memcpy(&low[index], cpu.vectors[index].data(), 16);
		std::memcpy(&high[index], cpu.vectors[index].data() + 16, 16);
	}
	space.context->SetXMMRegistersFromState(thread, low.data(), high.data());
	state.mxcsr = cpu.mxcsr;

	std::uint8_t abridged = 0;
	for (std::size_t index = 0; index < cpu.x87.size(); ++index) {
		state.mm[index][0] = cpu.x87[index].significand;
		state.mm[index][1] = cpu.x87[index].sign_exponent;
		if (((cpu.x87_tag >> (index * 2U)) & 3U) != 3U)
			abridged |= static_cast<std::uint8_t>(1U << index);
	}
	state.AbridgedFTW = abridged;
	state.FCW = cpu.x87_control;
	for (unsigned bit = 0; bit < 8; ++bit)
		state.flags[X86State::X87FLAG_IE_LOC + bit] = (cpu.x87_status >> bit) & 1U;
	state.flags[X86State::X87FLAG_C0_LOC] = (cpu.x87_status >> 8U) & 1U;
	state.flags[X86State::X87FLAG_C1_LOC] = (cpu.x87_status >> 9U) & 1U;
	state.flags[X86State::X87FLAG_C2_LOC] = (cpu.x87_status >> 10U) & 1U;
	state.flags[X86State::X87FLAG_C3_LOC] = (cpu.x87_status >> 14U) & 1U;
	state.flags[X86State::X87FLAG_B_LOC] = (cpu.x87_status >> 15U) & 1U;
	state.flags[X86State::X87FLAG_TOP_LOC] = cpu.x87_top & 7U;

	state.es_idx = cpu.segment_selectors[0];
	/* FEXCore decodes by the CS descriptor's L bit; the space's mode, not
	 * whatever the selector holds, decides it, as for the gadget engine. */
	state.cs_idx = space.mode == ExecutionMode::long64 ? linux_user_code64 : linux_user_code32;
	state.ss_idx = cpu.segment_selectors[2];
	state.ds_idx = cpu.segment_selectors[3];
	state.fs_idx = cpu.segment_selectors[4];
	state.gs_idx = cpu.segment_selectors[5];
	state.es_cached = state.cs_cached = state.ss_cached = state.ds_cached = 0;
	state.fs_cached = cpu.fs_base;
	state.gs_cached = cpu.gs_base;
}

void save_state(Space &space, CpuState &cpu) noexcept
{
	auto *thread = space.thread;
	auto &state = thread->CurrentFrame->State;
	std::copy(std::begin(state.gregs), std::end(state.gregs), cpu.registers.begin());
	cpu.rip = state.rip;
	const auto flags = space.context->ReconstructCompactedEFLAGS(thread, false, nullptr, 0);
	/* Every instruction that completes clears RF; the runtime sets it again
	 * for a fault. */
	cpu.rflags = ((flags & user_rflags_mask) & ~rflags_resume_flag) | 2U;
	cpu.deferred_flags = {};

	std::array<__uint128_t, 16> low{};
	std::array<__uint128_t, 16> high{};
	space.context->ReconstructXMMRegisters(thread, low.data(), high.data());
	for (std::size_t index = 0; index < cpu.vectors.size(); ++index) {
		std::memcpy(cpu.vectors[index].data(), &low[index], 16);
		std::memcpy(cpu.vectors[index].data() + 16, &high[index], 16);
	}
	cpu.mxcsr = state.mxcsr;

	std::uint16_t tag = 0;
	for (std::size_t index = 0; index < cpu.x87.size(); ++index) {
		cpu.x87[index].significand = state.mm[index][0];
		cpu.x87[index].sign_exponent = static_cast<std::uint16_t>(state.mm[index][1]);
		cpu.mmx[index] = cpu.x87[index].significand;
		const std::uint16_t entry = (state.AbridgedFTW >> index) & 1U ? x87_tag_for(cpu.x87[index]) : 3U;
		tag = static_cast<std::uint16_t>(tag | (entry << (index * 2U)));
	}
	cpu.x87_tag = tag;
	cpu.x87_control = state.FCW;
	std::uint16_t status = 0;
	for (unsigned bit = 0; bit < 8; ++bit)
		status = static_cast<std::uint16_t>(status | ((state.flags[X86State::X87FLAG_IE_LOC + bit] & 1U) << bit));
	status = static_cast<std::uint16_t>(status | ((state.flags[X86State::X87FLAG_C0_LOC] & 1U) << 8U));
	status = static_cast<std::uint16_t>(status | ((state.flags[X86State::X87FLAG_C1_LOC] & 1U) << 9U));
	status = static_cast<std::uint16_t>(status | ((state.flags[X86State::X87FLAG_C2_LOC] & 1U) << 10U));
	status = static_cast<std::uint16_t>(status | ((state.flags[X86State::X87FLAG_C3_LOC] & 1U) << 14U));
	status = static_cast<std::uint16_t>(status | ((state.flags[X86State::X87FLAG_B_LOC] & 1U) << 15U));
	cpu.x87_top = state.flags[X86State::X87FLAG_TOP_LOC] & 7U;
	status = static_cast<std::uint16_t>(status | (cpu.x87_top << 11U));
	cpu.x87_status = status;

	cpu.segment_selectors = {state.es_idx, cpu.segment_selectors[1], state.ss_idx, state.ds_idx, state.fs_idx,
		state.gs_idx};
	cpu.fs_base = state.fs_cached;
	cpu.gs_base = state.gs_cached;
}

/* Linux's flat user descriptor: FEXCore takes the decode mode from the code
 * descriptor's L bit. */
[[nodiscard]] Core::CPUState::gdt_segment flat_user_segment(bool code, bool long_mode) noexcept
{
	Core::CPUState::gdt_segment segment{};
	Core::CPUState::SetGDTLimit(&segment, 0xfffffU);
	segment.Type = code ? 0xbU : 0x3U;
	segment.S = 1;
	segment.DPL = 3;
	segment.P = 1;
	segment.L = long_mode ? 1U : 0U;
	segment.D = long_mode ? 0U : 1U;
	segment.G = 1;
	return segment;
}

} // namespace

Space *FexEngine::Impl::create(unsigned long generation, ExecutionMode mode) noexcept
{
	initialize_fex_once();
	if (spaces.size() >= maximum_spaces) {
		auto oldest = std::min_element(spaces.begin(), spaces.end(),
			[](const auto &left, const auto &right) { return left->last_use < right->last_use; });
		spaces.erase(oldest);
	}
	std::unique_ptr<Space> space;
	try {
		space = std::make_unique<Space>();
		space->syscalls = std::make_unique<EmbeddedSyscallHandler>(*space);
		space->tlb = std::make_unique<Core::EmbedderTLBEntry[]>(tlb_entries);
		spaces.reserve(spaces.size() + 1);
	} catch (...) {
		return nullptr;
	}
	space->generation = generation;
	space->mode = mode;
	space->engine = this;
	if (!space->callret.reserve())
		return nullptr;
	space->flush_tlb();
	{
		std::lock_guard lock{context_build_lock()};
		using FEXCore::Config::Set;
		namespace C = FEXCore::Config;
		Set(C::CONFIG_IS64BIT_MODE, mode == ExecutionMode::long64 ? "1" : "0");
		const auto *orders_stores = space_orders_stores(generation) ? "1" : "0";
		Set(C::CONFIG_TSOENABLED, orders_stores);
		Set(C::CONFIG_VECTORTSOENABLED, orders_stores);
		Set(C::CONFIG_MEMCPYSETTSOENABLED, orders_stores);
		space->context = FEXCore::Context::Context::CreateNewContext(host_features);
		if (!space->context)
			return nullptr;
		space->context->SetSignalDelegator(&signal_delegator);
		space->context->SetSyscallHandler(space->syscalls.get());
		space->context->EnableEmbedding();
		if (!space->context->InitCore())
			return nullptr;
		space->thread = space->context->CreateThread(0, 0, nullptr);
	}
	if (space->thread == nullptr)
		return nullptr;
	space->join_registry();
	auto *frame = space->thread->CurrentFrame;
	space->thread->CallRetStackBase = space->callret.data();
	frame->State.callret_sp = space->callret.middle();
	space->gdt[4] = flat_user_segment(true, false);
	space->gdt[5] = flat_user_segment(false, false);
	space->gdt[6] = flat_user_segment(true, true);
	frame->State.segment_arrays[Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = space->gdt.data();
	frame->State.segment_arrays[Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = space->gdt.data();
	auto &pointers = frame->Pointers;
	pointers.CPUIDObj = 0;
	pointers.CPUIDFunction = reinterpret_cast<uint64_t>(&guest_cpuid);
	pointers.XCRFunction = reinterpret_cast<uint64_t>(&guest_xgetbv);
	pointers.EmbedderTLBMissHelper = reinterpret_cast<uint64_t>(&embedder_tlb_miss);
	pointers.EmbedderStringHelper = reinterpret_cast<uint64_t>(&embedder_string);
	frame->Embedder.TLB = reinterpret_cast<uint64_t>(space->tlb.get());
	frame->Embedder.HostContext = reinterpret_cast<uint64_t>(space.get());
	spaces.push_back(std::move(space));
	return spaces.back().get();
}

void FexEngine::Impl::publish_code_write(Space &space, std::uint64_t address, std::size_t size) noexcept
{
	if (size == 0)
		return;
	for (auto page = address & guest_page_mask; page < address + size; page += guest_page_size)
		memory->invalidate_code(page, execution_context);
	space.invalidate_code(address & guest_page_mask,
		((address + size - 1) & guest_page_mask) + guest_page_size - (address & guest_page_mask));
}

FexEngine::FexEngine(const ::uml::platform::HostCapabilities &host) noexcept
{
	try {
		impl_ = std::make_unique<Impl>(host);
	} catch (...) {
		impl_.reset();
	}
}

FexEngine::~FexEngine() = default;

bool FexEngine::consumed_sequences(unsigned long generation, ExecutionMode mode,
	std::uint64_t &mapping_sequence, std::uint64_t &code_sequence) const noexcept
{
	if (!impl_)
		return false;
	const auto *space = impl_->find(generation, mode);
	if (space == nullptr)
		return false;
	mapping_sequence = space->mapping_sequence;
	code_sequence = space->code_sequence;
	return true;
}

void FexEngine::apply_changes(unsigned long generation, ExecutionMode mode,
	const AddressSpaceChanges &changes) noexcept
{
	if (!impl_)
		return;
	auto *space = impl_->find(generation, mode);
	if (space == nullptr) {
		/* A new space caches nothing yet. */
		space = impl_->create(generation, mode);
		if (space == nullptr)
			return;
	} else {
		if (changes.all_mappings) {
			space->flush_tlb();
			space->clear_code();
		} else {
			for (const auto &range : changes.mapping_ranges) {
				space->retire_tlb_range(range.address, range.length);
				space->invalidate_code(range.address, range.length);
			}
		}
		if (changes.all_code) {
			space->clear_code();
		} else {
			for (const auto &range : changes.code_ranges)
				space->invalidate_code(range.address, range.length);
		}
	}
	space->mapping_sequence = changes.mapping_sequence;
	space->code_sequence = changes.code_sequence;
}

JitRunResult FexEngine::run(CpuState &cpu, GuestAddressSpace &memory, const JitRunRequest &request) noexcept
{
	JitRunResult run{};
	auto &result = run.execution;
	result.reason = ::uml::cpu::ExitReason::fatal;
	if (!impl_)
		return run;
	auto *space = impl_->find(request.generation, cpu.execution_mode);
	if (space == nullptr || space->mode != ExecutionMode::long64)
		return run;
	space->last_use = ++impl_->clock;
	impl_->memory = &memory;
	impl_->execution_context = request.execution_context;
	impl_->interrupt = request.interrupt;
	impl_->stepping = (cpu.rflags & rflags_trap_flag) != 0;

	auto *thread = space->thread;
	auto *frame = thread->CurrentFrame;
	load_state(*space, cpu);
	/* A step starts one instruction, and one iteration of a repeated
	 * string instruction. */
	const auto budget = impl_->stepping ? 1 :
		std::clamp<std::int64_t>(static_cast<std::int64_t>(request.instruction_budget), 1,
			maximum_dispatch_budget);
	frame->Embedder.Budget = budget;
	frame->Embedder.ExitReason = Core::EMBEDDER_EXIT_NONE;
	frame->Embedder.FaultAddress = 0;
	frame->Embedder.FaultAccess = 0;
	frame->SynchronousFaultData = {};
	frame->State.callret_sp = space->callret.middle();

	const auto host_fpcr = read_fpcr();
	write_fpcr(fpcr_for_mxcsr(cpu.mxcsr));
	/* Registrations of a page by another vCPU wait for this one to leave
	 * the engine once, so a store it translated lands first. */
	space->participant.engine_epoch.fetch_add(1, std::memory_order_seq_cst);
	space->context->ExecuteEmbedded(thread, false);
	space->participant.engine_epoch.fetch_add(1, std::memory_order_release);
	write_fpcr(host_fpcr);

	result.retired_instructions = static_cast<std::size_t>(budget - std::max<std::int64_t>(frame->Embedder.Budget, 0));
	save_state(*space, cpu);
	impl_->memory = nullptr;

	switch (frame->Embedder.ExitReason) {
	case Core::EMBEDDER_EXIT_BUDGET:
		/* A step whose instruction branched back to a block it had
		 * entered retired that instruction before the back edge ran out
		 * of budget. */
		result.reason = impl_->stepping && result.retired_instructions != 0 ?
			::uml::cpu::ExitReason::single_step : ::uml::cpu::ExitReason::instruction_budget;
		return run;
	case Core::EMBEDDER_EXIT_YIELD:
		/* Stopped between the elements of a repeated string instruction:
		 * x86 shows RF in the interrupted frame, and raises #DB after each
		 * iteration under TF. */
		cpu.rflags |= rflags_resume_flag;
		result.reason = impl_->stepping ? ::uml::cpu::ExitReason::single_step :
			::uml::cpu::ExitReason::instruction_budget;
		return run;
	case Core::EMBEDDER_EXIT_MEMORY_FAULT:
		result.reason = ::uml::cpu::ExitReason::memory_fault;
		result.fault.address = frame->Embedder.FaultAddress;
		result.fault.access = static_cast<MemoryAccess>(frame->Embedder.FaultAccess);
		return run;
	case Core::EMBEDDER_EXIT_BREAK:
		break;
	case Core::EMBEDDER_EXIT_UNSUPPORTED:
		/* An operation the embedding cannot perform yet: lane 5 owns
		 * them, and a guest that reaches one should say so. */
		std::fprintf(stderr, "Darwin x86 JIT: unsupported operation at rip=%#llx address=%#llx\n",
			static_cast<unsigned long long>(cpu.rip), static_cast<unsigned long long>(frame->Embedder.FaultAddress));
		return run;
	default:
		std::fprintf(stderr, "Darwin x86 JIT: dispatch returned without an exit at rip=%#llx\n",
			static_cast<unsigned long long>(cpu.rip));
		return run;
	}

	const auto fault = frame->SynchronousFaultData;
	switch (fault.Signal) {
	case Core::EMBEDDER_SIGNAL_SYSCALL:
		result.reason = ::uml::cpu::ExitReason::system_call;
		cpu.system_call_32bit = fault.TrapNo == 0x80;
		return run;
	case Core::EMBEDDER_SIGNAL_DIVIDE:
		result.reason = ::uml::cpu::ExitReason::floating_point_trap;
		result.syndrome = X86State::X86_TRAPNO_DE;
		return run;
	case Core::FAULT_SIGTRAP:
		if (fault.TrapNo == X86State::X86_TRAPNO_DB && fault.si_code == 2) {
			result.reason = ::uml::cpu::ExitReason::single_step;
			return run;
		}
		result.reason = ::uml::cpu::ExitReason::breakpoint;
		result.syndrome = fault.TrapNo;
		return run;
	case Core::FAULT_SIGSEGV:
		if (fault.TrapNo == X86State::X86_TRAPNO_PF) {
			/* An instruction fetch from a page that is not executable: the
			 * instruction's first byte, or the page its tail reaches. The
			 * block that reported it must not outlive the fault, which the
			 * kernel may resolve. */
			const auto address = memory.is_executable(cpu.rip) ? (cpu.rip | (guest_page_size - 1)) + 1 : cpu.rip;
			space->invalidate_code(cpu.rip, 16);
			result.reason = ::uml::cpu::ExitReason::memory_fault;
			result.fault = {address, MemoryAccess::execute};
			return run;
		}
		result.reason = ::uml::cpu::ExitReason::breakpoint;
		result.syndrome = fault.TrapNo;
		run.error_code = fault.err_code;
		return run;
	case Core::FAULT_SIGILL:
		result.reason = ::uml::cpu::ExitReason::illegal_instruction;
		if (trace_enabled())
			std::fprintf(stderr, "Darwin x86 JIT: #UD at rip=%#llx (trap %u code %u)\n",
				static_cast<unsigned long long>(cpu.rip), fault.TrapNo, fault.si_code);
		return run;
	default:
		std::fprintf(stderr, "Darwin x86 JIT: unexpected guest exception signal %u trap %u at rip=%#llx\n",
			fault.Signal, fault.TrapNo, static_cast<unsigned long long>(cpu.rip));
		return run;
	}
}

bool fex_engine_available(const ::uml::platform::HostCapabilities &host) noexcept
{
	return host.has(::uml::platform::ArmFeature::lse2) && host.has(::uml::platform::ArmFeature::lse) &&
		::uml::cpu::jit_memory::available();
}

} // namespace uml::cpu::x86_64::jit
