// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory_resource>
#include <source_location>
#include <utility>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;

extern "C" void uml_arm64_gadget_link_exit();
extern "C" void uml_arm64_gadget_branch_register_cached();
extern "C" void uml_arm64_gadget_branch_link_register_cached();
extern "C" void uml_arm64_gadget_return_cached();

namespace {

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

std::uintptr_t gadget_address(void (*gadget)())
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

/* Laid out as the executor's link record: the unlinked stream is one
 * link_exit word, which takes the PC from the BlockLink behind it. */
struct LinkTarget final {
	explicit LinkTarget(std::uint64_t pc) noexcept
		: fallback_stream{gadget_address(uml_arm64_gadget_link_exit)},
		  link{
			.stream = &fallback_stream,
			.instruction_count = 0,
			.guest_pc = pc,
		  }
	{
	}

	void resolve(const gadgets::BuildResult &target) noexcept
	{
		link.stream = target.program.words().data();
		link.instruction_count = target.program.instruction_count();
	}

	void reset() noexcept
	{
		link.stream = &fallback_stream;
		link.instruction_count = 0;
	}

	std::uintptr_t fallback_stream;
	gadgets::BlockLink link;
};
static_assert(offsetof(LinkTarget, link) == sizeof(std::uintptr_t));

gadgets::BuildResult build_target(std::uint64_t pc, std::uint16_t value)
{
	const std::array instructions{
		0xd280'0000U | (static_cast<std::uint32_t>(value) << 5),
		0xd400'0001U,
	};
	return gadgets::Builder{}.build(pc, instructions);
}

/* Cached calls also name their return block's link: the return ring pushes
 * it, and the call reads the return address it sets out of it. So the fixture
 * resolves each direct target to an unlinked record of its own, as the
 * executor does. */
const gadgets::BlockLink *unlinked(std::uint64_t pc)
{
	static std::map<std::uint64_t, LinkTarget> records;
	return &records.try_emplace(pc, pc).first->second.link;
}

gadgets::BuildResult build_cached(std::uint64_t pc,
	std::uint32_t instruction, gadgets::IndirectLink &site)
{
	const std::array instructions{instruction};
	return gadgets::Builder{}.build(
		pc, instructions,
		[](std::uint64_t target) { return unlinked(target); },
		std::pmr::get_default_resource(),
		[&](std::uint64_t source_pc) {
			require(source_pc == pc);
			return &site;
		});
}

void initialize(gadgets::IndirectLink &site, const LinkTarget &empty)
{
	for (auto &entry : site.entries) {
		entry.guest_pc = ~std::uint64_t{};
		entry.link = &empty.link;
	}
}

std::uint32_t branch_register(unsigned source)
{
	return 0xd61f'0000U | (source << 5);
}

std::uint32_t branch_link_register(unsigned source)
{
	return 0xd63f'0000U | (source << 5);
}

std::uint32_t return_register(unsigned source)
{
	return 0xd65f'0000U | (source << 5);
}

} // namespace

int main()
{
	constexpr std::uint64_t source_pc = 0x4000;
	LinkTarget empty{~std::uint64_t{}};

	/* A cold site exits exactly once and reports its private cache cell to the
	 * executor without dereferencing either nonmatching entry. */
	{
		gadgets::IndirectLink site{};
		initialize(site, empty);
		auto source = build_cached(
			source_pc, branch_register(5), site);
		require(source.program.words()[0] ==
			gadget_address(uml_arm64_gadget_branch_register_cached));
		arm64::CpuState cpu{};
		cpu.registers[5] = 0x5000;
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(source.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(cpu.pc == 0x5000);
		require(cpu.gadget_indirect_miss == &site);
		require(executed == 1);
	}

	/* Both ways chain directly into their translated target and preserve the
	 * combined instruction count. */
	{
		auto first_target = build_target(0x5000, 41);
		auto second_target = build_target(0x6000, 42);
		LinkTarget first{0x5000};
		LinkTarget second{0x6000};
		first.resolve(first_target);
		second.resolve(second_target);
		gadgets::IndirectLink site{};
		site.entries[0] = {first.link.guest_pc, &first.link};
		site.entries[1] = {second.link.guest_pc, &second.link};
		auto source = build_cached(
			source_pc, branch_register(5), site);
		for (const auto &[target, value] :
		     std::array<std::pair<std::uint64_t, std::uint64_t>, 2>{
			     {{0x5000, 41}, {0x6000, 42}}}) {
			arm64::CpuState cpu{};
			cpu.registers[5] = target;
			::uml::memory::Tlb tlb;
			std::size_t executed = 0;
			require(gadgets::execute(source.program, cpu, tlb, executed) ==
				::uml::cpu::EngineEvent::system_call);
			require(cpu.registers[0] == value);
			require(cpu.pc == target + 8);
			require(cpu.gadget_indirect_miss == nullptr);
			require(executed == 3);
		}

		/* Target eviction resets the stable BlockLink, so a still-matching PIC
		 * entry takes the safe executor fallback rather than a stale stream. */
		second.reset();
		arm64::CpuState cpu{};
		cpu.registers[5] = 0x6000;
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(source.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(cpu.pc == 0x6000);
		require(cpu.gadget_indirect_miss == nullptr);
		require(executed == 1);
	}

	/* BLR specializes the return address at translation and RET can use an
	 * arbitrary architectural source register while taking the same PIC path. */
	{
		auto target_program = build_target(0x7000, 77);
		LinkTarget target{0x7000};
		target.resolve(target_program);
		gadgets::IndirectLink site{};
		initialize(site, empty);
		site.entries[0] = {target.link.guest_pc, &target.link};

		auto call = build_cached(
			source_pc, branch_link_register(9), site);
		require(call.program.words()[0] ==
			gadget_address(uml_arm64_gadget_branch_link_register_cached));
		arm64::CpuState call_cpu{};
		call_cpu.registers[9] = 0x7000;
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(call.program, call_cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(call_cpu.registers[30] == source_pc + 4);
		require(call_cpu.registers[0] == 77);
		require(executed == 3);

		auto ret = build_cached(
			source_pc, return_register(12), site);
		require(ret.program.words()[0] ==
			gadget_address(uml_arm64_gadget_return_cached));
		arm64::CpuState return_cpu{};
		return_cpu.registers[12] = 0x7000;
		executed = 0;
		require(gadgets::execute(ret.program, return_cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(return_cpu.registers[0] == 77);
		require(executed == 3);
	}
}
