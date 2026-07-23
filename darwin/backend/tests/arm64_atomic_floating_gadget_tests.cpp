// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_floating_test_support.hpp"

namespace fp_atomic = arm64_atomic_floating_test;
namespace test = arm64_load_store_test;

extern "C" {
#define UML_DECLARE_NATIVE_VARIANTS(size, operation) \
	void uml_arm64_gadget_atomic_floating_native_##size##_##operation##_load_relaxed(); \
	void uml_arm64_gadget_atomic_floating_native_##size##_##operation##_load_release(); \
	void uml_arm64_gadget_atomic_floating_native_##size##_##operation##_load_acquire(); \
	void uml_arm64_gadget_atomic_floating_native_##size##_##operation##_load_acquire_release(); \
	void uml_arm64_gadget_atomic_floating_native_##size##_##operation##_store_relaxed(); \
	void uml_arm64_gadget_atomic_floating_native_##size##_##operation##_store_release();
#define UML_DECLARE_NATIVE_OPERATIONS(size) \
	UML_DECLARE_NATIVE_VARIANTS(size, add) \
	UML_DECLARE_NATIVE_VARIANTS(size, max) \
	UML_DECLARE_NATIVE_VARIANTS(size, maxnm) \
	UML_DECLARE_NATIVE_VARIANTS(size, min) \
	UML_DECLARE_NATIVE_VARIANTS(size, minnm)
UML_DECLARE_NATIVE_OPERATIONS(bf)
UML_DECLARE_NATIVE_OPERATIONS(h)
UML_DECLARE_NATIVE_OPERATIONS(s)
UML_DECLARE_NATIVE_OPERATIONS(d)
#undef UML_DECLARE_NATIVE_OPERATIONS
#undef UML_DECLARE_NATIVE_VARIANTS
}

[[nodiscard]] bool contains_instruction(void (*function)(),
	std::uint32_t expected)
{
	const auto address = reinterpret_cast<std::uintptr_t>(function);
	for (unsigned index = 0; index < 64; ++index) {
		std::uint32_t word{};
		std::memcpy(&word, reinterpret_cast<const void *>(
			address + index * sizeof(word)), sizeof(word));
		if (word == expected)
			return true;
	}
	return false;
}

