// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The generated SVE/SME encoding table (cpu/arm64/detail/sme_encodings.hpp)
 * and its decoder. Every row's LLVM MC witness must decode to that row and
 * every FP&SIMD row's witness to its own class, each row's mode must be the
 * one its ASL access checks demand on this profile (FEAT_SME without
 * FEAT_SVE or FEAT_SME_FA64), and a set of words assembled by LLVM MC must
 * decode as the architecture describes them. The generator's freshness test
 * pins the table to the Arm release; this test pins the decoder to the
 * table.
 */
#include "cpu/arm64/detail/sme_decode.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

namespace detail = ::uml::cpu::arm64::detail;
using ::uml::testing::fail;
using ::uml::testing::require;

constexpr detail::SmeFeatureMask all_features = detail::sme_feature_sme |
	detail::sme_feature_sme2 | detail::sme_feature_sme_f64f64 |
	detail::sme_feature_sme_i16i64 | detail::sme_feature_bf16 |
	detail::sme_feature_i8mm;
constexpr detail::SmeCheckMask all_checks = 0x7fU;
constexpr std::uint32_t largest_svl_bytes = 256;

/*
 * What each access check demands of PSTATE on this profile, from the
 * shared pseudocode: without FEAT_SVE, CheckSVEEnabled is
 * CheckStreamingSVEEnabled, which needs PSTATE.SM; the ZA forms need
 * PSTATE.ZA; CheckSMEZT0Enabled needs PSTATE.ZA and names ZT0; and
 * CheckSMEEnabled needs neither. CheckNonStreamingSVEEnabled always traps
 * here, so no implemented row may call it.
 */
constexpr detail::SmeModeMask expected_mode(detail::SmeCheckMask checks)
{
	detail::SmeModeMask mode = 0;
	if ((checks & (detail::sme_check_sve_enabled |
		       detail::sme_check_streaming_sve_enabled |
		       detail::sme_check_streaming_sve_and_za_enabled)) != 0U)
		mode |= detail::sme_mode_streaming;
	if ((checks & (detail::sme_check_streaming_sve_and_za_enabled |
		       detail::sme_check_sme_and_za_enabled |
		       detail::sme_check_sme_zt0_enabled)) != 0U)
		mode |= detail::sme_mode_za;
	if ((checks & detail::sme_check_sme_zt0_enabled) != 0U)
		mode |= detail::sme_mode_zt0;
	return mode;
}

void check_index_shape()
{
	require(detail::sme_encoding_witnesses.size() ==
		detail::sme_encodings.size());
	require(detail::fp_simd_streaming_witnesses.size() ==
		detail::fp_simd_streaming_classes.size());
	require(detail::sme_encoding_buckets.size() ==
		(std::size_t{1} << (32U - detail::sme_encoding_bucket_shift)) + 1U);
	require(detail::sme_encoding_buckets.front() == 0U);
	require(detail::sme_encoding_buckets.back() ==
		detail::sme_encoding_bucket_rows.size());
	require(detail::fp_simd_streaming_buckets.size() ==
		detail::sme_encoding_buckets.size());
	require(detail::fp_simd_streaming_buckets.back() ==
		detail::fp_simd_streaming_bucket_rows.size());
	for (std::size_t index = 1; index < detail::sme_encoding_buckets.size();
	     ++index) {
		require(detail::sme_encoding_buckets[index - 1U] <=
			detail::sme_encoding_buckets[index]);
		require(detail::fp_simd_streaming_buckets[index - 1U] <=
			detail::fp_simd_streaming_buckets[index]);
	}
	for (const std::uint16_t row : detail::sme_encoding_bucket_rows)
		require(row < detail::sme_encodings.size());
	for (const std::uint16_t row : detail::fp_simd_streaming_bucket_rows)
		require(row < detail::fp_simd_streaming_classes.size());
}

/* Every bucket a row's words can reach lists the row. */
template<typename Rows, typename Buckets, typename Slots>
void check_buckets_cover(const Rows &rows, const Buckets &buckets,
	const Slots &slots)
{
	const unsigned shift = detail::sme_encoding_bucket_shift;
	const std::uint32_t width = 32U - shift;
	for (std::size_t index = 0; index < rows.size(); ++index) {
		const std::uint32_t fixed = rows[index].mask >> shift;
		const std::uint32_t base = rows[index].value >> shift;
		for (std::uint32_t key = 0; key < (1U << width); ++key) {
			if ((key & fixed) != base)
				continue;
			bool listed = false;
			for (std::uint32_t slot = buckets[key];
			     slot < buckets[key + 1U]; ++slot)
				listed = listed || slots[slot] == index;
			if (!listed) {
				std::fprintf(stderr, "row %zu missing from bucket %u\n",
					index, key);
				fail("bucket index incomplete");
			}
		}
	}
}

