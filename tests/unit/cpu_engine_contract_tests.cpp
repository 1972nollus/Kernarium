/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "cpu/arm64/engine_execution.hpp"
#include "memory/static_guest_memory.hpp"
#include "arm64/mach_vm_protection_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <span>
#include <stdexcept>

namespace {

class TestMemory final : public uml::cpu::GuestAddressSpace {
public:
	explicit TestMemory(std::uint32_t instruction = 0xd400'0001U)
	{
		std::memcpy(bytes_.data(), &instruction, sizeof(instruction));
	}

	[[nodiscard]] bool read(
		std::uint64_t address, std::span<std::byte> destination) override
	{
		if (address > bytes_.size() ||
			destination.size() > bytes_.size() - address)
			return false;
		std::memcpy(destination.data(), bytes_.data() + address,
			destination.size());
		return true;
	}

	[[nodiscard]] bool write(
		std::uint64_t address,
		std::span<const std::byte> source) override
	{
		if (address > bytes_.size() || source.size() > bytes_.size() - address)
			return false;
		std::memcpy(bytes_.data() + address, source.data(), source.size());
		return true;
	}

private:
	std::array<std::byte, 4096> bytes_{};
};

class TranslatedBoundaryExecutor final {
public:
	[[nodiscard]] bool begin_execution(unsigned long, std::uint64_t)
	{
		++begin_calls;
		return true;
	}

	void end_execution() noexcept
	{
		++end_calls;
	}

	[[nodiscard]] uml::cpu::EngineEvent run_block(
		uml::cpu::arm64::CpuState &, uml::memory::StaticGuestMemory &,
		unsigned long, std::size_t &executed)
	{
		++block_calls;
		executed = block_size;
		return uml::cpu::EngineEvent::interrupt;
	}

	[[nodiscard]] bool preempted() const noexcept
	{
		return false;
	}

	[[nodiscard]] std::uint64_t fault_address() const noexcept
	{
		return 0;
	}

	[[nodiscard]] uml::memory::Access fault_access() const noexcept
	{
		return uml::memory::Access::read;
	}

