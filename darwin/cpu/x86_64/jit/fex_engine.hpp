// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* The x86-64 JIT engine: FEXCore's frontend, IR passes and arm64 backend,
 * embedded so that guest memory stays behind the static MMU. Only the macOS
 * x86-64 guest builds it (UML_BUILD_X86_64_JIT); no iOS product links it.
 *
 * One engine belongs to one vCPU thread. It keeps a FEXCore context and
 * thread per guest address space and execution mode, because FEXCore's block
 * caches are keyed by guest RIP alone. */

#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "memory/guest_address_space.hpp"
#include "platform/host_capabilities.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace uml::cpu::x86_64::jit {

struct GuestRange final {
	std::uint64_t address{};
	std::uint64_t length{};
};

/* What the address space reports changed since the engine last looked: the
 * ranges, or `all_*` when it cannot describe them. */
struct AddressSpaceChanges final {
	std::uint64_t mapping_sequence{};
	std::uint64_t code_sequence{};
	std::span<const GuestRange> mapping_ranges{};
	std::span<const GuestRange> code_ranges{};
	bool all_mappings{};
	bool all_code{};
};

struct JitRunRequest final {
	unsigned long generation{};
	/* Guest instructions this dispatch may start. */
	std::size_t instruction_budget{};
	/* The static MMU's execution context, through which a store into guest
	 * code publishes the change to other translators. */
	void *execution_context{};
	/* Polled between chunks of a long repeated string instruction. */
	const ::uml::cpu::InterruptToken *interrupt{};
};

struct JitRunResult final {
	::uml::cpu::ExecutionResult execution{};
	/* The error code of a #GP the run ended in: for INT n, its IDT entry. */
	std::int32_t error_code{};
};

struct FexEngineOptions final {
	/* Bytes of host code a compile first reserves per IR node, 0 for
	 * FEXCore's default. A unit that outgrows its reservation restarts with
	 * twice as much, so a small value costs only compile time; a test lowers
	 * it to make every large unit grow. */
	std::uint32_t code_bytes_per_node{};
};

class FexEngine final {
public:
	explicit FexEngine(const ::uml::platform::HostCapabilities &host,
		FexEngineOptions options = {}) noexcept;
	~FexEngine();
	FexEngine(const FexEngine &) = delete;
	FexEngine &operator=(const FexEngine &) = delete;

	/* The sequences this engine last consumed for `generation`, so the
	 * runtime can ask the address space only for what is new. Returns false
	 * when the engine holds nothing for it yet. */
	[[nodiscard]] bool consumed_sequences(unsigned long generation,
		ExecutionMode mode, std::uint64_t &mapping_sequence,
		std::uint64_t &code_sequence) const noexcept;

	/* Retires software-TLB entries and translations the changes cover, and
	 * makes the space's context on first use. */
	void apply_changes(unsigned long generation, ExecutionMode mode,
		const AddressSpaceChanges &changes) noexcept;

	/* Runs 64-bit guest code until it exits. RFLAGS.TF set on entry makes
	 * the run retire one instruction and report a single step. */
	[[nodiscard]] JitRunResult run(CpuState &cpu,
		::uml::memory::GuestAddressSpace &memory,
		const JitRunRequest &request) noexcept;

	/* Compiles this engine restarted with a larger code buffer. */
	[[nodiscard]] std::uint64_t code_buffer_restarts() const noexcept;

	struct Impl;

private:
	std::unique_ptr<Impl> impl_;
};

/* Whether this process can run the engine: JIT memory with a write window,
 * FEAT_LSE and FEAT_LSE2. */
[[nodiscard]] bool fex_engine_available(
	const ::uml::platform::HostCapabilities &host) noexcept;

} // namespace uml::cpu::x86_64::jit