void check_operands(const detail::SmeEncoding &row, std::uint32_t witness)
{
	require(row.operand_begin + row.operand_count <=
		detail::sme_encoding_operands.size());
	const detail::SmeOperand *operands =
		detail::sme_encoding_operands_of(row);
	for (std::uint8_t index = 0; index < row.operand_count; ++index) {
		const detail::SmeOperand &operand = operands[index];
		require(operand.segment_count >= 1U &&
			operand.segment_count <= operand.segments.size());
		unsigned width = 0;
		bool field = false;
		for (std::uint8_t part = 0; part < operand.segment_count; ++part) {
			const detail::SmeSegment &segment = operand.segments[part];
			require(segment.width >= 1U);
			width += segment.width;
			field = field || segment.field;
			if (segment.field)
				require(segment.low + segment.width <= 32U);
			else
				require(segment.constant < (1U << segment.width));
		}
		require(field, "an operand names no field");
		/* Z, V and X registers number 0-31; P registers and ZA tiles
		 * (ZAQ0-ZAQ15 at most) 0-15. */
		unsigned limit = 5;
		if (operand.kind == detail::SmeOperandKind::p ||
		    operand.kind == detail::SmeOperandKind::za_tile)
			limit = 4;
		require(width <= limit, "operand number too wide");
		require(operand.count >= 1U && operand.count <= 4U);
		require(operand.stride == 1U || operand.stride == 4U ||
			operand.stride == 8U);
		require(detail::sme_operand_number(operand, witness) <
			(1U << limit));
	}
}

void check_memory(const detail::SmeEncoding &row, std::uint32_t witness)
{
	const detail::SmeMemory *memory = detail::sme_encoding_memory_of(row);
	if (memory == nullptr)
		return;
	require(row.memory <= detail::sme_encoding_memory.size());
	const detail::SmeOperand *operands =
		detail::sme_encoding_operands_of(row);
	require(memory->base_operand < row.operand_count);
	require(operands[memory->base_operand].kind ==
		detail::SmeOperandKind::gpr_or_sp, "base is not Xn|SP");
	const bool indexed =
		memory->offset == detail::SmeAddressOffset::register_element;
	require(indexed == (memory->index_operand != detail::sme_no_operand));
	if (indexed) {
		require(memory->index_operand < row.operand_count);
		require(operands[memory->index_operand].kind ==
			detail::SmeOperandKind::gpr, "index is not Xm");
	}
	const bool immediate =
		memory->offset == detail::SmeAddressOffset::immediate_footprint ||
		memory->offset == detail::SmeAddressOffset::immediate_element ||
		memory->offset == detail::SmeAddressOffset::immediate_16;
	require(immediate == (memory->immediate_segment_count != 0U));
	require((memory->shape == detail::SmeMemoryShape::prefetch) ==
		(memory->direction == detail::SmeMemoryDirection::prefetch));
	const std::uint32_t esize = detail::sme_memory_esize(*memory, witness);
	require(esize >= 8U && esize <= 128U && (esize & (esize - 1U)) == 0U);
	require(memory->msize >= 8U && memory->msize <= 128U);
	require(memory->registers >= 1U && memory->registers <= 4U);
	const bool za_state = memory->shape == detail::SmeMemoryShape::za_slice ||
		memory->shape == detail::SmeMemoryShape::za_vector ||
		memory->shape == detail::SmeMemoryShape::zt0;
	if (za_state)
		require((row.mode & detail::sme_mode_za) != 0U);
	else
		require((row.mode & detail::sme_mode_streaming) != 0U,
			"a vector access outside streaming mode");
}

