// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_horizontal_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>

namespace test = arm64_floating_horizontal_test;

namespace {

namespace gadgets = uml::cpu::arm64::gadgets;

extern "C" {
#define DECLARE_SCALAR(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_d();
#define DECLARE_VECTOR(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_4h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_8h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_2s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_4s(); \
	void uml_arm64_gadget_floating_horizontal_##name##_2d();
#define DECLARE_BINARY(name) \
	DECLARE_SCALAR(name##_scalar) \
	DECLARE_VECTOR(name##_vector)
	DECLARE_BINARY(fabd)
	DECLARE_BINARY(fabd_checked)
DECLARE_BINARY(fmax)
DECLARE_BINARY(fmaxnm)
DECLARE_BINARY(fmin)
DECLARE_BINARY(fminnm)
DECLARE_BINARY(fmax_ah)
DECLARE_BINARY(fmaxnm_ah)
DECLARE_BINARY(fmin_ah)
DECLARE_BINARY(fminnm_ah)
#undef DECLARE_BINARY
#define DECLARE_PAIR(name) \
	DECLARE_SCALAR(name##_scalar) \
	DECLARE_VECTOR(name##_vector)
	DECLARE_PAIR(faddp)
	DECLARE_PAIR(faddp_checked)
DECLARE_PAIR(fmaxp)
DECLARE_PAIR(fmaxnmp)
DECLARE_PAIR(fminp)
DECLARE_PAIR(fminnmp)
DECLARE_PAIR(fmaxp_ah)
DECLARE_PAIR(fmaxnmp_ah)
DECLARE_PAIR(fminp_ah)
DECLARE_PAIR(fminnmp_ah)
#undef DECLARE_PAIR
#define DECLARE_REDUCE(name) \
	void uml_arm64_gadget_floating_horizontal_##name##_4h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_8h(); \
	void uml_arm64_gadget_floating_horizontal_##name##_4s();
DECLARE_REDUCE(fmaxv)
DECLARE_REDUCE(fmaxnmv)
DECLARE_REDUCE(fminv)
DECLARE_REDUCE(fminnmv)
DECLARE_REDUCE(fmaxv_ah)
DECLARE_REDUCE(fmaxnmv_ah)
DECLARE_REDUCE(fminv_ah)
DECLARE_REDUCE(fminnmv_ah)
#undef DECLARE_REDUCE
#undef DECLARE_VECTOR
#undef DECLARE_SCALAR
}

template<typename Function>
[[nodiscard]] std::uintptr_t address(Function function)
{
	return reinterpret_cast<std::uintptr_t>(function);
}

[[nodiscard]] std::uintptr_t entry_for_afp(
	const test::Form &form, bool afp)
{
	const unsigned scalar_shape = test::scalar_shape(form.width);
	const unsigned vector_shape =
		test::vector_shape(form.width, form.vector_bytes);
#define SCALAR(name) std::array<std::uintptr_t, 3>{ \
	address(uml_arm64_gadget_floating_horizontal_##name##_h), \
	address(uml_arm64_gadget_floating_horizontal_##name##_s), \
	address(uml_arm64_gadget_floating_horizontal_##name##_d)}
#define VECTOR(name) std::array<std::uintptr_t, 5>{ \
	address(uml_arm64_gadget_floating_horizontal_##name##_4h), \
	address(uml_arm64_gadget_floating_horizontal_##name##_8h), \
	address(uml_arm64_gadget_floating_horizontal_##name##_2s), \
	address(uml_arm64_gadget_floating_horizontal_##name##_4s), \
	address(uml_arm64_gadget_floating_horizontal_##name##_2d)}
#define REDUCE(name) std::array<std::uintptr_t, 3>{ \
	address(uml_arm64_gadget_floating_horizontal_##name##_4h), \
	address(uml_arm64_gadget_floating_horizontal_##name##_8h), \
	address(uml_arm64_gadget_floating_horizontal_##name##_4s)}
	static const std::array scalar_binary{
		SCALAR(fabd_scalar), std::array<std::uintptr_t, 3>{},
		SCALAR(fmax_scalar), SCALAR(fmaxnm_scalar),
		SCALAR(fmin_scalar), SCALAR(fminnm_scalar)};
	static const std::array vector_binary{
		VECTOR(fabd_vector), std::array<std::uintptr_t, 5>{},
		VECTOR(fmax_vector), VECTOR(fmaxnm_vector),
		VECTOR(fmin_vector), VECTOR(fminnm_vector)};
	static const std::array scalar_pair{
		std::array<std::uintptr_t, 3>{}, SCALAR(faddp_scalar),
		SCALAR(fmaxp_scalar), SCALAR(fmaxnmp_scalar),
		SCALAR(fminp_scalar), SCALAR(fminnmp_scalar)};
	static const std::array vector_pair{
		std::array<std::uintptr_t, 5>{}, VECTOR(faddp_vector),
		VECTOR(fmaxp_vector), VECTOR(fmaxnmp_vector),
		VECTOR(fminp_vector), VECTOR(fminnmp_vector)};
	static const std::array reduction{
		std::array<std::uintptr_t, 3>{},
		std::array<std::uintptr_t, 3>{},
		REDUCE(fmaxv), REDUCE(fmaxnmv),
			REDUCE(fminv), REDUCE(fminnmv)};
	if (!afp && form.operation <= test::Operation::add) {
		static const std::array scalar{
			SCALAR(fabd_checked_scalar),
			SCALAR(faddp_checked_scalar)};
		static const std::array vector{
			VECTOR(fabd_checked_vector),
			VECTOR(faddp_checked_vector)};
		const unsigned operation =
			static_cast<unsigned>(form.operation);
		if (form.kind == test::Kind::scalar_binary ||
		    form.kind == test::Kind::scalar_pairwise)
			return scalar[operation][scalar_shape];
		return vector[operation][vector_shape];
	}
	if (!afp && form.operation >= test::Operation::maximum &&
	    form.operation <= test::Operation::minimum_number) {
		static const std::array scalar_ah{
			std::array{SCALAR(fmax_ah_scalar),
				SCALAR(fmaxp_ah_scalar)},
			std::array{SCALAR(fmaxnm_ah_scalar),
				SCALAR(fmaxnmp_ah_scalar)},
			std::array{SCALAR(fmin_ah_scalar),
				SCALAR(fminp_ah_scalar)},
			std::array{SCALAR(fminnm_ah_scalar),
				SCALAR(fminnmp_ah_scalar)}};
		static const std::array vector_ah{
			std::array{VECTOR(fmax_ah_vector),
				VECTOR(fmaxp_ah_vector)},
			std::array{VECTOR(fmaxnm_ah_vector),
				VECTOR(fmaxnmp_ah_vector)},
			std::array{VECTOR(fmin_ah_vector),
				VECTOR(fminp_ah_vector)},
			std::array{VECTOR(fminnm_ah_vector),
				VECTOR(fminnmp_ah_vector)}};
		static const std::array reduction_ah{
			REDUCE(fmaxv_ah), REDUCE(fmaxnmv_ah),
			REDUCE(fminv_ah), REDUCE(fminnmv_ah)};
		const unsigned operation =
			static_cast<unsigned>(form.operation) -
			static_cast<unsigned>(test::Operation::maximum);
		const unsigned pair =
			form.kind == test::Kind::scalar_pairwise ||
			form.kind == test::Kind::vector_pairwise;
		if (form.kind == test::Kind::scalar_binary)
			return scalar_ah[operation][pair][scalar_shape];
		if (form.kind == test::Kind::vector_binary)
			return vector_ah[operation][pair][vector_shape];
		if (form.kind == test::Kind::scalar_pairwise)
			return scalar_ah[operation][pair][scalar_shape];
		if (form.kind == test::Kind::vector_pairwise)
			return vector_ah[operation][pair][vector_shape];
		const unsigned shape = form.width == 32U ? 2U :
			form.vector_bytes == 16U ? 1U : 0U;
		return reduction_ah[operation][shape];
	}
	const unsigned operation =
		static_cast<unsigned>(form.operation);
	if (form.kind == test::Kind::scalar_binary)
		return scalar_binary[operation][scalar_shape];
	if (form.kind == test::Kind::vector_binary)
		return vector_binary[operation][vector_shape];
	if (form.kind == test::Kind::scalar_pairwise)
		return scalar_pair[operation][scalar_shape];
	if (form.kind == test::Kind::vector_pairwise)
		return vector_pair[operation][vector_shape];
	const unsigned shape = form.width == 32U ? 2U :
		form.vector_bytes == 16U ? 1U : 0U;
	return reduction[operation][shape];
#undef REDUCE
#undef VECTOR
#undef SCALAR
}

[[nodiscard]] std::uintptr_t vector_offset(unsigned reg)
{
	return offsetof(test::arm64::CpuState, vectors) +
		reg * sizeof(test::arm64::VectorRegister);
}

[[nodiscard]] bool checked_entry(const test::Form &form)
{
	return !test::engine_tests::host_supports(
			test::engine_tests::NativeRequirements::features(
				::uml::cpu::ArmFeature::afp)) &&
		form.operation <= test::Operation::minimum_number;
}

struct BodyShape final {
	unsigned instructions{};
	unsigned conditional_branches{};
	bool terminal{};
};

[[nodiscard]] BodyShape body_shape(std::uintptr_t entry)
{
	const auto *code = reinterpret_cast<const std::uint32_t *>(entry);
	BodyShape result{};
	for (unsigned index = 0; index < 40U; ++index) {
		const auto instruction = code[index];
		if ((instruction & 0xffff'fc1fU) == 0xd61f'0000U) {
			result.terminal = true;
			break;
		}
		if ((instruction & 0xff00'0010U) == 0x5400'0000U ||
		    (instruction & 0x7e00'0000U) == 0x3400'0000U ||
		    (instruction & 0x7e00'0000U) == 0x3600'0000U)
			++result.conditional_branches;
		++result.instructions;
	}
	return result;
}

void direct_operands_live_symbols_and_hot_shapes()
{
	std::set<std::uintptr_t> selected;
	test::for_each_form([&](const test::Form &form) {
		for (const auto registers : test::register_cases) {
			const auto built = gadgets::Builder{}.build(
				test::engine_tests::engine_test_code_base,
				std::array{test::instruction(form, registers)});
			test::common::require(
				built.complete && built.program.words().size() >= 2U);
			const bool afp = test::engine_tests::host_supports(
				test::engine_tests::NativeRequirements::features(
					::uml::cpu::ArmFeature::afp));
			const auto entry = entry_for_afp(form, afp);
			test::common::require(built.program.words()[0] == entry);
			test::common::require(
				built.program.words().size() >= 3U);
			test::common::require(built.program.words()[1] ==
				vector_offset(registers.destination));
			test::common::require(built.program.words()[2] ==
				vector_offset(registers.first));
			const bool uses_second =
				form.kind == test::Kind::scalar_binary ||
				form.kind == test::Kind::vector_binary ||
				form.kind == test::Kind::vector_pairwise;
			if (uses_second) {
				test::common::require(
					built.program.words().size() >= 4U);
				test::common::require(built.program.words()[3] ==
					vector_offset(registers.second));
			}
			if (checked_entry(form)) {
				const unsigned descriptor_index =
					uses_second ? 4U : 3U;
				test::common::require(
					built.program.words().size() >
						descriptor_index);
				test::common::require(
					built.program.words()[descriptor_index] ==
					test::detail::floating_horizontal_operand(
						test::instruction(form, registers),
						test::detail::
							decode_floating_horizontal(
								test::instruction(
									form,
									registers))));
			}
			selected.insert(entry);
		}
	});
	test::common::require(selected.size() == 92U);
	std::set<std::uintptr_t> audited;
	std::map<std::uintptr_t, std::pair<bool, bool>> properties;
	test::for_each_form([&](const test::Form &form) {
		for (const bool afp : {true, false}) {
			const auto entry = entry_for_afp(form, afp);
			audited.insert(entry);
			properties[entry] = {
				form.kind == test::Kind::scalar_binary ||
					form.kind ==
						test::Kind::scalar_pairwise,
				!afp};
		}
	});
	test::common::require(audited.size() == 184U);
	for (const auto entry : audited) {
		const auto shape = body_shape(entry);
		test::common::require(shape.terminal);
		const bool checked = properties[entry].second;
		test::common::require(shape.conditional_branches <=
			(checked ? 2U : 1U));
		test::common::require(shape.instructions <=
			(checked ? 13U : 10U));
	}
}

} // namespace

int main()
{
	direct_operands_live_symbols_and_hot_shapes();
}