	[[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept
	{
		return 0;
	}

	static constexpr std::size_t block_size = 4096;
	std::size_t begin_calls{};
	std::size_t end_calls{};
	std::size_t block_calls{};
};

class ThrowingTranslatedExecutor final {
public:
	[[nodiscard]] bool begin_execution(unsigned long, std::uint64_t)
	{
		++begin_calls;
		if (entered)
			return false;
		entered = true;
		return true;
	}

	void end_execution() noexcept
	{
		++end_calls;
		entered = false;
	}

	[[nodiscard]] uml::cpu::EngineEvent run_block(
		uml::cpu::arm64::CpuState &, uml::memory::StaticGuestMemory &,
		unsigned long, std::size_t &executed)
	{
		++block_calls;
		executed = 0;
		if (throw_on_run)
			throw std::runtime_error{"injected translated-engine failure"};
		return uml::cpu::EngineEvent::fatal;
	}

	[[nodiscard]] bool preempted() const noexcept { return false; }
	[[nodiscard]] std::uint64_t fault_address() const noexcept { return 0; }
	[[nodiscard]] uml::memory::Access fault_access() const noexcept
	{
		return uml::memory::Access::read;
	}
	[[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept
	{
		return 0;
	}

	bool throw_on_run{true};
	bool entered{};
	std::size_t begin_calls{};
	std::size_t end_calls{};
	std::size_t block_calls{};
};

class ThrowingMemory final : public uml::cpu::GuestAddressSpace {
public:
	[[nodiscard]] bool read(
		std::uint64_t, std::span<std::byte>) override
	{
		throw std::runtime_error{"injected interpreter memory failure"};
	}

	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		throw std::runtime_error{"injected interpreter memory failure"};
	}
};

struct InterruptPollState final {
	std::size_t calls{};
	std::size_t pending_on_call{};
};

[[nodiscard]] bool poll_interrupt(void *opaque) noexcept
{
	auto &state = *static_cast<InterruptPollState *>(opaque);
	++state.calls;
	return state.calls >= state.pending_on_call;
}

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

void test_factory_contract()
{
	for (const auto kind : {uml::cpu::EngineKind::interpreter,
		uml::cpu::EngineKind::gadgets, uml::cpu::EngineKind::jit}) {
		auto engine = uml::cpu::make_cpu_engine(kind);
		require(engine != nullptr, "engine factory returned null");
		require(engine->kind() == kind, "engine kind did not survive factory");
		if (kind != uml::cpu::EngineKind::jit)
			require(!uml::testing::process_has_writable_executable_region(),
				"non-JIT engine factory created W+X memory");
	}
}

void test_interpreter_result_mapping()
{
	auto engine = uml::cpu::make_cpu_engine(uml::cpu::EngineKind::interpreter);
	TestMemory memory;
	uml::cpu::arm64::CpuState cpu;
	cpu.pc = 0;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.generation = 1,
		.instruction_budget = 16,
	};
	const auto result = engine->run(request);
	require(result.reason == uml::cpu::ExitReason::system_call,
		"interpreter exit reason was not normalized");
	require(result.retired_instructions == 1,
		"interpreter retired count was not preserved");
	require(!uml::testing::process_has_writable_executable_region(),
		"interpreter execution created W+X memory");
}

void test_interpreter_distinguishes_guest_illegal_from_coverage_gap()
{
	auto engine = uml::cpu::make_cpu_engine(uml::cpu::EngineKind::interpreter);
	uml::cpu::arm64::CpuState illegal_cpu;
	TestMemory illegal_memory{0x0000'0000U}; // UDF #0
	uml::cpu::ExecutionRequest illegal_request{
		.cpu = illegal_cpu,
		.memory = illegal_memory,
		.generation = 1,
		.instruction_budget = 1,
	};
	const auto illegal = engine->run(illegal_request);
	require(illegal.reason == uml::cpu::ExitReason::illegal_instruction,
		"architecturally undefined encoding was not guest-illegal");
	require(illegal.retired_instructions == 0,
		"undefined encoding unexpectedly retired");

	uml::cpu::arm64::CpuState unsupported_cpu;
	TestMemory unsupported_memory{0xd500'0000U};
	uml::cpu::ExecutionRequest unsupported_request{
		.cpu = unsupported_cpu,
		.memory = unsupported_memory,
		.generation = 1,
		.instruction_budget = 1,
	};
	const auto unsupported = engine->run(unsupported_request);
	require(unsupported.reason == uml::cpu::ExitReason::fatal,
		"valid unimplemented encoding escaped as a guest SIGILL");
	require(unsupported.retired_instructions == 0,
		"unimplemented encoding unexpectedly retired");
}

void test_interpreter_contains_memory_exceptions()
{
	auto engine = uml::cpu::make_cpu_engine(uml::cpu::EngineKind::interpreter);
	ThrowingMemory memory;
	uml::cpu::arm64::CpuState cpu;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.generation = 1,
		.instruction_budget = 1,
	};
	const auto result = engine->run(request);
	require(result.reason == uml::cpu::ExitReason::fatal,
		"interpreter exception escaped the noexcept CPU boundary");
	require(result.retired_instructions == 0,
		"failed interpreter memory access unexpectedly retired");
}

void test_translated_internal_interrupt_obeys_fairness_budget()
{
	TranslatedBoundaryExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = 7,
		.instruction_budget = 64 * 1024,
	};
	const auto result = uml::cpu::arm64::detail::run_translated(
		executor, request);
	require(result.reason == uml::cpu::ExitReason::instruction_budget,
		"internal translated event escaped as a guest interrupt");
	require(result.retired_instructions == 64 * 1024,
		"translated fairness retirement count changed");
	require(executor.block_calls == 16,
		"translated engine did not run through the exact fairness budget");
	require(executor.begin_calls == 1 && executor.end_calls == 1,
		"translated execution lifetime was not balanced");
}

void test_translated_internal_interrupt_delivers_pending_interrupt()
{
	TranslatedBoundaryExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu;
	InterruptPollState poll{.pending_on_call = 2};
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = 11,
		.instruction_budget = 64 * 1024,
		.interrupt = {
			.poll = poll_interrupt,
			.poll_context = &poll,
		},
	};
	const auto result = uml::cpu::arm64::detail::run_translated(
		executor, request);
	require(result.reason == uml::cpu::ExitReason::interrupt,
		"pending host/guest interrupt was not delivered");
	require(result.retired_instructions == TranslatedBoundaryExecutor::block_size,
		"interrupt boundary lost the exact retired count");
	require(executor.block_calls == 1 && poll.calls == 2,
		"interrupt was not sampled at the translated cold boundary");
	require(executor.begin_calls == 1 && executor.end_calls == 1,
		"interrupt exit did not close translated execution");
}

void test_translated_cleanup_is_exception_safe()
{
	ThrowingTranslatedExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = 13,
		.instruction_budget = 1,
	};
	bool threw = false;
	try {
		(void)uml::cpu::arm64::detail::run_translated(executor, request);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	require(threw, "translated failure injection did not throw");
	require(executor.begin_calls == 1 && executor.end_calls == 1,
		"throwing translated entry did not perform exactly one cleanup");
	require(!executor.entered,
		"throwing translated entry left its execution context entered");

	executor.throw_on_run = false;
	const auto retry = uml::cpu::arm64::detail::run_translated(executor, request);
	require(retry.reason == uml::cpu::ExitReason::fatal,
		"translated entry could not be retried after exception cleanup");
	require(executor.begin_calls == 2 && executor.end_calls == 2,
		"retried translated entry did not retain balanced cleanup");
}

} // namespace

int main()
{
	try {
		test_factory_contract();
		test_interpreter_result_mapping();
		test_interpreter_distinguishes_guest_illegal_from_coverage_gap();
		test_interpreter_contains_memory_exceptions();
		test_translated_internal_interrupt_obeys_fairness_budget();
		test_translated_internal_interrupt_delivers_pending_interrupt();
		test_translated_cleanup_is_exception_safe();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "CPU engine contract test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
}
