// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_multiply_test_support.hpp"

#include <cstddef>
#include <cstdio>

namespace uml::backend::tests::base_multiply {
namespace {

[[noreturn]] void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

void encoding_examples()
{
	require(encode(false, 0, 0, false, 2, 3, 1, 7) ==
			0x1b02'0c27U,
		"MADD W encoding mismatch");
	require(encode(true, 0, 0, true, 2, 3, 1, 7) ==
			0x9b02'8c27U,
		"MSUB X encoding mismatch");
	require(encode(true, 0, 1, false, 2, 3, 1, 7) ==
			0x9b22'0c27U,
		"SMADDL encoding mismatch");
	require(encode(true, 0, 2, false, 2, 31, 1, 7) ==
			0x9b42'7c27U,
		"SMULH encoding mismatch");
	require(encode(true, 0, 5, true, 2, 3, 1, 7) ==
			0x9ba2'8c27U,
		"UMSUBL encoding mismatch");
	require(encode(true, 0, 6, false, 2, 31, 1, 7) ==
			0x9bc2'7c27U,
		"UMULH encoding mismatch");
}

void every_reserved_decode_partition(EngineHarness &harness)
{
	std::size_t valid_partitions = 0;
	std::size_t other_feature_partitions = 0;
	std::size_t reserved_partitions = 0;
	for (unsigned sf = 0; sf < 2; ++sf)
		for (unsigned op54 = 0; op54 < 4; ++op54)
			for (unsigned operation = 0; operation < 8; ++operation)
				for (unsigned subtract = 0; subtract < 2; ++subtract)
					for (unsigned addend = 0; addend < 32; ++addend) {
						const unsigned left =
							(addend * 3U + operation +
							 op54) & 31U;
						const unsigned right =
							(addend * 7U + subtract +
							 sf) & 31U;
						const unsigned destination =
							(addend * 13U + operation +
							 subtract) & 31U;
						const auto instruction = encode(
							sf != 0U, op54, operation,
							subtract != 0U, right, addend,
							left, destination);
						if (is_valid(instruction)) {
							++valid_partitions;
							continue;
						}
						if (is_other_feature_partition(
							    instruction)) {
							++other_feature_partitions;
							continue;
						}
						++reserved_partitions;
						auto initial = seeded_state(
							0x3000'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(
								instruction) << 21) ^
							addend);
						char name[160];
						std::snprintf(name, sizeof(name),
							"reserved multiply partition sf=%u "
							"op54=%u op31=%u o0=%u ra=%u",
							sf, op54, operation, subtract,
							addend);
						check_illegal(harness, name,
							instruction, initial);
					}
	require(valid_partitions == 258U,
		"multiply valid partition count does not match specification");
	require(other_feature_partitions == 64U,
		"checked-pointer partition count does not match specification");
	require(reserved_partitions == 3774U,
		"multiply reserved partition count does not match specification");
	require(valid_partitions + other_feature_partitions +
			reserved_partitions == 4096U,
		"multiply decode partition enumeration is incomplete");
}

} // namespace

void run_reserved_tests(EngineHarness &harness)
{
	encoding_examples();
	every_reserved_decode_partition(harness);
}

} // namespace uml::backend::tests::base_multiply
