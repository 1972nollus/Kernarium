// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/detail/host_counter.hpp"
#include "jit_test_support.hpp"
#include "runtime/jit/jit_executor.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace uml::backend::tests;

namespace {

void direct_counter_block_is_verifier_safe()
{
	constexpr unsigned long generation = 716;
	constexpr std::uint64_t base = 0x8b'0000;
	JitBacking backing{1};
	backing.put32(0, 0xd53b'e005U); // MRS X5, CNTFRQ_EL0
	backing.put32(4, 0xd53b'e046U); // MRS X6, CNTVCT_EL0
	backing.put32(8, 0xd400'0001U); // SVC #0
	JitMapping mapping{generation, base, backing, 1};
	uml::backend::JitExecutor executor;
	uml::backend::StaticGuestMemory memory;
	uml::backend::arm64::CpuState cpu{};
	cpu.pc = base;

	const auto before =
		uml::backend::arm64::detail::read_host_virtual_counter();
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event =
		executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	const auto after =
		uml::backend::arm64::detail::read_host_virtual_counter();

	require(event == uml::backend::arm64::gadgets::Event::system_call);
	require(executed == 3);
	require(cpu.pc == base + 12);
	require(cpu.registers[5] ==
		uml::backend::arm64::detail::read_host_counter_frequency());
	require(cpu.registers[6] >= before);
	require(cpu.registers[6] <= after);
}

} // namespace

int main()
{
	require(::setenv("UML_VERIFY_JIT", "1", 1) == 0);
	require(darwin_uml_jit_available() == 1);
	direct_counter_block_is_verifier_safe();
	return 0;
}
