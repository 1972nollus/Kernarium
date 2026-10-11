// SPDX-License-Identifier: GPL-2.0-or-later
/* The SM = 0 vector-length family, the deliberate departure from the
 * architecture that lets a virtual CPU with FEAT_SME and without FEAT_SVE
 * count and scale a vector length of 16 bytes outside streaming mode
 * (cpu/arm64/detail/sm0_vector_length.hpp). This checks the shared
 * definition both engines translate from:
 *
 * - the family is exactly the six forms of the encoding table that the
 *   words name, over the whole 0x04 byte of the SVE space, and no other;
 * - the model the tests state from the pseudocode matches the semantic
 *   library, which executes the same words for a virtual CPU with SVE;
 * - each family word's base A64 word is an ADD, SUB, MOVZ, MOVN or NOP of
 *   the same effect on the general registers; and
 * - the family applies exactly where the virtual CPU has FEAT_SME and not
 *   the SVE profile.
 *
 * The engines' own tests (arm64-gadget-sme, arm64-jit-sme-control and
 * arm64-jit-sme-codegen) and the corpus (arm64-sm0-vl) run the words. */
#include "arm64_sm0_vector_length_support.hpp"

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/detail/sm0_vector_length.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/detail/sve_semantics.hpp"
#include "cpu/arm64/detail/sve_semantics_forms.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"
#include "cpu/host_capabilities.hpp"
#include "platform/apple/host_features.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

using ::uml::platform::ArmFeature;
using ::uml::platform::HostCapabilities;
using ::uml::testing::require;
namespace arm64 = ::uml::cpu::arm64;
namespace detail = ::uml::cpu::arm64::detail;
namespace model = ::uml::emulator::tests::sm0_vector_length;

/* The forms of the encoding table the family is: CNT[BHWD], DEC[BHWD],
 * INC[BHWD], RDVL, ADDVL and ADDPL. */
[[nodiscard]] bool family_form(std::uint16_t form) noexcept
{
	namespace named = detail::sve_semantics::form;
	return form == named::cntb_r_s || form == named::decb_r_rs ||
		form == named::incb_r_rs || form == named::rdvl_r_i ||
		form == named::addvl_r_ri || form == named::addpl_r_ri;
}

[[nodiscard]] bool table_family(std::uint32_t word) noexcept
{
	const auto *row = detail::find_sme_encoding(word);
	return row != nullptr && (row->form & detail::sme_form_sme_section) == 0U &&
		family_form(row->form);
}

void family_is_the_six_forms()
{
	std::size_t rows = 0;
	for (std::size_t index = 0; index < detail::sme_encodings.size(); ++index) {
		const auto &row = detail::sme_encodings[index];
		const bool family = (row.form & detail::sme_form_sme_section) == 0U &&
			family_form(row.form);
		require(detail::sm0_vector_length_word(
				detail::sme_encoding_witnesses[index]) == family);
		rows += family;
	}
	/* CNT, DEC and INC in four sizes each, RDVL, ADDVL and ADDPL. */
	require(rows == 15U);
	/* Every word of the SVE space's 0x04 byte, which holds all of them:
	 * the family is the table's rows and no word more, reserved patterns
	 * and multipliers included. */
	for (std::uint32_t low = 0; low < 0x0100'0000U; ++low) {
		const std::uint32_t word = 0x0400'0000U | low;
		if (detail::sm0_vector_length_word(word) != table_family(word)) {
			std::fprintf(stderr, "%08x: family %d, table %d\n", word,
				detail::sm0_vector_length_word(word), table_family(word));
			require(false);
		}
	}
	/* The word-level definition fits the equivalent function. */
	for (const auto word : model::words())
		require(detail::sm0_vector_length_word(word) &&
			detail::sm0_vector_length_equivalent(word).has_value());
	for (const std::uint32_t word :
	     {0x04bf'5800U /* RDSVL */, 0x0420'5800U /* ADDSVL */,
	      0x0460'5800U /* ADDSPL */, 0x0420'f3e0U /* SQINCB */,
	      0x0430'f3e0U /* SQINCB X */, 0x04a0'f7e0U /* UQINCW */,
	      0x04f0'c3e0U /* INCD vector */, 0x2520'8000U /* CNTP */,
	      0x2528'8800U /* SQINCP */, 0x0420'e3e0U & ~0x0030'0000U,
	      0xd503'201fU, 0x0000'0000U})
		require(!detail::sm0_vector_length_equivalent(word).has_value());
}

