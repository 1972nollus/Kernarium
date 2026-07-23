// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_test_support.hpp"

namespace test = arm64_fp8_test;

namespace {

void architectural_known_encodings()
{
	/* Independent known answers from the Arm encoding diagrams. In
	 * particular, vector FMLALL has U=0 and must not alias FCMLA (U=1). */
	test::common::require(test::multiply(32U, false, 0U) == 0x0e02'c420U);
	test::common::require(test::multiply(32U, false, 1U) == 0x0e42'c420U);
	test::common::require(test::multiply(32U, true, 0U) == 0x2f02'8020U);
}

void conversions()
{
	for (const bool bfloat : {false, true})
		for (const bool source2 : {false, true})
			for (const bool upper : {false, true}) {
				const auto decoded = test::detail::decode_fp8(
					test::convert_up(bfloat, source2, upper, {7, 19, 0}));
				test::common::require(decoded.recognized && decoded.valid);
				test::common::require(decoded.operation == (bfloat ?
					test::detail::Fp8Operation::convert_to_bfloat :
					test::detail::Fp8Operation::convert_to_half));
				test::common::require(decoded.source2 == source2);
				test::common::require(decoded.upper == upper);
				test::common::require(decoded.destination == 7U && decoded.first == 19U);
			}
	for (const unsigned width : {16U, 32U})
		for (const bool upper : {false, true}) {
			const auto decoded = test::detail::decode_fp8(
				test::convert_down(width, upper, {5, 17, 23}));
			test::common::require(decoded.recognized && decoded.valid);
			test::common::require(decoded.operation == (width == 16U ?
				test::detail::Fp8Operation::convert_from_half :
				test::detail::Fp8Operation::convert_from_single));
			test::common::require(decoded.upper == upper);
			test::common::require(decoded.destination == 5U &&
				decoded.first == 17U && decoded.second == 23U);
		}
}

void dot_products()
{
	for (const unsigned width : {16U, 32U})
		for (const bool indexed : {false, true})
			for (const bool wide : {false, true}) {
				const unsigned count = indexed ? (width == 16U ? 8U : 4U) : 1U;
				for (unsigned index = 0; index < count; ++index) {
					const test::Registers registers{9, 13,
						width == 16U && indexed ? 7U : 21U};
					const auto decoded = test::detail::decode_fp8(
						test::dot(width, indexed, wide, index, registers));
					test::common::require(decoded.recognized && decoded.valid);
					test::common::require(decoded.operation == (width == 16U ?
						test::detail::Fp8Operation::dot_half :
						test::detail::Fp8Operation::dot_single));
					test::common::require(decoded.by_element == indexed);
					test::common::require(!indexed || decoded.index == index);
					test::common::require(decoded.second == registers.second);
				}
			}
}

void multiply_and_matrix()
{
	for (const unsigned width : {16U, 32U}) {
		const unsigned selections = width == 16U ? 2U : 4U;
		for (const bool indexed : {false, true})
			for (unsigned selection = 0; selection < selections; ++selection) {
				const unsigned count = indexed ? 16U : 1U;
				for (unsigned index = 0; index < count; ++index) {
					const auto decoded = test::detail::decode_fp8(test::multiply(
						width, indexed, selection, index,
						{3, 11, indexed ? 6U : 27U}));
					test::common::require(decoded.recognized && decoded.valid);
					test::common::require(decoded.operation == (width == 16U ?
						test::detail::Fp8Operation::multiply_half :
						test::detail::Fp8Operation::multiply_single));
					test::common::require(decoded.selection == selection);
					test::common::require(decoded.by_element == indexed);
					test::common::require(!indexed || decoded.index == index);
				}
			}
		const auto decoded = test::detail::decode_fp8(
			test::matrix(width, {4, 8, 12}));
		test::common::require(decoded.recognized && decoded.valid);
		test::common::require(decoded.operation == (width == 16U ?
			test::detail::Fp8Operation::matrix_half :
			test::detail::Fp8Operation::matrix_single));
	}
}

void exact_encoding_constants()
{
	test::common::require(test::convert_up(false, false, false) == 0x2e21'7820U);
	test::common::require(test::convert_up(true, true, true) == 0x6ee1'7820U);
	test::common::require(test::convert_down(16, false) == 0x0e42'f420U);
	test::common::require(test::convert_down(32, true) == 0x4e02'f420U);
	test::common::require(test::dot(16, false, false) == 0x0e42'fc20U);
	test::common::require(test::dot(32, false, false) == 0x0e02'fc20U);
	test::common::require(test::matrix(16) == 0x6e02'ec20U);
	test::common::require(test::matrix(32) == 0x6e82'ec20U);
}

} // namespace

int main()
{
	architectural_known_encodings();
	conversions();
	dot_products();
	multiply_and_matrix();
	exact_encoding_constants();
}
