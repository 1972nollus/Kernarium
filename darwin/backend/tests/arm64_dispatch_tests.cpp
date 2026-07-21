// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/detail/instruction_helpers.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_runtime.h"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <source_location>
#include <span>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace arm64 = uml::backend::arm64;

extern "C" const std::uint32_t uml_arm64_neon_inventory_begin[];
extern "C" const std::uint32_t uml_arm64_neon_inventory_end[];

static void require(bool condition,
		    const std::source_location location =
			    std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

class FlatMemory final : public arm64::GuestMemory {
public:
	FlatMemory(std::uint64_t base, std::size_t size) : base_(base), bytes_(size) {}

	[[nodiscard]] bool read(std::uint64_t address,
				std::span<std::byte> destination) override
	{
		const auto source = range(address, destination.size());
		if (source.empty() && !destination.empty())
			return false;
		std::ranges::copy(source, destination.begin());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
				 std::span<const std::byte> source) override
	{
		auto destination = range(address, source.size());
		if (destination.empty() && !source.empty())
			return false;
		std::ranges::copy(source, destination.begin());
		return true;
	}

	void put32(std::uint64_t address, std::uint32_t instruction)
	{
		std::array<std::byte, 4> encoded{};
		for (std::size_t index = 0; index < encoded.size(); ++index)
			encoded[index] = static_cast<std::byte>(instruction >> (index * 8));
		require(write(address, encoded));
	}

	void invalidate_code(std::uint64_t address) override
	{
		invalidated_code_.push_back(address);
	}

	[[nodiscard]] const std::vector<std::uint64_t> &invalidated_code() const
	{
		return invalidated_code_;
	}

	[[nodiscard]] std::uint64_t get64(std::uint64_t address)
	{
		std::array<std::byte, 8> encoded{};
		require(read(address, encoded));
		std::uint64_t value = 0;
		for (std::size_t index = 0; index < encoded.size(); ++index)
			value |= static_cast<std::uint64_t>(
				std::to_integer<unsigned char>(encoded[index])) <<
				(index * 8);
		return value;
	}

private:
	[[nodiscard]] std::span<std::byte> range(std::uint64_t address,
						 std::size_t size)
	{
		if (address < base_)
			return {};
		const auto offset = address - base_;
		if (offset > bytes_.size() || size > bytes_.size() - offset)
			return {};
		return std::span{bytes_}.subspan(static_cast<std::size_t>(offset), size);
	}

	std::uint64_t base_;
	std::vector<std::byte> bytes_;
	std::vector<std::uint64_t> invalidated_code_;
};

static void complete_neon_inventory_decodes()
{
	FlatMemory memory(0, 0x1000);
	for (auto instruction = uml_arm64_neon_inventory_begin;
	     instruction != uml_arm64_neon_inventory_end; ++instruction) {
		arm64::CpuState cpu{};
		const auto result = arm64::Dispatcher{}.execute(cpu, memory,
			*instruction);
		if (result.reason == arm64::StopReason::unsupported_instruction ||
		    result.reason == arm64::StopReason::illegal_instruction) {
			std::fprintf(stderr, "NEON inventory decode failed: 0x%08x\n",
				*instruction);
			std::abort();
		}
	}
}

static void crypto_extension_known_answers()
{
	FlatMemory memory(0, 0x1000);
	arm64::Dispatcher dispatcher;
	const auto set_bytes = [](arm64::VectorRegister &vector,
				  unsigned first) {
		for (unsigned index = 0; index < vector.bytes.size(); ++index)
			vector.bytes[index] = static_cast<std::byte>(first + index);
	};
	const auto set_words = [](arm64::VectorRegister &vector,
				  const std::array<std::uint32_t, 4> &words) {
		for (std::size_t index = 0; index < words.size(); ++index)
			arm64::detail::encode_little_endian(words[index],
				std::span{vector.bytes}.subspan(index * 4, 4));
	};
	const auto set_doublewords = [](arm64::VectorRegister &vector,
				       const std::array<std::uint64_t, 2> &words) {
		for (std::size_t index = 0; index < words.size(); ++index)
			arm64::detail::encode_little_endian(words[index],
				std::span{vector.bytes}.subspan(index * 8, 8));
	};
	const auto require_hex = [](const arm64::VectorRegister &vector,
				    std::string_view expected) {
		const auto nibble = [](char value) -> unsigned {
			if (value >= '0' && value <= '9')
				return static_cast<unsigned>(value - '0');
			return static_cast<unsigned>(value - 'a' + 10);
		};
		require(expected.size() == vector.bytes.size() * 2);
		for (std::size_t index = 0; index < vector.bytes.size(); ++index) {
			const auto byte = (nibble(expected[index * 2]) << 4) |
				nibble(expected[index * 2 + 1]);
			require(vector.bytes[index] == static_cast<std::byte>(byte));
		}
	};

	struct CryptoCase final {
		std::uint32_t instruction;
		std::string_view expected;
	};
	constexpr std::array aes_cases{
		CryptoCase{0x4e28'4820U, "cacacacacacacacacacacacacacacaca"},
		CryptoCase{0x4e28'5820U, "7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c7c"},
		CryptoCase{0x4e28'6820U, "12171015161314111a1f181d1e1b1c19"},
		CryptoCase{0x4e28'7820U, "1a1f181d1e1b1c191217101516131411"},
	};
	for (const auto &test : aes_cases) {
		arm64::CpuState cpu{};
		set_bytes(cpu.vectors[0], 0);
		set_bytes(cpu.vectors[1], 16);
		require(dispatcher.execute(cpu, memory, test.instruction).reason ==
			arm64::StopReason::continued);
		require_hex(cpu.vectors[0], test.expected);
	}

	for (const auto &test : std::array{
		CryptoCase{0x0e22'e020U, "00001100240035005000410074006500"},
		CryptoCase{0x4e22'e020U, "c000d100e400f50090008100b400a500"},
	}) {
		arm64::CpuState cpu{};
		set_bytes(cpu.vectors[1], 0);
		set_bytes(cpu.vectors[2], 16);
		require(dispatcher.execute(cpu, memory, test.instruction).reason ==
			arm64::StopReason::continued);
		require_hex(cpu.vectors[0], test.expected);
	}
	for (const auto instruction : {0x0ee2'e020U, 0x4ee2'e020U}) {
		arm64::CpuState cpu{};
		const auto offset = instruction == 0x4ee2'e020U ? 8U : 0U;
		arm64::detail::encode_little_endian(0x0123'4567'89ab'cdefULL,
			std::span{cpu.vectors[1].bytes}.subspan(offset, 8));
		arm64::detail::encode_little_endian(0xfedc'ba98'7654'3210ULL,
			std::span{cpu.vectors[2].bytes}.subspan(offset, 8));
		require(dispatcher.execute(cpu, memory, instruction).reason ==
			arm64::StopReason::continued);
		require_hex(cpu.vectors[0], "f010c8289878a040b0508868d838e000");
	}

	constexpr std::array destination{
		0x0123'4567U, 0x89ab'cdefU, 0xfedc'ba98U, 0x7654'3210U};
	constexpr std::array first{
		0x0f1e'2d3cU, 0x4b5a'6978U, 0x8796'a5b4U, 0xc3d2'e1f0U};
	constexpr std::array second{
		0x1111'1111U, 0x2222'2222U, 0x3333'3333U, 0x4444'4444U};
	constexpr std::array sha_cases{
		CryptoCase{0x5e02'0020U, "f28d735ae6db9bd3358282417129dd50"},
		CryptoCase{0x5e02'1020U, "c9aff3dd4e68d747cf5c5c9b25cc6e11"},
		CryptoCase{0x5e02'2020U, "3475284ee1e9382315805d7a7129dd50"},
		CryptoCase{0x5e28'0820U, "4f8bc703000000000000000000000000"},
		CryptoCase{0x5e02'3020U, "eeeeeeeedddddddd97a4f1c22c1f4a79"},
		CryptoCase{0x5e28'1820U, "3e58f294b6d07a1cd0b61c7a5dd44cc5"},
		CryptoCase{0x5e02'4020U, "c2edd73b817a63555e325bcd65b43a29"},
		CryptoCase{0x5e02'5020U, "a4faa20a6262fb9693c065b30d407eec"},
		CryptoCase{0x5e28'2820U, "b311813e80df2b8a4bee7e214a2c0769"},
		CryptoCase{0x5e02'6020U, "12e2704c5e2ffebb161008302cb8cf59"},
	};
	for (const auto &test : sha_cases) {
		arm64::CpuState cpu{};
		set_words(cpu.vectors[0], destination);
		set_words(cpu.vectors[1], first);
		set_words(cpu.vectors[2], second);
		require(dispatcher.execute(cpu, memory, test.instruction).reason ==
			arm64::StopReason::continued);
		require_hex(cpu.vectors[0], test.expected);
	}

	constexpr std::array destination64{
		0x0123'4567'89ab'cdefULL, 0xfedc'ba98'7654'3210ULL};
	constexpr std::array first64{
		0x0f1e'2d3c'4b5a'6978ULL, 0x8796'a5b4'c3d2'e1f0ULL};
	constexpr std::array second64{
		0x1111'1111'2222'2222ULL, 0x3333'3333'4444'4444ULL};
	constexpr std::array third64{
		0x5555'5555'6666'6666ULL, 0x7777'7777'8888'8888ULL};
	constexpr std::array sha3_sha512_cases{
		CryptoCase{0xce62'8020U, "66a47f56d0bc96dd996d3f5c6f421720"},
		CryptoCase{0xce62'8420U, "b36ea1ffc958ad41e4090e8ab7dade5a"},
		CryptoCase{0xcec0'8020U, "4db35c1deb7d906f170cc5d781ef7a7e"},
		CryptoCase{0xce62'8820U, "1f9b271a2e671648cd52c7d38d55a594"},
		CryptoCase{0xce02'0c20U, "3c2d1e0f78695a4b3c2d1e0ff0e1d2c3"},
		CryptoCase{0xce62'8c20U, "3c2d1e0f1e0f3c2d78695a4bd2c3f0e1"},
		CryptoCase{0xce82'4420U, "bcb4169e070fad25cbc343cb525ada52"},
		CryptoCase{0xce22'0c20U, "78695a4b3c2d1e0fb4a59687b4a59687"},
	};
	for (const auto &test : sha3_sha512_cases) {
		arm64::CpuState cpu{};
		set_doublewords(cpu.vectors[0], destination64);
		set_doublewords(cpu.vectors[1], first64);
		set_doublewords(cpu.vectors[2], second64);
		set_doublewords(cpu.vectors[3], third64);
		require(dispatcher.execute(cpu, memory, test.instruction).reason ==
			arm64::StopReason::continued);
		require_hex(cpu.vectors[0], test.expected);
	}

	struct CrcCase final {
		std::uint32_t instruction;
		std::uint32_t expected;
	};
	constexpr std::array crc_cases{
		CrcCase{0x1ac2'4020U, 0x437b'dd3cU},
		CrcCase{0x1ac2'4420U, 0xe7fb'56daU},
		CrcCase{0x1ac2'4820U, 0x9e6e'72f6U},
		CrcCase{0x9ac2'4c20U, 0xdf04'eaaaU},
		CrcCase{0x1ac2'5020U, 0xeb0d'fffbU},
		CrcCase{0x1ac2'5420U, 0xbbd4'f0f7U},
		CrcCase{0x1ac2'5820U, 0x14f8'd626U},
		CrcCase{0x9ac2'5c20U, 0x679d'fef3U},
	};
	for (const auto &test : crc_cases) {
		arm64::CpuState cpu{};
		cpu.registers[0] = ~std::uint64_t{};
		cpu.registers[1] = 0x1234'5678U;
		cpu.registers[2] = 0xfedc'ba98'7654'3210ULL;
		require(dispatcher.execute(cpu, memory, test.instruction).reason ==
			arm64::StopReason::continued);
		require(cpu.registers[0] == test.expected);
	}
}

static void neon_saturating_halving_and_shift_semantics()
{
	FlatMemory memory(0, 0x1000);
	arm64::Dispatcher dispatcher;
	{
		arm64::CpuState cpu{};
		cpu.vectors[1].bytes.fill(std::byte{0xfd});
		cpu.vectors[2].bytes.fill(std::byte{2});
		require(dispatcher.execute(cpu, memory, 0x0e22'0420U).reason ==
			arm64::StopReason::continued); // shadd v0.8b, v1.8b, v2.8b
		for (std::size_t lane = 0; lane < 8; ++lane)
			if (cpu.vectors[0].bytes[lane] != std::byte{0xff}) {
				std::fprintf(stderr, "shadd lane %zu = %02x\n", lane,
					std::to_integer<unsigned>(cpu.vectors[0].bytes[lane]));
				std::abort();
			}
	}
	{
		arm64::CpuState cpu{};
		cpu.vectors[1].bytes.fill(std::byte{0x7f});
		cpu.vectors[2].bytes.fill(std::byte{1});
		require(dispatcher.execute(cpu, memory, 0x0e22'0c20U).reason ==
			arm64::StopReason::continued); // sqadd v0.8b, v1.8b, v2.8b
		for (std::size_t lane = 0; lane < 8; ++lane)
			require(cpu.vectors[0].bytes[lane] == std::byte{0x7f});
		require((cpu.fpsr & (1U << 27)) != 0);
	}
	{
		arm64::CpuState cpu{};
		cpu.vectors[1].bytes.fill(std::byte{});
		cpu.vectors[2].bytes.fill(std::byte{1});
		require(dispatcher.execute(cpu, memory, 0x2e22'2c20U).reason ==
			arm64::StopReason::continued); // uqsub v0.8b, v1.8b, v2.8b
		for (std::size_t lane = 0; lane < 8; ++lane)
			require(cpu.vectors[0].bytes[lane] == std::byte{});
		require((cpu.fpsr & (1U << 27)) != 0);
	}
	{
		arm64::CpuState cpu{};
		for (std::size_t lane = 0; lane < 4; ++lane) {
			arm64::detail::encode_little_endian(0x8000,
				std::span{cpu.vectors[1].bytes}.subspan(lane * 2, 2));
			arm64::detail::encode_little_endian(0x8000,
				std::span{cpu.vectors[2].bytes}.subspan(lane * 2, 2));
		}
		require(dispatcher.execute(cpu, memory, 0x0e62'b420U).reason ==
			arm64::StopReason::continued); // sqdmulh v0.4h, v1.4h, v2.4h
		for (std::size_t lane = 0; lane < 4; ++lane)
			require(arm64::detail::decode_little_endian(
				std::span{cpu.vectors[0].bytes}.subspan(lane * 2, 2)) ==
				0x7fff);
		require((cpu.fpsr & (1U << 27)) != 0);
	}
	{
		arm64::CpuState cpu{};
		cpu.vectors[0].bytes.fill(std::byte{10});
		cpu.vectors[1].bytes.fill(std::byte{8});
		require(dispatcher.execute(cpu, memory, 0x0f0f'1420U).reason ==
			arm64::StopReason::continued); // ssra v0.8b, v1.8b, #1
		for (std::size_t lane = 0; lane < 8; ++lane)
			if (cpu.vectors[0].bytes[lane] != std::byte{14}) {
				std::fprintf(stderr, "ssra lane %zu = %02x\n", lane,
					std::to_integer<unsigned>(cpu.vectors[0].bytes[lane]));
				std::abort();
			}
	}
	{
		arm64::CpuState cpu{};
		cpu.vectors[1].bytes.fill(std::byte{0x7f});
		require(dispatcher.execute(cpu, memory, 0x0f09'7420U).reason ==
			arm64::StopReason::continued); // sqshl v0.8b, v1.8b, #1
		for (std::size_t lane = 0; lane < 8; ++lane)
			require(cpu.vectors[0].bytes[lane] == std::byte{0x7f});
		require((cpu.fpsr & (1U << 27)) != 0);
	}
}

static void neon_narrow_convert_and_indexed_semantics()
{
	FlatMemory memory(0, 0x1000);
	arm64::Dispatcher dispatcher;
	{
		arm64::CpuState cpu{};
		for (std::size_t lane = 0; lane < 8; ++lane)
			arm64::detail::encode_little_endian(lane == 0 ? 200 :
				lane == 1 ? 0xff38 : lane,
				std::span{cpu.vectors[1].bytes}.subspan(lane * 2, 2));
		require(dispatcher.execute(cpu, memory, 0x0e21'4820U).reason ==
			arm64::StopReason::continued); // sqxtn v0.8b, v1.8h
		require(cpu.vectors[0].bytes[0] == std::byte{0x7f});
		require(cpu.vectors[0].bytes[1] == std::byte{0x80});
		require((cpu.fpsr & (1U << 27)) != 0);
	}
	{
		arm64::CpuState cpu{};
		for (std::size_t lane = 0; lane < 2; ++lane) {
			arm64::detail::encode_little_endian(std::bit_cast<std::uint32_t>(
				lane == 0 ? 1.2F : -1.2F),
				std::span{cpu.vectors[1].bytes}.subspan(lane * 4, 4));
		}
		require(dispatcher.execute(cpu, memory, 0x0ea1'8820U).reason ==
			arm64::StopReason::continued); // frintp v0.2s, v1.2s
		require(std::bit_cast<float>(static_cast<std::uint32_t>(
			arm64::detail::decode_little_endian(
				std::span{cpu.vectors[0].bytes}.subspan(0, 4)))) == 2.0F);
		require(std::bit_cast<float>(static_cast<std::uint32_t>(
			arm64::detail::decode_little_endian(
				std::span{cpu.vectors[0].bytes}.subspan(4, 4)))) == -1.0F);
	}
	{
		arm64::CpuState cpu{};
		for (std::size_t lane = 0; lane < 4; ++lane) {
			arm64::detail::encode_little_endian(2,
				std::span{cpu.vectors[1].bytes}.subspan(lane * 2, 2));
			arm64::detail::encode_little_endian(lane == 1 ? 3 : 9,
				std::span{cpu.vectors[2].bytes}.subspan(lane * 2, 2));
			arm64::detail::encode_little_endian(1,
				std::span{cpu.vectors[0].bytes}.subspan(lane * 2, 2));
		}
		require(dispatcher.execute(cpu, memory, 0x2f52'0020U).reason ==
			arm64::StopReason::continued); // mla v0.4h, v1.4h, v2.h[1]
		for (std::size_t lane = 0; lane < 4; ++lane)
			require(arm64::detail::decode_little_endian(
				std::span{cpu.vectors[0].bytes}.subspan(lane * 2, 2)) == 7);
	}
	{
		arm64::CpuState cpu{};
		arm64::detail::encode_little_endian(0x8000'0000U,
			std::span{cpu.vectors[1].bytes}.subspan(12, 4));
		require(dispatcher.execute(cpu, memory, 0x4e1c'2c20U).reason ==
			arm64::StopReason::continued); // smov x0, v1.s[3]
		require(cpu.registers[0] == 0xffff'ffff'8000'0000ULL);
	}
}

static void neon_interleaved_structure_semantics()
{
	FlatMemory memory(0, 0x1000);
	std::array<std::byte, 16> interleaved{};
	for (std::size_t lane = 0; lane < 8; ++lane) {
		interleaved[lane * 2] = static_cast<std::byte>(lane);
		interleaved[lane * 2 + 1] = static_cast<std::byte>(0x10 + lane);
	}
	require(memory.write(0, interleaved));
	arm64::CpuState cpu{};
	require(arm64::Dispatcher{}.execute(cpu, memory, 0x0c40'8000U).reason ==
		arm64::StopReason::continued); // ld2 {v0.8b, v1.8b}, [x0]
	for (std::size_t lane = 0; lane < 8; ++lane) {
		require(cpu.vectors[0].bytes[lane] == static_cast<std::byte>(lane));
		require(cpu.vectors[1].bytes[lane] ==
			static_cast<std::byte>(0x10 + lane));
	}
}

static void neon_widening_and_reduction_semantics()
{
	FlatMemory memory(0, 0x1000);
	arm64::Dispatcher dispatcher;
	{
		arm64::CpuState cpu{};
		cpu.vectors[1].bytes.fill(std::byte{0x80});
		cpu.vectors[2].bytes.fill(std::byte{0x7f});
		require(dispatcher.execute(cpu, memory, 0x0e22'7020U).reason ==
			arm64::StopReason::continued); // sabdl v0.8h, v1.8b, v2.8b
		for (std::size_t lane = 0; lane < 8; ++lane)
			require(arm64::detail::decode_little_endian(
				std::span{cpu.vectors[0].bytes}.subspan(lane * 2, 2)) == 255);
	}
	{
		arm64::CpuState cpu{};
		for (std::size_t lane = 0; lane < 4; ++lane) {
			arm64::detail::encode_little_endian(0x8000,
				std::span{cpu.vectors[1].bytes}.subspan(lane * 2, 2));
			arm64::detail::encode_little_endian(0x8000,
				std::span{cpu.vectors[2].bytes}.subspan(lane * 2, 2));
		}
		require(dispatcher.execute(cpu, memory, 0x0e62'd020U).reason ==
			arm64::StopReason::continued); // sqdmull v0.4s, v1.4h, v2.4h
		for (std::size_t lane = 0; lane < 4; ++lane)
			require(arm64::detail::decode_little_endian(
				std::span{cpu.vectors[0].bytes}.subspan(lane * 4, 4)) ==
				0x7fff'ffffU);
		require((cpu.fpsr & (1U << 27)) != 0);
	}
	{
		arm64::CpuState cpu{};
		const std::array values{
			std::numeric_limits<float>::quiet_NaN(), -2.0F, 3.0F, 1.0F};
		for (std::size_t lane = 0; lane < values.size(); ++lane)
			arm64::detail::encode_little_endian(std::bit_cast<std::uint32_t>(
				values[lane]), std::span{cpu.vectors[1].bytes}.subspan(
					lane * 4, 4));
		require(dispatcher.execute(cpu, memory, 0x6e30'c820U).reason ==
			arm64::StopReason::continued); // fmaxnmv s0, v1.4s
		require(std::bit_cast<float>(static_cast<std::uint32_t>(
			arm64::detail::vector_half(cpu.vectors[0], 0))) == 3.0F);
	}
	{
		arm64::CpuState cpu{};
		arm64::detail::encode_little_endian(std::bit_cast<std::uint32_t>(1.25F),
			std::span{cpu.vectors[1].bytes}.subspan(0, 4));
		arm64::detail::encode_little_endian(std::bit_cast<std::uint32_t>(2.75F),
			std::span{cpu.vectors[1].bytes}.subspan(4, 4));
		require(dispatcher.execute(cpu, memory, 0x7e30'd820U).reason ==
			arm64::StopReason::continued); // faddp s0, v1.2s
		require(std::bit_cast<float>(static_cast<std::uint32_t>(
			arm64::detail::vector_half(cpu.vectors[0], 0))) == 4.0F);
	}
}

class SplitAtomicMemory final : public arm64::GuestMemory {
public:
	SplitAtomicMemory(std::uint64_t code, std::uint64_t data)
		: data_(data), code_(code, 0x1000)
	{
	}

	[[nodiscard]] bool read(std::uint64_t address,
				std::span<std::byte> destination) override
	{
		if (address == data_ && destination.size() == sizeof(read_value_)) {
			std::memcpy(destination.data(), &read_value_, destination.size());
			return true;
		}
		return code_.read(address, destination);
	}

	[[nodiscard]] bool write(std::uint64_t address,
				 std::span<const std::byte> source) override
	{
		if (address == data_ && source.size() == sizeof(write_value_)) {
			std::memcpy(&write_value_, source.data(), source.size());
			return true;
		}
		return code_.write(address, source);
	}

	[[nodiscard]] std::byte *atomic_pointer(
		std::uint64_t address, std::size_t size,
		arm64::MemoryAccess access) override
	{
		if (address != data_ || size != sizeof(read_value_))
			return nullptr;
		return reinterpret_cast<std::byte *>(
			access == arm64::MemoryAccess::write ?
				&write_value_ : &read_value_);
	}

	void put32(std::uint64_t address, std::uint32_t instruction)
	{
		code_.put32(address, instruction);
	}

	std::uint64_t &read_value() noexcept { return read_value_; }
	std::uint64_t &write_value() noexcept { return write_value_; }

private:
	std::uint64_t data_;
	FlatMemory code_;
	alignas(16) std::uint64_t read_value_{};
	alignas(16) std::uint64_t write_value_{};
};

static void arithmetic_and_memory()
{
	constexpr std::uint64_t base = 0x1000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base + 0, 0xd280'00a0U); // movz x0, #5
	memory.put32(base + 4, 0x9100'1c01U); // add x1, x0, #7
	memory.put32(base + 8, 0xf900'07e1U); // str x1, [sp, #8]
	memory.put32(base + 12, 0xf940'07e2U); // ldr x2, [sp, #8]
	memory.put32(base + 16, 0xd400'0001U); // svc #0

	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.sp = base + 0x800;
	const arm64::Dispatcher dispatcher;
	const auto result = dispatcher.run(cpu, memory, 16);
	require(result.reason == arm64::StopReason::system_call);
	require(result.instructions == 5);
	require(cpu.registers[0] == 5);
	require(cpu.registers[1] == 12);
	require(cpu.registers[2] == 12);
	require(memory.get64(cpu.sp + 8) == 12);
	require(cpu.pc == base + 20);
}

static void static_control_flow()
{
	constexpr std::uint64_t base = 0x4000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base + 0, 0xd280'0020U); // movz x0, #1
	memory.put32(base + 4, 0x1400'0002U); // b +8
	memory.put32(base + 8, 0xd280'0040U); // movz x0, #2 (skipped)
	memory.put32(base + 12, 0x9100'0c00U); // add x0, x0, #3
	memory.put32(base + 16, 0xd420'0000U); // brk #0

	arm64::CpuState cpu{};
	cpu.pc = base;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 16);
	require(result.reason == arm64::StopReason::breakpoint);
	require(result.instructions == 4);
	require(cpu.registers[0] == 4);
	require(cpu.pc == base + 16);
}

static void architectural_undefined_instruction()
{
	constexpr std::uint64_t base = 0x6000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x0000'beefU); // udf #0xbeef

	arm64::CpuState cpu{};
	cpu.pc = base;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 1);
	require(result.reason == arm64::StopReason::illegal_instruction);
	require(result.address == base);
	require(result.instruction == 0x0000'beefU);
	require(cpu.pc == base);
}

static void thread_pointer_access()
{
	constexpr std::uint64_t base = 0x8000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base + 0, 0xd282'4683U); // movz x3, #0x1234
	memory.put32(base + 4, 0xd51b'd043U); // msr tpidr_el0, x3
	memory.put32(base + 8, 0xd53b'd044U); // mrs x4, tpidr_el0
	memory.put32(base + 12, 0xd400'0001U); // svc #0

	arm64::CpuState cpu{};
	cpu.pc = base;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 16);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.tls == 0x1234);
	require(cpu.registers[4] == 0x1234);
}

static void architectural_system_registers()
{
	constexpr std::uint64_t base = 0x8800;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0xd53b'00e5U);     // mrs x5, dczid_el0
	memory.put32(base + 4, 0xd51b'4406U); // msr fpcr, x6
	memory.put32(base + 8, 0xd53b'4407U); // mrs x7, fpcr
	memory.put32(base + 12, 0xd51b'4428U); // msr fpsr, x8
	memory.put32(base + 16, 0xd53b'4429U); // mrs x9, fpsr
	memory.put32(base + 20, 0xd53b'002aU); // mrs x10, ctr_el0
	memory.put32(base + 24, 0xd53b'420bU); // mrs x11, nzcv
	memory.put32(base + 28, 0xd51b'420cU); // msr nzcv, x12
	memory.put32(base + 32, 0xd53b'420dU); // mrs x13, nzcv
	memory.put32(base + 36, 0xd53b'e00eU); // mrs x14, cntfrq_el0
	memory.put32(base + 40, 0xd53b'e04fU); // mrs x15, cntvct_el0
	memory.put32(base + 44, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[6] = 0xaaaa'bbbb'0400'0000ULL;
	cpu.registers[8] = 0xcccc'dddd'0800'0000ULL;
	cpu.registers[12] = 0xffff'ffff'5000'0000ULL;
	cpu.nzcv = 0xa000'0000U;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 16);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[5] == 1U << 4);
	require(cpu.fpcr == 0x0400'0000U);
	require(cpu.registers[7] == 0x0400'0000U);
	require(cpu.fpsr == 0x0800'0000U);
	require(cpu.registers[9] == 0x0800'0000U);
	require(cpu.registers[10] == 0x8444'8004U);
	require(cpu.registers[11] == 0xa000'0000U);
	require(cpu.nzcv == 0x5000'0000U);
	require(cpu.registers[13] == 0x5000'0000U);
	require(cpu.registers[14] == 1'000'000'000U);
	require(cpu.registers[15] != 0);
}

static void architectural_hint()
{
	constexpr std::uint64_t base = 0x9000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0xd503'245fU);     // bti c
	memory.put32(base + 4, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 4);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.pc == base + 8);
}

static void architectural_barriers()
{
	constexpr std::uint64_t base = 0x9800;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0xd503'3bbfU);      // dmb ish
	memory.put32(base + 4, 0xd503'3f9fU);  // dsb sy
	memory.put32(base + 8, 0xd503'3fdfU);  // isb
	memory.put32(base + 12, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.pc == base + 16);
}

static void architectural_cache_maintenance()
{
	constexpr std::uint64_t base = 0x9c00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0xd50b'7b22U);      // dc cvau, x2
	memory.put32(base + 4, 0xd503'3b9fU);  // dsb ish
	memory.put32(base + 8, 0xd50b'7522U);  // ic ivau, x2
	memory.put32(base + 12, 0xd503'3b9fU); // dsb ish
	memory.put32(base + 16, 0xd503'3fdfU); // isb
	memory.put32(base + 20, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[2] = 0x1234'5000;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.pc == base + 24);
	require(memory.invalidated_code().size() == 1);
	require(memory.invalidated_code()[0] == cpu.registers[2]);
}

static void integer_register_pair()
{
	constexpr std::uint64_t base = 0xa000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0xa9bf'7bfdU);     // stp x29, x30, [sp, #-16]!
	memory.put32(base + 4, 0xa8c1'7bfdU); // ldp x29, x30, [sp], #16
	memory.put32(base + 8, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.sp = base + 0x800;
	cpu.registers[29] = 0x1234;
	cpu.registers[30] = 0x5678;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 4);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.sp == base + 0x800);
	require(cpu.registers[29] == 0x1234);
	require(cpu.registers[30] == 0x5678);
}

static void logical_bitmask_immediate()
{
	constexpr std::uint64_t base = 0xb000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x121a'7884U);     // and w4, w4, #0xffffffdf
	memory.put32(base + 4, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[4] = 0xffff'ffff'ffff'ffffULL;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 4);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[4] == 0xffff'ffdfU);
}

static void add_sub_register_forms()
{
	constexpr std::uint64_t base = 0xc000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x8b07'0000U);     // add x0, x0, x7
	memory.put32(base + 4, 0x8b20'4c60U); // add x0, x3, w0, uxtw #3
	memory.put32(base + 8, 0xeb07'007fU); // cmp x3, x7
	memory.put32(base + 12, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[0] = 2;
	cpu.registers[3] = 5;
	cpu.registers[7] = 3;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[0] == 45);
	require(cpu.nzcv == 0x2000'0000U);
}

static void add_sub_with_carry_forms()
{
	constexpr std::uint64_t base = 0xc400;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x9a1f'01adU);      // adc x13, x13, xzr
	memory.put32(base + 4, 0x3a02'0020U);  // adcs w0, w1, w2
	memory.put32(base + 8, 0xda05'0083U);  // sbc x3, x4, x5
	memory.put32(base + 12, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.nzcv = 0x2000'0000U;
	cpu.registers[13] = ~std::uint64_t{0};
	cpu.registers[1] = 0xffff'ffffU;
	cpu.registers[2] = 0;
	cpu.registers[4] = 10;
	cpu.registers[5] = 3;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[13] == 0);
	require(cpu.registers[0] == 0);
	require(cpu.registers[3] == 7);
	require(cpu.nzcv == 0x6000'0000U);
}

static void variable_shift_register_forms()
{
	constexpr std::uint64_t base = 0xc800;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x9ac4'2442U);      // lsr x2, x2, x4
	memory.put32(base + 4, 0x1ac4'2803U);  // asr w3, w0, w4
	memory.put32(base + 8, 0x9ac4'2c05U);  // ror x5, x0, x4
	memory.put32(base + 12, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[0] = 0x8000'0000'0000'0001ULL;
	cpu.registers[2] = 0x100;
	cpu.registers[4] = 4;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[2] == 0x10);
	require(cpu.registers[3] == 0);
	require(cpu.registers[5] == 0x1800'0000'0000'0000ULL);
}

static void scalar_division()
{
	constexpr std::uint64_t base = 0xca00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x9ac1'0800U);      // udiv x0, x0, x1
	memory.put32(base + 4, 0x1ac3'0c42U);  // sdiv w2, w2, w3
	memory.put32(base + 8, 0x9ac5'0c84U);  // sdiv x4, x4, x5
	memory.put32(base + 12, 0x1ac7'08c6U); // udiv w6, w6, w7
	memory.put32(base + 16, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[0] = 100;
	cpu.registers[1] = 4;
	cpu.registers[2] = static_cast<std::uint32_t>(-21);
	cpu.registers[3] = 4;
	cpu.registers[4] = std::uint64_t{1} << 63;
	cpu.registers[5] = ~std::uint64_t{0};
	cpu.registers[6] = 123;
	cpu.registers[7] = 0;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[0] == 25);
	require(cpu.registers[2] == static_cast<std::uint32_t>(-5));
	require(cpu.registers[4] == std::uint64_t{1} << 63);
	require(cpu.registers[6] == 0);
}

static void scalar_bit_reverse_and_count()
{
	constexpr std::uint64_t base = 0xcc00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0xdac0'0042U);      // rbit x2, x2
	memory.put32(base + 4, 0xdac0'1043U);  // clz x3, x2
	memory.put32(base + 8, 0xdac0'1444U);  // cls x4, x2
	memory.put32(base + 12, 0xdac0'0cc5U); // rev x5, x6
	memory.put32(base + 16, 0x5ac0'0907U); // rev w7, w8
	memory.put32(base + 20, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[2] = 1;
	cpu.registers[6] = 0x0102'0304'0506'0708ULL;
	cpu.registers[8] = 0x1122'3344U;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[2] == 0x8000'0000'0000'0000ULL);
	require(cpu.registers[3] == 0);
	require(cpu.registers[4] == 0);
	require(cpu.registers[5] == 0x0807'0605'0403'0201ULL);
	require(cpu.registers[7] == 0x4433'2211U);
}

static void scalar_floating_compare()
{
	constexpr std::uint64_t base = 0xc800;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x1e60'2000U);      // fcmp d0, d0
	memory.put32(base + 4, 0x1e20'2028U);  // fcmp s1, #0.0
	memory.put32(base + 8, 0x1e7f'd7e1U);  // fccmp d31, d31, #1, le
	memory.put32(base + 12, 0x1e60'c409U); // fccmp d0, d0, #9, gt
	memory.put32(base + 16, 0x1e24'cc62U); // fcsel s2, s3, s4, gt
	memory.put32(base + 20, 0x1e60'2000U); // fcmp d0, d0
	memory.put32(base + 24, 0x1e67'1cc5U); // fcsel d5, d6, d7, ne
	memory.put32(base + 28, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	arm64::detail::set_vector_half(
		cpu.vectors[0], 0, std::bit_cast<std::uint64_t>(1.5));
	arm64::detail::set_vector_half(cpu.vectors[1], 0,
		std::bit_cast<std::uint32_t>(-1.0F));
	arm64::detail::set_vector_half(cpu.vectors[3], 0,
		std::bit_cast<std::uint32_t>(3.0F));
	arm64::detail::set_vector_half(cpu.vectors[4], 0,
		std::bit_cast<std::uint32_t>(4.0F));
	arm64::detail::set_vector_half(cpu.vectors[6], 0,
		std::bit_cast<std::uint64_t>(6.0));
	arm64::detail::set_vector_half(cpu.vectors[7], 0,
		std::bit_cast<std::uint64_t>(7.0));
	cpu.vectors[2].bytes.fill(std::byte{0xaa});
	cpu.vectors[5].bytes.fill(std::byte{0xaa});
	auto result = arm64::Dispatcher{}.step(cpu, memory);
	require(result.reason == arm64::StopReason::continued);
	require(cpu.nzcv == 0x6000'0000U);
	result = arm64::Dispatcher{}.step(cpu, memory);
	require(result.reason == arm64::StopReason::continued);
	require(cpu.nzcv == 0x8000'0000U);
	result = arm64::Dispatcher{}.step(cpu, memory);
	require(result.reason == arm64::StopReason::continued);
	require(cpu.nzcv == 0x6000'0000U);
	result = arm64::Dispatcher{}.step(cpu, memory);
	require(result.reason == arm64::StopReason::continued);
	require(cpu.nzcv == 0x9000'0000U);
	result = arm64::Dispatcher{}.step(cpu, memory);
	require(result.reason == arm64::StopReason::continued);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::vector_half(cpu.vectors[2], 0))) == 3.0F);
	result = arm64::Dispatcher{}.step(cpu, memory);
	require(result.reason == arm64::StopReason::continued);
	require(cpu.nzcv == 0x6000'0000U);
	result = arm64::Dispatcher{}.step(cpu, memory);
	require(result.reason == arm64::StopReason::continued);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[5], 0)) == 7.0);
	for (std::size_t index = 4; index < 16; ++index)
		require(cpu.vectors[2].bytes[index] == std::byte{});
	for (std::size_t index = 8; index < 16; ++index)
		require(cpu.vectors[5].bytes[index] == std::byte{});
	result = arm64::Dispatcher{}.step(cpu, memory);
	require(result.reason == arm64::StopReason::system_call);
}

static void scalar_floating_moves_and_conversions()
{
	constexpr std::uint64_t base = 0xce00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x1e64'901fU);      // fmov d31, #10.0
	memory.put32(base + 4, 0x1e60'43efU);  // fmov d15, d31
	memory.put32(base + 8, 0x9e62'0060U);  // scvtf d0, x3
	memory.put32(base + 12, 0x9e23'0011U); // ucvtf s17, x0
	memory.put32(base + 16, 0x9e78'0004U); // fcvtzs x4, d0
	memory.put32(base + 20, 0x1e60'c006U); // fabs d6, d0
	memory.put32(base + 24, 0x1e7f'28c7U); // fadd d7, d6, d31
	memory.put32(base + 28, 0x1e2a'1928U); // fdiv s8, s9, s10
	memory.put32(base + 32, 0x1e62'40c9U); // fcvt s9, d6
	memory.put32(base + 36, 0x1e22'c12aU); // fcvt d10, s9
	memory.put32(base + 40, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[0] = 25;
	cpu.registers[3] = static_cast<std::uint64_t>(-37);
	arm64::detail::set_vector_half(cpu.vectors[9], 0,
		std::bit_cast<std::uint32_t>(25.0F));
	arm64::detail::set_vector_half(cpu.vectors[10], 0,
		std::bit_cast<std::uint32_t>(5.0F));
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 16);
	require(result.reason == arm64::StopReason::system_call);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[31], 0)) == 10.0);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[15], 0)) == 10.0);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[0], 0)) == -37.0);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::vector_half(cpu.vectors[17], 0))) == 25.0F);
	require(cpu.registers[4] == static_cast<std::uint64_t>(-37));
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[6], 0)) == 37.0);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[7], 0)) == 47.0);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::vector_half(cpu.vectors[8], 0))) == 5.0F);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::vector_half(cpu.vectors[9], 0))) == 37.0F);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[10], 0)) == 37.0);
}

static void scalar_fixed_point_conversions()
{
	constexpr std::uint64_t base = 0xce80;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x1e18'dfe0U);      // fcvtzs w0, s31, #9
	memory.put32(base + 4, 0x9e59'bc41U);  // fcvtzu x1, d2, #17
	memory.put32(base + 8, 0x1e02'e483U);  // scvtf s3, w4, #7
	memory.put32(base + 12, 0x9e43'7cc5U); // ucvtf d5, x6, #33
	memory.put32(base + 16, 0x1e19'f107U); // fcvtzu w7, s8, #4
	memory.put32(base + 20, 0x9e58'f949U); // fcvtzs x9, d10, #2
	memory.put32(base + 24, 0x1e03'e18bU); // ucvtf s11, w12, #8
	memory.put32(base + 28, 0x9e42'c1cdU); // scvtf d13, x14, #16
	memory.put32(base + 32, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	arm64::detail::set_vector_half(cpu.vectors[31], 0,
		std::bit_cast<std::uint32_t>(1.5F));
	arm64::detail::set_vector_half(cpu.vectors[2], 0,
		std::bit_cast<std::uint64_t>(1.25));
	cpu.registers[4] = static_cast<std::uint32_t>(-192);
	cpu.registers[6] = std::uint64_t{3} << 33;
	arm64::detail::set_vector_half(cpu.vectors[8], 0,
		std::bit_cast<std::uint32_t>(1.5F));
	arm64::detail::set_vector_half(cpu.vectors[10], 0,
		std::bit_cast<std::uint64_t>(-2.75));
	cpu.registers[12] = 640;
	cpu.registers[14] = static_cast<std::uint64_t>(
		static_cast<std::int64_t>(-98'304));
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 16);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[0] == 768);
	require(cpu.registers[1] == 163'840);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::vector_half(cpu.vectors[3], 0))) == -1.5F);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[5], 0)) == 3.0);
	require(cpu.registers[7] == 24);
	require(cpu.registers[9] == static_cast<std::uint64_t>(
		static_cast<std::int64_t>(-11)));
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::vector_half(cpu.vectors[11], 0))) == 2.5F);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[13], 0)) == -1.5);
}

static void scalar_floating_to_integer_rounding()
{
	constexpr std::uint64_t base = 0xcee0;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x9e64'03e3U);      // fcvtas x3, d31
	memory.put32(base + 4, 0x1e25'00a4U);  // fcvtau w4, s5
	memory.put32(base + 8, 0x9e60'00e6U);  // fcvtns x6, d7
	memory.put32(base + 12, 0x1e21'0128U); // fcvtnu w8, s9
	memory.put32(base + 16, 0x9e68'016aU); // fcvtps x10, d11
	memory.put32(base + 20, 0x1e29'01acU); // fcvtpu w12, s13
	memory.put32(base + 24, 0x9e70'01eeU); // fcvtms x14, d15
	memory.put32(base + 28, 0x1e31'0230U); // fcvtmu w16, s17
	memory.put32(base + 32, 0x9e78'0272U); // fcvtzs x18, d19
	memory.put32(base + 36, 0x1e39'02b4U); // fcvtzu w20, s21
	memory.put32(base + 40, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	arm64::detail::set_vector_half(cpu.vectors[31], 0,
		std::bit_cast<std::uint64_t>(2.5));
	arm64::detail::set_vector_half(cpu.vectors[5], 0,
		std::bit_cast<std::uint32_t>(-2.5F));
	arm64::detail::set_vector_half(cpu.vectors[7], 0,
		std::bit_cast<std::uint64_t>(2.5));
	arm64::detail::set_vector_half(cpu.vectors[9], 0,
		std::bit_cast<std::uint32_t>(3.5F));
	arm64::detail::set_vector_half(cpu.vectors[11], 0,
		std::bit_cast<std::uint64_t>(-2.1));
	arm64::detail::set_vector_half(cpu.vectors[13], 0,
		std::bit_cast<std::uint32_t>(2.1F));
	arm64::detail::set_vector_half(cpu.vectors[15], 0,
		std::bit_cast<std::uint64_t>(-2.1));
	arm64::detail::set_vector_half(cpu.vectors[17], 0,
		std::bit_cast<std::uint32_t>(2.9F));
	arm64::detail::set_vector_half(cpu.vectors[19], 0,
		std::bit_cast<std::uint64_t>(-2.9));
	arm64::detail::set_vector_half(cpu.vectors[21], 0,
		std::bit_cast<std::uint32_t>(2.9F));
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 16);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[3] == 3);
	require(cpu.registers[4] == 0);
	require(cpu.registers[6] == 2);
	require(cpu.registers[8] == 4);
	require(cpu.registers[10] == static_cast<std::uint64_t>(
		static_cast<std::int64_t>(-2)));
	require(cpu.registers[12] == 3);
	require(cpu.registers[14] == static_cast<std::uint64_t>(
		static_cast<std::int64_t>(-3)));
	require(cpu.registers[16] == 2);
	require(cpu.registers[18] == static_cast<std::uint64_t>(
		static_cast<std::int64_t>(-2)));
	require(cpu.registers[20] == 2);
}

static void scalar_floating_multiply_add()
{
	constexpr std::uint64_t base = 0xcf00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x1f02'0c20U);      // fmadd s0, s1, s2, s3
	memory.put32(base + 4, 0x1f06'9ca4U);  // fmsub s4, s5, s6, s7
	memory.put32(base + 8, 0x1f6a'2d28U);  // fnmadd d8, d9, d10, d11
	memory.put32(base + 12, 0x1f6e'bdacU); // fnmsub d12, d13, d14, d15
	memory.put32(base + 16, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	for (const auto index : {1U, 5U})
		arm64::detail::set_vector_half(cpu.vectors[index], 0,
			std::bit_cast<std::uint32_t>(2.0F));
	for (const auto index : {2U, 6U})
		arm64::detail::set_vector_half(cpu.vectors[index], 0,
			std::bit_cast<std::uint32_t>(3.0F));
	arm64::detail::set_vector_half(cpu.vectors[3], 0,
		std::bit_cast<std::uint32_t>(4.0F));
	arm64::detail::set_vector_half(cpu.vectors[7], 0,
		std::bit_cast<std::uint32_t>(20.0F));
	for (const auto index : {9U, 13U})
		arm64::detail::set_vector_half(cpu.vectors[index], 0,
			std::bit_cast<std::uint64_t>(2.0));
	for (const auto index : {10U, 14U})
		arm64::detail::set_vector_half(cpu.vectors[index], 0,
			std::bit_cast<std::uint64_t>(3.0));
	arm64::detail::set_vector_half(cpu.vectors[11], 0,
		std::bit_cast<std::uint64_t>(4.0));
	arm64::detail::set_vector_half(cpu.vectors[15], 0,
		std::bit_cast<std::uint64_t>(20.0));
	for (const auto destination : {0U, 4U, 8U, 12U})
		cpu.vectors[destination].bytes.fill(std::byte{0xaa});
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::vector_half(cpu.vectors[0], 0))) == 10.0F);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::vector_half(cpu.vectors[4], 0))) == 14.0F);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[8], 0)) == -10.0);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[12], 0)) == -14.0);
	for (std::size_t index = 4; index < 16; ++index) {
		require(cpu.vectors[0].bytes[index] == std::byte{});
		require(cpu.vectors[4].bytes[index] == std::byte{});
	}
	for (std::size_t index = 8; index < 16; ++index) {
		require(cpu.vectors[8].bytes[index] == std::byte{});
		require(cpu.vectors[12].bytes[index] == std::byte{});
	}
}

static void scalar_floating_rounding()
{
	constexpr std::uint64_t base = 0xcf80;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x1e24'4020U);      // frintn s0, s1
	memory.put32(base + 4, 0x1e24'c062U);  // frintp s2, s3
	memory.put32(base + 8, 0x1e65'40a4U);  // frintm d4, d5
	memory.put32(base + 12, 0x1e65'c0e6U); // frintz d6, d7
	memory.put32(base + 16, 0x1e26'4128U); // frinta s8, s9
	memory.put32(base + 20, 0x1e67'416aU); // frintx d10, d11
	memory.put32(base + 24, 0x1e67'c1acU); // frinti d12, d13
	memory.put32(base + 28, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.fpcr = 2U << 22; // Round toward minus infinity for FRINTX/FRINTI.
	arm64::detail::set_vector_half(cpu.vectors[1], 0,
		std::bit_cast<std::uint32_t>(2.5F));
	arm64::detail::set_vector_half(cpu.vectors[3], 0,
		std::bit_cast<std::uint32_t>(-1.25F));
	arm64::detail::set_vector_half(cpu.vectors[5], 0,
		std::bit_cast<std::uint64_t>(-1.25));
	arm64::detail::set_vector_half(cpu.vectors[7], 0,
		std::bit_cast<std::uint64_t>(-1.75));
	arm64::detail::set_vector_half(cpu.vectors[9], 0,
		std::bit_cast<std::uint32_t>(-2.5F));
	arm64::detail::set_vector_half(cpu.vectors[11], 0,
		std::bit_cast<std::uint64_t>(1.25));
	arm64::detail::set_vector_half(cpu.vectors[13], 0,
		std::bit_cast<std::uint64_t>(1.75));
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 12);
	require(result.reason == arm64::StopReason::system_call);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::vector_half(cpu.vectors[0], 0))) == 2.0F);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::vector_half(cpu.vectors[2], 0))) == -1.0F);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[4], 0)) == -2.0);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[6], 0)) == -1.0);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::vector_half(cpu.vectors[8], 0))) == -3.0F);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[10], 0)) == 1.0);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[12], 0)) == 1.0);
	require((cpu.fpsr & (1U << 4)) != 0);
}

static void addressing_and_conditional_select()
{
	constexpr std::uint64_t base = 0xd000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0xf81f'8fe0U);      // str x0, [sp, #-8]!
	memory.put32(base + 4, 0xf840'87e1U);  // ldr x1, [sp], #8
	memory.put32(base + 8, 0xda9f'0022U);  // csinv x2, x1, xzr, eq
	memory.put32(base + 12, 0xf8b4'6800U); // prfm pldl1keep, [x0, x20]
	memory.put32(base + 16, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.sp = base + 0x800;
	cpu.registers[0] = 0x1234;
	cpu.nzcv = 0;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.sp == base + 0x800);
	require(cpu.registers[1] == 0x1234);
	require(cpu.registers[2] == ~std::uint64_t{0});
}

static void scalar_acquire_release_exclusive()
{
	constexpr std::uint64_t base = 0xdc00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0xc89f'fc22U);      // stlr x2, [x1]
	memory.put32(base + 4, 0xc8df'fc23U);  // ldar x3, [x1]
	memory.put32(base + 8, 0xc85f'7c24U);  // ldxr x4, [x1]
	memory.put32(base + 12, 0xc805'fc26U); // stlxr w5, x6, [x1]
	memory.put32(base + 16, 0xc8df'fc27U); // ldar x7, [x1]
	memory.put32(base + 20, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[1] = base + 0x800;
	cpu.registers[2] = 0x1122'3344'5566'7788ULL;
	cpu.registers[6] = 0x8877'6655'4433'2211ULL;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[3] == 0x1122'3344'5566'7788ULL);
	require(cpu.registers[4] == 0x1122'3344'5566'7788ULL);
	require(cpu.registers[5] == 0);
	require(cpu.registers[7] == 0x8877'6655'4433'2211ULL);
}

static void scalar_same_value_store_invalidates_exclusive()
{
	constexpr std::uint64_t base = 0xdd00;
	constexpr std::uint64_t data = base + 0x800;
	constexpr std::uint64_t value = 0x1122'3344'5566'7788ULL;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0xc85f'7c22U);     // ldxr x2, [x1]
	memory.put32(base + 4, 0xd400'0001U); // svc #0
	memory.put32(base + 8, 0xc89f'fc24U); // stlr x4, [x1]
	memory.put32(base + 12, 0xd400'0001U); // svc #0
	memory.put32(base + 16, 0xc805'fc23U); // stlxr w5, x3, [x1]
	memory.put32(base + 20, 0xd400'0001U); // svc #0
	std::array<std::byte, sizeof(value)> encoded{};
	std::memcpy(encoded.data(), &value, sizeof(value));
	require(memory.write(data, encoded));

	arm64::CpuState reserver{};
	reserver.pc = base;
	reserver.registers[1] = data;
	reserver.registers[3] = 0x8877'6655'4433'2211ULL;
	const auto load = arm64::Dispatcher{}.run(reserver, memory, 2);
	require(load.reason == arm64::StopReason::system_call);
	require(reserver.registers[2] == value);

	arm64::CpuState writer{};
	writer.pc = base + 8;
	writer.registers[1] = data;
	writer.registers[4] = value;
	const auto store = arm64::Dispatcher{}.run(writer, memory, 2);
	require(store.reason == arm64::StopReason::system_call);

	reserver.pc = base + 16;
	const auto exclusive_store = arm64::Dispatcher{}.run(reserver, memory, 2);
	require(exclusive_store.reason == arm64::StopReason::system_call);
	require(reserver.registers[5] == 1);
	std::uint64_t actual = 0;
	std::array<std::byte, sizeof(actual)> result{};
	require(memory.read(data, result));
	std::memcpy(&actual, result.data(), sizeof(actual));
	require(actual == value);
}

static void split_alias_fails_exclusive_store()
{
	constexpr std::uint64_t base = 0xdd80;
	constexpr std::uint64_t data = 0x1'0000;
	constexpr std::uint64_t initial = 0x1122'3344'5566'7788ULL;
	SplitAtomicMemory memory(base, data);
	memory.put32(base, 0xc85f'7c22U);     // ldxr x2, [x1]
	memory.put32(base + 4, 0xd400'0001U); // svc #0
	memory.put32(base + 8, 0xc805'fc23U); // stlxr w5, x3, [x1]
	memory.put32(base + 12, 0xd400'0001U); // svc #0
	memory.read_value() = initial;
	memory.write_value() = initial;

	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[1] = data;
	cpu.registers[3] = 0x8877'6655'4433'2211ULL;
	require(arm64::Dispatcher{}.run(cpu, memory, 2).reason ==
		arm64::StopReason::system_call);
	require(cpu.registers[2] == initial);
	cpu.pc = base + 8;
	require(arm64::Dispatcher{}.run(cpu, memory, 2).reason ==
		arm64::StopReason::system_call);
	require(cpu.registers[5] == 1);
	require(memory.read_value() == initial);
	require(memory.write_value() == initial);
}

static void unrelated_store_preserves_exclusive()
{
	constexpr std::uint64_t base = 0xde00;
	constexpr std::uint64_t reserved_data = base + 0x800;
	constexpr std::uint64_t unrelated_data = base + 0x820;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0xc85f'7c22U);      // ldxr x2, [x1]
	memory.put32(base + 4, 0xd400'0001U);  // svc #0
	memory.put32(base + 8, 0xc89f'fc24U);  // stlr x4, [x1]
	memory.put32(base + 12, 0xd400'0001U); // svc #0
	memory.put32(base + 16, 0xc805'fc23U); // stlxr w5, x3, [x1]
	memory.put32(base + 20, 0xd400'0001U); // svc #0

	arm64::CpuState reserver{};
	reserver.pc = base;
	reserver.registers[1] = reserved_data;
	reserver.registers[3] = 0x8877'6655'4433'2211ULL;
	require(arm64::Dispatcher{}.run(reserver, memory, 2).reason ==
		arm64::StopReason::system_call);

	arm64::CpuState writer{};
	writer.pc = base + 8;
	writer.registers[1] = unrelated_data;
	writer.registers[4] = 0x0123'4567'89ab'cdefULL;
	require(arm64::Dispatcher{}.run(writer, memory, 2).reason ==
		arm64::StopReason::system_call);

	reserver.pc = base + 16;
	require(arm64::Dispatcher{}.run(reserver, memory, 2).reason ==
		arm64::StopReason::system_call);
	require(reserver.registers[5] == 0);
	std::uint64_t actual = 0;
	std::array<std::byte, sizeof(actual)> bytes{};
	require(memory.read(reserved_data, bytes));
	std::memcpy(&actual, bytes.data(), sizeof(actual));
	require(actual == reserver.registers[3]);
}

static void bitfield_aliases()
{
	constexpr std::uint64_t base = 0xe000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x1301'7c84U);     // asr w4, w4, #1
	memory.put32(base + 4, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[4] = 0x8000'0000U;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 4);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[4] == 0xc000'0000U);
}

static void extract_register_forms()
{
	constexpr std::uint64_t base = 0xe800;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x93c0'8260U);      // extr x0, x19, x0, #32
	memory.put32(base + 4, 0x1383'1c41U);  // extr w1, w2, w3, #7
	memory.put32(base + 8, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[0] = 0x1122'3344'5566'7788ULL;
	cpu.registers[19] = 0x99aa'bbcc'ddee'ff00ULL;
	cpu.registers[2] = 0xaabb'ccddU;
	cpu.registers[3] = 0x1122'3344U;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 6);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[0] == 0xddee'ff00'1122'3344ULL);
	require(cpu.registers[1] == 0xba22'4466U);
}

static void simd_zero_and_pair_store()
{
	constexpr std::uint64_t base = 0xf000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4f00'041fU);     // movi v31.4s, #0
	memory.put32(base + 4, 0xad00'7c7fU); // stp q31, q31, [x3]
	memory.put32(base + 8, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[3] = base + 0x800;
	cpu.vectors[31].bytes.fill(std::byte{0xff});
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	for (std::uint64_t offset = 0; offset < 32; offset += 8)
		require(memory.get64(cpu.registers[3] + offset) == 0);
}

static void simd_single_load_store_addressing()
{
	constexpr std::uint64_t base = 0x10'000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x3d80'0000U);      // str q0, [x0]
	memory.put32(base + 4, 0x3ca3'6800U);  // str q0, [x0, x3]
	memory.put32(base + 8, 0x3c9f'00a0U);  // stur q0, [x5, #-16]
	memory.put32(base + 12, 0x3dc0'0001U); // ldr q1, [x0]
	memory.put32(base + 16, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[0] = base + 0x800;
	cpu.registers[3] = 0x20;
	cpu.registers[5] = base + 0x850;
	for (std::size_t index = 0; index < cpu.vectors[0].bytes.size(); ++index)
		cpu.vectors[0].bytes[index] = static_cast<std::byte>(index + 1);
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.vectors[1].bytes == cpu.vectors[0].bytes);
	require(memory.get64(base + 0x800) == 0x0807'0605'0403'0201ULL);
	require(memory.get64(base + 0x820) == 0x0807'0605'0403'0201ULL);
	require(memory.get64(base + 0x840) == 0x0807'0605'0403'0201ULL);
}

static void simd_contiguous_structure_load_store()
{
	constexpr std::uint64_t base = 0x14'000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4c00'7020U);      // st1 {v0.16b}, [x1]
	memory.put32(base + 4, 0x4c40'a022U);  // ld1 {v2.16b,v3.16b}, [x1]
	memory.put32(base + 8, 0x0c9f'a0c4U);  // st1 {v4.8b,v5.8b}, [x6], #16
	memory.put32(base + 12, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[1] = base + 0x800;
	cpu.registers[6] = base + 0x840;
	for (std::size_t index = 0; index < cpu.vectors[0].bytes.size(); ++index)
		cpu.vectors[0].bytes[index] = static_cast<std::byte>(0xa0 + index);
	cpu.vectors[4].bytes.fill(std::byte{0x44});
	cpu.vectors[5].bytes.fill(std::byte{0x55});
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.vectors[2].bytes == cpu.vectors[0].bytes);
	for (const auto byte : cpu.vectors[3].bytes)
		require(byte == std::byte{});
	require(memory.get64(base + 0x840) == 0x4444'4444'4444'4444ULL);
	require(memory.get64(base + 0x848) == 0x5555'5555'5555'5555ULL);
	require(cpu.registers[6] == base + 0x850);
}

static void simd_single_structure_load_store()
{
	constexpr std::uint64_t base = 0x14'800;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4d40'cc1fU);      // ld1r {v31.2d}, [x0]
	memory.put32(base + 4, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[0] = base + 0x800;
	const std::array value{
		std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
		std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
	};
	require(memory.write(cpu.registers[0], value));
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 4);
	require(result.reason == arm64::StopReason::system_call);
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[31].bytes}.first(8)) ==
		0x0102'0304'0506'0708ULL);
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[31].bytes}.subspan(8, 8)) ==
		0x0102'0304'0506'0708ULL);
}

static void simd_signed_compare_zero()
{
	constexpr std::uint64_t base = 0x15'000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4e20'9801U);      // cmeq v1.16b, v0.16b, #0
	memory.put32(base + 4, 0x6e60'8802U);  // cmge v2.8h, v0.8h, #0
	memory.put32(base + 8, 0x4ee0'a803U);  // cmlt v3.2d, v0.2d, #0
	memory.put32(base + 12, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.vectors[0].bytes.fill(std::byte{1});
	cpu.vectors[0].bytes[0] = std::byte{};
	cpu.vectors[0].bytes[1] = std::byte{0x80};
	cpu.vectors[0].bytes[7] = std::byte{0x80};
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.vectors[1].bytes[0] == std::byte{0xff});
	require(cpu.vectors[1].bytes[1] == std::byte{});
	require(cpu.vectors[2].bytes[0] == std::byte{});
	require(cpu.vectors[2].bytes[1] == std::byte{});
	require(cpu.vectors[2].bytes[2] == std::byte{0xff});
	require(cpu.vectors[2].bytes[3] == std::byte{0xff});
	for (std::size_t index = 0; index < 8; ++index)
		require(cpu.vectors[3].bytes[index] == std::byte{0xff});
	for (std::size_t index = 8; index < 16; ++index)
		require(cpu.vectors[3].bytes[index] == std::byte{});
}

static void simd_scalar_signed_compare_zero()
{
	constexpr std::uint64_t base = 0x15'800;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x5ee0'8801U);      // cmgt d1, d0, #0
	memory.put32(base + 4, 0x7ee0'8802U);  // cmge d2, d0, #0
	memory.put32(base + 8, 0x5ee0'9803U);  // cmeq d3, d0, #0
	memory.put32(base + 12, 0x7ee0'9804U); // cmle d4, d0, #0
	memory.put32(base + 16, 0x5ee0'a805U); // cmlt d5, d0, #0
	memory.put32(base + 20, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.vectors[0].bytes.fill(std::byte{0xff});
	for (unsigned index = 1; index <= 5; ++index)
		cpu.vectors[index].bytes.fill(std::byte{0x55});
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 12);
	require(result.reason == arm64::StopReason::system_call);
	for (unsigned index = 1; index <= 3; ++index)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[index].bytes}.first(8)) == 0);
	for (unsigned index = 4; index <= 5; ++index)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[index].bytes}.first(8)) ==
			std::numeric_limits<std::uint64_t>::max());
	for (unsigned index = 1; index <= 5; ++index)
		for (std::size_t byte = 8; byte < 16; ++byte)
			require(cpu.vectors[index].bytes[byte] == std::byte{});
}

static void simd_integer_compare_register()
{
	constexpr std::uint64_t base = 0x15'800;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x6ee3'3486U);      // cmhi v6.2d, v4.2d, v3.2d
	memory.put32(base + 4, 0x4e61'3444U);  // cmgt v4.8h, v2.8h, v1.8h
	memory.put32(base + 8, 0x6e20'8c23U);  // cmeq v3.16b, v1.16b, v0.16b
	memory.put32(base + 12, 0x4e69'8d07U); // cmtst v7.8h, v8.8h, v9.8h
	memory.put32(base + 16, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.vectors[0].bytes.fill(std::byte{0x20});
	cpu.vectors[1].bytes.fill(std::byte{0x20});
	cpu.vectors[1].bytes[3] = std::byte{0x21};
	cpu.vectors[2].bytes.fill(std::byte{0x30});
	cpu.vectors[2].bytes[0] = std::byte{0xff};
	cpu.vectors[2].bytes[1] = std::byte{0xff};
	arm64::detail::set_vector_half(cpu.vectors[4], 0, 9);
	arm64::detail::set_vector_half(cpu.vectors[4], 1, 2);
	arm64::detail::set_vector_half(cpu.vectors[3], 0, 8);
	arm64::detail::set_vector_half(cpu.vectors[3], 1, 3);
	cpu.vectors[8].bytes.fill(std::byte{0x0f});
	cpu.vectors[9].bytes.fill(std::byte{0xf0});
	cpu.vectors[9].bytes[2] = std::byte{0x01};
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t index = 0; index < 16; ++index)
		require(cpu.vectors[3].bytes[index] ==
			(index == 3 ? std::byte{} : std::byte{0xff}));
	require(cpu.vectors[4].bytes[0] == std::byte{});
	require(cpu.vectors[4].bytes[1] == std::byte{});
	for (std::size_t index = 2; index < 16; ++index)
		require(cpu.vectors[4].bytes[index] == std::byte{0xff});
	for (std::size_t index = 0; index < 8; ++index)
		require(cpu.vectors[6].bytes[index] == std::byte{0xff});
	for (std::size_t index = 8; index < 16; ++index)
		require(cpu.vectors[6].bytes[index] == std::byte{});
	for (std::size_t index = 0; index < 16; ++index)
		require(cpu.vectors[7].bytes[index] ==
			(index == 2 || index == 3 ? std::byte{0xff} : std::byte{}));
}

static void simd_bitwise_register_operations()
{
	constexpr std::uint64_t base = 0x15'c00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4e22'1c20U);      // and v0.16b, v1.16b, v2.16b
	memory.put32(base + 4, 0x0e65'1c83U);  // bic v3.8b, v4.8b, v5.8b
	memory.put32(base + 8, 0x4ea8'1ce6U);  // orr v6.16b, v7.16b, v8.16b
	memory.put32(base + 12, 0x0eeb'1d49U); // orn v9.8b, v10.8b, v11.8b
	memory.put32(base + 16, 0x6e2e'1dacU); // eor v12.16b, v13.16b, v14.16b
	memory.put32(base + 20, 0x2e71'1e0fU); // bsl v15.8b, v16.8b, v17.8b
	memory.put32(base + 24, 0x6eb4'1e72U); // bit v18.16b, v19.16b, v20.16b
	memory.put32(base + 28, 0x2ef7'1ed5U); // bif v21.8b, v22.8b, v23.8b
	memory.put32(base + 32, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	for (auto &vector : cpu.vectors)
		vector.bytes.fill(std::byte{});
	cpu.vectors[1].bytes.fill(std::byte{0xf0});
	cpu.vectors[2].bytes.fill(std::byte{0x3c});
	cpu.vectors[4].bytes.fill(std::byte{0xf0});
	cpu.vectors[5].bytes.fill(std::byte{0x0f});
	cpu.vectors[7].bytes.fill(std::byte{0xf0});
	cpu.vectors[8].bytes.fill(std::byte{0x0f});
	cpu.vectors[11].bytes.fill(std::byte{0xf0});
	cpu.vectors[13].bytes.fill(std::byte{0xaa});
	cpu.vectors[14].bytes.fill(std::byte{0xff});
	cpu.vectors[15].bytes.fill(std::byte{0xf0});
	cpu.vectors[16].bytes.fill(std::byte{0xaa});
	cpu.vectors[17].bytes.fill(std::byte{0x55});
	cpu.vectors[18].bytes.fill(std::byte{0xf0});
	cpu.vectors[19].bytes.fill(std::byte{0xaa});
	cpu.vectors[20].bytes.fill(std::byte{0x0f});
	cpu.vectors[21].bytes.fill(std::byte{0xf0});
	cpu.vectors[22].bytes.fill(std::byte{0xaa});
	cpu.vectors[23].bytes.fill(std::byte{0x0f});
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 16);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t index = 0; index < 16; ++index) {
		require(cpu.vectors[0].bytes[index] == std::byte{0x30});
		require(cpu.vectors[6].bytes[index] == std::byte{0xff});
		require(cpu.vectors[12].bytes[index] == std::byte{0x55});
		require(cpu.vectors[18].bytes[index] == std::byte{0xfa});
	}
	for (std::size_t index = 0; index < 8; ++index) {
		require(cpu.vectors[3].bytes[index] == std::byte{0xf0});
		require(cpu.vectors[9].bytes[index] == std::byte{0x0f});
		require(cpu.vectors[15].bytes[index] == std::byte{0xa5});
		require(cpu.vectors[21].bytes[index] == std::byte{0xa0});
	}
	for (std::size_t index = 8; index < 16; ++index) {
		require(cpu.vectors[3].bytes[index] == std::byte{});
		require(cpu.vectors[9].bytes[index] == std::byte{});
		require(cpu.vectors[15].bytes[index] == std::byte{});
		require(cpu.vectors[21].bytes[index] == std::byte{});
	}
}

static void simd_integer_min_max()
{
	constexpr std::uint64_t base = 0x15'e00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4e22'6420U);      // smax v0.16b, v1.16b, v2.16b
	memory.put32(base + 4, 0x4e65'6c83U);  // smin v3.8h, v4.8h, v5.8h
	memory.put32(base + 8, 0x6ea8'64e6U);  // umax v6.4s, v7.4s, v8.4s
	memory.put32(base + 12, 0x6e2b'6d49U); // umin v9.16b, v10.16b, v11.16b
	memory.put32(base + 16, 0x4e2e'a5acU); // smaxp v12.16b, v13.16b, v14.16b
	memory.put32(base + 20, 0x6eb4'a672U); // umaxp v18.4s, v19.4s, v20.4s
	memory.put32(base + 24, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.vectors[1].bytes.fill(std::byte{0x80});
	cpu.vectors[2].bytes.fill(std::byte{0x7f});
	cpu.vectors[4].bytes.fill(std::byte{0xff});
	cpu.vectors[5].bytes.fill(std::byte{});
	cpu.vectors[7].bytes.fill(std::byte{0xff});
	cpu.vectors[8].bytes.fill(std::byte{1});
	cpu.vectors[10].bytes.fill(std::byte{0xaa});
	cpu.vectors[11].bytes.fill(std::byte{0x55});
	for (std::size_t index = 0; index < 16; index += 2) {
		cpu.vectors[13].bytes[index] = std::byte{0xff};
		cpu.vectors[13].bytes[index + 1] = std::byte{1};
		cpu.vectors[14].bytes[index] = std::byte{3};
		cpu.vectors[14].bytes[index + 1] = std::byte{2};
	}
	for (std::size_t lane = 0; lane < 4; ++lane) {
		arm64::detail::encode_little_endian(lane * 4 + 1,
			std::span{cpu.vectors[19].bytes}.subspan(lane * 4, 4));
		arm64::detail::encode_little_endian(lane * 4 + 2,
			std::span{cpu.vectors[20].bytes}.subspan(lane * 4, 4));
	}
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 12);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t index = 0; index < 16; ++index) {
		require(cpu.vectors[0].bytes[index] == std::byte{0x7f});
		require(cpu.vectors[3].bytes[index] == std::byte{0xff});
		require(cpu.vectors[6].bytes[index] == std::byte{0xff});
		require(cpu.vectors[9].bytes[index] == std::byte{0x55});
		require(cpu.vectors[12].bytes[index] ==
			(index < 8 ? std::byte{1} : std::byte{3}));
	}
	for (std::size_t lane = 0; lane < 4; ++lane) {
		const auto value = arm64::detail::decode_little_endian(
			std::span{cpu.vectors[18].bytes}.subspan(lane * 4, 4));
		require(value == (lane < 2 ? lane * 8 + 5 : (lane - 2) * 8 + 6));
	}
}

static void simd_unary_bits_and_reversal()
{
	constexpr std::uint64_t base = 0x15'f00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4e20'5820U);      // cnt v0.16b, v1.16b
	memory.put32(base + 4, 0x2e60'5862U);  // rbit v2.8b, v3.8b
	memory.put32(base + 8, 0x6e20'58a4U);  // mvn v4.16b, v5.16b
	memory.put32(base + 12, 0x4e20'08e6U); // rev64 v6.16b, v7.16b
	memory.put32(base + 16, 0x4e20'1a30U); // rev16 v16.16b, v17.16b
	memory.put32(base + 20, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.vectors[1].bytes.fill(std::byte{0xf0});
	cpu.vectors[3].bytes.fill(std::byte{1});
	cpu.vectors[5].bytes.fill(std::byte{0xaa});
	for (std::size_t index = 0; index < 16; ++index) {
		cpu.vectors[7].bytes[index] = static_cast<std::byte>(index);
		cpu.vectors[17].bytes[index] = static_cast<std::byte>(index);
	}
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 10);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t index = 0; index < 16; ++index) {
		require(cpu.vectors[0].bytes[index] == std::byte{4});
		require(cpu.vectors[4].bytes[index] == std::byte{0x55});
		require(cpu.vectors[6].bytes[index] ==
			static_cast<std::byte>((index & ~std::size_t{7}) +
				(7 - (index & 7))));
		require(cpu.vectors[16].bytes[index] ==
			static_cast<std::byte>(index ^ 1));
	}
	for (std::size_t index = 0; index < 8; ++index)
		require(cpu.vectors[2].bytes[index] == std::byte{0x80});
	for (std::size_t index = 8; index < 16; ++index)
		require(cpu.vectors[2].bytes[index] == std::byte{});
}

static void simd_integer_reductions()
{
	constexpr std::uint64_t base = 0x15'f80;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x0e31'bbffU);      // addv b31, v31.8b
	memory.put32(base + 4, 0x4e30'a8e6U);  // smaxv b6, v7.16b
	memory.put32(base + 8, 0x6e71'a9acU);  // uminv h12, v13.8h
	memory.put32(base + 12, 0x0e30'3820U); // saddlv h0, v1.8b
	memory.put32(base + 16, 0x4e70'3862U); // saddlv s2, v3.8h
	memory.put32(base + 20, 0x6e30'38a4U); // uaddlv h4, v5.16b
	memory.put32(base + 24, 0x6eb0'3928U); // uaddlv d8, v9.4s
	memory.put32(base + 28, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.vectors[31].bytes.fill(std::byte{1});
	cpu.vectors[7].bytes.fill(std::byte{0x80});
	cpu.vectors[7].bytes[9] = std::byte{0x7f};
	cpu.vectors[13].bytes.fill(std::byte{0xff});
	cpu.vectors[13].bytes[4] = std::byte{0x34};
	cpu.vectors[13].bytes[5] = std::byte{0x12};
	cpu.vectors[1].bytes.fill(std::byte{1});
	cpu.vectors[1].bytes[0] = std::byte{0x80};
	for (std::size_t lane = 0; lane < 8; ++lane)
		arm64::detail::encode_little_endian(lane == 0 ? 0x8000 : 1,
			std::span{cpu.vectors[3].bytes}.subspan(lane * 2, 2));
	cpu.vectors[5].bytes.fill(std::byte{0xff});
	for (std::size_t lane = 0; lane < 4; ++lane)
		arm64::detail::encode_little_endian(0xffff'ffffU,
			std::span{cpu.vectors[9].bytes}.subspan(lane * 4, 4));
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 12);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.vectors[31].bytes[0] == std::byte{8});
	require(cpu.vectors[6].bytes[0] == std::byte{0x7f});
	require(cpu.vectors[12].bytes[0] == std::byte{0x34});
	require(cpu.vectors[12].bytes[1] == std::byte{0x12});
	for (std::size_t index = 1; index < 16; ++index) {
		require(cpu.vectors[31].bytes[index] == std::byte{});
		require(cpu.vectors[6].bytes[index] == std::byte{});
	}
	for (std::size_t index = 2; index < 16; ++index)
		require(cpu.vectors[12].bytes[index] == std::byte{});
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[0].bytes}.first(2)) == 0xff87);
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[2].bytes}.first(4)) == 0xffff'8007U);
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[4].bytes}.first(2)) == 0x0ff0);
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[8].bytes}.first(8)) == 0x3'ffff'fffcULL);
	for (std::size_t index = 2; index < 16; ++index) {
		require(cpu.vectors[0].bytes[index] == std::byte{});
		require(cpu.vectors[4].bytes[index] == std::byte{});
	}
	for (std::size_t index = 4; index < 16; ++index)
		require(cpu.vectors[2].bytes[index] == std::byte{});
	for (std::size_t index = 8; index < 16; ++index)
		require(cpu.vectors[8].bytes[index] == std::byte{});
}

static void simd_byte_extract()
{
	constexpr std::uint64_t base = 0x15'fc0;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x6e02'4020U);     // ext v0.16b, v1.16b, v2.16b, #8
	memory.put32(base + 4, 0x2e05'1883U); // ext v3.8b, v4.8b, v5.8b, #3
	memory.put32(base + 8, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	for (std::size_t index = 0; index < 16; ++index) {
		cpu.vectors[1].bytes[index] = static_cast<std::byte>(index);
		cpu.vectors[2].bytes[index] = static_cast<std::byte>(0x10 + index);
	}
	for (std::size_t index = 0; index < 8; ++index) {
		cpu.vectors[4].bytes[index] = static_cast<std::byte>(0x40 + index);
		cpu.vectors[5].bytes[index] = static_cast<std::byte>(0x50 + index);
	}
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 6);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t index = 0; index < 16; ++index)
		require(cpu.vectors[0].bytes[index] ==
			static_cast<std::byte>(8 + index));
	for (std::size_t index = 0; index < 8; ++index)
		require(cpu.vectors[3].bytes[index] ==
			static_cast<std::byte>(index < 5 ? 0x43 + index :
				0x50 + index - 5));
	for (std::size_t index = 8; index < 16; ++index)
		require(cpu.vectors[3].bytes[index] == std::byte{});
}

static void simd_integer_arithmetic()
{
	constexpr std::uint64_t base = 0x15'fe0;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4e22'8420U);      // add v0.16b, v1.16b, v2.16b
	memory.put32(base + 4, 0x6ea8'84e6U);  // sub v6.4s, v7.4s, v8.4s
	memory.put32(base + 8, 0x4e2b'bd49U);  // addp v9.16b, v10.16b, v11.16b
	memory.put32(base + 12, 0x4eb1'9e0fU); // mul v15.4s, v16.4s, v17.4s
	memory.put32(base + 16, 0x5ef4'8672U); // add d18, d19, d20
	memory.put32(base + 20, 0x7ef7'86d5U); // sub d21, d22, d23
	memory.put32(base + 24, 0x5ef1'bb19U); // addp d25, v24.2d
	memory.put32(base + 28, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.vectors[1].bytes.fill(std::byte{0xf0});
	cpu.vectors[2].bytes.fill(std::byte{0x20});
	for (std::size_t lane = 0; lane < 4; ++lane) {
		arm64::detail::encode_little_endian(3,
			std::span{cpu.vectors[7].bytes}.subspan(lane * 4, 4));
		arm64::detail::encode_little_endian(1,
			std::span{cpu.vectors[8].bytes}.subspan(lane * 4, 4));
		arm64::detail::encode_little_endian(2,
			std::span{cpu.vectors[16].bytes}.subspan(lane * 4, 4));
		arm64::detail::encode_little_endian(3,
			std::span{cpu.vectors[17].bytes}.subspan(lane * 4, 4));
	}
	for (std::size_t index = 0; index < 16; ++index)
		cpu.vectors[10].bytes[index] = static_cast<std::byte>(index + 1);
	cpu.vectors[11].bytes.fill(std::byte{1});
	arm64::detail::set_vector_half(cpu.vectors[19], 0, 9);
	arm64::detail::set_vector_half(cpu.vectors[20], 0, 4);
	arm64::detail::set_vector_half(cpu.vectors[22], 0, 17);
	arm64::detail::set_vector_half(cpu.vectors[23], 0, 6);
	arm64::detail::set_vector_half(cpu.vectors[24], 0,
		0xffff'ffff'ffff'fff0ULL);
	arm64::detail::set_vector_half(cpu.vectors[24], 1, 0x30);
	cpu.vectors[18].bytes.fill(std::byte{0xff});
	cpu.vectors[21].bytes.fill(std::byte{0xff});
	cpu.vectors[25].bytes.fill(std::byte{0xff});
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 10);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t index = 0; index < 16; ++index) {
		require(cpu.vectors[0].bytes[index] == std::byte{0x10});
		require(cpu.vectors[9].bytes[index] ==
			(index < 8 ? static_cast<std::byte>(index * 4 + 3) :
				std::byte{2}));
	}
	for (std::size_t lane = 0; lane < 4; ++lane) {
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[6].bytes}.subspan(lane * 4, 4)) == 2);
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[15].bytes}.subspan(lane * 4, 4)) == 6);
	}
	require(arm64::detail::vector_half(cpu.vectors[18], 0) == 13);
	require(arm64::detail::vector_half(cpu.vectors[21], 0) == 11);
	require(arm64::detail::vector_half(cpu.vectors[25], 0) == 0x20);
	require(arm64::detail::vector_half(cpu.vectors[18], 1) == 0);
	require(arm64::detail::vector_half(cpu.vectors[21], 1) == 0);
	require(arm64::detail::vector_half(cpu.vectors[25], 1) == 0);
}

static void simd_narrowing_arithmetic()
{
	constexpr std::uint64_t base = 0x15'ff8;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x0e21'4022U);      // addhn v2.8b, v1.8h, v1.8h
	memory.put32(base + 4, 0x6e6b'4149U);  // raddhn2 v9.8h, v10.4s, v11.4s
	memory.put32(base + 8, 0x0eae'61acU);  // subhn v12.2s, v13.2d, v14.2d
	memory.put32(base + 12, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	for (std::size_t lane = 0; lane < 8; ++lane)
		arm64::detail::encode_little_endian(0x2300,
			std::span{cpu.vectors[1].bytes}.subspan(lane * 2, 2));
	cpu.vectors[9].bytes.fill(std::byte{0xaa});
	for (std::size_t lane = 0; lane < 4; ++lane) {
		arm64::detail::encode_little_endian(0x1234'8000,
			std::span{cpu.vectors[10].bytes}.subspan(lane * 4, 4));
		arm64::detail::encode_little_endian(0,
			std::span{cpu.vectors[11].bytes}.subspan(lane * 4, 4));
	}
	for (std::size_t lane = 0; lane < 2; ++lane) {
		arm64::detail::encode_little_endian(0x1234'5678'0000'0000ULL,
			std::span{cpu.vectors[13].bytes}.subspan(lane * 8, 8));
		arm64::detail::encode_little_endian(0,
			std::span{cpu.vectors[14].bytes}.subspan(lane * 8, 8));
	}
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t index = 0; index < 8; ++index) {
		require(cpu.vectors[2].bytes[index] == std::byte{0x46});
		require(cpu.vectors[9].bytes[index] == std::byte{0xaa});
	}
	for (std::size_t lane = 0; lane < 4; ++lane)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[9].bytes}.subspan(8 + lane * 2, 2)) ==
			0x1235);
	for (std::size_t lane = 0; lane < 2; ++lane)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[12].bytes}.subspan(lane * 4, 4)) ==
			0x1234'5678);
}

static void simd_narrowing_moves()
{
	constexpr std::uint64_t base = 0x15'fc0;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x0e21'2822U);      // xtn v2.8b, v1.8h
	memory.put32(base + 4, 0x4e21'2823U);  // xtn2 v3.16b, v1.8h
	memory.put32(base + 8, 0x0ea1'2bffU);  // xtn v31.2s, v31.2d
	memory.put32(base + 12, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	for (std::size_t lane = 0; lane < 8; ++lane)
		arm64::detail::encode_little_endian(0x1200U + lane,
			std::span{cpu.vectors[1].bytes}.subspan(lane * 2, 2));
	cpu.vectors[3].bytes.fill(std::byte{0xaa});
	arm64::detail::set_vector_half(
		cpu.vectors[31], 0, 0x1122'3344'5566'7788ULL);
	arm64::detail::set_vector_half(
		cpu.vectors[31], 1, 0x99aa'bbcc'ddee'ff00ULL);
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t lane = 0; lane < 8; ++lane) {
		require(cpu.vectors[2].bytes[lane] == static_cast<std::byte>(lane));
		require(cpu.vectors[2].bytes[lane + 8] == std::byte{});
		require(cpu.vectors[3].bytes[lane] == std::byte{0xaa});
		require(cpu.vectors[3].bytes[lane + 8] ==
			static_cast<std::byte>(lane));
	}
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[31].bytes}.subspan(0, 4)) == 0x5566'7788U);
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[31].bytes}.subspan(4, 4)) == 0xddee'ff00U);
	for (std::size_t index = 8; index < 16; ++index)
		require(cpu.vectors[31].bytes[index] == std::byte{});
}

static void simd_widening_arithmetic()
{
	constexpr std::uint64_t base = 0x15'fe0;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x6e7e'13ffU);      // uaddw2 v31.4s, v31.4s, v30.8h
	memory.put32(base + 4, 0x0e24'0062U);  // saddl v2.8h, v3.8b, v4.8b
	memory.put32(base + 8, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	for (std::size_t lane = 0; lane < 4; ++lane) {
		arm64::detail::encode_little_endian((lane + 1) * 100,
			std::span{cpu.vectors[31].bytes}.subspan(lane * 4, 4));
		arm64::detail::encode_little_endian(lane + 1,
			std::span{cpu.vectors[30].bytes}.subspan(8 + lane * 2, 2));
	}
	for (std::size_t lane = 0; lane < 8; ++lane) {
		cpu.vectors[3].bytes[lane] = std::byte{0xfe};
		cpu.vectors[4].bytes[lane] = std::byte{3};
	}
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 6);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t lane = 0; lane < 4; ++lane)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[31].bytes}.subspan(lane * 4, 4)) ==
			(lane + 1) * 101);
	for (std::size_t lane = 0; lane < 8; ++lane)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[2].bytes}.subspan(lane * 2, 2)) == 1);
}

static void simd_widening_multiply()
{
	constexpr std::uint64_t base = 0x15'f00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x0e22'c020U);      // smull v0.8h, v1.8b, v2.8b
	memory.put32(base + 4, 0x6e65'c083U);  // umull2 v3.4s, v4.8h, v5.8h
	memory.put32(base + 8, 0x0ea8'80e6U);  // smlal v6.2d, v7.2s, v8.2s
	memory.put32(base + 12, 0x6e6b'8149U); // umlal2 v9.4s, v10.8h, v11.8h
	memory.put32(base + 16, 0x0e2e'a1acU); // smlsl v12.8h, v13.8b, v14.8b
	memory.put32(base + 20, 0x6eb1'a20fU); // umlsl2 v15.2d, v16.4s, v17.4s
	memory.put32(base + 24, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.vectors[1].bytes[0] = std::byte{0xfe};
	cpu.vectors[1].bytes[1] = std::byte{3};
	cpu.vectors[2].bytes[0] = std::byte{4};
	cpu.vectors[2].bytes[1] = std::byte{0xfb};
	for (std::size_t lane = 0; lane < 4; ++lane) {
		arm64::detail::encode_little_endian(0x100 + lane,
			std::span{cpu.vectors[4].bytes}.subspan(8 + lane * 2, 2));
		arm64::detail::encode_little_endian(2 + lane,
			std::span{cpu.vectors[5].bytes}.subspan(8 + lane * 2, 2));
		arm64::detail::encode_little_endian(10 + lane,
			std::span{cpu.vectors[9].bytes}.subspan(lane * 4, 4));
		arm64::detail::encode_little_endian(3 + lane,
			std::span{cpu.vectors[10].bytes}.subspan(8 + lane * 2, 2));
		arm64::detail::encode_little_endian(4 + lane,
			std::span{cpu.vectors[11].bytes}.subspan(8 + lane * 2, 2));
	}
	arm64::detail::set_vector_half(cpu.vectors[6], 0, 100);
	arm64::detail::set_vector_half(cpu.vectors[6], 1, 200);
	arm64::detail::encode_little_endian(0xffff'fffdU,
		std::span{cpu.vectors[7].bytes}.subspan(0, 4));
	arm64::detail::encode_little_endian(4,
		std::span{cpu.vectors[7].bytes}.subspan(4, 4));
	arm64::detail::encode_little_endian(5,
		std::span{cpu.vectors[8].bytes}.subspan(0, 4));
	arm64::detail::encode_little_endian(0xffff'fffaU,
		std::span{cpu.vectors[8].bytes}.subspan(4, 4));
	for (std::size_t lane = 0; lane < 8; ++lane) {
		arm64::detail::encode_little_endian(100,
			std::span{cpu.vectors[12].bytes}.subspan(lane * 2, 2));
		cpu.vectors[13].bytes[lane] = static_cast<std::byte>(lane + 1);
		cpu.vectors[14].bytes[lane] = std::byte{2};
	}
	arm64::detail::set_vector_half(cpu.vectors[15], 0, 1000);
	arm64::detail::set_vector_half(cpu.vectors[15], 1, 2000);
	arm64::detail::encode_little_endian(7,
		std::span{cpu.vectors[16].bytes}.subspan(8, 4));
	arm64::detail::encode_little_endian(8,
		std::span{cpu.vectors[16].bytes}.subspan(12, 4));
	arm64::detail::encode_little_endian(9,
		std::span{cpu.vectors[17].bytes}.subspan(8, 4));
	arm64::detail::encode_little_endian(10,
		std::span{cpu.vectors[17].bytes}.subspan(12, 4));
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 10);
	require(result.reason == arm64::StopReason::system_call);
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[0].bytes}.subspan(0, 2)) == 0xfff8);
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[0].bytes}.subspan(2, 2)) == 0xfff1);
	for (std::size_t lane = 0; lane < 4; ++lane)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[3].bytes}.subspan(lane * 4, 4)) ==
			(0x100 + lane) * (2 + lane));
	require(arm64::detail::vector_half(cpu.vectors[6], 0) == 85);
	require(arm64::detail::vector_half(cpu.vectors[6], 1) == 176);
	for (std::size_t lane = 0; lane < 4; ++lane)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[9].bytes}.subspan(lane * 4, 4)) ==
			10 + lane + (3 + lane) * (4 + lane));
	for (std::size_t lane = 0; lane < 8; ++lane)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[12].bytes}.subspan(lane * 2, 2)) ==
			100 - (lane + 1) * 2);
	require(arm64::detail::vector_half(cpu.vectors[15], 0) == 937);
	require(arm64::detail::vector_half(cpu.vectors[15], 1) == 1920);
}

static void simd_integer_float_conversions()
{
	constexpr std::uint64_t base = 0x15'f80;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4e61'dbffU);      // scvtf v31.2d, v31.2d
	memory.put32(base + 4, 0x6e21'd862U);  // ucvtf v2.4s, v3.4s
	memory.put32(base + 8, 0x4ea1'b8a4U);  // fcvtzs v4.4s, v5.4s
	memory.put32(base + 12, 0x7e61'd8e6U); // ucvtf d6, d7
	memory.put32(base + 16, 0x5e21'd928U); // scvtf s8, s9
	memory.put32(base + 20, 0x7ee1'b96aU); // fcvtzu d10, d11
	memory.put32(base + 24, 0x5ea1'b9acU); // fcvtzs s12, s13
	memory.put32(base + 28, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	arm64::detail::set_vector_half(cpu.vectors[31], 0,
		static_cast<std::uint64_t>(-2));
	arm64::detail::set_vector_half(cpu.vectors[31], 1, 3);
	for (std::size_t lane = 0; lane < 4; ++lane) {
		arm64::detail::encode_little_endian(lane + 1,
			std::span{cpu.vectors[3].bytes}.subspan(lane * 4, 4));
		arm64::detail::encode_little_endian(std::bit_cast<std::uint32_t>(
			static_cast<float>(lane) - 2.75F),
			std::span{cpu.vectors[5].bytes}.subspan(lane * 4, 4));
	}
	arm64::detail::set_vector_half(cpu.vectors[7], 0, 42);
	arm64::detail::encode_little_endian(
		static_cast<std::uint32_t>(static_cast<std::int32_t>(-7)),
		std::span{cpu.vectors[9].bytes}.first(4));
	arm64::detail::set_vector_half(cpu.vectors[11], 0,
		std::bit_cast<std::uint64_t>(19.75));
	arm64::detail::encode_little_endian(std::bit_cast<std::uint32_t>(-9.75F),
		std::span{cpu.vectors[13].bytes}.first(4));
	for (const auto destination : {6U, 8U, 10U, 12U})
		cpu.vectors[destination].bytes.fill(std::byte{0xaa});
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 12);
	require(result.reason == arm64::StopReason::system_call);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[31], 0)) == -2.0);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[31], 1)) == 3.0);
	for (std::size_t lane = 0; lane < 4; ++lane) {
		require(std::bit_cast<float>(static_cast<std::uint32_t>(
			arm64::detail::decode_little_endian(
				std::span{cpu.vectors[2].bytes}.subspan(lane * 4, 4)))) ==
			static_cast<float>(lane + 1));
		const auto expected = static_cast<std::int32_t>(
			std::trunc(static_cast<float>(lane) - 2.75F));
		require(static_cast<std::int32_t>(
			arm64::detail::decode_little_endian(
				std::span{cpu.vectors[4].bytes}.subspan(lane * 4, 4))) ==
				expected);
	}
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[6], 0)) == 42.0);
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::decode_little_endian(
			std::span{cpu.vectors[8].bytes}.first(4)))) == -7.0F);
	require(arm64::detail::vector_half(cpu.vectors[10], 0) == 19);
	require(static_cast<std::int32_t>(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[12].bytes}.first(4))) == -9);
	for (std::size_t index = 8; index < 16; ++index) {
		require(cpu.vectors[6].bytes[index] == std::byte{});
		require(cpu.vectors[10].bytes[index] == std::byte{});
	}
	for (std::size_t index = 4; index < 16; ++index) {
		require(cpu.vectors[8].bytes[index] == std::byte{});
		require(cpu.vectors[12].bytes[index] == std::byte{});
	}
}

static void simd_floating_arithmetic()
{
	constexpr std::uint64_t base = 0x15'f40;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4e7e'd7ffU);      // fadd v31.2d, v31.2d, v30.2d
	memory.put32(base + 4, 0x4ea2'd420U);  // fsub v0.4s, v1.4s, v2.4s
	memory.put32(base + 8, 0x6e25'dc83U);  // fmul v3.4s, v4.4s, v5.4s
	memory.put32(base + 12, 0x7ea8'd4e6U); // fabd s6, s7, s8
	memory.put32(base + 16, 0x7eeb'd549U); // fabd d9, d10, d11
	memory.put32(base + 20, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	for (std::size_t lane = 0; lane < 2; ++lane) {
		arm64::detail::encode_little_endian(std::bit_cast<std::uint64_t>(
			static_cast<double>(lane + 1)),
			std::span{cpu.vectors[31].bytes}.subspan(lane * 8, 8));
		arm64::detail::encode_little_endian(std::bit_cast<std::uint64_t>(
			static_cast<double>((lane + 1) * 3)),
			std::span{cpu.vectors[30].bytes}.subspan(lane * 8, 8));
	}
	for (std::size_t lane = 0; lane < 4; ++lane) {
		for (const auto [reg, value] : std::array{
			std::pair{1U, static_cast<float>(lane + 10)},
			std::pair{2U, static_cast<float>(lane)},
			std::pair{4U, static_cast<float>(lane + 1)},
			std::pair{5U, 2.0F}})
			arm64::detail::encode_little_endian(
				std::bit_cast<std::uint32_t>(value),
				std::span{cpu.vectors[reg].bytes}.subspan(lane * 4, 4));
	}
	arm64::detail::set_vector_half(cpu.vectors[7], 0,
		std::bit_cast<std::uint32_t>(-1.5F));
	arm64::detail::set_vector_half(cpu.vectors[8], 0,
		std::bit_cast<std::uint32_t>(2.25F));
	arm64::detail::set_vector_half(cpu.vectors[10], 0,
		std::bit_cast<std::uint64_t>(10.0));
	arm64::detail::set_vector_half(cpu.vectors[11], 0,
		std::bit_cast<std::uint64_t>(-4.0));
	cpu.vectors[6].bytes.fill(std::byte{0xaa});
	cpu.vectors[9].bytes.fill(std::byte{0xaa});
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t lane = 0; lane < 2; ++lane)
		require(std::bit_cast<double>(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[31].bytes}.subspan(lane * 8, 8))) ==
			static_cast<double>((lane + 1) * 4));
	for (std::size_t lane = 0; lane < 4; ++lane) {
		require(std::bit_cast<float>(static_cast<std::uint32_t>(
			arm64::detail::decode_little_endian(
				std::span{cpu.vectors[0].bytes}.subspan(lane * 4, 4)))) ==
			10.0F);
		require(std::bit_cast<float>(static_cast<std::uint32_t>(
			arm64::detail::decode_little_endian(
				std::span{cpu.vectors[3].bytes}.subspan(lane * 4, 4)))) ==
			static_cast<float>((lane + 1) * 2));
	}
	require(std::bit_cast<float>(static_cast<std::uint32_t>(
		arm64::detail::vector_half(cpu.vectors[6], 0))) == 3.75F);
	require(std::bit_cast<double>(
		arm64::detail::vector_half(cpu.vectors[9], 0)) == 14.0);
	for (std::size_t index = 4; index < 16; ++index)
		require(cpu.vectors[6].bytes[index] == std::byte{});
	for (std::size_t index = 8; index < 16; ++index)
		require(cpu.vectors[9].bytes[index] == std::byte{});
}

static void simd_floating_unary()
{
	constexpr std::uint64_t base = 0x15'fc0;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4ea0'f820U);      // fabs v0.4s, v1.4s
	memory.put32(base + 4, 0x4ee0'f862U);  // fabs v2.2d, v3.2d
	memory.put32(base + 8, 0x6ea0'f8a4U);  // fneg v4.4s, v5.4s
	memory.put32(base + 12, 0x6ee0'f8e6U); // fneg v6.2d, v7.2d
	memory.put32(base + 16, 0x6ea1'f928U); // fsqrt v8.4s, v9.4s
	memory.put32(base + 20, 0x6ee1'f96aU); // fsqrt v10.2d, v11.2d
	memory.put32(base + 24, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	for (std::size_t lane = 0; lane < 4; ++lane) {
		for (const auto [reg, value] : std::array{
			std::pair{1U, -static_cast<float>(lane + 1)},
			std::pair{5U, static_cast<float>(lane + 1)},
			std::pair{9U, static_cast<float>((lane + 1) * (lane + 1))}})
			arm64::detail::encode_little_endian(
				std::bit_cast<std::uint32_t>(value),
				std::span{cpu.vectors[reg].bytes}.subspan(lane * 4, 4));
	}
	for (std::size_t lane = 0; lane < 2; ++lane) {
		for (const auto [reg, value] : std::array{
			std::pair{3U, -static_cast<double>(lane + 1)},
			std::pair{7U, static_cast<double>(lane + 1)},
			std::pair{11U,
				static_cast<double>((lane + 4) * (lane + 4))}})
			arm64::detail::encode_little_endian(
				std::bit_cast<std::uint64_t>(value),
				std::span{cpu.vectors[reg].bytes}.subspan(lane * 8, 8));
	}
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t lane = 0; lane < 4; ++lane) {
		for (const auto [reg, value] : std::array{
			std::pair{0U, static_cast<float>(lane + 1)},
			std::pair{4U, -static_cast<float>(lane + 1)},
			std::pair{8U, static_cast<float>(lane + 1)}})
			require(std::bit_cast<float>(static_cast<std::uint32_t>(
				arm64::detail::decode_little_endian(
					std::span{cpu.vectors[reg].bytes}.subspan(
						lane * 4, 4)))) == value);
	}
	for (std::size_t lane = 0; lane < 2; ++lane) {
		for (const auto [reg, value] : std::array{
			std::pair{2U, static_cast<double>(lane + 1)},
			std::pair{6U, -static_cast<double>(lane + 1)},
			std::pair{10U, static_cast<double>(lane + 4)}})
			require(std::bit_cast<double>(
				arm64::detail::decode_little_endian(
					std::span{cpu.vectors[reg].bytes}.subspan(
						lane * 8, 8))) == value);
	}
}

static void simd_byte_table_lookup()
{
	constexpr std::uint64_t base = 0x15'ff0;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4e01'2080U);      // tbl v0.16b, {v4.16b,v5.16b}, v1.16b
	memory.put32(base + 4, 0x4e15'3272U);  // tbx v18.16b, {v19.16b,v20.16b}, v21.16b
	memory.put32(base + 8, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	for (std::size_t index = 0; index < 16; ++index) {
		cpu.vectors[4].bytes[index] = static_cast<std::byte>(index);
		cpu.vectors[5].bytes[index] = static_cast<std::byte>(16 + index);
		cpu.vectors[1].bytes[index] = static_cast<std::byte>(31 - index);
		cpu.vectors[19].bytes[index] = static_cast<std::byte>(0x40 + index);
		cpu.vectors[20].bytes[index] = static_cast<std::byte>(0x50 + index);
		cpu.vectors[21].bytes[index] = static_cast<std::byte>(
			index == 15 ? 32 : index);
	}
	cpu.vectors[18].bytes.fill(std::byte{0xee});
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 6);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t index = 0; index < 16; ++index) {
		require(cpu.vectors[0].bytes[index] ==
			static_cast<std::byte>(31 - index));
		require(cpu.vectors[18].bytes[index] ==
			(index == 15 ? std::byte{0xee} :
				static_cast<std::byte>(0x40 + index)));
	}
}

static void simd_lane_permutations()
{
	constexpr std::uint64_t base = 0x15'800;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4e02'1823U);      // uzp1 v3.16b, v1.16b, v2.16b
	memory.put32(base + 4, 0x4e02'5824U);  // uzp2 v4.16b, v1.16b, v2.16b
	memory.put32(base + 8, 0x4e02'2825U);  // trn1 v5.16b, v1.16b, v2.16b
	memory.put32(base + 12, 0x4e02'6826U); // trn2 v6.16b, v1.16b, v2.16b
	memory.put32(base + 16, 0x4e02'3827U); // zip1 v7.16b, v1.16b, v2.16b
	memory.put32(base + 20, 0x4e02'7828U); // zip2 v8.16b, v1.16b, v2.16b
	memory.put32(base + 24, 0x0e02'3829U); // zip1 v9.8b, v1.8b, v2.8b
	memory.put32(base + 28, 0x4e5a'1b3aU); // uzp1 v26.8h, v25.8h, v26.8h
	memory.put32(base + 32, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	for (std::size_t index = 0; index < 16; ++index) {
		cpu.vectors[1].bytes[index] = static_cast<std::byte>(index);
		cpu.vectors[2].bytes[index] = static_cast<std::byte>(0x20 + index);
	}
	for (std::size_t lane = 0; lane < 8; ++lane) {
		arm64::detail::encode_little_endian(0x100U + lane,
			std::span{cpu.vectors[25].bytes}.subspan(lane * 2, 2));
		arm64::detail::encode_little_endian(0x200U + lane,
			std::span{cpu.vectors[26].bytes}.subspan(lane * 2, 2));
	}
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 12);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t index = 0; index < 8; ++index) {
		require(cpu.vectors[3].bytes[index] ==
			static_cast<std::byte>(index * 2));
		require(cpu.vectors[3].bytes[index + 8] ==
			static_cast<std::byte>(0x20 + index * 2));
		require(cpu.vectors[4].bytes[index] ==
			static_cast<std::byte>(index * 2 + 1));
		require(cpu.vectors[4].bytes[index + 8] ==
			static_cast<std::byte>(0x21 + index * 2));
		require(cpu.vectors[5].bytes[index * 2] ==
			static_cast<std::byte>(index * 2));
		require(cpu.vectors[5].bytes[index * 2 + 1] ==
			static_cast<std::byte>(0x20 + index * 2));
		require(cpu.vectors[6].bytes[index * 2] ==
			static_cast<std::byte>(index * 2 + 1));
		require(cpu.vectors[6].bytes[index * 2 + 1] ==
			static_cast<std::byte>(0x21 + index * 2));
	}
	for (std::size_t index = 0; index < 8; ++index) {
		require(cpu.vectors[7].bytes[index * 2] ==
			static_cast<std::byte>(index));
		require(cpu.vectors[7].bytes[index * 2 + 1] ==
			static_cast<std::byte>(0x20 + index));
		require(cpu.vectors[8].bytes[index * 2] ==
			static_cast<std::byte>(index + 8));
		require(cpu.vectors[8].bytes[index * 2 + 1] ==
			static_cast<std::byte>(0x28 + index));
	}
	for (std::size_t index = 0; index < 4; ++index) {
		require(cpu.vectors[9].bytes[index * 2] ==
			static_cast<std::byte>(index));
		require(cpu.vectors[9].bytes[index * 2 + 1] ==
			static_cast<std::byte>(0x20 + index));
	}
	for (std::size_t index = 8; index < 16; ++index)
		require(cpu.vectors[9].bytes[index] == std::byte{});
	for (std::size_t lane = 0; lane < 4; ++lane) {
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[26].bytes}.subspan(lane * 2, 2)) ==
			0x100U + lane * 2);
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[26].bytes}.subspan((lane + 4) * 2, 2)) ==
			0x200U + lane * 2);
	}
}

static void simd_narrowing_shift_right()
{
	constexpr std::uint64_t base = 0x16'000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x0f0c'8422U);      // shrn v2.8b, v1.8h, #4
	memory.put32(base + 4, 0x4f0c'8423U);  // shrn2 v3.16b, v1.8h, #4
	memory.put32(base + 8, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	for (std::size_t lane = 0; lane < 8; ++lane) {
		cpu.vectors[1].bytes[lane * 2] = std::byte{0xf0};
		cpu.vectors[1].bytes[lane * 2 + 1] = std::byte{0x0f};
		cpu.vectors[3].bytes[lane] = std::byte{0xaa};
	}
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	for (std::size_t lane = 0; lane < 8; ++lane) {
		require(cpu.vectors[2].bytes[lane] == std::byte{0xff});
		require(cpu.vectors[2].bytes[lane + 8] == std::byte{});
		require(cpu.vectors[3].bytes[lane] == std::byte{0xaa});
		require(cpu.vectors[3].bytes[lane + 8] == std::byte{0xff});
	}
}

static void simd_immediate_shifts()
{
	constexpr std::uint64_t base = 0x16'800;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x6f0d'0401U);      // ushr v1.16b, v0.16b, #3
	memory.put32(base + 4, 0x4f1b'0462U);  // sshr v2.8h, v3.8h, #5
	memory.put32(base + 8, 0x4f27'54a4U);  // shl v4.4s, v5.4s, #7
	memory.put32(base + 12, 0x6f77'44e6U); // sri v6.2d, v7.2d, #9
	memory.put32(base + 16, 0x6f0a'5528U); // sli v8.16b, v9.16b, #2
	memory.put32(base + 20, 0x7f60'054bU); // ushr d11, d10, #32
	memory.put32(base + 24, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.vectors[0].bytes.fill(std::byte{0xf8});
	for (std::size_t lane = 0; lane < 8; ++lane)
		arm64::detail::encode_little_endian(0xfff0,
			std::span{cpu.vectors[3].bytes}.subspan(lane * 2, 2));
	for (std::size_t lane = 0; lane < 4; ++lane)
		arm64::detail::encode_little_endian(3,
			std::span{cpu.vectors[5].bytes}.subspan(lane * 4, 4));
	cpu.vectors[6].bytes.fill(std::byte{0xaa});
	cpu.vectors[7].bytes.fill(std::byte{0x55});
	cpu.vectors[8].bytes.fill(std::byte{0x03});
	cpu.vectors[9].bytes.fill(std::byte{0x11});
	arm64::detail::encode_little_endian(0x1122'3344'5566'7788ULL,
		std::span{cpu.vectors[10].bytes}.first(8));
	cpu.vectors[11].bytes.fill(std::byte{0xaa});
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 12);
	require(result.reason == arm64::StopReason::system_call);
	for (const auto byte : cpu.vectors[1].bytes)
		require(byte == std::byte{0x1f});
	for (std::size_t lane = 0; lane < 8; ++lane)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[2].bytes}.subspan(lane * 2, 2)) ==
			0xffff);
	for (std::size_t lane = 0; lane < 4; ++lane)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[4].bytes}.subspan(lane * 4, 4)) ==
			384);
	for (const auto byte : cpu.vectors[8].bytes)
		require(byte == std::byte{0x47});
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[11].bytes}.first(8)) == 0x1122'3344ULL);
	for (std::size_t index = 8; index < cpu.vectors[11].bytes.size(); ++index)
		require(cpu.vectors[11].bytes[index] == std::byte{});
}

static void simd_variable_shifts()
{
	constexpr std::uint64_t base = 0x16'c00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4e22'4420U);      // sshl v0.16b, v1.16b, v2.16b
	memory.put32(base + 4, 0x6e65'4483U);  // ushl v3.8h, v4.8h, v5.8h
	memory.put32(base + 8, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.vectors[1].bytes.fill(std::byte{3});
	cpu.vectors[2].bytes.fill(std::byte{1});
	cpu.vectors[1].bytes[1] = std::byte{0x80};
	cpu.vectors[2].bytes[1] = std::byte{0xff};
	for (std::size_t lane = 0; lane < 8; ++lane) {
		arm64::detail::encode_little_endian(0x8000,
			std::span{cpu.vectors[4].bytes}.subspan(lane * 2, 2));
		arm64::detail::encode_little_endian(0xffff,
			std::span{cpu.vectors[5].bytes}.subspan(lane * 2, 2));
	}
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 6);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.vectors[0].bytes[0] == std::byte{6});
	require(cpu.vectors[0].bytes[1] == std::byte{0xc0});
	for (std::size_t index = 2; index < 16; ++index)
		require(cpu.vectors[0].bytes[index] == std::byte{6});
	for (std::size_t lane = 0; lane < 8; ++lane)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[3].bytes}.subspan(lane * 2, 2)) ==
			0x4000);
}

static void simd_scalar_immediate_shifts()
{
	constexpr std::uint64_t base = 0x16'a00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x5f7d'0420U);      // sshr d0, d1, #3
	memory.put32(base + 4, 0x7f7c'0462U);  // ushr d2, d3, #4
	memory.put32(base + 8, 0x5f7b'24a4U);  // srshr d4, d5, #5
	memory.put32(base + 12, 0x7f7a'24e6U); // urshr d6, d7, #6
	memory.put32(base + 16, 0x7f79'4528U); // sri d8, d9, #7
	memory.put32(base + 20, 0x5f48'556aU); // shl d10, d11, #8
	memory.put32(base + 24, 0x7f49'55acU); // sli d12, d13, #9
	memory.put32(base + 28, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	arm64::detail::set_vector_half(cpu.vectors[1], 0,
		static_cast<std::uint64_t>(static_cast<std::int64_t>(-16)));
	arm64::detail::set_vector_half(cpu.vectors[3], 0,
		0xf000'0000'0000'0000ULL);
	arm64::detail::set_vector_half(cpu.vectors[5], 0, 47);
	arm64::detail::set_vector_half(cpu.vectors[7], 0, 95);
	arm64::detail::set_vector_half(cpu.vectors[8], 0,
		0xffff'ffff'ffff'ffffULL);
	arm64::detail::set_vector_half(cpu.vectors[9], 0, 0);
	arm64::detail::set_vector_half(cpu.vectors[11], 0, 1);
	arm64::detail::set_vector_half(cpu.vectors[12], 0, 0x1ff);
	arm64::detail::set_vector_half(cpu.vectors[13], 0, 1);
	for (const auto destination : {0U, 2U, 4U, 6U, 8U, 10U, 12U})
		arm64::detail::set_vector_half(cpu.vectors[destination], 1,
			0xaaaa'aaaa'aaaa'aaaaULL);
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 12);
	require(result.reason == arm64::StopReason::system_call);
	require(arm64::detail::vector_half(cpu.vectors[0], 0) ==
		0xffff'ffff'ffff'fffeULL);
	require(arm64::detail::vector_half(cpu.vectors[2], 0) ==
		0x0f00'0000'0000'0000ULL);
	require(arm64::detail::vector_half(cpu.vectors[4], 0) == 1);
	require(arm64::detail::vector_half(cpu.vectors[6], 0) == 1);
	require(arm64::detail::vector_half(cpu.vectors[8], 0) ==
		0xfe00'0000'0000'0000ULL);
	require(arm64::detail::vector_half(cpu.vectors[10], 0) == 256);
	require(arm64::detail::vector_half(cpu.vectors[12], 0) == 0x3ff);
	for (const auto destination : {0U, 2U, 4U, 6U, 8U, 10U, 12U})
		require(arm64::detail::vector_half(
			cpu.vectors[destination], 1) == 0);
}

static void simd_shift_left_long()
{
	constexpr std::uint64_t base = 0x16'e00;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x0f20'a7ffU);      // sshll v31.2d, v31.2s, #0
	memory.put32(base + 4, 0x6f0b'a462U);  // ushll2 v2.8h, v3.16b, #3
	memory.put32(base + 8, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	arm64::detail::encode_little_endian(0xffff'fffeU,
		std::span{cpu.vectors[31].bytes}.subspan(0, 4));
	arm64::detail::encode_little_endian(3U,
		std::span{cpu.vectors[31].bytes}.subspan(4, 4));
	for (std::size_t index = 0; index < 16; ++index)
		cpu.vectors[3].bytes[index] = static_cast<std::byte>(index);
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 6);
	require(result.reason == arm64::StopReason::system_call);
	require(arm64::detail::vector_half(cpu.vectors[31], 0) ==
		std::uint64_t{0xffff'ffff'ffff'fffeULL});
	require(arm64::detail::vector_half(cpu.vectors[31], 1) == 3);
	for (std::size_t lane = 0; lane < 8; ++lane)
		require(arm64::detail::decode_little_endian(
			std::span{cpu.vectors[2].bytes}.subspan(lane * 2, 2)) ==
			(lane + 8) * 8);
}

static void simd_general_register_moves()
{
	constexpr std::uint64_t base = 0x17'000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x9e66'0042U);      // fmov x2, d2
	memory.put32(base + 4, 0x9e67'0083U);  // fmov d3, x4
	memory.put32(base + 8, 0x1e26'00c5U);  // fmov w5, s6
	memory.put32(base + 12, 0x1e27'0107U); // fmov s7, w8
	memory.put32(base + 16, 0x9eae'0149U); // fmov x9, v10.d[1]
	memory.put32(base + 20, 0x9eaf'018bU); // fmov v11.d[1], x12
	memory.put32(base + 24, 0x4e18'1cffU); // mov v31.d[1], x7
	memory.put32(base + 28, 0x4e08'3cc6U); // mov x6, v6.d[0]
	memory.put32(base + 32, 0x0e1f'3daaU); // umov w10, v13.b[15]
	memory.put32(base + 36, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	arm64::detail::set_vector_half(
		cpu.vectors[2], 0, 0x1122'3344'5566'7788ULL);
	arm64::detail::set_vector_half(
		cpu.vectors[6], 0, 0xffff'ffff'aabb'ccddULL);
	arm64::detail::set_vector_half(
		cpu.vectors[10], 1, 0x8877'6655'4433'2211ULL);
	arm64::detail::set_vector_half(
		cpu.vectors[11], 0, 0xfeed'face'cafe'beefULL);
	cpu.registers[4] = 0x0123'4567'89ab'cdefULL;
	cpu.registers[8] = 0xffff'ffff'7654'3210ULL;
	cpu.registers[12] = 0x1020'3040'5060'7080ULL;
	cpu.registers[7] = 0x1357'9bdf'2468'ace0ULL;
	cpu.vectors[13].bytes[15] = std::byte{0x5a};
	arm64::detail::set_vector_half(
		cpu.vectors[31], 0, 0xfeed'face'cafe'beefULL);
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 12);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[2] == 0x1122'3344'5566'7788ULL);
	require(arm64::detail::vector_half(cpu.vectors[3], 0) ==
		0x0123'4567'89ab'cdefULL);
	require(arm64::detail::vector_half(cpu.vectors[3], 1) == 0);
	require(cpu.registers[5] == 0xaabb'ccddULL);
	require(arm64::detail::vector_half(cpu.vectors[7], 0) == 0x7654'3210ULL);
	require(arm64::detail::vector_half(cpu.vectors[7], 1) == 0);
	require(cpu.registers[9] == 0x8877'6655'4433'2211ULL);
	require(arm64::detail::vector_half(cpu.vectors[11], 0) ==
		0xfeed'face'cafe'beefULL);
	require(cpu.registers[6] == 0xffff'ffff'aabb'ccddULL);
	require(cpu.registers[10] == 0x5a);
	require(arm64::detail::vector_half(cpu.vectors[11], 1) ==
		0x1020'3040'5060'7080ULL);
	require(arm64::detail::vector_half(cpu.vectors[31], 0) ==
		0xfeed'face'cafe'beefULL);
	require(arm64::detail::vector_half(cpu.vectors[31], 1) ==
		0x1357'9bdf'2468'ace0ULL);
}

static void simd_duplicate_general_register()
{
	constexpr std::uint64_t base = 0x18'000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4e01'0c20U);      // dup v0.16b, w1
	memory.put32(base + 4, 0x0e02'0c42U);  // dup v2.4h, w2
	memory.put32(base + 8, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[1] = 0xab;
	cpu.registers[2] = 0x1234;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	for (const auto byte : cpu.vectors[0].bytes)
		require(byte == std::byte{0xab});
	for (std::size_t index = 0; index < 8; index += 2) {
		require(cpu.vectors[2].bytes[index] == std::byte{0x34});
		require(cpu.vectors[2].bytes[index + 1] == std::byte{0x12});
	}
	for (std::size_t index = 8; index < 16; ++index)
		require(cpu.vectors[2].bytes[index] == std::byte{});
}

static void simd_insert_vector_element()
{
	constexpr std::uint64_t base = 0x17'800;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x6e03'079fU);      // mov v31.b[1], v28.b[0]
	memory.put32(base + 4, 0x6e0e'7462U);  // mov v2.h[3], v3.h[7]
	memory.put32(base + 8, 0x6e14'64a4U);  // mov v4.s[2], v5.s[3]
	memory.put32(base + 12, 0x6e18'04e6U); // mov v6.d[1], v7.d[0]
	memory.put32(base + 16, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.vectors[31].bytes.fill(std::byte{0xcc});
	cpu.vectors[28].bytes[0] = std::byte{0xa5};
	cpu.vectors[2].bytes.fill(std::byte{0xaa});
	arm64::detail::encode_little_endian(0x1234,
		std::span{cpu.vectors[3].bytes}.subspan(14, 2));
	cpu.vectors[4].bytes.fill(std::byte{0xbb});
	arm64::detail::encode_little_endian(0x89ab'cdef,
		std::span{cpu.vectors[5].bytes}.subspan(12, 4));
	arm64::detail::set_vector_half(
		cpu.vectors[6], 0, 0xfeed'face'cafe'beefULL);
	arm64::detail::set_vector_half(
		cpu.vectors[6], 1, 0x1111'2222'3333'4444ULL);
	arm64::detail::set_vector_half(
		cpu.vectors[7], 0, 0x0123'4567'89ab'cdefULL);
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	if (result.reason != arm64::StopReason::system_call)
		std::fprintf(stderr, "insert-element stop reason=%u pc=%#llx insn=%#x\n",
			static_cast<unsigned>(result.reason),
			static_cast<unsigned long long>(result.address),
			result.instruction);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.vectors[31].bytes[0] == std::byte{0xcc});
	require(cpu.vectors[31].bytes[1] == std::byte{0xa5});
	for (std::size_t index = 2; index < 16; ++index)
		require(cpu.vectors[31].bytes[index] == std::byte{0xcc});
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[2].bytes}.subspan(6, 2)) == 0x1234);
	require(cpu.vectors[2].bytes[5] == std::byte{0xaa});
	require(cpu.vectors[2].bytes[8] == std::byte{0xaa});
	require(arm64::detail::decode_little_endian(
		std::span{cpu.vectors[4].bytes}.subspan(8, 4)) == 0x89ab'cdef);
	require(cpu.vectors[4].bytes[7] == std::byte{0xbb});
	require(cpu.vectors[4].bytes[12] == std::byte{0xbb});
	require(arm64::detail::vector_half(cpu.vectors[6], 0) ==
		0xfeed'face'cafe'beefULL);
	require(arm64::detail::vector_half(cpu.vectors[6], 1) ==
		0x0123'4567'89ab'cdefULL);
}

static void simd_duplicate_vector_element()
{
	constexpr std::uint64_t base = 0x18'800;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x4e0b'0420U);     // dup v0.16b, v1.b[5]
	memory.put32(base + 4, 0x0e02'07ffU); // dup v31.4h, v31.h[0]
	memory.put32(base + 8, 0x5e1c'0462U); // mov s2, v3.s[3]
	memory.put32(base + 12, 0x5e18'04a4U); // mov d4, v5.d[1]
	memory.put32(base + 16, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.vectors[1].bytes[5] = std::byte{0xab};
	cpu.vectors[31].bytes[0] = std::byte{0x34};
	cpu.vectors[31].bytes[1] = std::byte{0x12};
	arm64::detail::encode_little_endian(0x89ab'cdefU,
		std::span{cpu.vectors[3].bytes}.subspan(12, 4));
	arm64::detail::set_vector_half(cpu.vectors[5], 1,
		0x0123'4567'89ab'cdefULL);
	cpu.vectors[2].bytes.fill(std::byte{0xff});
	cpu.vectors[4].bytes.fill(std::byte{0xff});
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 8);
	require(result.reason == arm64::StopReason::system_call);
	for (const auto byte : cpu.vectors[0].bytes)
		require(byte == std::byte{0xab});
	for (std::size_t index = 0; index < 8; index += 2) {
		require(cpu.vectors[31].bytes[index] == std::byte{0x34});
		require(cpu.vectors[31].bytes[index + 1] == std::byte{0x12});
	}
	for (std::size_t index = 8; index < 16; ++index)
		require(cpu.vectors[31].bytes[index] == std::byte{});
	require(arm64::detail::vector_half(cpu.vectors[2], 0) ==
		0x89ab'cdefU);
	require(arm64::detail::vector_half(cpu.vectors[2], 1) == 0);
	require(arm64::detail::vector_half(cpu.vectors[4], 0) ==
		0x0123'4567'89ab'cdefULL);
	require(arm64::detail::vector_half(cpu.vectors[4], 1) == 0);
}

static void integer_multiply_add()
{
	constexpr std::uint64_t base = 0x11'000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x9b01'0273U);     // madd x19, x19, x1, x0
	memory.put32(base + 4, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[0] = 7;
	cpu.registers[1] = 3;
	cpu.registers[19] = 5;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 4);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[19] == 22);
}

static void integer_multiply_long()
{
	constexpr std::uint64_t base = 0x12'000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x9ba7'7c21U);     // umull x1, w1, w7
	memory.put32(base + 4, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[1] = 0xffff'ffffU;
	cpu.registers[7] = 2;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 4);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.registers[1] == 0x1'ffff'fffeULL);
}

static void conditional_compare_immediate()
{
	constexpr std::uint64_t base = 0x13'000;
	FlatMemory memory(base, 0x1000);
	memory.put32(base, 0x7a4a'1800U);     // ccmp w0, #10, #0, ne
	memory.put32(base + 4, 0xd400'0001U); // svc #0
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[0] = 10;
	cpu.nzcv = 0;
	const auto result = arm64::Dispatcher{}.run(cpu, memory, 4);
	require(result.reason == arm64::StopReason::system_call);
	require(cpu.nzcv == 0x6000'0000U);
}

class TemporaryBacking final {
public:
	explicit TemporaryBacking(std::size_t size)
	{
		char path[] = "/private/tmp/uml-static-mmu.XXXXXX";
		fd_ = mkstemp(path);
		require(fd_ >= 0);
		require(unlink(path) == 0);
		require(ftruncate(fd_, static_cast<off_t>(size)) == 0);
	}

	TemporaryBacking(const TemporaryBacking &) = delete;
	TemporaryBacking &operator=(const TemporaryBacking &) = delete;

	~TemporaryBacking()
	{
		if (fd_ >= 0)
			close(fd_);
	}

	[[nodiscard]] int fd() const noexcept { return fd_; }

	void put32(std::size_t offset, std::uint32_t instruction)
	{
		std::array<std::byte, 4> encoded{};
		for (std::size_t index = 0; index < encoded.size(); ++index)
			encoded[index] = static_cast<std::byte>(instruction >> (index * 8));
		require(pwrite(fd_, encoded.data(), encoded.size(),
			offset) == static_cast<ssize_t>(encoded.size()));
	}

	void put(std::size_t offset, std::span<const std::byte> bytes)
	{
		require(pwrite(fd_, bytes.data(), bytes.size(), offset) ==
			static_cast<ssize_t>(bytes.size()));
	}

private:
	int fd_{-1};
};

static void uml_static_entry_bridge()
{
	constexpr unsigned long generation = 77;
	constexpr std::uint64_t base = 0x1'0000;
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xd280'00a0U);  // movz x0, #5
	backing.put32(4, 0x9100'1c01U);  // add x1, x0, #7
	backing.put32(8, 0xf900'07e1U);  // str x1, [sp, #8]
	backing.put32(12, 0xd400'0001U); // svc #0

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		1, 1) == 0);
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	registers.sp = base + 0x800;
	unsigned long tls = 0x1234;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 5);
	require(registers.x[1] == 12);
	require(registers.pc == base + 16);
	void *stored = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, registers.sp + 8, DARWIN_UML_STATIC_READ,
		&stored) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t value = 0;
	std::memcpy(&value, stored, sizeof(value));
	require(value == 12);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_descriptor_reuse_invalidation()
{
	constexpr unsigned long first_generation = 107;
	constexpr unsigned long second_generation = 108;
	constexpr std::uint64_t base = 0x4'0000;
	constexpr std::uint64_t first_value = 0x1111'2222'3333'4444ULL;
	constexpr std::uint64_t second_value = 0xaaaa'bbbb'cccc'ddddULL;
	char first_path[] = "/tmp/uml-static-fd-first-XXXXXX";
	int descriptor = mkstemp(first_path);
	require(descriptor >= 0);
	unlink(first_path);
	require(ftruncate(descriptor, DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	require(pwrite(descriptor, &first_value, sizeof(first_value), 0) ==
		static_cast<ssize_t>(sizeof(first_value)));
	require(darwin_uml_static_vm_activate(first_generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), descriptor, 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	const int reused_descriptor = descriptor;
	require(darwin_uml_close(descriptor) == 0);

	char second_path[] = "/tmp/uml-static-fd-second-XXXXXX";
	descriptor = mkstemp(second_path);
	require(descriptor >= 0);
	unlink(second_path);
	if (descriptor != reused_descriptor) {
		require(dup2(descriptor, reused_descriptor) == reused_descriptor);
		require(darwin_uml_close(descriptor) == 0);
		descriptor = reused_descriptor;
	}
	require(ftruncate(descriptor, DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	require(pwrite(descriptor, &second_value, sizeof(second_value), 0) ==
		static_cast<ssize_t>(sizeof(second_value)));
	require(darwin_uml_static_vm_activate(second_generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), descriptor, 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(
		second_generation, base, DARWIN_UML_STATIC_READ,
		&translated) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t actual = 0;
	std::memcpy(&actual, translated, sizeof(actual));
	require(actual == second_value);
	require(darwin_uml_close(descriptor) == 0);
	require(darwin_uml_static_vm_destroy(first_generation) == 0);
	require(darwin_uml_static_vm_destroy(second_generation) == 0);
}

static void uml_static_host_interrupt_sequence()
{
	using namespace std::chrono_literals;
	constexpr unsigned long generation = 106;
	constexpr std::uint64_t base = 0x3'c000;
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0x1400'0000U); // b .
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);

	unsigned int sequence = 0;
	darwin_uml_static_guest_interrupt_sequence(&sequence, sequence);
	std::thread interrupt{[&] {
		std::this_thread::sleep_for(10ms);
		__atomic_fetch_add(&sequence, 1U, __ATOMIC_RELEASE);
	}};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	interrupt.join();
	darwin_uml_static_guest_interrupt_sequence(nullptr, 0);
	require(event == DARWIN_UML_EVENT_INTERRUPT);
	require(registers.pc == base);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_signal_return_bridge()
{
	constexpr std::uint64_t base = 0x1'4000;
	for (const auto [generation, fallback] :
	     std::array<std::pair<unsigned long, bool>, 2>{{{78, false}, {79, true}}}) {
		TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
		if (fallback) {
			backing.put32(0, 0x8b01'0000U); // add x0, x0, x1
			backing.put32(4, 0xd65f'03c0U); // ret
		} else {
			backing.put32(0, 0xd65f'03c0U); // ret
		}

		require(darwin_uml_static_vm_activate(generation) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(base), backing.fd(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE,
			PROT_READ | PROT_EXEC, 1, 0) == 0);
		darwin_uml_guest_registers registers{};
		darwin_uml_guest_fp_registers fp{};
		registers.pc = base;
		registers.lr = DARWIN_UML_SIGRETURN_TRAMPOLINE;
		registers.x[0] = 4;
		registers.x[1] = 5;
		unsigned long tls = 0;
		const int event = darwin_uml_static_enter_guest(
			&registers, &fp, sizeof(fp), generation, &tls);
		require(event == DARWIN_UML_EVENT_SYSCALL);
		require(registers.x[8] == DARWIN_UML_NR_RT_SIGRETURN);
		require(registers.pc == DARWIN_UML_SIGRETURN_TRAMPOLINE + 4);
		if (fallback)
			require(registers.x[0] == 9);
		require(darwin_uml_static_vm_destroy(generation) == 0);
	}
}

static void uml_static_integer_addressing_gadgets()
{
	constexpr unsigned long generation = 89;
	constexpr std::uint64_t base = 0x1'8000;
	constexpr auto page_size = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr auto data = base + page_size;
	TemporaryBacking backing{3 * page_size};
	backing.put32(0, 0x3840'1403U);      // ldrb w3, [x0], #1
	backing.put32(4, 0x381f'fc23U);      // strb w3, [x1, #-1]!
	backing.put32(8, 0x7865'7844U);      // ldrh w4, [x2, x5, lsl #1]
	backing.put32(12, 0x389f'd0e6U);     // ldursb x6, [x7, #-3]
	backing.put32(16, 0x78c0'27e8U);     // ldrsh w8, [sp], #2
	backing.put32(20, 0xf82b'f949U);     // str x9, [x10, x11, sxtx #3]
	backing.put32(24, 0xf840'01acU);     // ldur x12, [x13]
	backing.put32(28, 0xa9bf'3e0eU);     // stp x14, x15, [x16, #-16]!
	backing.put32(32, 0xa8c1'4a11U);     // ldp x17, x18, [x16], #16
	backing.put32(36, 0xa940'52b3U);     // ldp x19, x20, [x21]
	backing.put32(40, 0xd400'0001U);     // svc #0

	const std::array byte_value{std::byte{0xa5}};
	const std::array half_value{std::byte{0x34}, std::byte{0x12}};
	const std::array signed_byte{std::byte{0x80}};
	const std::array signed_half{std::byte{0x00}, std::byte{0x80}};
	const std::array cross_page{
		std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
		std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88},
		std::byte{0x99}, std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc},
		std::byte{0xdd}, std::byte{0xee}, std::byte{0xf0}, std::byte{0x0f}};
	backing.put(page_size, byte_value);
	backing.put(page_size + 0x24, half_value);
	backing.put(page_size + 0x30, signed_byte);
	backing.put(page_size + 0x40, signed_half);
	backing.put(2 * page_size - 4, cross_page);

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0, 3 * page_size,
		PROT_READ | PROT_WRITE | PROT_EXEC, 1, 1) == 0);
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	registers.x[0] = data;
	registers.x[1] = data + 0x11;
	registers.x[2] = data + 0x20;
	registers.x[5] = 2;
	registers.x[7] = data + 0x33;
	registers.sp = data + 0x40;
	registers.x[9] = 0x1122'3344'5566'7788ULL;
	registers.x[10] = data + 0x50;
	registers.x[11] = 1;
	registers.x[13] = base + 2 * page_size - 4;
	registers.x[14] = 0x0102'0304'0506'0708ULL;
	registers.x[15] = 0x1112'1314'1516'1718ULL;
	registers.x[16] = data + 0x80;
	registers.x[21] = base + 2 * page_size - 4;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 44);
	require(registers.x[0] == data + 1);
	require(registers.x[1] == data + 0x10);
	require(registers.x[3] == 0xa5);
	require(registers.x[4] == 0x1234);
	require(registers.x[6] == 0xffff'ffff'ffff'ff80ULL);
	require(registers.x[8] == 0xffff'8000U);
	require(registers.sp == data + 0x42);
	require(registers.x[12] == 0x8877'6655'4433'2211ULL);
	require(registers.x[16] == data + 0x80);
	require(registers.x[17] == registers.x[14]);
	require(registers.x[18] == registers.x[15]);
	require(registers.x[19] == 0x8877'6655'4433'2211ULL);
	require(registers.x[20] == 0x0ff0'eedd'ccbb'aa99ULL);
	void *stored = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, data + 0x10, DARWIN_UML_STATIC_READ,
		&stored) == DARWIN_UML_STATIC_TRANSLATED);
	require(*static_cast<unsigned char *>(stored) == 0xa5);
	require(darwin_uml_static_vm_translate(
		generation, data + 0x58, DARWIN_UML_STATIC_READ,
		&stored) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t stored_value = 0;
	std::memcpy(&stored_value, stored, sizeof(stored_value));
	require(stored_value == registers.x[9]);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_permission_fault()
{
	constexpr unsigned long generation = 91;
	constexpr std::uint64_t base = 0x2'0000;
	TemporaryBacking backing{2 * DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xd280'0aa0U);  // movz x0, #0x55
	backing.put32(4, 0xf900'0020U);  // str x0, [x1]
	backing.put32(8, 0xd400'0001U);  // svc #0

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base + DARWIN_UML_GUEST_PAGE_SIZE),
		backing.fd(), DARWIN_UML_GUEST_PAGE_SIZE,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ, 0, 1) == 0);
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	registers.x[1] = base + DARWIN_UML_GUEST_PAGE_SIZE;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_FAULT);
	require(registers.pc == base + 4);
	require(registers.fault.address == base + DARWIN_UML_GUEST_PAGE_SIZE);
	require(registers.fault.error_code == DARWIN_UML_FAULT_WRITE);
	require(registers.fault.trap_number == DARWIN_UML_FAULT_PAGE);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_illegal_instruction()
{
	constexpr unsigned long generation = 109;
	constexpr std::uint64_t base = 0x2'2000;
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0x0000'0000U); // udf #0

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION);
	require(registers.pc == base);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_mapping_change_ranges()
{
	constexpr unsigned long generation = 97;
	constexpr std::uint64_t base = 0x2'4000;
	TemporaryBacking backing{2 * DARWIN_UML_GUEST_PAGE_SIZE};
	require(darwin_uml_static_vm_activate(generation) == 0);
	const auto initial = darwin_uml_static_vm_changes(generation);
	const auto initial_sequences =
		darwin_uml_static_vm_sequences(generation);
	require(initial_sequences.mapping_changes == initial);
	require(initial_sequences.code_changes ==
		darwin_uml_static_vm_code_changes(generation));
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		2 * DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	std::array<darwin_uml_static_mapping_change, 4> ranges{};
	std::uint64_t current = 0;
	require(darwin_uml_static_vm_mapping_changes(
		generation, initial, ranges.data(), ranges.size(), &current) == 1);
	require(ranges[0].address == base);
	require(ranges[0].length == 2 * DARWIN_UML_GUEST_PAGE_SIZE);
	require(current == darwin_uml_static_vm_changes(generation));
	const auto mapped_sequences =
		darwin_uml_static_vm_sequences(generation);
	require(mapped_sequences.mapping_changes == current);
	require(mapped_sequences.code_changes == initial_sequences.code_changes);

	/* Replaying an unchanged Linux PTE batch must not invalidate the TLB. */
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		2 * DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	require(darwin_uml_static_vm_changes(generation) == current);

	require(darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(base + DARWIN_UML_GUEST_PAGE_SIZE),
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ) == 0);
	const auto before_protect = current;
	require(darwin_uml_static_vm_mapping_changes(
		generation, before_protect, ranges.data(), ranges.size(), &current) == 1);
	require(ranges[0].address == base + DARWIN_UML_GUEST_PAGE_SIZE);
	require(ranges[0].length == DARWIN_UML_GUEST_PAGE_SIZE);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_partial_uaccess_copy_preserves_sibling_writes()
{
	constexpr unsigned long generation = 130;
	constexpr std::uint64_t base = 0x2'8000;
	constexpr std::size_t write_offset = 32;
	constexpr std::size_t concurrent_offset = 512;
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> stale_shadow{};
	stale_shadow.fill(std::byte{0x11});
	require(pwrite(backing.fd(), stale_shadow.data(), stale_shadow.size(), 0) ==
		static_cast<ssize_t>(stale_shadow.size()));
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(generation, base,
		DARWIN_UML_STATIC_WRITE, &translated) == DARWIN_UML_STATIC_TRANSLATED);
	auto *bytes = static_cast<std::byte *>(translated);
	bytes[concurrent_offset] = std::byte{0x7a};
	const std::array replacement{
		std::byte{0xa0}, std::byte{0xa1}, std::byte{0xa2}, std::byte{0xa3}};
	std::copy(replacement.begin(), replacement.end(),
		stale_shadow.begin() + write_offset);
	require(darwin_uml_static_vm_copy_from_buffer(
		base + write_offset, stale_shadow.data() + write_offset,
		replacement.size()) == 0);
	require(std::equal(replacement.begin(), replacement.end(),
		bytes + write_offset));
	require(bytes[concurrent_offset] == std::byte{0x7a});
	require(darwin_uml_static_vm_copy_from_buffer(base, nullptr, 0) == 0);
	require(darwin_uml_static_vm_copy_from_buffer(
		base + DARWIN_UML_GUEST_PAGE_SIZE - 1, stale_shadow.data(), 2) == -1);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_thread_local_generations()
{
	constexpr unsigned long first_generation = 101;
	constexpr unsigned long second_generation = 102;
	constexpr std::uint64_t base = 0x3'0000;
	constexpr std::uint64_t first_value = 0x1111'2222'3333'4444ULL;
	constexpr std::uint64_t second_value = 0xaaaa'bbbb'cccc'ddddULL;
	TemporaryBacking backing{2 * DARWIN_UML_GUEST_PAGE_SIZE};
	require(pwrite(backing.fd(), &first_value, sizeof(first_value), 0) ==
		static_cast<ssize_t>(sizeof(first_value)));
	require(pwrite(backing.fd(), &second_value, sizeof(second_value),
		DARWIN_UML_GUEST_PAGE_SIZE) ==
		static_cast<ssize_t>(sizeof(second_value)));
	std::barrier first_activated{2};
	std::barrier second_activated{2};
	std::thread first{[&] {
		require(darwin_uml_static_vm_activate(first_generation) == 0);
		first_activated.arrive_and_wait();
		second_activated.arrive_and_wait();
		require(darwin_uml_static_vm_map(reinterpret_cast<void *>(base),
			backing.fd(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
			PROT_READ | PROT_WRITE, 0, 1) == 0);
	}};
	std::thread second{[&] {
		first_activated.arrive_and_wait();
		require(darwin_uml_static_vm_activate(second_generation) == 0);
		second_activated.arrive_and_wait();
		require(darwin_uml_static_vm_map(reinterpret_cast<void *>(base),
			backing.fd(), DARWIN_UML_GUEST_PAGE_SIZE,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	}};
	first.join();
	second.join();
	void *first_host = nullptr;
	void *second_host = nullptr;
	require(darwin_uml_static_vm_translate(first_generation, base,
		DARWIN_UML_STATIC_READ, &first_host) == DARWIN_UML_STATIC_TRANSLATED);
	require(darwin_uml_static_vm_translate(second_generation, base,
		DARWIN_UML_STATIC_READ, &second_host) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t first_actual = 0;
	std::uint64_t second_actual = 0;
	std::memcpy(&first_actual, first_host, sizeof(first_actual));
	std::memcpy(&second_actual, second_host, sizeof(second_actual));
	require(first_actual == first_value);
	require(second_actual == second_value);
	const auto first_mapping_changes =
		darwin_uml_static_vm_changes(first_generation);
	const auto second_mapping_changes =
		darwin_uml_static_vm_changes(second_generation);
	const auto first_code_changes =
		darwin_uml_static_vm_code_changes(first_generation);
	const auto second_code_changes =
		darwin_uml_static_vm_code_changes(second_generation);
	const auto first_sequences =
		darwin_uml_static_vm_sequences(first_generation);
	const auto second_sequences =
		darwin_uml_static_vm_sequences(second_generation);
	require(first_sequences.mapping_changes == first_mapping_changes);
	require(first_sequences.code_changes == first_code_changes);
	require(second_sequences.mapping_changes == second_mapping_changes);
	require(second_sequences.code_changes == second_code_changes);
	require(darwin_uml_static_vm_activate(second_generation) == 0);
	require(darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(base), DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ) == 0);
	require(darwin_uml_static_vm_changes(first_generation) ==
		first_mapping_changes);
	require(darwin_uml_static_vm_changes(second_generation) >
		second_mapping_changes);
	require(darwin_uml_static_vm_code_changes(first_generation) ==
		first_code_changes);
	require(darwin_uml_static_vm_code_changes(second_generation) ==
		second_code_changes);
	darwin_uml_static_vm_invalidate_code(base);
	require(darwin_uml_static_vm_code_changes(first_generation) ==
		first_code_changes);
	require(darwin_uml_static_vm_code_changes(second_generation) >
		second_code_changes);
	require(darwin_uml_static_vm_destroy(first_generation) == 0);
	require(darwin_uml_static_vm_destroy(second_generation) == 0);
}

static void uml_static_concurrent_translation_readers()
{
	constexpr std::size_t reader_count = 16;
	constexpr std::size_t translation_count = 20'000;
	constexpr std::size_t protection_count = 4'000;
	constexpr unsigned long first_generation = 106;
	constexpr unsigned long writer_generation =
		first_generation + reader_count;
	constexpr std::uint64_t base = 0x3'2000;
	TemporaryBacking backing{(reader_count + 1) *
		DARWIN_UML_GUEST_PAGE_SIZE};

	for (std::size_t index = 0; index <= reader_count; ++index) {
		const std::uint64_t value = 0x5a5a'0000'0000'0000ULL + index;
		require(pwrite(backing.fd(), &value, sizeof(value),
			index * DARWIN_UML_GUEST_PAGE_SIZE) ==
			static_cast<ssize_t>(sizeof(value)));
		const auto generation = first_generation + index;
		require(darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(base), backing.fd(),
			index * DARWIN_UML_GUEST_PAGE_SIZE,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
			0, 1) == 0);
	}

	std::barrier start{reader_count + 1};
	std::vector<std::thread> readers;
	readers.reserve(reader_count);
	for (std::size_t index = 0; index < reader_count; ++index) {
		readers.emplace_back([&, index] {
			const auto generation = first_generation + index;
			const std::uint64_t expected =
				0x5a5a'0000'0000'0000ULL + index;
			std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> page{};
			std::memcpy(page.data(), &expected, sizeof(expected));
			require(darwin_uml_static_vm_activate(generation) == 0);
			start.arrive_and_wait();
			for (std::size_t iteration = 0;
			     iteration < translation_count; ++iteration) {
				if (iteration % 1'000 == 0)
					require(darwin_uml_static_vm_copy_from_buffer(
						base, page.data(), page.size()) == 0);
				void *host = nullptr;
				require(darwin_uml_static_vm_translate(generation, base,
					DARWIN_UML_STATIC_READ, &host) ==
					DARWIN_UML_STATIC_TRANSLATED);
				std::uint64_t actual = 0;
				std::memcpy(&actual, host, sizeof(actual));
				require(actual == expected);
			}
		});
	}
	std::thread writer{[&] {
		require(darwin_uml_static_vm_activate(writer_generation) == 0);
		start.arrive_and_wait();
		for (std::size_t iteration = 0; iteration < protection_count;
		     ++iteration) {
			const int protection = iteration % 2 == 0 ? PROT_READ :
				PROT_READ | PROT_WRITE;
			require(darwin_uml_static_vm_protect(
				reinterpret_cast<void *>(base),
				DARWIN_UML_GUEST_PAGE_SIZE, protection) == 0);
		}
	}};
	for (auto &reader : readers)
		reader.join();
	writer.join();
	for (std::size_t index = 0; index <= reader_count; ++index)
		require(darwin_uml_static_vm_destroy(first_generation + index) == 0);
}

static void uml_static_tlb_shootdown_quiesces_execution()
{
	using namespace std::chrono_literals;
	constexpr unsigned long generation = 103;
	constexpr std::uint64_t base = 0x3'4000;
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(reinterpret_cast<void *>(base),
		backing.fd(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);

	std::barrier execution_entered{2};
	std::barrier invalidation_started{2};
	std::atomic_bool release_execution{};
	std::atomic_bool invalidation_complete{};
	void *context = nullptr;
	std::thread execution{[&] {
		context = darwin_uml_static_vm_execution_context_create(generation);
		require(context != nullptr);
		require(darwin_uml_static_vm_execution_context_begin(
			context, base) == 0);
		execution_entered.arrive_and_wait();
		release_execution.wait(false, std::memory_order_acquire);
		require(darwin_uml_static_vm_execution_context_end(context) == 0);
		darwin_uml_static_vm_execution_context_destroy(context);
	}};
	execution_entered.arrive_and_wait();
	std::thread invalidation{[&] {
		invalidation_started.arrive_and_wait();
		require(darwin_uml_static_vm_unmap_generation(
			generation, reinterpret_cast<void *>(base),
			DARWIN_UML_GUEST_PAGE_SIZE) == 0);
		invalidation_complete.store(true, std::memory_order_release);
		invalidation_complete.notify_all();
	}};
	invalidation_started.arrive_and_wait();
	std::this_thread::sleep_for(20ms);
	require(!invalidation_complete.load(std::memory_order_acquire));
	const auto interrupted =
		darwin_uml_static_vm_execution_context_sequences(context);
	require(interrupted.execution_interrupt != 0);
	release_execution.store(true, std::memory_order_release);
	release_execution.notify_all();
	execution.join();
	invalidation.join();
	require(invalidation_complete.load(std::memory_order_acquire));
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(generation, base,
		DARWIN_UML_STATIC_READ, &translated) == DARWIN_UML_STATIC_UNMAPPED);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_batch_targets_explicit_generation()
{
	constexpr unsigned long active_generation = 104;
	constexpr unsigned long target_generation = 105;
	constexpr std::uint64_t base = 0x3'8000;
	constexpr std::uint64_t value = 0x1234'5678'9abc'def0ULL;
	constexpr std::uint64_t replacement = 0x0fed'cba9'8765'4321ULL;
	TemporaryBacking backing{2 * DARWIN_UML_GUEST_PAGE_SIZE};
	require(pwrite(backing.fd(), &value, sizeof(value), 0) ==
		static_cast<ssize_t>(sizeof(value)));
	require(pwrite(backing.fd(), &replacement, sizeof(replacement),
		DARWIN_UML_GUEST_PAGE_SIZE) ==
		static_cast<ssize_t>(sizeof(replacement)));
	require(darwin_uml_static_vm_activate(active_generation) == 0);

	/* Page-table synchronization may run on behalf of an mm other than the
	 * one active on this Darwin thread.  The batch and every operation inside
	 * it must target that explicit generation, and a mismatched nested batch
	 * must fail without disturbing the outer writer gate.
	 */
	require(darwin_uml_static_vm_batch_begin(target_generation) == 0);
	require(darwin_uml_static_vm_batch_begin(active_generation) == -1);
	require(darwin_uml_static_vm_map_generation(
		target_generation, reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	void *target_host = nullptr;
	require(darwin_uml_static_vm_translate(target_generation, base,
		DARWIN_UML_STATIC_READ, &target_host) == DARWIN_UML_STATIC_UNMAPPED);
	require(darwin_uml_static_vm_batch_end() == 0);

	void *active_host = nullptr;
	require(darwin_uml_static_vm_translate(active_generation, base,
		DARWIN_UML_STATIC_READ, &active_host) == DARWIN_UML_STATIC_UNMAPPED);
	require(darwin_uml_static_vm_translate(target_generation, base,
		DARWIN_UML_STATIC_READ, &target_host) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t actual = 0;
	std::memcpy(&actual, target_host, sizeof(actual));
	require(actual == value);

	/* An invalidation which wins while a page-table snapshot is buffered
	 * makes that snapshot stale.  The commit must be rejected instead of
	 * resurrecting its old physical mapping.
	 */
	require(darwin_uml_static_vm_batch_begin(target_generation) == 0);
	require(darwin_uml_static_vm_map_generation(
		target_generation, reinterpret_cast<void *>(base), backing.fd(),
		DARWIN_UML_GUEST_PAGE_SIZE, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	std::thread invalidation{[&] {
		require(darwin_uml_static_vm_unmap_generation(
			target_generation, reinterpret_cast<void *>(base),
			DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	}};
	invalidation.join();
	require(darwin_uml_static_vm_batch_end() == -EAGAIN);
	target_host = nullptr;
	require(darwin_uml_static_vm_translate(target_generation, base,
		DARWIN_UML_STATIC_READ, &target_host) == DARWIN_UML_STATIC_UNMAPPED);
	require(darwin_uml_static_vm_destroy(active_generation) == 0);
	require(darwin_uml_static_vm_destroy(target_generation) == 0);
}

static void uml_static_walker_turnstile_blocks_host_thread()
{
	using namespace std::chrono_literals;
	constexpr unsigned long generation = 106;
	std::atomic_bool started{};
	std::atomic_bool acquired{};

	require(darwin_uml_static_vm_walker_lock(generation) == 0);
	require(darwin_uml_static_vm_walker_lock(generation) == -1);
	require(darwin_uml_static_vm_walker_unlock(generation + 1) == -1);
	std::thread sibling{[&] {
		started.store(true, std::memory_order_release);
		started.notify_all();
		require(darwin_uml_static_vm_walker_lock(generation) == 0);
		acquired.store(true, std::memory_order_release);
		require(darwin_uml_static_vm_walker_unlock(generation) == 0);
	}};
	started.wait(false, std::memory_order_acquire);
	std::this_thread::sleep_for(20ms);
	require(!acquired.load(std::memory_order_acquire));
	require(darwin_uml_static_vm_walker_unlock(generation) == 0);
	sibling.join();
	require(acquired.load(std::memory_order_acquire));
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

int main()
{
	complete_neon_inventory_decodes();
	crypto_extension_known_answers();
	neon_saturating_halving_and_shift_semantics();
	neon_narrow_convert_and_indexed_semantics();
	neon_interleaved_structure_semantics();
	neon_widening_and_reduction_semantics();
	arithmetic_and_memory();
	static_control_flow();
	architectural_undefined_instruction();
	thread_pointer_access();
	architectural_system_registers();
	architectural_hint();
	architectural_barriers();
	architectural_cache_maintenance();
	integer_register_pair();
	logical_bitmask_immediate();
	add_sub_register_forms();
	add_sub_with_carry_forms();
	variable_shift_register_forms();
	scalar_division();
	scalar_bit_reverse_and_count();
	scalar_floating_compare();
	scalar_floating_moves_and_conversions();
	scalar_fixed_point_conversions();
	scalar_floating_to_integer_rounding();
	scalar_floating_multiply_add();
	scalar_floating_rounding();
	addressing_and_conditional_select();
	scalar_acquire_release_exclusive();
	scalar_same_value_store_invalidates_exclusive();
	split_alias_fails_exclusive_store();
	unrelated_store_preserves_exclusive();
	bitfield_aliases();
	extract_register_forms();
	simd_zero_and_pair_store();
	simd_single_load_store_addressing();
	simd_contiguous_structure_load_store();
	simd_single_structure_load_store();
	simd_signed_compare_zero();
	simd_scalar_signed_compare_zero();
	simd_integer_compare_register();
	simd_bitwise_register_operations();
	simd_integer_min_max();
	simd_unary_bits_and_reversal();
	simd_integer_reductions();
	simd_byte_extract();
	simd_integer_arithmetic();
	simd_narrowing_arithmetic();
	simd_narrowing_moves();
	simd_widening_arithmetic();
	simd_widening_multiply();
	simd_integer_float_conversions();
	simd_floating_unary();
	simd_floating_arithmetic();
	simd_lane_permutations();
	simd_byte_table_lookup();
	simd_narrowing_shift_right();
	simd_immediate_shifts();
	simd_scalar_immediate_shifts();
	simd_variable_shifts();
	simd_shift_left_long();
	simd_general_register_moves();
	simd_insert_vector_element();
	simd_duplicate_general_register();
	simd_duplicate_vector_element();
	integer_multiply_add();
	integer_multiply_long();
	conditional_compare_immediate();
	uml_static_entry_bridge();
	uml_static_descriptor_reuse_invalidation();
	uml_static_host_interrupt_sequence();
	uml_static_signal_return_bridge();
	uml_static_integer_addressing_gadgets();
	uml_static_permission_fault();
	uml_static_illegal_instruction();
	uml_static_mapping_change_ranges();
	uml_static_partial_uaccess_copy_preserves_sibling_writes();
	uml_static_thread_local_generations();
	uml_static_concurrent_translation_readers();
	uml_static_tlb_shootdown_quiesces_execution();
	uml_static_batch_targets_explicit_generation();
	uml_static_walker_turnstile_blocks_host_thread();
	return 0;
}