void check_rows()
{
	std::size_t implemented = 0;
	for (std::size_t index = 0; index < detail::sme_encodings.size();
	     ++index) {
		const detail::SmeEncoding &row = detail::sme_encodings[index];
		const std::uint32_t witness = detail::sme_encoding_witnesses[index];
		require((witness & row.mask) == row.value, "witness off its row");
		require(detail::sme_encoding_space(witness));
		if (detail::find_sme_encoding(witness) != &row) {
			std::fprintf(stderr, "row %zu witness %08x\n", index,
				witness);
			fail("a witness decodes to another row");
		}
		require((row.form & 0x7fffU) != 0U);
		require((row.checks & ~all_checks) == 0U);
		require((row.requirement & ~all_features) == 0U);
		if (row.requirement == 0U) {
			require(row.checks == 0U && row.mode == 0U &&
				row.operand_count == 0U && row.memory == 0U &&
				row.min_svl_bytes == 0U, "an absent row has detail");
			require(detail::decode_sme(witness, all_features,
					largest_svl_bytes) == nullptr);
			continue;
		}
		++implemented;
		require((row.requirement & detail::sme_feature_sme) != 0U);
		require(row.checks != 0U, "an implemented row has no check");
		require((row.checks & detail::sme_check_non_streaming_sve_enabled) ==
			0U, "an implemented row traps in both modes");
		if (row.mode != expected_mode(row.checks)) {
			std::fprintf(stderr, "row %zu checks %02x mode %02x\n",
				index, row.checks, row.mode);
			fail("mode differs from the access checks");
		}
		if ((row.mode & detail::sme_mode_zt0) != 0U)
			require((row.requirement & detail::sme_feature_sme2) != 0U,
				"ZT0 without FEAT_SME2");
		require(row.min_svl_bytes == 0U || row.min_svl_bytes == 32U ||
			row.min_svl_bytes == 64U || row.min_svl_bytes == 128U ||
			row.min_svl_bytes == 256U);
		const std::uint32_t svl =
			row.min_svl_bytes == 0U ? 16U : row.min_svl_bytes;
		require(detail::decode_sme(witness, row.requirement, svl) == &row);
		require(detail::decode_sme(witness, all_features, svl) == &row);
		if (row.min_svl_bytes != 0U)
			require(detail::decode_sme(witness, all_features,
					row.min_svl_bytes / 2U) == nullptr);
		for (unsigned bit = 0; bit < 8U; ++bit) {
			const auto feature =
				static_cast<detail::SmeFeatureMask>(1U << bit);
			if ((row.requirement & feature) == 0U)
				continue;
			const auto others =
				static_cast<detail::SmeFeatureMask>(all_features & ~feature);
			require(detail::decode_sme(witness, others,
					largest_svl_bytes) == nullptr);
		}
		check_operands(row, witness);
		check_memory(row, witness);
	}
	std::printf("sme encoding table: %zu rows, %zu implemented\n",
		detail::sme_encodings.size(), implemented);
}

void check_fp_simd_rows()
{
	std::size_t advsimd = 0;
	for (std::size_t index = 0;
	     index < detail::fp_simd_streaming_classes.size(); ++index) {
		const detail::FpSimdStreamingClass &row =
			detail::fp_simd_streaming_classes[index];
		const std::uint32_t witness =
			detail::fp_simd_streaming_witnesses[index];
		require((witness & row.mask) == row.value, "witness off its row");
		require(!detail::sme_encoding_space(witness));
		const std::uint32_t bucket =
			witness >> detail::sme_encoding_bucket_shift;
		std::size_t first = detail::fp_simd_streaming_classes.size();
		for (std::uint32_t slot = detail::fp_simd_streaming_buckets[bucket];
		     slot < detail::fp_simd_streaming_buckets[bucket + 1U]; ++slot) {
			const std::uint16_t candidate =
				detail::fp_simd_streaming_bucket_rows[slot];
			const detail::FpSimdStreamingClass &other =
				detail::fp_simd_streaming_classes[candidate];
			if ((witness & other.mask) == other.value) {
				first = candidate;
				break;
			}
		}
		if (first != index) {
			std::fprintf(stderr, "FP&SIMD row %zu witness %08x\n",
				index, witness);
			fail("an FP&SIMD witness decodes to another row");
		}
		require(detail::classify_fp_simd_streaming(witness) ==
			(row.advsimd ? detail::FpSimdStreaming::advsimd
				     : detail::FpSimdStreaming::fp));
		advsimd += row.advsimd ? 1U : 0U;
	}
	std::printf("FP&SIMD streaming classes: %zu rows, %zu AdvSIMD\n",
		detail::fp_simd_streaming_classes.size(), advsimd);
}

struct Expected final {
	const char *text;
	std::uint32_t word;
	detail::SmeFeatureMask requirement;
	detail::SmeModeMask mode;
};

