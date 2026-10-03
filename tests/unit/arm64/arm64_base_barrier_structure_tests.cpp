// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_engine_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace uml::emulator::tests::base_barrier {

namespace arm64 = uml::cpu::arm64;

inline constexpr std::uint32_t dsb_base = 0xd503'309fU;
inline constexpr std::uint32_t dmb_base = 0xd503'30bfU;
inline constexpr std::uint32_t isb_base = 0xd503'30dfU;
inline constexpr std::uint32_t dsb_nxs_base = 0xd503'323fU;
inline constexpr std::uint32_t sb_base = 0xd503'30ffU;
inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

using uml::testing::fail;
using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t encode_standard(
	std::uint32_t base, unsigned option) noexcept
{
	return base | ((option & 15U) << 8U);
}

[[nodiscard]] constexpr std::uint32_t encode_nxs(unsigned scope) noexcept
{
	return dsb_nxs_base | ((scope & 3U) << 10U);
}

} // namespace uml::emulator::tests::base_barrier

namespace a64 = uml::testing::a64;

namespace uml::emulator::tests::base_barrier {
namespace {

namespace gadgets = arm64::gadgets;

extern "C" {
#define DECLARE_STANDARD_BARRIER(kind, option) \
	void uml_arm64_gadget_##kind##_##option();
#define DECLARE_STANDARD_OPTION(option) \
	DECLARE_STANDARD_BARRIER(dmb, option) \
	DECLARE_STANDARD_BARRIER(dsb, option)
DECLARE_STANDARD_OPTION(0)
DECLARE_STANDARD_OPTION(1)
DECLARE_STANDARD_OPTION(2)
DECLARE_STANDARD_OPTION(3)
DECLARE_STANDARD_OPTION(4)
DECLARE_STANDARD_OPTION(5)
DECLARE_STANDARD_OPTION(6)
DECLARE_STANDARD_OPTION(7)
DECLARE_STANDARD_OPTION(8)
DECLARE_STANDARD_OPTION(9)
DECLARE_STANDARD_OPTION(10)
DECLARE_STANDARD_OPTION(11)
DECLARE_STANDARD_OPTION(12)
DECLARE_STANDARD_OPTION(13)
DECLARE_STANDARD_OPTION(14)
DECLARE_STANDARD_OPTION(15)
#undef DECLARE_STANDARD_OPTION
#undef DECLARE_STANDARD_BARRIER
void uml_arm64_gadget_isb();
void uml_arm64_gadget_speculation_barrier();
void uml_arm64_gadget_speculation_barrier_fallback();
void uml_arm64_gadget_illegal();
}

template<typename Function>
[[nodiscard]] std::uintptr_t address(Function function)
{
	return reinterpret_cast<std::uintptr_t>(function);
}

#define BARRIER_ADDRESS(kind, option) \
	address(uml_arm64_gadget_##kind##_##option)

const std::array<std::uintptr_t, 16> dmb_gadgets{
	BARRIER_ADDRESS(dmb, 0), BARRIER_ADDRESS(dmb, 1),
	BARRIER_ADDRESS(dmb, 2), BARRIER_ADDRESS(dmb, 3),
	BARRIER_ADDRESS(dmb, 4), BARRIER_ADDRESS(dmb, 5),
	BARRIER_ADDRESS(dmb, 6), BARRIER_ADDRESS(dmb, 7),
	BARRIER_ADDRESS(dmb, 8), BARRIER_ADDRESS(dmb, 9),
	BARRIER_ADDRESS(dmb, 10), BARRIER_ADDRESS(dmb, 11),
	BARRIER_ADDRESS(dmb, 12), BARRIER_ADDRESS(dmb, 13),
	BARRIER_ADDRESS(dmb, 14), BARRIER_ADDRESS(dmb, 15),
};

const std::array<std::uintptr_t, 16> dsb_gadgets{
	BARRIER_ADDRESS(dsb, 0), BARRIER_ADDRESS(dsb, 1),
	BARRIER_ADDRESS(dsb, 2), BARRIER_ADDRESS(dsb, 3),
	BARRIER_ADDRESS(dsb, 4), BARRIER_ADDRESS(dsb, 5),
	BARRIER_ADDRESS(dsb, 6), BARRIER_ADDRESS(dsb, 7),
	BARRIER_ADDRESS(dsb, 8), BARRIER_ADDRESS(dsb, 9),
	BARRIER_ADDRESS(dsb, 10), BARRIER_ADDRESS(dsb, 11),
	BARRIER_ADDRESS(dsb, 12), BARRIER_ADDRESS(dsb, 13),
	BARRIER_ADDRESS(dsb, 14), BARRIER_ADDRESS(dsb, 15),
};

#undef BARRIER_ADDRESS

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	const std::array code{instruction, terminal_system_call};
	return gadgets::Builder{}.build(engine_test_code_base, code, {},
		std::pmr::get_default_resource());
}

[[nodiscard]] unsigned runtime_instruction_count(std::uintptr_t entry)
{
	const auto shape = a64::body_shape(entry, {.window = 16U});
	if (!shape.reached_dispatch)
		fail("barrier gadget did not reach threaded dispatch");
	return shape.instructions + 1U;
}

[[nodiscard]] unsigned conditional_branches(std::uintptr_t entry)
{
	const auto shape = a64::body_shape(entry, {.window = 16U});
	if (!shape.reached_dispatch)
		fail("barrier gadget did not terminate in its inspection window");
	return shape.conditional_branches;
}

void standard_translation()
{
	for (unsigned option = 0; option < 16U; ++option) {
		const auto dmb = build(encode_standard(dmb_base, option));
		const auto dsb = build(encode_standard(dsb_base, option));
		const auto isb = build(encode_standard(isb_base, option));
		require(dmb.complete && dmb.program.words().size() == 3U &&
				dmb.program.words().front() == dmb_gadgets[option],
			"DMB option was not selected at translation");
		require(dsb.complete && dsb.program.words().size() == 3U &&
				dsb.program.words().front() == dsb_gadgets[option],
			"DSB option was not selected at translation");
		require(isb.complete && isb.program.words().size() == 3U &&
				isb.program.words().front() ==
					address(uml_arm64_gadget_isb),
			"ISB option retained runtime decoding");
	}
}

void extension_translation()
{
	/* The virtual CPU implements no FEAT_XS, so DSB nXS is UNDEFINED. */
	for (unsigned scope = 0; scope < 4U; ++scope) {
		const auto program = build(encode_nxs(scope));
		require(program.complete && program.program.words().size() == 2U &&
				program.program.words().front() ==
					address(uml_arm64_gadget_illegal),
			"DSB nXS did not select strict illegal");
	}

	const auto sb = build(sb_base);
	const auto expected_sb = ::uml::platform::apple::probed_host_capabilities().has(ArmFeature::sb) ?
		address(uml_arm64_gadget_speculation_barrier) :
		address(uml_arm64_gadget_speculation_barrier_fallback);
	require(sb.complete && sb.program.words().size() == 3U &&
			sb.program.words().front() == expected_sb,
		"SB host choice was not resolved at translation");

	for (unsigned crm = 1; crm < 16U; ++crm) {
		const auto invalid = build(sb_base | (crm << 8U));
		require(invalid.complete && invalid.program.words().size() == 2U &&
				invalid.program.words().front() ==
					address(uml_arm64_gadget_illegal),
			"unallocated SB option did not select strict illegal");
	}
}

void hot_path_shape()
{
	for (const auto entry : dmb_gadgets) {
		require(conditional_branches(entry) == 0U,
			"DMB hot path retained a conditional branch");
		require(runtime_instruction_count(entry) == 3U,
			"DMB hot path is not one barrier plus dispatch");
	}
	for (const auto entry : dsb_gadgets) {
		require(conditional_branches(entry) == 0U,
			"DSB hot path retained a conditional branch");
		require(runtime_instruction_count(entry) == 3U,
			"DSB hot path is not one barrier plus dispatch");
	}
	require(conditional_branches(address(uml_arm64_gadget_isb)) == 0U &&
			runtime_instruction_count(
				address(uml_arm64_gadget_isb)) == 3U,
		"ISB hot path is not one barrier plus dispatch");
	require(conditional_branches(
			address(uml_arm64_gadget_speculation_barrier)) == 0U &&
			runtime_instruction_count(
				address(uml_arm64_gadget_speculation_barrier)) == 3U,
		"native SB hot path is not one barrier plus dispatch");
	require(conditional_branches(
			address(uml_arm64_gadget_speculation_barrier_fallback)) ==
				0U &&
			runtime_instruction_count(address(
				uml_arm64_gadget_speculation_barrier_fallback)) ==
				4U,
		"portable SB hot path is not two barriers plus dispatch");
}

} // namespace
} // namespace uml::emulator::tests::base_barrier

int main()
{
	uml::emulator::tests::base_barrier::standard_translation();
	uml::emulator::tests::base_barrier::extension_translation();
	uml::emulator::tests::base_barrier::hot_path_shape();
	return 0;
}
