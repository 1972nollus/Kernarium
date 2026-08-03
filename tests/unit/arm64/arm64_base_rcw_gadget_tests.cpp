// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_rcw_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <set>

extern "C" {
#define UML_DECLARE_RCW_SCALAR_FULL(operation, soft, ordering) \
	void uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rr(); \
	void uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rz(); \
	void uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zr(); \
	void uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zz();
#define UML_DECLARE_RCW_SCALAR_LIVE(operation, soft, ordering) \
	void uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rr(); \
	void uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zr();
#define UML_DECLARE_RCW_SCALAR_ORDERS(operation, soft) \
	UML_DECLARE_RCW_SCALAR_FULL(operation, soft, relaxed) \
	UML_DECLARE_RCW_SCALAR_FULL(operation, soft, release) \
	UML_DECLARE_RCW_SCALAR_LIVE(operation, soft, acquire) \
	UML_DECLARE_RCW_SCALAR_LIVE(operation, soft, acquire_release)
#define UML_DECLARE_RCW_SCALAR_SOFT(operation) \
	UML_DECLARE_RCW_SCALAR_ORDERS(operation, hard) \
	UML_DECLARE_RCW_SCALAR_ORDERS(operation, soft)
UML_DECLARE_RCW_SCALAR_SOFT(cas)
UML_DECLARE_RCW_SCALAR_SOFT(bic)
UML_DECLARE_RCW_SCALAR_SOFT(swp)
UML_DECLARE_RCW_SCALAR_SOFT(orr)
#undef UML_DECLARE_RCW_SCALAR_SOFT
#undef UML_DECLARE_RCW_SCALAR_ORDERS
#undef UML_DECLARE_RCW_SCALAR_LIVE
#undef UML_DECLARE_RCW_SCALAR_FULL

#define UML_DECLARE_RCW_PAIR_CAS_MODES(soft, ordering) \
	void uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_rr(); \
	void uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_rz(); \
	void uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_zr(); \
	void uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_zz();
#define UML_DECLARE_RCW_PAIR_CAS_ORDERS(soft) \
	UML_DECLARE_RCW_PAIR_CAS_MODES(soft, relaxed) \
	UML_DECLARE_RCW_PAIR_CAS_MODES(soft, release) \
	UML_DECLARE_RCW_PAIR_CAS_MODES(soft, acquire) \
	UML_DECLARE_RCW_PAIR_CAS_MODES(soft, acquire_release)
UML_DECLARE_RCW_PAIR_CAS_ORDERS(hard)
UML_DECLARE_RCW_PAIR_CAS_ORDERS(soft)
#undef UML_DECLARE_RCW_PAIR_CAS_ORDERS
#undef UML_DECLARE_RCW_PAIR_CAS_MODES

#define UML_DECLARE_RCW_PAIR_MODIFY(operation, soft) \
	void uml_arm64_gadget_rcw_pair_##operation##_##soft##_relaxed(); \
	void uml_arm64_gadget_rcw_pair_##operation##_##soft##_release(); \
	void uml_arm64_gadget_rcw_pair_##operation##_##soft##_acquire(); \
	void uml_arm64_gadget_rcw_pair_##operation##_##soft##_acquire_release();
#define UML_DECLARE_RCW_PAIR_MODIFY_SOFT(operation) \
	UML_DECLARE_RCW_PAIR_MODIFY(operation, hard) \
	UML_DECLARE_RCW_PAIR_MODIFY(operation, soft)
UML_DECLARE_RCW_PAIR_MODIFY_SOFT(bic)
UML_DECLARE_RCW_PAIR_MODIFY_SOFT(swp)
UML_DECLARE_RCW_PAIR_MODIFY_SOFT(orr)
#undef UML_DECLARE_RCW_PAIR_MODIFY_SOFT
#undef UML_DECLARE_RCW_PAIR_MODIFY

void uml_arm64_gadget_illegal();
void uml_arm64_gadget_atomic_sp_alignment_write();
}

namespace rcw = arm64_base_rcw_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using Gadget = void (*)();
using RegisterModes = std::array<Gadget, 4>;
using OrderingModes = std::array<RegisterModes, 4>;
using SoftModes = std::array<OrderingModes, 2>;
using ScalarOperations = std::array<SoftModes, 4>;

#define UML_RCW_SCALAR_FULL(operation, soft, ordering) RegisterModes{ \
	uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rr, \
	uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rz, \
	uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zr, \
	uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zz}
