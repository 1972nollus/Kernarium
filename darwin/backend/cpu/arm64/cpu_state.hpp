// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace uml::backend::arm64 {

struct alignas(16) VectorRegister final {
	std::array<std::byte, 16> bytes{};
};

struct CpuState final {
	std::array<std::uint64_t, 31> registers{};
	std::uint64_t sp{};
	std::uint64_t pc{};
	std::uint32_t nzcv{};
	std::array<VectorRegister, 32> vectors{};
	std::uint32_t fpcr{};
	std::uint32_t fpsr{};
	std::uint64_t tls{};
	std::uint64_t exclusive_address{~std::uint64_t{0}};
	std::uintptr_t exclusive_key{};
	std::uint64_t exclusive_value{};
	std::uint64_t exclusive_version{};
	std::uint8_t exclusive_size{};
	const std::byte *deferred_vectors{};
	std::uint32_t deferred_fpcr{};
	std::uint32_t deferred_fpsr{};
	bool fp_loaded{true};
	bool fp_dirty{};

	void defer_fp_load(const void *source, std::uint32_t source_fpcr,
			  std::uint32_t source_fpsr) noexcept
	{
		deferred_vectors = static_cast<const std::byte *>(source);
		deferred_fpcr = source_fpcr;
		deferred_fpsr = source_fpsr;
		fp_loaded = false;
		fp_dirty = false;
	}

	void access_fp_state() noexcept
	{
		if (!fp_loaded) {
			std::memcpy(vectors.data(), deferred_vectors, sizeof(vectors));
			fpcr = deferred_fpcr;
			fpsr = deferred_fpsr;
			fp_loaded = true;
		}
		fp_dirty = true;
	}

	[[nodiscard]] std::uint64_t read_register(unsigned index,
					     bool stack_pointer) const noexcept
	{
		if (index < registers.size())
			return registers[index];
		return stack_pointer ? sp : 0;
	}

	void write_register(unsigned index, std::uint64_t value,
			    bool stack_pointer) noexcept
	{
		if (index < registers.size())
			registers[index] = value;
		else if (stack_pointer)
			sp = value;
	}
};

} // namespace uml::backend::arm64
