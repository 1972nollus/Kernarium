/* SPDX-License-Identifier: GPL-2.0-or-later */
/* x86 SIMD floating-point exactness. Each cell runs one register-form
 * instruction through the gadget engine, in a guest loop over a deterministic
 * operand stream, under one MXCSR (DAZ, FTZ and rounding control), and
 * compares every result lane with an x86 model built from correctly rounded
 * host arithmetic and the x86 rules the host does not share: first-source
 * NaN priority, the negative indefinite NaN, DAZ input and FTZ output
 * flushes with underflow detected after rounding, MIN and MAX returning the
 * second source, RCP and RSQRT treating denormal inputs as zero, and FMA's
 * operand order. The model's rules were checked against an Intel host. The
 * stream mixes uniform values, special values and exponents near the
 * underflow threshold. `--iterations N` sets the loop count per cell; the
 * default keeps the test short. */
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "memory/guest_address_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using uml::cpu::x86_64::CpuState;
using uml::cpu::x86_64::ExecutionMode;
using uml::cpu::x86_64::Register;

/* One flat mapping from address zero: code, stack, inputs and outputs. */
class FlatMemory final : public uml::memory::GuestAddressSpace {
public:
	explicit FlatMemory(std::size_t size) : bytes_(size) {}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		if (!inside(address, destination.size()))
			return fault(address, uml::memory::MemoryAccess::read);
		std::memcpy(destination.data(), bytes_.data() + address,
			destination.size());
		return true;
	}

	[[nodiscard]] bool fetch(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		if (!inside(address, destination.size()))
			return fault(address, uml::memory::MemoryAccess::execute);
		std::memcpy(destination.data(), bytes_.data() + address,
			destination.size());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) override
	{
		if (!inside(address, source.size()))
			return fault(address, uml::memory::MemoryAccess::write);
		std::memcpy(bytes_.data() + address, source.data(), source.size());
		return true;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, uml::memory::MemoryAccess access) override
	{
		if (!inside(address, size)) {
			(void)fault(address, access);
			return nullptr;
		}
		return bytes_.data() + address;
	}

	[[nodiscard]] uml::memory::MemoryFault last_fault() const noexcept override
	{
		return fault_;
	}

	[[nodiscard]] bool has_fault() const noexcept override { return faulted_; }

	[[nodiscard]] std::byte *at(std::uint64_t address) noexcept
	{
		return bytes_.data() + address;
	}

private:
	[[nodiscard]] bool inside(std::uint64_t address,
		std::size_t size) const noexcept
	{
		return address <= bytes_.size() && size <= bytes_.size() - address;
	}

	[[nodiscard]] bool fault(std::uint64_t address,
		uml::memory::MemoryAccess access) noexcept
	{
		fault_ = {address, access};
		faulted_ = true;
		return false;
	}

	std::vector<std::byte> bytes_;
	uml::memory::MemoryFault fault_{};
	bool faulted_{};
};

/* ---- host arithmetic: one correctly rounded instruction under the live
 * FPCR, whose rounding mode is the cell's and whose FZ, DN and AH are clear.
 */

class HostRounding final {
public:
	/* MXCSR numbers the directed modes the other way round from FPCR. */
	explicit HostRounding(unsigned rounding_control) noexcept
	{
		__asm__ volatile("mrs %0, fpcr" : "=r"(saved_));
		static constexpr std::array<std::uint64_t, 4> rmode{0, 2, 1, 3};
		const std::uint64_t fpcr = rmode[rounding_control & 3U] << 22U;
		__asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
	}
	~HostRounding() { __asm__ volatile("msr fpcr, %0" :: "r"(saved_)); }
	HostRounding(const HostRounding &) = delete;
	HostRounding &operator=(const HostRounding &) = delete;

private:
	std::uint64_t saved_{};
};

enum class Op : std::uint8_t { add, sub, mul, div, sqrt, min, max, rcp, rsqrt };