#define UML_RCW_SCALAR_ACQUIRE(operation, soft, ordering, suppressed) \
	RegisterModes{ \
	uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_rr, \
	uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##suppressed##_rz, \
	uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##ordering##_zr, \
	uml_arm64_gadget_rcw_scalar_##operation##_##soft##_##suppressed##_zz}
#define UML_RCW_SCALAR_ORDERS(operation, soft) OrderingModes{ \
	UML_RCW_SCALAR_FULL(operation, soft, relaxed), \
	UML_RCW_SCALAR_FULL(operation, soft, release), \
	UML_RCW_SCALAR_ACQUIRE(operation, soft, acquire, relaxed), \
	UML_RCW_SCALAR_ACQUIRE( \
		operation, soft, acquire_release, release)}
#define UML_RCW_SCALAR_SOFT(operation) SoftModes{ \
	UML_RCW_SCALAR_ORDERS(operation, hard), \
	UML_RCW_SCALAR_ORDERS(operation, soft)}
constexpr ScalarOperations scalar_gadgets{
	UML_RCW_SCALAR_SOFT(cas),
	UML_RCW_SCALAR_SOFT(bic),
	UML_RCW_SCALAR_SOFT(swp),
	UML_RCW_SCALAR_SOFT(orr),
};
#undef UML_RCW_SCALAR_SOFT
#undef UML_RCW_SCALAR_ORDERS
#undef UML_RCW_SCALAR_ACQUIRE
#undef UML_RCW_SCALAR_FULL

using PairCasSoft = std::array<OrderingModes, 2>;
#define UML_RCW_PAIR_CAS_MODES(soft, ordering) RegisterModes{ \
	uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_rr, \
	uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_rz, \
	uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_zr, \
	uml_arm64_gadget_rcw_pair_cas_##soft##_##ordering##_zz}
#define UML_RCW_PAIR_CAS_ORDERS(soft) OrderingModes{ \
	UML_RCW_PAIR_CAS_MODES(soft, relaxed), \
	UML_RCW_PAIR_CAS_MODES(soft, release), \
	UML_RCW_PAIR_CAS_MODES(soft, acquire), \
	UML_RCW_PAIR_CAS_MODES(soft, acquire_release)}
constexpr PairCasSoft pair_cas_gadgets{
	UML_RCW_PAIR_CAS_ORDERS(hard),
	UML_RCW_PAIR_CAS_ORDERS(soft),
};
#undef UML_RCW_PAIR_CAS_ORDERS
#undef UML_RCW_PAIR_CAS_MODES

using PairModifyOrder = std::array<Gadget, 4>;
using PairModifySoft = std::array<PairModifyOrder, 2>;
using PairModifyOperations = std::array<PairModifySoft, 3>;
#define UML_RCW_PAIR_MODIFY_ORDERS(operation, soft) PairModifyOrder{ \
	uml_arm64_gadget_rcw_pair_##operation##_##soft##_relaxed, \
	uml_arm64_gadget_rcw_pair_##operation##_##soft##_release, \
	uml_arm64_gadget_rcw_pair_##operation##_##soft##_acquire, \
	uml_arm64_gadget_rcw_pair_##operation##_##soft##_acquire_release}
#define UML_RCW_PAIR_MODIFY_SOFT(operation) PairModifySoft{ \
	UML_RCW_PAIR_MODIFY_ORDERS(operation, hard), \
	UML_RCW_PAIR_MODIFY_ORDERS(operation, soft)}
constexpr PairModifyOperations pair_modify_gadgets{
	UML_RCW_PAIR_MODIFY_SOFT(bic),
	UML_RCW_PAIR_MODIFY_SOFT(swp),
	UML_RCW_PAIR_MODIFY_SOFT(orr),
};
#undef UML_RCW_PAIR_MODIFY_SOFT
#undef UML_RCW_PAIR_MODIFY_ORDERS

[[nodiscard]] std::uintptr_t address_of(Gadget gadget)
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	const std::array stream{
		instruction,
		atomic::terminal_system_call,
	};
	return gadgets::Builder{}.build(
		engine::engine_test_code_base, stream);
}

void require_stream(const gadgets::BuildResult &built, Gadget expected,
	std::uintptr_t operand, unsigned rn)
{
	const auto words = built.program.words();
	const std::size_t selected = rn == 31U ? 1U : 0U;
	rcw::require(built.complete && built.program.instruction_count() == 2U,
		"RCW production translation completeness");
	rcw::require(words.size() >= selected + 4U &&
			words[selected] == address_of(expected),
		"RCW translation-selected gadget");
	if (rn == 31U)
		rcw::require(words[0] == address_of(
			uml_arm64_gadget_atomic_sp_alignment_write),
			"RCW SP alignment prefix");
	rcw::require(words[selected + 1U] == operand,
		"RCW translation-resolved direct CpuState offsets");
	rcw::require(words[selected + 2U] == engine::engine_test_code_base,
		"RCW stream fault PC");
}