/* Words LLVM MC 22.1.8 assembles, with what the architecture says of them. */
constexpr Expected expected_words[] = {
	{"add z0.s, z1.s, z2.s", 0x04a2'0020U, detail::sme_feature_sme,
		detail::sme_mode_streaming},
	{"addvl x0, x0, #1", 0x0420'5020U, detail::sme_feature_sme,
		detail::sme_mode_streaming},
	{"addsvl x0, x0, #1", 0x0420'5820U, detail::sme_feature_sme, 0},
	{"rdsvl x0, #1", 0x04bf'5820U, detail::sme_feature_sme, 0},
	{"fmopa za0.s, p0/m, p1/m, z0.s, z1.s", 0x8081'2000U,
		detail::sme_feature_sme,
		detail::sme_mode_streaming | detail::sme_mode_za},
	{"fmopa za0.d, p0/m, p1/m, z0.d, z1.d", 0x80c1'2000U,
		detail::sme_feature_sme | detail::sme_feature_sme_f64f64,
		detail::sme_mode_streaming | detail::sme_mode_za},
	{"smopa za0.d, p0/m, p1/m, z0.h, z1.h", 0xa0c1'2000U,
		detail::sme_feature_sme | detail::sme_feature_sme_i16i64,
		detail::sme_mode_streaming | detail::sme_mode_za},
	{"ldr za[w12, 0], [x0]", 0xe100'0000U, detail::sme_feature_sme,
		detail::sme_mode_za},
	{"zero {za}", 0xc008'00ffU, detail::sme_feature_sme,
		detail::sme_mode_za},
	{"ldr zt0, [x0]", 0xe11f'8000U,
		detail::sme_feature_sme | detail::sme_feature_sme2,
		detail::sme_mode_za | detail::sme_mode_zt0},
	{"ld1w {z0.s, z8.s}, pn8/z, [x0]", 0xa140'4000U,
		detail::sme_feature_sme | detail::sme_feature_sme2,
		detail::sme_mode_streaming},
	{"ld1d {z0.d}, p0/z, [x0, z1.d, lsl #3]", 0xc5e1'c000U, 0, 0},
};

void check_expected_words()
{
	for (const Expected &expected : expected_words) {
		const detail::SmeEncoding *row =
			detail::find_sme_encoding(expected.word);
		if (row == nullptr || row->requirement != expected.requirement ||
		    row->mode != expected.mode) {
			std::fprintf(stderr, "%s\n", expected.text);
			fail("a known word decodes unexpectedly");
		}
	}

	/* An SVE gather: in the space and the table, never implemented. */
	require(detail::decode_sme(0xc5e1'c000U, all_features,
			largest_svl_bytes) == nullptr);
	/* SMSTART is a Base MSR (immediate) form, outside the space. */
	require(!detail::sme_encoding_space(0xd503'477fU));

	/* LD1B {z0.b}, p0/z, [x0, #1, mul vl]: contiguous, one vector of bytes. */
	const detail::SmeEncoding *ld1b = detail::find_sme_encoding(0xa401'a000U);
	require(ld1b != nullptr);
	const detail::SmeMemory *memory = detail::sme_encoding_memory_of(*ld1b);
	require(memory != nullptr &&
		memory->shape == detail::SmeMemoryShape::contiguous &&
		memory->direction == detail::SmeMemoryDirection::load &&
		memory->offset == detail::SmeAddressOffset::immediate_footprint &&
		detail::sme_memory_esize(*memory, 0xa401'a000U) == 8U &&
		memory->msize == 8U && memory->registers == 1U &&
		memory->immediate_signed && memory->immediate_segment_count == 1U &&
		detail::sme_segment_value(memory->immediate[0], 0xa401'a000U) == 1U);

	/* ST1W {z0.s}, p0, [x0, x1, lsl #2]: the element size is a field. */
	const detail::SmeEncoding *st1w = detail::find_sme_encoding(0xe541'4000U);
	require(st1w != nullptr);
	memory = detail::sme_encoding_memory_of(*st1w);
	require(memory != nullptr &&
		memory->direction == detail::SmeMemoryDirection::store &&
		memory->offset == detail::SmeAddressOffset::register_element &&
		memory->esize_shift.width == 1U &&
		detail::sme_memory_esize(*memory, 0xe541'4000U) == 32U &&
		detail::sme_memory_esize(*memory, 0xe561'4000U) == 64U &&
		memory->msize == 32U);
	const detail::SmeOperand *operands =
		detail::sme_encoding_operands_of(*st1w);
	require(detail::sme_operand_number(operands[memory->index_operand],
			0xe541'4000U) == 1U);

	/* LD1W {z0.s, z8.s}: a strided pair. */
	const detail::SmeEncoding *strided =
		detail::find_sme_encoding(0xa140'4000U);
	require(strided != nullptr);
	memory = detail::sme_encoding_memory_of(*strided);
	require(memory != nullptr &&
		memory->shape == detail::SmeMemoryShape::strided &&
		memory->registers == 2U && memory->stride == 8U);
	bool pair = false;
	operands = detail::sme_encoding_operands_of(*strided);
	for (std::uint8_t index = 0; index < strided->operand_count; ++index)
		pair = pair || (operands[index].kind == detail::SmeOperandKind::z &&
			operands[index].count == 2U && operands[index].stride == 8U);
	require(pair, "the strided pair has no two-register operand");

	/* LDR ZT0 and PRFB. */
	memory = detail::sme_encoding_memory_of(
		*detail::find_sme_encoding(0xe11f'8000U));
	require(memory != nullptr && memory->shape == detail::SmeMemoryShape::zt0);
	memory = detail::sme_encoding_memory_of(
		*detail::find_sme_encoding(0x85c0'0000U));
	require(memory != nullptr &&
		memory->shape == detail::SmeMemoryShape::prefetch);

	/* UZP { z0.q-z3.q }: allocated only with an SVL of 512 bits or more. */
	require(detail::decode_sme(0xc137'e082U, all_features, 64U) != nullptr);
	require(detail::decode_sme(0xc137'e082U, all_features, 32U) == nullptr);
	require(detail::sme_mode_permits(
		*detail::find_sme_encoding(0xc137'e082U), true, false));
	require(!detail::sme_mode_permits(
		*detail::find_sme_encoding(0xc137'e082U), false, true));
	require(!detail::sme_mode_permits(
		*detail::find_sme_encoding(0xe11f'8000U), true, false));

	struct Class final {
		const char *text;
		std::uint32_t word;
		detail::FpSimdStreaming expected;
	};
	constexpr Class classes[] = {
		{"fadd s0, s1, s2", 0x1e22'2820U, detail::FpSimdStreaming::fp},
		{"add v0.4s, v1.4s, v2.4s", 0x4ea2'8420U,
			detail::FpSimdStreaming::advsimd},
		{"ldr q0, [x0]", 0x3dc0'0000U, detail::FpSimdStreaming::fp},
		{"ld1 {v0.16b}, [x0]", 0x4c40'7000U,
			detail::FpSimdStreaming::advsimd},
		{"frecpe s0, s1", 0x5ea1'd820U, detail::FpSimdStreaming::fp},
		{"frecpe v0.4s, v1.4s", 0x4ea1'd820U,
			detail::FpSimdStreaming::advsimd},
		{"umov w0, v1.s[0]", 0x0e04'3c20U, detail::FpSimdStreaming::fp},
		{"umov w0, v1.s[1]", 0x0e0c'3c20U,
			detail::FpSimdStreaming::advsimd},
		{"fmov x0, d1", 0x9e66'0020U, detail::FpSimdStreaming::fp},
		{"fcvtzs s0, s1", 0x5ea1'b820U, detail::FpSimdStreaming::advsimd},
		{"fcvtzs w0, s1", 0x1e38'0020U, detail::FpSimdStreaming::fp},
	};
	for (const Class &item : classes) {
		if (detail::classify_fp_simd_streaming(item.word) != item.expected) {
			std::fprintf(stderr, "%s\n", item.text);
			fail("a known FP&SIMD word has the wrong class");
		}
	}
	/* A Base word has no FP&SIMD class. */
	require(detail::classify_fp_simd_streaming(0x8b02'0020U) ==
		detail::FpSimdStreaming::unclassified);
}

} // namespace

int main()
{
	check_index_shape();
	check_buckets_cover(detail::sme_encodings, detail::sme_encoding_buckets,
		detail::sme_encoding_bucket_rows);
	check_buckets_cover(detail::fp_simd_streaming_classes,
		detail::fp_simd_streaming_buckets,
		detail::fp_simd_streaming_bucket_rows);
	check_rows();
	check_fp_simd_rows();
	check_expected_words();
	return 0;
}
