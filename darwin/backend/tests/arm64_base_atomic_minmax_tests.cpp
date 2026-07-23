// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_test_support.hpp"

#include <array>

namespace atomic_test = arm64_atomic_test;
namespace test = arm64_load_store_test;

int main()
{
	constexpr std::array operations{4U, 5U, 6U, 7U};
	atomic_test::interpreter_matrix(operations);
	atomic_test::interpreter_aliases_and_register_modes(operations);
	atomic_test::interpreter_register_overlaps(operations);
	atomic_test::interpreter_alignment_faults_and_reservations(operations);

	test::StaticMapping mapping{0x3e0'0000, 179,
		"/private/tmp/uml-base-atomic-minmax.XXXXXX"};
	const auto address = mapping.guest() + 4 * test::StaticMapping::page_size;
	atomic_test::gadget_matrix(operations, mapping, address);
	atomic_test::gadget_aliases_and_register_modes(operations, mapping, address);
	atomic_test::gadget_register_overlaps(operations, mapping, address);
	atomic_test::gadget_alignment_faults(operations, mapping, address);
}