void scalar_translation_specialization()
{
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	for (const auto &operation : scalar_gadgets)
		for (const auto &softness : operation)
			for (const auto &ordering : softness)
				for (const auto gadget : ordering)
					expected.insert(address_of(gadget));

	for (unsigned operation = 0; operation < 4; ++operation) {
		for (const bool soft : {false, true}) {
			for (const bool acquire : {false, true}) {
				for (const bool release : {false, true}) {
					for (const unsigned rn : {2U, 31U}) {
						for (const unsigned source : {3U, 31U}) {
							for (const unsigned result : {4U, 31U}) {
								const unsigned ordering =
									(acquire && result != 31U ? 2U : 0U) |
									(release ? 1U : 0U);
								const unsigned registers =
									(source == 31U ? 2U : 0U) |
									(result == 31U ? 1U : 0U);
								const auto expected_gadget =
									scalar_gadgets[operation]
										[static_cast<unsigned>(soft)]
										[ordering][registers];
								const auto built = build(
									rcw::scalar_instruction(
										static_cast<rcw::Operation>(operation),
										soft, acquire, release, rn, source,
										result));
								require_stream(built, expected_gadget,
									rcw::scalar_stream_operand(
										rn, source, result),
									rn);
								selected.insert(
									address_of(expected_gadget));
							}
						}
					}
				}
			}
		}
	}
	rcw::require(selected == expected && selected.size() == 96U,
		"scalar RCW live specialized gadget set");
}

void pair_translation_specialization()
{
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	for (const auto &softness : pair_cas_gadgets)
		for (const auto &ordering : softness)
			for (const auto gadget : ordering)
				expected.insert(address_of(gadget));
	for (const auto &operation : pair_modify_gadgets)
		for (const auto &softness : operation)
			for (const auto gadget : softness)
				expected.insert(address_of(gadget));

	for (const bool soft : {false, true}) {
		for (const bool acquire : {false, true}) {
			for (const bool release : {false, true}) {
				const unsigned ordering =
					(acquire ? 2U : 0U) | (release ? 1U : 0U);
				for (const unsigned rn : {6U, 31U}) {
					for (const unsigned replacement : {2U, 30U}) {
						for (const unsigned compare : {4U, 30U}) {
							const unsigned registers =
								(replacement == 30U ? 2U : 0U) |
								(compare == 30U ? 1U : 0U);
							const auto expected_gadget =
								pair_cas_gadgets
									[static_cast<unsigned>(soft)]
									[ordering][registers];
							const auto built = build(
								rcw::pair_compare_swap(
									soft, acquire, release, rn, compare,
									replacement));
							require_stream(built, expected_gadget,
								rcw::pair_stream_operand(
									rn, replacement, compare),
								rn);
							selected.insert(address_of(expected_gadget));
						}
					}
				}
				for (unsigned operation = 1; operation <= 3; ++operation) {
					const auto expected_gadget =
						pair_modify_gadgets[operation - 1U]
							[static_cast<unsigned>(soft)][ordering];
					for (const unsigned rn : {6U, 31U}) {
						const auto built = build(rcw::pair_modify_raw(
							soft, acquire, release, operation,
							rn, 3, 4));
						require_stream(built, expected_gadget,
							rcw::pair_stream_operand(rn, 3, 4), rn);
						selected.insert(address_of(expected_gadget));
					}
				}
			}
		}
	}
	rcw::require(selected == expected && selected.size() == 56U,
		"pair RCW live specialized gadget set");
}

void invalid_words_fold_to_common_illegal()
{
	const auto illegal = address_of(uml_arm64_gadget_illegal);
	for (const auto instruction : {
	     rcw::pair_compare_swap(false, false, false, 6, 3, 4),
	     rcw::pair_compare_swap(false, false, false, 6, 2, 5),
	     rcw::pair_modify_raw(false, false, false, 1, 6, 31, 4),
	     rcw::pair_modify_raw(false, false, false, 1, 6, 3, 31),
	     rcw::pair_modify_raw(false, false, false, 1, 6, 3, 3),
	     rcw::scalar_modify_raw(false, false, false, 7, 2, 3, 4),
	     rcw::pair_modify_raw(false, false, false, 7, 6, 3, 4),
	     rcw::pair_modify_raw(true, false, false, 7, 6, 3, 4),
	     rcw::pair_modify_raw(true, false, false, 0, 6, 3, 4),
	}) {
		const auto built = build(instruction);
		rcw::require(
			built.complete && !built.program.words().empty() &&
				built.program.words().front() == illegal,
			"invalid RCW word did not select common illegal gadget");
	}
}

} // namespace

int main()
{
	scalar_translation_specialization();
	pair_translation_specialization();
	invalid_words_fold_to_common_illegal();
}
