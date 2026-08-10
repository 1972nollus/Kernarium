// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "platform/apple/host_features.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <array>
#include <algorithm>
#include <span>

using namespace uml::emulator::tests;

namespace {

constexpr unsigned long hwcap_fp = 1UL << 0;
constexpr unsigned long hwcap_asimd = 1UL << 1;
constexpr unsigned long hwcap_aes = 1UL << 3;
constexpr unsigned long hwcap_pmull = 1UL << 4;
constexpr unsigned long hwcap_sha1 = 1UL << 5;
constexpr unsigned long hwcap_sha2 = 1UL << 6;
constexpr unsigned long hwcap_crc32 = 1UL << 7;
constexpr unsigned long hwcap_atomics = 1UL << 8;
constexpr unsigned long hwcap_fphp = 1UL << 9;
constexpr unsigned long hwcap_asimdhp = 1UL << 10;
constexpr unsigned long hwcap_asimdrdm = 1UL << 12;
constexpr unsigned long hwcap_jscvt = 1UL << 13;
constexpr unsigned long hwcap_fcma = 1UL << 14;
constexpr unsigned long hwcap_lrcpc = 1UL << 15;
constexpr unsigned long hwcap_sha3 = 1UL << 17;
constexpr unsigned long hwcap_asimddp = 1UL << 20;
constexpr unsigned long hwcap_sha512 = 1UL << 21;
constexpr unsigned long hwcap_asimdfhm = 1UL << 23;
constexpr unsigned long hwcap_ilrcpc = 1UL << 26;
constexpr unsigned long hwcap2_frint = 1UL << 8;
constexpr unsigned long hwcap2_i8mm = 1UL << 13;
constexpr unsigned long hwcap2_bf16 = 1UL << 14;
constexpr std::uint64_t feature_gate_pc = 0x4f90'0000;

[[nodiscard]] constexpr std::uint32_t pstate_immediate(
	unsigned immediate, unsigned op2) noexcept
{
	return 0xd503'401fU | ((immediate & 15U) << 8U) |
		((op2 & 7U) << 5U);
}

void require_propagated(unsigned long capabilities, unsigned long bit,
	bool supported)
{
	require(((capabilities & bit) != 0) == supported);
}

void guest_capabilities_match_host()
{
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	const auto capabilities = darwin_uml_guest_hwcap();
	require_propagated(capabilities, hwcap_fp, true);
	require_propagated(capabilities, hwcap_asimd, true);
	require_propagated(capabilities, hwcap_aes, host.aes);
	require_propagated(capabilities, hwcap_pmull, host.pmull);
	require_propagated(capabilities, hwcap_sha1, host.sha1);
	require_propagated(capabilities, hwcap_sha2, host.sha2);
	require_propagated(capabilities, hwcap_crc32, host.crc32);
	require_propagated(capabilities, hwcap_atomics,
			   host.has(::uml::cpu::ArmFeature::lse));
	require_propagated(capabilities, hwcap_sha3, host.sha3);
	require_propagated(capabilities, hwcap_sha512, host.sha512);
	/* The dot family is advertised on exactly the predicate the gadget bank
	 * and the JIT use to select their native dot paths, so the guest can only
	 * be handed these opcodes where an engine executes them. */
	require_propagated(capabilities, hwcap_asimddp,
			   host.has(::uml::cpu::ArmFeature::dotprod));
	require_propagated(capabilities, hwcap_fphp, host.fp16);
	require_propagated(capabilities, hwcap_asimdhp, host.fp16);
	require_propagated(capabilities, hwcap_asimdrdm,
			   host.has(::uml::cpu::ArmFeature::rdm));
	require_propagated(capabilities, hwcap_asimdfhm,
			   host.has(::uml::cpu::ArmFeature::fhm));
	require_propagated(capabilities, hwcap_jscvt,
			   host.has(::uml::cpu::ArmFeature::jscvt) &&
				   host.jscvt_subnormal_flags_correct);
	require_propagated(capabilities, hwcap_fcma,
			   host.has(::uml::cpu::ArmFeature::fcma));
	require_propagated(capabilities, hwcap_lrcpc,
			   host.has(::uml::cpu::ArmFeature::lrcpc));
	require_propagated(capabilities, hwcap_ilrcpc,
			   host.has(::uml::cpu::ArmFeature::lrcpc2));

	constexpr auto advertised =
		hwcap_fp | hwcap_asimd | hwcap_aes | hwcap_pmull | hwcap_sha1 |
		hwcap_sha2 | hwcap_crc32 | hwcap_atomics | hwcap_fphp |
		hwcap_asimdhp | hwcap_asimdrdm | hwcap_jscvt | hwcap_sha3 |
		hwcap_asimddp | hwcap_sha512 | hwcap_asimdfhm | hwcap_fcma |
		hwcap_lrcpc | hwcap_ilrcpc;
	require((capabilities & ~advertised) == 0);

	const auto capabilities2 = darwin_uml_guest_hwcap2();
	require_propagated(capabilities2, hwcap2_i8mm,
			   host.has(::uml::cpu::ArmFeature::i8mm));
	require_propagated(capabilities2, hwcap2_frint,
			   host.has(::uml::cpu::ArmFeature::frintts));
	require_propagated(capabilities2, hwcap2_bf16,
			   host.has(::uml::cpu::ArmFeature::bf16));
	constexpr auto advertised2 = hwcap2_frint | hwcap2_i8mm | hwcap2_bf16;
	require((capabilities2 & ~advertised2) == 0);
}

[[nodiscard]] bool is_host_key_operation(std::uint32_t instruction)
{
	if ((instruction & 0xffff'c000U) == 0xdac1'0000U ||
	    (instruction & 0xffff'f800U) == 0xdac1'4000U ||
	    (instruction & 0xffe0'fc00U) == 0x9ac0'3000U ||
	    (instruction & 0xfedf'f800U) == 0xd61f'0800U ||
	    (instruction & 0xffff'fbffU) == 0xd65f'0bffU ||
	    (instruction & 0xff20'0400U) == 0xf820'0400U)
		return true;
	if ((instruction & 0xffff'f01fU) != 0xd503'201fU)
		return false;
	const unsigned immediate = (instruction >> 5) & 0x7fU;
	return immediate == 7U || immediate == 8U || immediate == 10U ||
		immediate == 12U || immediate == 14U ||
		(immediate >= 24U && immediate <= 31U);
}

void guest_key_operations_use_semantic_bridge()
{
	constexpr std::uint64_t pc = 0x4fa0'0000;
	constexpr std::array instructions{
		0xdac1'0020U, // register sign
		0xdac1'43e0U, // strip
		0x9ac2'3020U, // generic code
		0xd503'233fU, // fixed-register sign
		0xd61f'083fU, // authenticated branch
		0xf820'0420U, // authenticated load
	};
	for (std::size_t index = 0; index < instructions.size(); ++index) {
		const std::array guest{instructions[index]};
		const auto compiled =
			uml::cpu::arm64::jit::testing::compile_block(
				pc + index * 0x1000U, guest);
		require(compiled.guest_instructions == 1);
		require(compiled.hot_words <= compiled.words.size());
		bool calls_bridge = false;
		for (const auto word :
		     std::span<const std::uint32_t>{compiled.words}.first(
			     compiled.hot_words)) {
			require(!is_host_key_operation(word));
			calls_bridge |= word == 0xd63f'0200U; // blr x16
		}
		require(calls_bridge);
	}
}

void require_translation_feature_gate(std::uint32_t instruction,
	uml::cpu::ArmFeature feature, bool writes_native_fpmr = false)
{
	const std::array guest{instruction};
	const auto unavailable =
		uml::cpu::arm64::jit::testing::compile_block_with_feature_override(
			feature_gate_pc, guest, feature, false);
	require(unavailable.guest_illegal);
	require(unavailable.guest_instructions == 1);
	require(!unavailable.native_fp);
	require(!unavailable.native_fpmr);

	const auto available =
		uml::cpu::arm64::jit::testing::compile_block_with_feature_override(
			feature_gate_pc, guest, feature, true);
	require(!available.guest_illegal);
	require(available.guest_instructions == 1);
	require(!available.native_fp);
	require(available.native_fpmr == writes_native_fpmr);
}

void stateful_system_instructions_are_translation_feature_gated()
{
	using uml::cpu::ArmFeature;

	for (unsigned immediate = 0; immediate < 16; ++immediate) {
		require_translation_feature_gate(
			pstate_immediate(immediate, 1), ArmFeature::ssbs);
		require_translation_feature_gate(
			pstate_immediate(immediate, 2), ArmFeature::dit);
		require_translation_feature_gate(
			pstate_immediate(immediate, 4), ArmFeature::mte);
	}
	for (unsigned immediate = 2; immediate < 8; ++immediate)
		require_translation_feature_gate(
			pstate_immediate(immediate, 3), ArmFeature::sme);

	for (unsigned transfer = 0; transfer < 32; ++transfer) {
		require_translation_feature_gate(
			0xd53b'4440U | transfer, ArmFeature::fp8);
		require_translation_feature_gate(
			0xd51b'4440U | transfer, ArmFeature::fp8, true);
	}
}

void linked_blocks_use_persistent_internal_entry()
{
	constexpr std::uint64_t pc = 0x4fb0'0000;
	constexpr std::array guest{0x1400'0001U}; // b pc + 4
	const auto compiled =
		uml::cpu::arm64::jit::testing::compile_linked_block(pc, guest);
	constexpr std::array external_prologue{
		0xa9bc'7bfdU, // stp x29, x30, [sp, #-64]!
		0x9100'03fdU, // mov x29, sp
		0xa901'07e0U, // stp x0, x1, [sp, #16]
		0xa902'57e2U, // stp x2, x21, [sp, #32]
		0xa903'53f3U, // stp x19, x20, [sp, #48]
		0xaa00'03efU, // mov x15, x0
		0xaa01'03eeU, // mov x14, x1
		0xaa02'03f3U, // mov x19, x2
		0xd282'0014U, // movz x20, #4096
		0x3940'6275U, // ldrb w21, [x19, #24]
	};
	require(compiled.guest_instructions == 1);
	require(compiled.internal_entry_word == external_prologue.size());
	require(compiled.words.size() >= external_prologue.size());
	require(std::equal(external_prologue.begin(), external_prologue.end(),
			   compiled.words.begin()));

	const auto hot = std::span<const std::uint32_t>{compiled.words}.first(
		compiled.hot_words);
	const auto branch = std::find(hot.begin(), hot.end(), 0xd61f'01a0U);
	require(branch != hot.end()); // br x13 to the linked internal entry.
	const auto branch_word =
		static_cast<std::size_t>(branch - hot.begin());
	require(branch_word >= compiled.internal_entry_word);
	/* A successful transition must not reload the external arguments or
	 * unwind the owner frame before branching to its successor. */
	require(std::find(hot.begin(), branch, 0xf940'0be0U) == branch);
	require(std::find(hot.begin(), branch, 0xf940'0fe1U) == branch);
	require(std::find(hot.begin(), branch, 0xf940'13e2U) == branch);
	constexpr std::array external_epilogue{
		0xf900'0274U, // str x20, [x19]
		0xa943'53f3U, // ldp x19, x20, [sp, #48]
		0xf940'17f5U, // ldr x21, [sp, #40]
		0xa8c4'7bfdU, // ldp x29, x30, [sp], #64
		0xd65f'03c0U, // ret
	};
	for (const auto restoration : external_epilogue)
		require(std::find(hot.begin(), branch, restoration) == branch);
	require(std::search(hot.begin(), hot.end(),
			    external_epilogue.begin(),
			    external_epilogue.end()) != hot.end());
	require(uml::cpu::arm64::jit::testing::
			successful_link_host_word_savings() == 14);
}

void link_state_metadata_is_consistent()
{
	constexpr std::uint64_t pc = 0x4fc0'0000;
	constexpr std::array ordinary{0x8b02'0020U}; // add x0, x1, x2
	constexpr std::array vector_only{0x4ea2'8420U}; // add v0.4s,v1.4s,v2.4s
	constexpr std::array native_environment{
		0x1e62'2820U, // fadd d0, d1, d2
	};
	constexpr std::array auxiliary_write{
		0xd51b'4440U, // msr fpmr, x0
	};
	const auto plain =
		uml::cpu::arm64::jit::testing::compile_block(pc, ordinary);
	const auto vector =
		uml::cpu::arm64::jit::testing::compile_block(
			pc + 0x1000, vector_only);
	const auto native =
		uml::cpu::arm64::jit::testing::compile_block(
			pc + 0x2000, native_environment);
	const auto auxiliary =
		uml::cpu::arm64::jit::testing::compile_block(
			pc + 0x3000, auxiliary_write);

	require(!plain.accesses_fp_state && !plain.native_fp &&
		!plain.native_fpmr);
	require(vector.accesses_fp_state && !vector.native_fp &&
		!vector.native_fpmr);
	require(native.accesses_fp_state && native.native_fp);
	const bool native_fp8 = ::uml::platform::apple::probed_host_capabilities().has(
		::uml::cpu::ArmFeature::fp8);
	require(auxiliary.guest_illegal == !native_fp8);
	require(auxiliary.accesses_fp_state == native_fp8);
	require(auxiliary.native_fpmr == native_fp8);
	for (const auto *compiled :
	     std::array{&plain, &vector, &native, &auxiliary})
		require(compiled->accesses_fp_state ||
			(!compiled->native_fp && !compiled->native_fpmr));
}

void link_selector_layout_is_dense_and_compact()
{
	constexpr std::array<std::uint8_t, 10> capability_masks{
		0, 1, 2, 3, 6, 7, 10, 11, 14, 15,
	};
	require(uml::cpu::arm64::jit::testing::
			link_capability_selector_count() ==
		capability_masks.size());
	for (std::size_t selector = 0;
	     selector < capability_masks.size(); ++selector) {
		require(uml::cpu::arm64::jit::testing::
				link_selector_for_capability_mask(
					capability_masks[selector]) ==
			selector);
		require(uml::cpu::arm64::jit::testing::
				link_capability_mask_for_selector(selector) ==
			capability_masks[selector]);
	}
	for (std::uint8_t capabilities = 0; capabilities < 16;
	     ++capabilities) {
		const bool valid = std::find(capability_masks.begin(),
			capability_masks.end(), capabilities) !=
			capability_masks.end();
		if (valid) {
			require(uml::cpu::arm64::jit::testing::
				checked_link_selector_for_capability_mask(
					capabilities) ==
				uml::cpu::arm64::jit::testing::
					link_selector_for_capability_mask(
						capabilities));
			continue;
		}
		require(uml::cpu::arm64::jit::testing::
				link_selector_for_capability_mask(
					capabilities) == 0xffU);
		require(uml::cpu::arm64::jit::testing::
				checked_link_selector_for_capability_mask(
					capabilities) == 0U);
	}
	require(uml::cpu::arm64::jit::testing::
			link_capability_mask_for_selector(
				capability_masks.size()) == 0xffU);
	require(uml::cpu::arm64::jit::testing::block_link_record_size() == 128);
	require(uml::cpu::arm64::jit::testing::
			block_link_record_alignment() == 128);
	require(uml::cpu::arm64::jit::testing::
			block_link_entries_offset() == 16);
	require(uml::cpu::arm64::jit::testing::
			indirect_link_entry_size() == 16);
}

void tracked_translation_has_no_successor_branch()
{
	constexpr std::uint64_t pc = 0x4fc4'0000;
	constexpr std::array guest{0x1400'0001U}; // b pc + 4
	const auto compiled =
		uml::cpu::arm64::jit::testing::compile_tracked_block(pc, guest);
	const auto hot = std::span<const std::uint32_t>{compiled.words}.first(
		compiled.hot_words);
	require(std::find(hot.begin(), hot.end(), 0xd61f'01a0U) == hot.end());
}

} // namespace

int main()
{
	require(jit_available() == 1);
	guest_capabilities_match_host();
	link_selector_layout_is_dense_and_compact();
	require(uml::cpu::arm64::jit::testing::block_link_table_is_bounded(1));
	require(uml::cpu::arm64::jit::testing::block_link_table_is_bounded(7));
	linked_blocks_use_persistent_internal_entry();
	link_state_metadata_is_consistent();
	tracked_translation_has_no_successor_branch();
	guest_key_operations_use_semantic_bridge();
	stateful_system_instructions_are_translation_feature_gated();
	require(uml::cpu::arm64::jit::testing::
		invalid_emitter_metadata_is_rejected_for_testing());
	return 0;
}
