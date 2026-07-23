// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <source_location>
#include <span>
#include <sys/mman.h>
#include <unistd.h>

namespace arm64_load_store_test {

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

inline void require(
	bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

struct IntegerVariant final {
	unsigned size_code;
	unsigned operation;
	std::size_t size;
	bool load;
};

inline constexpr std::array integer_variants{
	IntegerVariant{0, 0, 1, false}, // STRB
	IntegerVariant{0, 1, 1, true},  // LDRB
	IntegerVariant{0, 2, 1, true},  // LDRSB X
	IntegerVariant{0, 3, 1, true},  // LDRSB W
	IntegerVariant{1, 0, 2, false}, // STRH
	IntegerVariant{1, 1, 2, true},  // LDRH
	IntegerVariant{1, 2, 2, true},  // LDRSH X
	IntegerVariant{1, 3, 2, true},  // LDRSH W
	IntegerVariant{2, 0, 4, false}, // STR W
	IntegerVariant{2, 1, 4, true},  // LDR W
	IntegerVariant{2, 2, 4, true},  // LDRSW X
	IntegerVariant{3, 0, 8, false}, // STR X
	IntegerVariant{3, 1, 8, true},  // LDR X
};

struct VectorVariant final {
	unsigned size_code;
	unsigned operation;
	std::size_t size;
	bool load;
};

inline constexpr std::array vector_variants{
	VectorVariant{0, 0, 1, false}, VectorVariant{0, 1, 1, true},
	VectorVariant{1, 0, 2, false}, VectorVariant{1, 1, 2, true},
	VectorVariant{2, 0, 4, false}, VectorVariant{2, 1, 4, true},
	VectorVariant{3, 0, 8, false}, VectorVariant{3, 1, 8, true},
	VectorVariant{0, 2, 16, false}, VectorVariant{0, 3, 16, true},
};

[[nodiscard]] inline constexpr std::uint32_t unsigned_immediate(
	unsigned size_code, unsigned operation, unsigned imm12,
	unsigned rn, unsigned rt, bool vector = false)
{
	return 0x3900'0000U | (size_code << 30) | (operation << 22) |
		(static_cast<unsigned>(vector) << 26) | ((imm12 & 0xfffU) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t immediate_other(
	unsigned size_code, unsigned operation, unsigned imm9, unsigned mode,
	unsigned rn, unsigned rt, bool vector = false)
{
	return 0x3800'0000U | (size_code << 30) | (operation << 22) |
		(static_cast<unsigned>(vector) << 26) | ((imm9 & 0x1ffU) << 12) |
		((mode & 3U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t register_offset(
	unsigned size_code, unsigned operation, unsigned rm, unsigned option,
	bool scaled, unsigned mode, unsigned rn, unsigned rt,
	bool vector = false)
{
	return 0x3820'0000U | (size_code << 30) | (operation << 22) |
		(static_cast<unsigned>(vector) << 26) | ((rm & 31U) << 16) |
		((option & 7U) << 13) | (static_cast<unsigned>(scaled) << 12) |
		((mode & 3U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t pair(
	unsigned operation, bool vector, bool load, unsigned mode,
	unsigned imm7, unsigned rn, unsigned rt, unsigned rt2)
{
	return 0x2800'0000U | ((operation & 3U) << 30) |
		(static_cast<unsigned>(vector) << 26) | ((mode & 3U) << 23) |
		(static_cast<unsigned>(load) << 22) | ((imm7 & 0x7fU) << 15) |
		((rt2 & 31U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t simd_multiple_structure(
	bool full_vector, bool load, bool post_index, unsigned offset_register,
	unsigned opcode, unsigned size, unsigned rn, unsigned rt)
{
	return 0x0c00'0000U | (static_cast<unsigned>(full_vector) << 30) |
		(static_cast<unsigned>(post_index) << 23) |
		(static_cast<unsigned>(load) << 22) |
		((offset_register & 31U) << 16) | ((opcode & 15U) << 12) |
		((size & 3U) << 10) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::uint32_t simd_single_structure(
	bool full_vector, bool load, bool post_index, unsigned extra_register,
	unsigned offset_register, unsigned opcode, unsigned scale_bit,
	unsigned size, unsigned rn, unsigned rt)
{
	return 0x0d00'0000U | (static_cast<unsigned>(full_vector) << 30) |
		(static_cast<unsigned>(post_index) << 23) |
		(static_cast<unsigned>(load) << 22) |
		((extra_register & 1U) << 21) |
		((offset_register & 31U) << 16) | ((opcode & 7U) << 13) |
		((scale_bit & 1U) << 12) | ((size & 3U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline constexpr std::int64_t signed_imm7(unsigned imm7)
{
	return static_cast<std::int64_t>(
		static_cast<std::uint64_t>(imm7 & 0x7fU) << 57) >> 57;
}

[[nodiscard]] inline constexpr std::int64_t signed_imm9(unsigned imm9)
{
	return static_cast<std::int64_t>(
		static_cast<std::uint64_t>(imm9 & 0x1ffU) << 55) >> 55;
}

[[nodiscard]] inline constexpr std::uint64_t sign_extend(
	std::uint64_t value, unsigned bits)
{
	const auto shift = 64U - bits;
	return static_cast<std::uint64_t>(
		static_cast<std::int64_t>(value << shift) >> shift);
}

[[nodiscard]] inline constexpr std::uint64_t expected_integer_load(
	const IntegerVariant &variant, std::uint64_t raw)
{
	if (variant.operation < 2)
		return raw & (variant.size == 8 ? ~std::uint64_t{0} :
			((std::uint64_t{1} << (variant.size * 8)) - 1));
	auto value = sign_extend(raw, static_cast<unsigned>(variant.size * 8));
	if (variant.operation == 3)
		value &= 0xffff'ffffU;
	return value;
}

[[nodiscard]] inline constexpr std::uint64_t extend_offset(
	std::uint64_t value, unsigned option, unsigned shift)
{
	switch (option) {
	case 2: value &= 0xffff'ffffU; break;
	case 3: break;
	case 6: value = sign_extend(value & 0xffff'ffffU, 32); break;
	case 7: break;
	default: return 0;
	}
	return value << shift;
}

class ProbeMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t address,
				std::span<std::byte> output) override
	{
		last_address = address;
		last_size = output.size();
		++reads;
		if (fault_read)
			return false;
		std::copy_n(payload.begin(), output.size(), output.begin());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
				 std::span<const std::byte> input) override
	{
		last_address = address;
		last_size = input.size();
		++writes;
		if (fault_write)
			return false;
		written.fill(std::byte{});
		std::copy(input.begin(), input.end(), written.begin());
		return true;
	}

	void reset_counts()
	{
		reads = 0;
		writes = 0;
		last_address = 0;
		last_size = 0;
	}

	std::array<std::byte, 16> payload{
		std::byte{0x81}, std::byte{0xfe}, std::byte{0xd3}, std::byte{0xa4},
		std::byte{0x75}, std::byte{0x46}, std::byte{0x17}, std::byte{0xe8},
		std::byte{0xb9}, std::byte{0x8a}, std::byte{0x5b}, std::byte{0x2c},
		std::byte{0xfd}, std::byte{0xce}, std::byte{0x9f}, std::byte{0x60}};
	std::array<std::byte, 16> written{};
	std::uint64_t last_address{};
	std::size_t last_size{};
	std::size_t reads{};
	std::size_t writes{};
	bool fault_read{};
	bool fault_write{};
};

[[nodiscard]] inline std::uint64_t payload_value(const ProbeMemory &memory,
						 std::size_t size)
{
	std::uint64_t value{};
	std::memcpy(&value, memory.payload.data(), size);
	return value;
}

class StaticMapping final {
public:
	static constexpr std::size_t page_size = 4096;
	/* Covers the largest 12-bit scaled Q offset (65520 bytes), while still
	 * leaving pages on either side for signed and cross-page cases. */
	static constexpr std::size_t length = 24 * page_size;

	StaticMapping(std::uint64_t guest, unsigned long generation,
		      const char *pattern) : guest_{guest}, generation_{generation}
	{
		std::array<char, 64> path{};
		require(std::strlen(pattern) + 1 <= path.size());
		std::memcpy(path.data(), pattern, std::strlen(pattern) + 1);
		fd_ = mkstemp(path.data());
		require(fd_ >= 0);
		require(unlink(path.data()) == 0);
		require(ftruncate(fd_, static_cast<off_t>(length)) == 0);
		host_ = static_cast<std::byte *>(mmap(nullptr, length,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
		require(host_ != MAP_FAILED);
		require(darwin_uml_static_vm_activate(generation_) == 0);
		require(darwin_uml_static_vm_map(reinterpret_cast<void *>(guest_), fd_,
			0, length, PROT_READ | PROT_WRITE, 0, 1) == 0);
		tlb_ = std::make_unique<gadgets::Tlb>();
		tlb_->prepare(generation_, darwin_uml_static_vm_changes(generation_));
	}

	StaticMapping(const StaticMapping &) = delete;
	StaticMapping &operator=(const StaticMapping &) = delete;

	~StaticMapping()
	{
		tlb_.reset();
		(void)darwin_uml_static_vm_unmap(
			reinterpret_cast<void *>(guest_), length);
		if (host_ != MAP_FAILED)
			(void)munmap(host_, length);
		if (fd_ >= 0)
			(void)close(fd_);
	}

	[[nodiscard]] std::byte *at(std::uint64_t address)
	{
		require(address >= guest_ && address < guest_ + length);
		return host_ + (address - guest_);
	}

	[[nodiscard]] gadgets::Event execute(std::uint64_t pc,
		std::uint32_t instruction, arm64::CpuState &cpu)
	{
		const std::array stream{instruction, 0xd400'0001U};
		return execute(pc, std::span<const std::uint32_t>{stream}, cpu);
	}

	[[nodiscard]] gadgets::Event execute(std::uint64_t pc,
		std::span<const std::uint32_t> instructions, arm64::CpuState &cpu)
	{
		const auto built = gadgets::Builder{}.build(pc, instructions);
		require(built.complete);
		cpu.pc = pc;
		return gadgets::execute(built.program, cpu, *tlb_);
	}

	[[nodiscard]] std::uint64_t guest() const { return guest_; }

private:
	std::uint64_t guest_{};
	unsigned long generation_{};
	int fd_{-1};
	std::byte *host_{reinterpret_cast<std::byte *>(MAP_FAILED)};
	std::unique_ptr<gadgets::Tlb> tlb_;
};

inline void fill_registers(arm64::CpuState &cpu)
{
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0x8000'0000'1000'0000ULL |
			(static_cast<std::uint64_t>(index) << 8) | index;
	cpu.sp = 0x4000;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
}

} // namespace arm64_load_store_test