/* A tiny interpreter for the base words the family translates to. */
[[nodiscard]] bool run_base_word(std::uint32_t word,
	model::Registers &registers) noexcept
{
	const unsigned rd = word & 31U;
	const unsigned rn = (word >> 5) & 31U;
	if (word == detail::sm0_vector_length_nop)
		return true;
	if ((word & 0xffe0'0000U) == 0xd280'0000U ||
	    (word & 0xffe0'0000U) == 0x9280'0000U) { // MOVZ, MOVN (hw 0)
		const std::uint64_t immediate = (word >> 5) & 0xffffU;
		if (rd != 31U)
			registers.x[rd] = (word & 0x4000'0000U) != 0U ? immediate :
				~immediate;
		return true;
	}
	if ((word & 0xff80'0000U) == 0x9100'0000U ||
	    (word & 0xff80'0000U) == 0xd100'0000U) { // ADD, SUB (immediate)
		require((word & 0x0040'0000U) == 0U); // no shift
		const std::uint64_t immediate = (word >> 10) & 0xfffU;
		const std::uint64_t base = rn == 31U ? registers.sp :
			registers.x[rn];
		const std::uint64_t result = (word & 0x4000'0000U) != 0U ?
			base - immediate : base + immediate;
		if (rd == 31U)
			registers.sp = result;
		else
			registers.x[rd] = result;
		return true;
	}
	return false;
}

[[nodiscard]] model::Registers sample_registers(unsigned seed) noexcept
{
	model::Registers registers;
	for (unsigned index = 0; index < registers.x.size(); ++index)
		registers.x[index] = 0x5a00'0000'0000'0000ULL |
			(std::uint64_t{index} << 32) | (index * 0x1111U + seed);
	registers.sp = 0x7700'1230ULL + seed;
	return registers;
}

[[nodiscard]] bool same(const model::Registers &a,
	const model::Registers &b) noexcept
{
	return a.x == b.x && a.sp == b.sp;
}

/* Each family word's base word has the effect the model states. */
void equivalents_have_the_effect_of_the_family()
{
	std::size_t checked = 0;
	for (const auto word : model::words()) {
		const auto equivalent = detail::sm0_vector_length_equivalent(word);
		require(equivalent.has_value());
		auto expected = sample_registers(1);
		auto actual = expected;
		require(model::model(word, expected));
		if (!run_base_word(*equivalent, actual)) {
			std::fprintf(stderr, "%08x: %08x is not a base word\n", word,
				*equivalent);
			require(false);
		}
		if (!same(expected, actual)) {
			std::fprintf(stderr, "%08x: %08x differs from the model\n", word,
				*equivalent);
			require(false);
		}
		++checked;
	}
	require(checked > 4000U);
}

class NoMemory final : public detail::SveMemory {
public:
	std::byte *translate(std::uint64_t,
		::uml::memory::MemoryAccess) noexcept override
	{
		return nullptr;
	}
	void stored(std::byte *, std::size_t) noexcept override {}
};

/* The semantic library executes the family for a virtual CPU with the SVE
 * profile; the model, and so the base words, are what it computes. */
void model_matches_the_semantic_library()
{
	const detail::SmeFeatureMask features = detail::sme_feature_sve |
		detail::sme_feature_sve2;
	NoMemory memory;
	for (const auto word : model::words()) {
		const auto *row = detail::decode_sve_non_streaming(word, features);
		require(row != nullptr);
		require(detail::sve_semantics_covers(*row));
		const auto start = sample_registers(2);
		arm64::CpuState cpu;
		cpu.registers = start.x;
		cpu.sp = start.sp;
		const auto result = detail::execute_sve(cpu, word, *row, memory);
		require(result.event == detail::SveEvent::retired);
		auto expected = start;
		require(model::model(word, expected));
		model::Registers actual;
		actual.x = cpu.registers;
		actual.sp = cpu.sp;
		if (!same(expected, actual)) {
			std::fprintf(stderr, "%08x: the library differs from the model\n",
				word);
			require(false);
		}
	}
}

[[nodiscard]] HostCapabilities host_with(
	std::initializer_list<ArmFeature> features, unsigned svl)
{
	HostCapabilities host;
	for (const auto feature : features)
		host.supported.set(static_cast<std::size_t>(feature));
	host.sme_svl_bytes = static_cast<std::uint16_t>(svl);
	return host;
}

/* Only a virtual CPU with FEAT_SME and without FEAT_SVE applies the family:
 * with SVE the engines' SVE path executes the words, and without SME they
 * are as undefined as any SVE word. */
void family_applies_with_sme_and_without_sve()
{
	const auto sme = {ArmFeature::sme, ArmFeature::sme_f64f64,
		ArmFeature::sme_i16i64};
	auto sme_only = host_with(sme, 64);
	require(detail::implements_sm0_vector_length(sme_only));
	for (const unsigned svl : {16U, 32U, 128U, 256U})
		require(detail::implements_sm0_vector_length(host_with(sme, svl)));
	auto sme2 = sme_only;
	sme2.supported.set(static_cast<std::size_t>(ArmFeature::sme2));
	require(detail::implements_sm0_vector_length(sme2));

	/* SVE or an SVE token the profile does not complete. */
	auto with_sve = sme_only;
	for (const auto token : arm64::sve_features)
		with_sve.supported.set(static_cast<std::size_t>(token));
	require(arm64::implements_sve(with_sve));
	require(!detail::implements_sm0_vector_length(with_sve));
	auto partial = sme_only;
	partial.supported.set(static_cast<std::size_t>(ArmFeature::sve));
	require(detail::implements_sm0_vector_length(partial));

	/* No SME: a missing token, or a streaming vector length it lacks. */
	require(!detail::implements_sm0_vector_length(HostCapabilities{}));
	require(!detail::implements_sm0_vector_length(host_with(sme, 0)));
	require(!detail::implements_sm0_vector_length(host_with(sme, 48)));
	require(!detail::implements_sm0_vector_length(host_with(
		{ArmFeature::sme}, 64)));
	require(!detail::implements_sm0_vector_length(
		host_with({ArmFeature::sve, ArmFeature::sve2}, 0)));
}

} // namespace

int main()
{
	family_is_the_six_forms();
	equivalents_have_the_effect_of_the_family();
	model_matches_the_semantic_library();
	family_applies_with_sme_and_without_sve();
	return 0;
}