void verify_native_words()
{
#define UML_VERIFY_NATIVE_VARIANT(size, size_bits, operation, opc, variant, \
	acquire, release, return_old) \
	do { \
		constexpr std::uint32_t expected = 0x3c20'0000U | size_bits | \
			(static_cast<unsigned>(acquire) << 23) | \
			(static_cast<unsigned>(release) << 22) | (15U << 16) | \
			(static_cast<unsigned>(!return_old) << 15) | (opc << 12) | \
			(7U << 5) | (return_old ? 16U : 31U); \
		test::require(contains_instruction( \
			uml_arm64_gadget_atomic_floating_native_##size##_##operation##_##variant, \
			expected)); \
	} while (false)
#define UML_VERIFY_NATIVE_VARIANTS(size, size_bits, operation, opc) \
	UML_VERIFY_NATIVE_VARIANT(size, size_bits, operation, opc, load_relaxed, \
		false, false, true); \
	UML_VERIFY_NATIVE_VARIANT(size, size_bits, operation, opc, load_release, \
		false, true, true); \
	UML_VERIFY_NATIVE_VARIANT(size, size_bits, operation, opc, load_acquire, \
		true, false, true); \
	UML_VERIFY_NATIVE_VARIANT(size, size_bits, operation, opc, \
		load_acquire_release, true, true, true); \
	UML_VERIFY_NATIVE_VARIANT(size, size_bits, operation, opc, store_relaxed, \
		false, false, false); \
	UML_VERIFY_NATIVE_VARIANT(size, size_bits, operation, opc, store_release, \
		false, true, false)
#define UML_VERIFY_NATIVE_OPERATIONS(size, size_bits) \
	UML_VERIFY_NATIVE_VARIANTS(size, size_bits, add, 0); \
	UML_VERIFY_NATIVE_VARIANTS(size, size_bits, max, 4); \
	UML_VERIFY_NATIVE_VARIANTS(size, size_bits, maxnm, 6); \
	UML_VERIFY_NATIVE_VARIANTS(size, size_bits, min, 5); \
	UML_VERIFY_NATIVE_VARIANTS(size, size_bits, minnm, 7)
	UML_VERIFY_NATIVE_OPERATIONS(bf, 0x0000'0000U);
	UML_VERIFY_NATIVE_OPERATIONS(h, 0x4000'0000U);
	UML_VERIFY_NATIVE_OPERATIONS(s, 0x8000'0000U);
	UML_VERIFY_NATIVE_OPERATIONS(d, 0xc000'0000U);
#undef UML_VERIFY_NATIVE_OPERATIONS
#undef UML_VERIFY_NATIVE_VARIANTS
#undef UML_VERIFY_NATIVE_VARIANT
}

int main()
{
	verify_native_words();
	test::StaticMapping mapping{0x560'0000, 205,
		"/private/tmp/uml-atomic-floating-gadget.XXXXXX"};
	const auto address = mapping.guest() + 4 * test::StaticMapping::page_size;
	constexpr std::array observed{0x3f80ULL, 0x3c00ULL,
		0x3f80'0000ULL, 0x3ff0'0000'0000'0000ULL};
	constexpr std::array operand{0x4000ULL, 0x4000ULL,
		0x4000'0000ULL, 0x4000'0000'0000'0000ULL};

	const auto differential = [&](std::uint32_t encoded,
		std::uint64_t old_value, std::uint64_t source_value,
		std::uint32_t fpcr) {
		const auto size = fp_atomic::size_bytes(encoded >> 30);
		fp_atomic::atomic::AtomicMemory memory;
		fp_atomic::arm64::CpuState expected{};
		expected.fpcr = fpcr;
		expected.fpsr = 0x0800'009fU;
		fp_atomic::set_vector(expected, 4, 0xdead'beef, size,
			std::byte{0x5a});
		const auto interpreted = fp_atomic::execute_interpreter(expected,
			memory, encoded, old_value, source_value, true);
		test::require(interpreted.reason ==
			fp_atomic::arm64::StopReason::continued);

		fp_atomic::arm64::CpuState actual{};
		actual.fpcr = fpcr;
		actual.fpsr = 0x0800'009fU;
		actual.registers[2] = address;
		fp_atomic::set_vector(actual, 3, source_value, size,
			std::byte{0xa5});
		fp_atomic::set_vector(actual, 4, 0xdead'beef, size,
			std::byte{0x5a});
		fp_atomic::put_mapping(mapping, address, old_value, size);
		const auto event = mapping.execute(fp_atomic::pc, encoded, actual);
		test::require(event == fp_atomic::gadgets::Event::system_call &&
			fp_atomic::get_mapping(mapping, address, size) ==
				memory.get(fp_atomic::interpreter_address, size) &&
			actual.vectors[3].bytes == expected.vectors[3].bytes &&
			actual.vectors[4].bytes == expected.vectors[4].bytes &&
			actual.fpcr == expected.fpcr && actual.fpsr == expected.fpsr);
	};

	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		for (const auto operation : fp_atomic::operations) {
			for (const bool store_only : {false, true}) {
				for (const bool acquire : {false, true}) {
					if (store_only && acquire)
						continue;
					for (const bool release : {false, true})
						differential(fp_atomic::instruction(size_code,
							operation, acquire, release, store_only),
							observed[size_code], operand[size_code], 0);
				}
			}
		}
	}

	/* Exercise fallback control-flow edges that native LSFE handles
	 * intrinsically: BF rounding, forced DN/AH, FIZ, and suppressed FPSR. */
	differential(fp_atomic::instruction(0, fp_atomic::Operation::add,
		false, false, false), 0x3f80, 0x3b80, 1U << 22);
	differential(fp_atomic::instruction(0, fp_atomic::Operation::maximum,
		false, false, false), 0x7fc5, 0x3f80, 2U | 0x9f00U);
	differential(fp_atomic::instruction(2, fp_atomic::Operation::add,
		true, true, false), 1, 0, 1U);
	differential(fp_atomic::instruction(3,
		fp_atomic::Operation::maximum_number, false, true, true),
		0x7ff8'1234'5678'9abc, 0x3ff0'0000'0000'0000, 0x9f00U);
}
