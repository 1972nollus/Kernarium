// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* The AArch32 translator the gadget engine's builder (program_builder_a32.cpp)
 * is made of. The class is declared here so that the families whose lowering
 * is long -- the integer core, VFP, Advanced SIMD -- can be defined in
 * translation units of their own; nothing outside the builder's files
 * includes this header. */
#include "a32_builder.hpp"
#include "a32_forms.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/layout/assembly_offsets.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace uml::cpu::arm64::gadgets::a32::detail {

using namespace aarch32;

/* CpuState::registers[15] and [16], where PC read as an operand and a
 * destination PC live. */
constexpr std::uintptr_t scratch0 = 15 * 8;
constexpr std::uintptr_t scratch1 = 16 * 8;

/* Where D0-D31 and S0-S31 are: they overlay V0-V15 (D(2n) is the low half
 * of V(n)), so a VFP or Advanced SIMD register is a byte offset. */
constexpr std::uint64_t vector_base = UML_GADGET_CPU_V0;
[[nodiscard]] constexpr std::uint64_t s_register(int number) noexcept
{
	return vector_base + 4U * static_cast<std::uint64_t>(number);
}
[[nodiscard]] constexpr std::uint64_t d_register(int number) noexcept
{
	return vector_base + 8U * static_cast<std::uint64_t>(number);
}

template <typename Gadget>
[[nodiscard]] std::uintptr_t address_of(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

class Translator final {
public:
	Translator(const BuildInput &input,
		   const Builder::LinkResolver &links,
		   std::pmr::memory_resource *storage,
		   const Builder::IndirectLinkResolver &indirects,
		   const Profile &profile, std::size_t block_instructions)
		: input_{input}, links_{links}, indirects_{indirects},
		  profile_{profile}, words_{storage}, it_{input.it},
		  thumb_{key_isa(input.key) == GuestIsa::t32},
		  unit_bytes_{thumb_ ? 2U : 4U},
		  limit_{input.single ? 1U : block_instructions},
		  soft_limit_{input.single ? 1U : block_instructions - 5U},
		  available_{input.code.size() / unit_bytes_}
	{}

	[[nodiscard]] BuildOutput run();

private:
	struct Result final {
		bool terminal{};
		bool flags{};
	};

	/* ---------------------------------------------------------- output */
	void push(std::uintptr_t word) { words_.push_back(word); }
	void push(G which) { words_.push_back(gadget(which)); }
	template <typename Gadget>
	void push_external(Gadget which)
	{
		words_.push_back(address_of(which));
	}
	void push_link(std::uint64_t key)
	{
		push(reinterpret_cast<std::uintptr_t>(links_(key)));
	}
	[[nodiscard]] bool linked() const noexcept { return !!links_; }

	/* ------------------------------------------------------- positions */
	[[nodiscard]] std::uint32_t pc_value() const noexcept
	{
		return key_pc(key_) + (thumb_ ? 4U : 8U);
	}
	[[nodiscard]] std::uint32_t pc_aligned() const noexcept
	{
		return pc_value() & ~3U;
	}
	[[nodiscard]] static std::uint64_t target_key(std::uint32_t address,
						      bool to_thumb) noexcept
	{
		return to_thumb ? t32_key(address & ~1U) : a32_key(address);
	}
	/* The value LR gets for a call returning to the next instruction. */
	[[nodiscard]] std::uint32_t return_value() const noexcept
	{
		return key_pc(next_key_) | (thumb_ ? 1U : 0U);
	}

	/* ------------------------------------------------ conditional runs */
	void close_run();
	void begin_instruction(unsigned cond);
	void flags_written() noexcept { run_dirty_ = true; }
	/* A terminal that does not end the block gives back the instructions
	 * translated after it, which the block's precharge counted. */
	void credit();

	/* --------------------------------------------------- the registers */
	[[nodiscard]] std::uintptr_t source(int number, std::uintptr_t slot);
	void set_scratch(std::uintptr_t slot, std::uint32_t value);

	/* -------------------------------------------------------- families */
	Result emit(const Instruction &in, const FormPlan &plan);
	Result emit_dp(const Instruction &in, const FormPlan &plan);
	Result emit_dp_values(const Instruction &in, unsigned op, bool s,
			      unsigned shape_class, std::uintptr_t rd,
			      std::uintptr_t rn, std::uintptr_t rm,
			      std::uintptr_t rs, std::uint32_t immediate,
			      std::optional<unsigned> carry);
	Result emit_mov_constant(const Instruction &in, int d,
				 std::uint32_t value);
	Result emit_multiply(const Instruction &in, const FormPlan &plan);
	Result emit_integer(const Instruction &in, const FormPlan &plan);
	Result emit_single(const Instruction &in, const FormPlan &plan);
	Result emit_double(const Instruction &in, const FormPlan &plan);
	Result emit_block_transfer(const Instruction &in, const FormPlan &plan);
	Result emit_exclusive(const Instruction &in, const FormPlan &plan);
	Result emit_ordered(const Instruction &in, const FormPlan &plan);
	Result emit_branch(const Instruction &in, const FormPlan &plan);
	Result emit_system(const Instruction &in, const FormPlan &plan);
	Result emit_library(const Instruction &in);
	[[nodiscard]] std::uint64_t library_operand(const Instruction &in,
						    bool pc_value = false) const noexcept;
	/* VFP: program_builder_a32_vfp.cpp. */
	Result emit_vfp(const Instruction &in, const FormPlan &plan);
	Result emit_vfp_convert(const Instruction &in, const FormPlan &plan);
	Result emit_vfp_core(const Instruction &in, const FormPlan &plan);
	Result emit_vfp_memory(const Instruction &in, const FormPlan &plan);
	void emit_indirect(std::uintptr_t source, bool interwork, bool returns);
	void emit_direct_exit(std::uint64_t target);
	void emit_fallthrough();
	void finish_stop(bool instruction_read, std::uint32_t word);

	const BuildInput &input_;
	const Builder::LinkResolver &links_;
	const Builder::IndirectLinkResolver &indirects_;
	const Profile &profile_;
	ProgramWords words_;
	ItState it_;
	bool thumb_;
	std::size_t unit_bytes_;
	std::size_t limit_;
	std::size_t soft_limit_;
	std::size_t available_;

	std::size_t unit_{};
	std::size_t compiled_{};
	std::uint64_t key_{};
	std::uint64_t next_key_{};
	bool ended_{};
	bool complete_{true};
	std::uint64_t stop_address_{};
	std::uint32_t stop_instruction_{};
	std::size_t units_{};

	bool run_active_{};
	unsigned run_cond_{14};
	std::size_t run_operand_{};
	bool run_dirty_{};
	/* Operands the end of the block patches: the word, and the number of
	 * instructions translated through the one that holds it. */
	std::vector<std::pair<std::size_t, std::size_t>> patches_;
	/* The block holds an instruction that reads or writes V (and so is
	 * entered through the FP state link). */
	bool uses_fp_{};
};

} // namespace uml::cpu::arm64::gadgets::a32::detail