[[nodiscard]] std::uint64_t host32(Op op, std::uint32_t a, std::uint32_t b)
{
	std::uint32_t r{};
	switch (op) {
	case Op::add:
		__asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fadd s2, s0, s1\n"
			" fmov %w0, s2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	case Op::sub:
		__asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fsub s2, s0, s1\n"
			" fmov %w0, s2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	case Op::mul:
		__asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fmul s2, s0, s1\n"
			" fmov %w0, s2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	case Op::div:
		__asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fdiv s2, s0, s1\n"
			" fmov %w0, s2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	default:
		__asm__ volatile("fmov s1, %w1\n fsqrt s2, s1\n fmov %w0, s2"
			: "=r"(r) : "r"(b) : "v1", "v2");
		break;
	}
	return r;
}

[[nodiscard]] std::uint64_t host64(Op op, std::uint64_t a, std::uint64_t b)
{
	std::uint64_t r{};
	switch (op) {
	case Op::add:
		__asm__ volatile("fmov d0, %1\n fmov d1, %2\n fadd d2, d0, d1\n"
			" fmov %0, d2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	case Op::sub:
		__asm__ volatile("fmov d0, %1\n fmov d1, %2\n fsub d2, d0, d1\n"
			" fmov %0, d2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	case Op::mul:
		__asm__ volatile("fmov d0, %1\n fmov d1, %2\n fmul d2, d0, d1\n"
			" fmov %0, d2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	case Op::div:
		__asm__ volatile("fmov d0, %1\n fmov d1, %2\n fdiv d2, d0, d1\n"
			" fmov %0, d2" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "v2");
		break;
	default:
		__asm__ volatile("fmov d1, %1\n fsqrt d2, d1\n fmov %0, d2"
			: "=r"(r) : "r"(b) : "v1", "v2");
		break;
	}
	return r;
}

/* Ordered a < b. */
[[nodiscard]] bool host_less(bool dbl, std::uint64_t a, std::uint64_t b)
{
	std::uint32_t r{};
	if (dbl)
		__asm__ volatile("fmov d0, %1\n fmov d1, %2\n fcmp d0, d1\n"
			" cset %w0, mi" : "=r"(r) : "r"(a), "r"(b) : "v0", "v1", "cc");
	else
		__asm__ volatile("fmov s0, %w1\n fmov s1, %w2\n fcmp s0, s1\n"
			" cset %w0, mi" : "=r"(r)
			: "r"(static_cast<std::uint32_t>(a)),
			  "r"(static_cast<std::uint32_t>(b)) : "v0", "v1", "cc");
	return r != 0;
}

[[nodiscard]] std::uint64_t host(bool dbl, Op op, std::uint64_t a,
	std::uint64_t b)
{
	return dbl ? host64(op, a, b) :
		host32(op, static_cast<std::uint32_t>(a),
			static_cast<std::uint32_t>(b));
}

/* ---- the x86 model ---- */

struct Format final {
	bool dbl;
	[[nodiscard]] std::uint64_t sign() const noexcept
	{
		return dbl ? 0x8000'0000'0000'0000ULL : 0x8000'0000ULL;
	}
	[[nodiscard]] std::uint64_t magnitude() const noexcept
	{
		return dbl ? 0x7fff'ffff'ffff'ffffULL : 0x7fff'ffffULL;
	}
	[[nodiscard]] std::uint64_t exponent() const noexcept
	{
		return dbl ? 0x7ff0'0000'0000'0000ULL : 0x7f80'0000ULL;
	}
	[[nodiscard]] std::uint64_t quiet_bit() const noexcept
	{
		return dbl ? 0x0008'0000'0000'0000ULL : 0x0040'0000ULL;
	}
	[[nodiscard]] std::uint64_t minimum_normal() const noexcept
	{
		return dbl ? 0x0010'0000'0000'0000ULL : 0x0080'0000ULL;
	}
	[[nodiscard]] std::uint64_t indefinite() const noexcept
	{
		return sign() | exponent() | quiet_bit();
	}
	[[nodiscard]] bool nan(std::uint64_t x) const noexcept
	{
		return (x & magnitude()) > exponent();
	}
	[[nodiscard]] bool denormal(std::uint64_t x) const noexcept
	{
		return (x & exponent()) == 0 && (x & magnitude()) != 0;
	}
	[[nodiscard]] std::uint64_t flush(std::uint64_t x) const noexcept
	{
		return denormal(x) ? (x & sign()) : x;
	}
	[[nodiscard]] std::uint64_t quiet(std::uint64_t x) const noexcept
	{
		return x | quiet_bit();
	}
};

/* x86 detects underflow after rounding: a result is tiny when, rounded to
 * full precision with an unbounded exponent, it lies below the smallest
 * normal. A denormal result surely is; a product or quotient the host
 * rounded up to exactly the smallest normal is when the same operation,
 * scaled into the normal range, stays below its scaled threshold. Sums and
 * differences that small are exact. */
[[nodiscard]] bool tiny(Format f, Op op, std::uint64_t a, std::uint64_t b,
	std::uint64_t result)
{
	const auto magnitude = result & f.magnitude();
	if (f.denormal(result))
		return true;
	if (magnitude != f.minimum_normal() || (op != Op::mul && op != Op::div))
		return false;
	const int k = f.dbl ? 512 : 64;
	const std::uint64_t scale = f.dbl ?
		static_cast<std::uint64_t>(1023 + k) << 52U :
		static_cast<std::uint64_t>(127 + k) << 23U;
	const auto scaled = host(f.dbl, Op::mul, a, scale);
	const auto rescaled = host(f.dbl, op, scaled, b) & f.magnitude();
	const std::uint64_t threshold = f.dbl ?
		static_cast<std::uint64_t>(1 + k) << 52U :
		static_cast<std::uint64_t>(1 + k) << 23U;
	return rescaled < threshold;
}

/* One lane of an SSE arithmetic, minimum or maximum, square root,
 * reciprocal or reciprocal square root. `a` is the first source, `b` the
 * second (the only one of the unary operations). */
[[nodiscard]] std::uint64_t x86_lane(Format f, Op op, std::uint64_t a,
	std::uint64_t b, bool daz, bool ftz)
{
	if (op == Op::rcp || op == Op::rsqrt) {
		/* MXCSR does not govern them: a denormal input is zero, and a
		 * reciprocal of 2^126 or more is zero too. */
		b = f.flush(b);
		if (f.nan(b))
			return f.quiet(b);
		if (op == Op::rcp) {
			const auto magnitude = b & f.magnitude();
			if (magnitude >= 0x7e80'0000ULL && magnitude <= 0x7f80'0000ULL)
				return b & f.sign();
			return host(f.dbl, Op::div, f.dbl ?
				0x3ff0'0000'0000'0000ULL : 0x3f80'0000ULL, b);
		}
		if ((b & f.sign()) != 0 && (b & f.magnitude()) != 0)
			return f.indefinite();
		const auto root = host(f.dbl, Op::sqrt, 0, b);
		return host(f.dbl, Op::div, f.dbl ?
			0x3ff0'0000'0000'0000ULL : 0x3f80'0000ULL, root);
	}
	if (daz) {
		a = f.flush(a);
		b = f.flush(b);
	}
	if (op == Op::min || op == Op::max) {
		if (f.nan(a) || f.nan(b))
			return b;
		const bool take_first = op == Op::min ? host_less(f.dbl, a, b) :
			host_less(f.dbl, b, a);
		return take_first ? a : b;
	}
	if (op == Op::sqrt) {
		if (f.nan(b))
			return f.quiet(b);
	} else if (f.nan(a) || f.nan(b)) {
		return f.nan(a) ? f.quiet(a) : f.quiet(b);
	}
	const auto result = host(f.dbl, op, a, b);
	if (f.nan(result))
		return f.indefinite();
	if (ftz && tiny(f, op, a, b, result))
		return result & f.sign();
	return result;
}

/* ---- operand stream (X-AFP's oracle generator) ---- */

class Operands final {
public:
	[[nodiscard]] std::uint64_t next() noexcept
	{
		state_ ^= state_ << 13U;
		state_ ^= state_ >> 7U;
		state_ ^= state_ << 17U;
		return state_;
	}

	[[nodiscard]] std::uint64_t pick(bool dbl) noexcept
	{
		static constexpr std::array<std::uint32_t, 28> singles{
			0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff,
			0x00400000, 0x00800000, 0x80800000, 0x00800001, 0x3f800000,
			0xbf800000, 0x3fc00000, 0x7f7fffff, 0xff7fffff, 0x7f800000,
			0xff800000, 0x7fc00000, 0x7fc12345, 0xffc00000, 0xffc54321,
			0x7f800001, 0x7fa00000, 0xff800001, 0xffbfffff, 0x1f800000,
			0x20000000, 0x5f800000};
		static constexpr std::array<std::uint64_t, 28> doubles{
			0, 0x8000000000000000, 1, 0x8000000000000001,
			0x000fffffffffffff, 0x800fffffffffffff, 0x0008000000000000,
			0x0010000000000000, 0x8010000000000000, 0x0010000000000001,
			0x3ff0000000000000, 0xbff0000000000000, 0x3ff8000000000000,
			0x7fefffffffffffff, 0xffefffffffffffff, 0x7ff0000000000000,
			0xfff0000000000000, 0x7ff8000000000000, 0x7ff8000000012345,
			0xfff8000000000000, 0xfff8000000054321, 0x7ff0000000000001,
			0x7ff4000000000000, 0xfff0000000000001, 0xfff7ffffffffffff,
			0x1ff0000000000000, 0x2000000000000000, 0x5ff0000000000000};
		const auto r = next();
		switch (r & 7U) {
		case 0:
		case 1:
			return dbl ? doubles[(r >> 8U) % doubles.size()] :
				singles[(r >> 8U) % singles.size()];
		case 2: {
			/* An exponent near the bottom or the top. */
			const auto e = (r >> 8U) & 7U;
			const auto m = next();
			if (dbl) {
				static constexpr std::array<std::uint64_t, 8> exponents{
					0, 1, 2, 3, 0x3fe, 0x7fd, 0x7fe, 0x7ff};
				return ((m & 1U) << 63U) | (exponents[e] << 52U) |
					(m >> 12U);
			}
			static constexpr std::array<std::uint64_t, 8> exponents{
				0, 1, 2, 3, 0x7e, 0xfd, 0xfe, 0xff};
			return ((m & 1U) << 31U) | (exponents[e] << 23U) |
				((m >> 9U) & 0x7fffffU);
		}
		case 3: {
			/* Around the square root of the smallest normal, so that
			 * products and quotients land near the threshold. */
			const auto m = next();
			if (dbl) {
				const std::uint64_t e = 0x1ff + (r >> 8U) % 5U - 2U;
				return ((m & 1U) << 63U) | (e << 52U) | (m >> 12U);
			}
			const std::uint64_t e = 0x3f + (r >> 8U) % 5U - 2U;
			return ((m & 1U) << 31U) | (e << 23U) | ((m >> 9U) & 0x7fffffU);
		}
		default:
			return dbl ? next() : (next() & 0xffffffffU);
		}
	}

	/* A first and second source whose product or quotient lies within a
	 * few units in the last place of the smallest normal, where rounding
	 * decides whether the result is tiny. */
	[[nodiscard]] std::array<std::uint64_t, 2> threshold_pair(bool dbl,
		Op op) noexcept
	{
		const Format f{dbl};
		const auto r = next();
		const auto m = next();
		/* A normal value in [2^-16, 1) for a product's first source, or in
		 * [1, 2^16) for a quotient's divisor. */
		const std::uint64_t shift = (r & 15U) + (op == Op::mul ? 1U : 0U);
		const std::uint64_t value = dbl ?
			((0x3ffULL - (op == Op::mul ? shift : 0U) +
				(op == Op::mul ? 0U : shift)) << 52U) | (m >> 12U) :
			((0x7fULL - (op == Op::mul ? shift : 0U) +
				(op == Op::mul ? 0U : shift)) << 23U) | ((m >> 9U) & 0x7fffffU);
		std::uint64_t first = value;
		std::uint64_t second = value;
		if (op == Op::mul)
			second = host(dbl, Op::div, f.minimum_normal(), value);
		else
			first = host(dbl, Op::mul, f.minimum_normal(), value);
		auto &moved = op == Op::mul ? second : first;
		moved = moved + ((r >> 8U) % 7U) - 3U;
		if ((r & 0x10000U) != 0)
			first |= f.sign();
		if ((r & 0x20000U) != 0)
			second |= f.sign();
		return {first, second};
	}

private:
	std::uint64_t state_{0x9e3779b97f4a7c15ULL};
};

/* ---- instructions ---- */

enum class Shape : std::uint8_t {
	sse_scalar,  /* xmm0 = xmm0 op xmm1, lane 0, upper lanes kept */
	sse_packed,  /* xmm0 = xmm0 op xmm1, ymm0's upper half kept */
	vex256,      /* ymm0 = ymm1 op ymm2 */
};

struct Instruction final {
	std::string_view name;
	Op op;
	bool dbl;
	Shape shape;
	std::vector<std::uint8_t> bytes;
};

[[nodiscard]] std::vector<Instruction> instructions()
{
	struct Family {
		std::string_view name;
		Op op;
		std::uint8_t opcode;
		bool packed_only;
		bool single_only;
	};
	static constexpr std::array<Family, 9> families{{
		{"add", Op::add, 0x58, false, false},
		{"sub", Op::sub, 0x5c, false, false},
		{"mul", Op::mul, 0x59, false, false},
		{"div", Op::div, 0x5e, false, false},
		{"sqrt", Op::sqrt, 0x51, false, false},
		{"min", Op::min, 0x5d, false, false},
		{"max", Op::max, 0x5f, false, false},
		{"rcp", Op::rcp, 0x53, false, true},
		{"rsqrt", Op::rsqrt, 0x52, false, true},
	}};
	std::vector<Instruction> list;
	for (const auto &family : families) {
		const bool unary = family.op == Op::sqrt || family.op == Op::rcp ||
			family.op == Op::rsqrt;
		for (const bool dbl : {false, true}) {
			if (dbl && family.single_only)
				continue;
			const std::uint8_t packed_prefix = dbl ? 0x66 : 0;
			const std::uint8_t scalar_prefix = dbl ? 0xf2 : 0xf3;
			std::vector<std::uint8_t> scalar{scalar_prefix, 0x0f,
				family.opcode, 0xc1};
			std::vector<std::uint8_t> packed;
			if (packed_prefix != 0)
				packed.push_back(packed_prefix);
			packed.insert(packed.end(), {0x0f, family.opcode, 0xc1});
			/* VEX.256: vvvv names ymm1, or none for a unary operation;
			 * pp is 00 for singles and 01 (66) for doubles. */
			const std::uint8_t vvvv = unary ? 0x0f : 0x0e;
			const std::uint8_t vex = static_cast<std::uint8_t>(0x80U |
				(vvvv << 3U) | 0x04U | (dbl ? 1U : 0U));
			std::vector<std::uint8_t> wide{0xc5, vex, family.opcode, 0xc2};
			list.push_back({family.name, family.op, dbl, Shape::sse_scalar,
				scalar});
			list.push_back({family.name, family.op, dbl, Shape::sse_packed,
				packed});
			list.push_back({family.name, family.op, dbl, Shape::vex256,
				wide});
		}
	}
	return list;
}

[[nodiscard]] const char *shape_name(Shape shape) noexcept
{
	switch (shape) {
	case Shape::sse_scalar: return "scalar";
	case Shape::sse_packed: return "packed";
	case Shape::vex256: return "vex256";
	}
	return "?";
}

/* ---- the guest loop ---- */

constexpr std::uint64_t code_address = 0x1000;
constexpr std::uint64_t stack_top = 0x8000;
constexpr std::uint64_t input_address = 0x10000;
constexpr std::size_t input_stride = 96;
constexpr std::size_t output_stride = 48;
constexpr std::size_t chunk = 16384;
constexpr std::uint64_t output_address = input_address + chunk * input_stride;
constexpr std::size_t memory_size = output_address + chunk * output_stride;

/* Loads ymm0, ymm1 and ymm2, runs the instruction, and stores ymm0, RAX
 * and RFLAGS, `rcx` times. */
[[nodiscard]] std::vector<std::byte> loop_code(
	std::span<const std::uint8_t> instruction)
{
	std::vector<std::uint8_t> code{
		0xc5, 0xfe, 0x6f, 0x06,             /* vmovdqu ymm0, [rsi] */
		0xc5, 0xfe, 0x6f, 0x4e, 0x20,       /* vmovdqu ymm1, [rsi+32] */
		0xc5, 0xfe, 0x6f, 0x56, 0x40,       /* vmovdqu ymm2, [rsi+64] */
	};
	code.insert(code.end(), instruction.begin(), instruction.end());
	code.insert(code.end(), {
		0x9c, 0x5a,                         /* pushfq; pop rdx */
		0xc5, 0xfe, 0x7f, 0x07,             /* vmovdqu [rdi], ymm0 */
		0x48, 0x89, 0x47, 0x20,             /* mov [rdi+32], rax */
		0x48, 0x89, 0x57, 0x28,             /* mov [rdi+40], rdx */
		0x48, 0x83, 0xc6, 0x60,             /* add rsi, 96 */
		0x48, 0x83, 0xc7, 0x30,             /* add rdi, 48 */
		0xff, 0xc9,                         /* dec ecx */
	});
	const auto back = -static_cast<int>(code.size() + 2U);
	code.push_back(0x75);                   /* jnz loop */
	code.push_back(static_cast<std::uint8_t>(back));
	code.insert(code.end(), {0x0f, 0x05}); /* syscall */
	std::vector<std::byte> bytes(code.size());
	std::memcpy(bytes.data(), code.data(), code.size());
	return bytes;
}

[[nodiscard]] bool run_engine(FlatMemory &memory,
	uml::cpu::x86_64::gadgets::Executor &executor, std::uint32_t mxcsr,
	std::size_t count)
{
	using namespace uml::cpu::x86_64;
	CpuState cpu{};
	cpu.mxcsr = mxcsr;
	cpu.rip = code_address;
	cpu.gpr(Register::rsp) = stack_top;
	cpu.gpr(Register::rsi) = input_address;
	cpu.gpr(Register::rdi) = output_address;
	cpu.gpr(Register::rcx) = count;
	for (std::size_t dispatch = 0; dispatch < count * 16U + 64U;
		++dispatch) {
		const auto selected = executor.select_program(memory, cpu.rip,
			ExecutionMode::long64);
		if (selected.status != gadgets::BuildStatus::built ||
			selected.program == nullptr)
			return false;
		const auto result = gadgets::execute(*selected.program, cpu,
			memory, executor, 1U << 20U);
		/* SYSCALL leaves its return address in RCX. */
		if (result.event == uml::cpu::EngineEvent::system_call)
			return cpu.gpr(Register::rsi) ==
				input_address + count * input_stride;
		if (result.event != uml::cpu::EngineEvent::continued)
			return false;
	}
	return false;
}

struct Lanes final {
	std::array<std::uint64_t, 8> value{};
	std::size_t count{};
};

[[nodiscard]] std::uint64_t lane(const std::byte *bytes, bool dbl,
	std::size_t index) noexcept
{
	if (dbl) {
		std::uint64_t value{};
		std::memcpy(&value, bytes + index * 8U, 8U);
		return value;
	}
	std::uint32_t value{};
	std::memcpy(&value, bytes + index * 4U, 4U);
	return value;
}

void set_lane(std::byte *bytes, bool dbl, std::size_t index,
	std::uint64_t value) noexcept
{
	if (dbl) {
		std::memcpy(bytes + index * 8U, &value, 8U);
		return;
	}
	const auto narrow = static_cast<std::uint32_t>(value);
	std::memcpy(bytes + index * 4U, &narrow, 4U);
}

/* The model's ymm0 for one iteration. */
void model_result(const Instruction &instruction, const std::byte *input,
	bool daz, bool ftz, std::byte *output)
{
	const Format f{instruction.dbl};
	const std::size_t lanes = instruction.dbl ? 4U : 8U;
	const auto *first = instruction.shape == Shape::vex256 ?
		input + 32 : input;
	const auto *second = instruction.shape == Shape::vex256 ?
		input + 64 : input + 32;
	std::memcpy(output, input, 32);
	const std::size_t computed = instruction.shape == Shape::sse_scalar ? 1U :
		instruction.shape == Shape::sse_packed ? lanes / 2U : lanes;
	for (std::size_t index = 0; index < computed; ++index)
		set_lane(output, instruction.dbl, index,
			x86_lane(f, instruction.op, lane(first, f.dbl, index),
				lane(second, f.dbl, index), daz, ftz));
}

struct Totals final {
	std::uint64_t lanes{};
	std::uint64_t differing{};
	std::uint64_t kept_differing{};
	bool engine_failed{};
};

struct Shown final {
	unsigned count{};
};

Totals run_cell(const Instruction &instruction, bool daz, bool ftz,
	unsigned rounding, std::size_t iterations, Shown &shown)
{
	FlatMemory memory{memory_size};
	const auto code = loop_code(instruction.bytes);
	(void)memory.write(code_address, code);
	uml::cpu::x86_64::gadgets::Executor executor;
	executor.prepare(memory);
	const std::uint32_t mxcsr = 0x1f80U | (daz ? 0x40U : 0U) |
		(ftz ? 0x8000U : 0U) | (rounding << 13U);
	Operands operands;
	Totals totals;
	const std::size_t lanes = instruction.dbl ? 4U : 8U;
	/* Lanes the instruction writes; an SSE shape keeps ymm0's upper half,
	 * and a scalar one also lanes 1 and up, which the model copies. */
	const std::size_t width = instruction.shape == Shape::vex256 ? lanes :
		lanes / 2U;
	std::vector<std::byte> expected(32);
	for (std::size_t done = 0; done < iterations;) {
		const auto count = std::min(chunk, iterations - done);
		for (std::size_t row = 0; row < count; ++row) {
			auto *input = memory.at(input_address + row * input_stride);
			for (std::size_t reg = 0; reg < 3; ++reg)
				for (std::size_t index = 0; index < lanes; ++index)
					set_lane(input + reg * 32U, instruction.dbl, index,
						operands.pick(instruction.dbl));
			if (instruction.op != Op::mul && instruction.op != Op::div)
				continue;
			/* A quarter of the lanes of products and quotients sit at
			 * the underflow threshold. */
			auto *first = instruction.shape == Shape::vex256 ?
				input + 32 : input;
			for (std::size_t index = 0; index < lanes; ++index) {
				if ((operands.next() & 3U) != 0)
					continue;
				const auto pair = operands.threshold_pair(instruction.dbl,
					instruction.op);
				set_lane(first, instruction.dbl, index, pair[0]);
				set_lane(first + 32, instruction.dbl, index, pair[1]);
			}
		}
		if (!run_engine(memory, executor, mxcsr, count)) {
			totals.engine_failed = true;
			return totals;
		}
		const HostRounding host_rounding{rounding};
		for (std::size_t row = 0; row < count; ++row) {
			const auto *input = memory.at(input_address + row * input_stride);
			const auto *actual = memory.at(output_address +
				row * output_stride);
			model_result(instruction, input, daz, ftz, expected.data());
			for (std::size_t index = 0; index < lanes; ++index) {
				const auto want = lane(expected.data(), instruction.dbl, index);
				const auto got = lane(actual, instruction.dbl, index);
				const bool operated = index < width;
				if (operated)
					++totals.lanes;
				if (want == got)
					continue;
				if (operated)
					++totals.differing;
				else
					++totals.kept_differing;
				if (shown.count < 12U) {
					++shown.count;
					std::fprintf(stderr, "  %.*s%s %s daz=%d ftz=%d rc=%u "
						"lane %zu a=%#llx b=%#llx engine=%#llx x86=%#llx\n",
						static_cast<int>(instruction.name.size()),
						instruction.name.data(), instruction.dbl ? "d" : "s",
						shape_name(instruction.shape), daz ? 1 : 0,
						ftz ? 1 : 0, rounding, index,
						static_cast<unsigned long long>(lane(
							instruction.shape == Shape::vex256 ?
								input + 32 : input, instruction.dbl, index)),
						static_cast<unsigned long long>(lane(
							instruction.shape == Shape::vex256 ?
								input + 64 : input + 32, instruction.dbl,
							index)),
						static_cast<unsigned long long>(got),
						static_cast<unsigned long long>(want));
				}
			}
		}
		done += count;
	}
	return totals;
}

} // namespace

int main(int argc, char **argv)
{
	std::size_t iterations = 2000;
	std::string_view only;
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument{argv[index]};
		if (argument == "--iterations" && index + 1 < argc)
			iterations = std::strtoull(argv[++index], nullptr, 10);
		else if (argument == "--only" && index + 1 < argc)
			only = argv[++index];
		else {
			std::fprintf(stderr, "usage: %s [--iterations N] [--only NAME]\n",
				argv[0]);
			return 2;
		}
	}
	std::uint64_t lanes{};
	std::uint64_t differing{};
	std::uint64_t kept{};
	bool failed = false;
	Shown shown;
	std::printf("instruction\tshape\tdaz\tftz\trc\tlanes\tdiffering\n");
	for (const auto &instruction : instructions()) {
		if (!only.empty() && instruction.name != only)
			continue;
		for (const bool daz : {false, true})
		for (const bool ftz : {false, true})
		for (unsigned rounding = 0; rounding < 4; ++rounding) {
			const auto totals = run_cell(instruction, daz, ftz, rounding,
				iterations, shown);
			if (totals.engine_failed) {
				std::fprintf(stderr, "x86-64 SSE exactness: %.*s did not run "
					"to its end\n", static_cast<int>(instruction.name.size()),
					instruction.name.data());
				failed = true;
				continue;
			}
			lanes += totals.lanes;
			differing += totals.differing;
			kept += totals.kept_differing;
			std::printf("%.*s%s\t%s\t%d\t%d\t%u\t%llu\t%llu\n",
				static_cast<int>(instruction.name.size()),
				instruction.name.data(), instruction.dbl ? "d" : "s",
				shape_name(instruction.shape), daz ? 1 : 0, ftz ? 1 : 0,
				rounding, static_cast<unsigned long long>(totals.lanes),
				static_cast<unsigned long long>(totals.differing));
		}
	}
	std::printf("TOTAL lanes %llu differing %llu kept-lane mismatches %llu\n",
		static_cast<unsigned long long>(lanes),
		static_cast<unsigned long long>(differing),
		static_cast<unsigned long long>(kept));
	return failed || differing != 0 || kept != 0 ? 1 : 0;
}
