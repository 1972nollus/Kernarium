// SPDX-License-Identifier: GPL-2.0-or-later
/* The gadget engine's non-streaming SVE control core (gadgets::sve_support
 * = control): PTRUE, PFALSE and PTEST, the predicate logical operations,
 * CNTP, INCP and DECP, WHILE* and its fusion with B.cond, CNT*, INC*, DEC*,
 * RDVL, ADDVL and ADDPL, SETFFR, RDFFR and WRFFR, and MOVPRFX, DUP, CPY,
 * SEL and the unpredicated logical forms, which the generated gadgets of
 * the SVE part (engine/generate_sve.py) run on AdvSIMD at VL 16.
 *
 * No host implements SVE, so each test injects the SVE profile into the
 * capabilities a translation is built for and drives CpuState directly:
 * P and FFR in their mask form, Z as `vectors`. The references below follow
 * the A64 pseudocode element by element, independently of the lowerings.
 * Programs that hold an SVE word access the FP state and mark the task as
 * having executed SVE, through the C++ entry and through the SVE link
 * entry. A sweep checks that no program translated for SVCR.SM = 1 selects
 * an entry of the SVE part, and no program for SM = 0 a streaming SVE one. */
#include "arm64_gadget_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/detail/sve_semantics.hpp"
#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/gadgets/sve_helpers.hpp"
#include "cpu/arm64/gadgets/sve_support.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"
#include "platform/apple/host_features.hpp"
#include "sve_forms.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <compare>
#include <crt_externs.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach-o/dyld.h>
#include <memory_resource>
#include <set>
#include <spawn.h>
#include <span>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
extern const char uml_arm64_gadget_sve_section_begin[];
extern const char uml_arm64_gadget_sve_section_end[];
extern const char uml_arm64_gadget_bank_begin[];
void uml_arm64_gadget_link_exit();
void uml_arm64_gadget_sve_link_entry();
void uml_arm64_gadget_sm_twins_begin();
void uml_arm64_gadget_sm_twins_end();
extern const std::uintptr_t uml_arm64_gadget_sm_generated_entries[];
extern const std::uintptr_t uml_arm64_gadget_sm_generated_entries_end[];
extern const std::uintptr_t uml_arm64_gadget_sm_memory_entries[];
extern const std::uintptr_t uml_arm64_gadget_sm_memory_entries_end[];
extern const std::uintptr_t uml_arm64_gadget_sve_generated_entries[];
extern const std::uintptr_t uml_arm64_gadget_sve_generated_entries_end[];
}

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
namespace apple = uml::platform::apple;
using uml::cpu::ArmFeature;
using uml::cpu::EngineEvent;
using uml::platform::HostCapabilities;
using uml::memory::StaticGuestMemory;
using namespace uml::emulator::tests;

namespace {

constexpr std::uint64_t code_base = 0x82'0000;
constexpr std::size_t mapped_pages = 2;
constexpr std::uint32_t svc = 0xd400'0001U;
constexpr std::uint32_t smstart_sm = 0xd503'437fU;
constexpr std::uint32_t smstop_sm = 0xd503'427fU;
constexpr std::uint32_t nzcv_v = 0x1000'0000U;

using Bytes = std::array<std::uint8_t, 16>;

/* Hosts. */

void clear(HostCapabilities &host, std::initializer_list<ArmFeature> features)
{
	for (const auto feature : features)
		host.supported.reset(static_cast<std::size_t>(feature));
}

/* The probed host without SME, and with SVE and SVE2 injected unless
 * `sve` is false; the other SVE tokens stay off. */
[[nodiscard]] HostCapabilities sve_host(bool sve = true)
{
	auto host = ::uml::cpu::host_capabilities();
	clear(host, {ArmFeature::sme, ArmFeature::sme2, ArmFeature::sme_f64f64,
		ArmFeature::sme_i16i64, ArmFeature::sve, ArmFeature::sve2,
		ArmFeature::sve_aes, ArmFeature::sve_pmull128,
		ArmFeature::sve_sha3});
	host.sme_svl_bytes = 0;
	host.sme_hardware = false;
	if (sve)
		for (const auto feature : {ArmFeature::sve, ArmFeature::sve2})
			host.supported.set(static_cast<std::size_t>(feature));
	return host;
}

/* SVE with an injected SME2 profile at `svl`, which never claims the
 * hardware. */
[[nodiscard]] HostCapabilities sve_sme_host(unsigned svl = 64)
{
	auto host = sve_host();
	for (const auto feature : {ArmFeature::sme, ArmFeature::sme2,
		     ArmFeature::sme_f64f64, ArmFeature::sme_i16i64})
		host.supported.set(static_cast<std::size_t>(feature));
	host.sme_svl_bytes = static_cast<std::uint16_t>(svl);
	return host;
}

/* State helpers. P and FFR must stay in mask form whatever writes them. */

[[nodiscard]] bool canonical(const arm64::SvePredicate &mask)
{
	return std::ranges::all_of(mask, [](std::uint8_t byte) {
		return byte == 0U || byte == 0xffU;
	});
}

[[nodiscard]] std::uint16_t pred(const arm64::CpuState &cpu, unsigned n)
{
	require(canonical(cpu.sve_p[n]));
	return arm64::sve_predicate_packed(cpu.sve_p[n]);
}

[[nodiscard]] std::uint16_t ffr(const arm64::CpuState &cpu)
{
	require(canonical(cpu.sve_ffr));
	return arm64::sve_predicate_packed(cpu.sve_ffr);
}

void set_pred(arm64::CpuState &cpu, unsigned n, std::uint16_t packed)
{
	cpu.sve_p[n] = arm64::sve_predicate_mask(packed);
}

[[nodiscard]] Bytes vec(const arm64::CpuState &cpu, unsigned n)
{
	Bytes bytes{};
	std::memcpy(bytes.data(), cpu.vectors[n].bytes.data(), bytes.size());
	return bytes;
}

void set_vec(arm64::CpuState &cpu, unsigned n, const Bytes &bytes)
{
	std::memcpy(cpu.vectors[n].bytes.data(), bytes.data(), bytes.size());
}

[[nodiscard]] std::uint64_t element(const Bytes &bytes, unsigned esize,
	unsigned index)
{
	std::uint64_t value = 0;
	std::memcpy(&value, bytes.data() + index * esize, esize);
	return value;
}

void set_element(Bytes &bytes, unsigned esize, unsigned index,
	std::uint64_t value)
{
	std::memcpy(bytes.data() + index * esize, &value, esize);
}

/* Element `index` of `esize` bytes is active in a packed predicate: the
 * predicate bit of its lowest byte. */
[[nodiscard]] constexpr bool active(std::uint16_t predicate, unsigned esize,
	unsigned index)
{
	return ((predicate >> (index * esize)) & 1U) != 0U;
}

[[nodiscard]] constexpr std::uint16_t element_pattern(unsigned esize)
{
	std::uint16_t pattern = 0;
	for (unsigned bit = 0; bit < 16U; bit += esize)
		pattern = static_cast<std::uint16_t>(pattern | (1U << bit));
	return pattern;
}

class Random final {
public:
	explicit Random(std::uint64_t seed) noexcept : state_{seed} {}

	std::uint64_t next() noexcept
	{
		state_ += 0x9e37'79b9'7f4a'7c15ULL;
		auto value = state_;
		value = (value ^ (value >> 30)) * 0xbf58'476d'1ce4'e5b9ULL;
		value = (value ^ (value >> 27)) * 0x94d0'49bb'1331'11ebULL;
		return value ^ (value >> 31);
	}

	std::uint16_t predicate() noexcept
	{
		/* Mostly random, sometimes empty, full or one run. */
		switch (next() % 8U) {
		case 0:
			return 0;
		case 1:
			return 0xffff;
		case 2:
			return static_cast<std::uint16_t>(
				(1U << (next() % 17U)) - 1U);
		default:
			return static_cast<std::uint16_t>(next());
		}
	}

	Bytes bytes() noexcept
	{
		Bytes result{};
		const auto low = next();
		const auto high = next();
		std::memcpy(result.data(), &low, 8);
		std::memcpy(result.data() + 8, &high, 8);
		return result;
	}

private:
	std::uint64_t state_;
};

/* References. */

/* PredTest(mask, result, esize): N, the first active element of the result
 * is set; Z, no active element is; C, the last active one is not (set with
 * no active element); V clear. */
[[nodiscard]] std::uint32_t pred_test(std::uint16_t mask,
	std::uint16_t result, unsigned esize)
{
	bool first = true;
	bool n = false;
	bool z = true;
	bool last = false;
	for (unsigned index = 0; index < 16U / esize; ++index) {
		if (!active(mask, esize, index))
			continue;
		const bool set = active(result, esize, index);
		if (first)
			n = set;
		first = false;
		z = z && !set;
		last = set;
	}
	return (n ? 0x8000'0000U : 0U) | (z ? 0x4000'0000U : 0U) |
		(last ? 0U : 0x2000'0000U);
}

/* DecodePredCount() at VL 16. */
[[nodiscard]] unsigned pattern_count(unsigned pattern, unsigned esize)
{
	const unsigned elements = 16U / esize;
	if (pattern == 0U) {
		unsigned power = 1;
		while (power * 2U <= elements)
			power *= 2U;
		return power;
	}
	if (pattern >= 1U && pattern <= 8U)
		return pattern <= elements ? pattern : 0U;
	if (pattern >= 9U && pattern <= 13U)
		return (16U << (pattern - 9U)) <= elements ?
			16U << (pattern - 9U) : 0U;
	if (pattern == 29U)
		return elements / 4U * 4U;
	if (pattern == 30U)
		return elements / 3U * 3U;
	if (pattern == 31U)
		return elements;
	return 0;
}

[[nodiscard]] std::uint16_t leading(unsigned count, unsigned esize)
{
	std::uint16_t result = 0;
	for (unsigned index = 0; index < count && index < 16U / esize; ++index)
		result = static_cast<std::uint16_t>(result |
			(1U << (index * esize)));
	return result;
}

[[nodiscard]] unsigned count_active(std::uint16_t predicate, unsigned esize)
{
	return static_cast<unsigned>(std::popcount(static_cast<unsigned>(
		predicate & element_pattern(esize))));
}

/* Encoders. Each builds the word the A64 encoding names; the encodings
 * test checks a sample against assembler output. */

[[nodiscard]] constexpr std::uint32_t ptrue(unsigned pd, unsigned size,
	unsigned pattern, bool flags = false)
{
	return (flags ? 0x2519'e000U : 0x2518'e000U) | (size << 22) |
		(pattern << 5) | pd;
}

[[nodiscard]] constexpr std::uint32_t ptest(unsigned pg, unsigned pn)
{
	return 0x2550'c000U | (pg << 10) | (pn << 5);
}

[[nodiscard]] constexpr std::uint32_t predicate_logical(std::uint32_t base,
	unsigned pd, unsigned pg, unsigned pn, unsigned pm)
{
	return base | (pm << 16) | (pg << 10) | (pn << 5) | pd;
}

[[nodiscard]] constexpr std::uint32_t cntp(unsigned rd, unsigned pg,
	unsigned pn, unsigned size)
{
	return 0x2520'8000U | (size << 22) | (pg << 10) | (pn << 5) | rd;
}

/* INCP or DECP Xdn, Pm.T, or Zdn.T with `vector`. */
[[nodiscard]] constexpr std::uint32_t incp(unsigned rdn, unsigned pm,
	unsigned size, bool decrement, bool vector)
{
	return (vector ? 0x252c'8000U : 0x252c'8800U) |
		(decrement ? 0x1'0000U : 0U) | (size << 22) | (pm << 5) | rdn;
}

/* CNT[BHWD] Xd, pattern, MUL #mul. */
[[nodiscard]] constexpr std::uint32_t cnt(unsigned rd, unsigned size,
	unsigned pattern, unsigned mul)
{
	return 0x0420'e000U | (size << 22) | ((mul - 1U) << 16) |
		(pattern << 5) | rd;
}

/* INC[BHWD] or DEC[BHWD] Xdn, or Zdn.T with `vector`. */
[[nodiscard]] constexpr std::uint32_t inc(unsigned rdn, unsigned size,
	unsigned pattern, unsigned mul, bool decrement, bool vector)
{
	return (vector ? 0x0430'c000U : 0x0430'e000U) | (size << 22) |
		((mul - 1U) << 16) | (pattern << 5) |
		(decrement ? 0x400U : 0U) | rdn;
}

[[nodiscard]] constexpr std::uint32_t rdvl(unsigned rd, int imm)
{
	return 0x04bf'5000U | ((static_cast<unsigned>(imm) & 63U) << 5) | rd;
}

/* ADDVL, or ADDPL with `predicate`, Xd|SP, Xn|SP, #imm. */
[[nodiscard]] constexpr std::uint32_t addvl(unsigned rd, unsigned rn,
	int imm, bool predicate)
{
	return (predicate ? 0x0460'5000U : 0x0420'5000U) | (rn << 16) |
		((static_cast<unsigned>(imm) & 63U) << 5) | rd;
}

constexpr std::uint32_t setffr = 0x252c'9000U;

[[nodiscard]] constexpr std::uint32_t rdffr(unsigned pd)
{
	return 0x2519'f000U | pd;
}

[[nodiscard]] constexpr std::uint32_t rdffr_z(unsigned pd, unsigned pg,
	bool flags)
{
	return (flags ? 0x2558'f000U : 0x2518'f000U) | (pg << 5) | pd;
}

[[nodiscard]] constexpr std::uint32_t wrffr(unsigned pn)
{
	return 0x2528'9000U | (pn << 5);
}

/* WHILE<op> Pd.T, Rn, Rm: the U, lt and eq bits by op. */
enum class While : std::uint8_t { ge, gt, lt, le, hs, hi, lo, ls };

[[nodiscard]] constexpr std::uint32_t while_compare(While op, unsigned pd,
	unsigned rn, unsigned rm, unsigned size, bool sf)
{
	constexpr std::array<std::uint32_t, 8> bits{0x000U, 0x010U, 0x400U,
		0x410U, 0x800U, 0x810U, 0xc00U, 0xc10U};
	return 0x2520'0000U | bits[static_cast<unsigned>(op)] | (size << 22) |
		(rm << 16) | (sf ? 0x1000U : 0U) | (rn << 5) | pd;
}

[[nodiscard]] constexpr std::uint32_t while_conflict(bool read_write,
	unsigned pd, unsigned rn, unsigned rm, unsigned size)
{
	return (read_write ? 0x2520'3010U : 0x2520'3000U) | (size << 22) |
		(rm << 16) | (rn << 5) | pd;
}

[[nodiscard]] constexpr std::uint32_t movz(unsigned rd, unsigned imm)
{
	return 0xd280'0000U | (imm << 5) | rd;
}

/* Running. */

[[nodiscard]] gadgets::BuildResult build(std::span<const std::uint32_t> code,
	const HostCapabilities &host, std::uint8_t svcr = 0,
	gadgets::Builder::LinkResolver resolve = {})
{
	gadgets::BuildOptions options{.host_capabilities = host};
	options.svcr = svcr;
	return gadgets::Builder{}.build(code_base, code, std::move(resolve),
		std::pmr::get_default_resource(), {}, options);
}

/* One translated block, run on CpuState values the test sets. */
class Block final {
public:
	Block(std::span<const std::uint32_t> code,
		const HostCapabilities &host = sve_host())
		: built_{build(code, host)}, size_{code.size()}
	{
		require(built_.program.holds_sve());
		require(built_.program.accesses_fp_state());
	}

	/* Runs to the closing SVC, which every block here ends with. */
	void run(arm64::CpuState &cpu)
	{
		std::size_t executed = 0;
		cpu.pc = code_base;
		require(gadgets::execute(built_.program, cpu, tlb_, executed) ==
			EngineEvent::system_call);
		require(executed == size_);
		require(cpu.sve_live == 1U);
		require(cpu.gadget_zero == 0U);
	}

	[[nodiscard]] EngineEvent run_event(arm64::CpuState &cpu,
		std::size_t &executed)
	{
		cpu.pc = code_base;
		return gadgets::execute(built_.program, cpu, tlb_, executed);
	}

private:
	gadgets::BuildResult built_;
	std::size_t size_;
	uml::memory::Tlb tlb_;
};

/* What one word does to a fresh CpuState with `prepare` applied: runs
 * `word; svc` and returns the state after. */
template <typename Prepare>
[[nodiscard]] arm64::CpuState after(std::uint32_t word, Prepare prepare)
{
	const std::array code{word, svc};
	Block block{code};
	arm64::CpuState cpu{};
	prepare(cpu);
	block.run(cpu);
	return cpu;
}

[[nodiscard]] EngineEvent run_illegal(std::span<const std::uint32_t> code,
	const HostCapabilities &host, std::size_t &executed,
	arm64::CpuState &cpu)
{
	const auto built = build(code, host);
	uml::memory::Tlb tlb;
	cpu.pc = code_base;
	executed = 0;
	return gadgets::execute(built.program, cpu, tlb, executed);
}

/* An executor over a mapped guest page, for the paths that need linking:
 * the link entry of a stored block and the fused branch. */
class Machine final {
public:
	Machine(std::span<const std::uint32_t> code, const HostCapabilities &host)
		: generation_{next_generation_++}, backing_{mapped_pages + 1},
		  mapping_{generation_, code_base, backing_, mapped_pages,
			  PROT_READ | PROT_WRITE | PROT_EXEC},
		  executor_{gadgets::BuildOptions{.host_capabilities = host}},
		  file_{arm64::SmeRegisterFile::for_host(host)}
	{
		for (std::size_t index = 0; index < code.size(); ++index)
			backing_.put32(index * sizeof(std::uint32_t), code[index]);
		if (file_.data() != nullptr) {
			cpu.bind_sme_registers(file_);
			task_.assign(arm64::sme_storage_size(file_.svl()),
				std::byte{0});
		}
		cpu.defer_sme_load(file_.data() != nullptr ? task_.data() :
			nullptr, 0, 0);
	}

	/* Runs from code_base until an exit other than a continued block,
	 * counting the blocks the executor entered from C++. */
	EngineEvent run(std::size_t limit = 16)
	{
		cpu.pc = code_base;
		require(executor_.begin_execution(generation_, cpu.pc));
		EngineEvent event = EngineEvent::fatal;
		retired = 0;
		blocks = 0;
		for (std::size_t block = 0; block < limit; ++block) {
			std::size_t executed = 0;
			event = executor_.run_block(cpu, memory_, generation_,
				executed);
			retired += executed;
			++blocks;
			if (event != EngineEvent::continued &&
			    event != EngineEvent::interrupt)
				break;
		}
		executor_.end_execution();
		gadgets::restore_native_fp_environment(cpu);
		return event;
	}

	/* Guest bytes at code_base + offset, in the mapped pages. */
	void put64(std::size_t offset, std::uint64_t value)
	{
		backing_.put64(offset, value);
	}

	[[nodiscard]] std::uint64_t fault_address() const
	{
		return executor_.fault_address();
	}

	/* The mapped pages' bytes. */
	void get(std::size_t offset, std::span<std::byte> bytes) const
	{
		backing_.get(offset, bytes);
	}

	void put(std::size_t offset, std::span<const std::byte> bytes)
	{
		backing_.put(offset, bytes);
	}

	[[nodiscard]] std::uint32_t floating_point_syndrome() const
	{
		return executor_.floating_point_syndrome();
	}

	arm64::CpuState cpu{};
	std::size_t retired{};
	std::size_t blocks{};

private:
	static inline unsigned long next_generation_ = 7900;
	unsigned long generation_;
	JitBacking backing_;
	JitMapping mapping_;
	StaticGuestMemory memory_;
	gadgets::GadgetExecutor executor_;
	arm64::SmeRegisterFile file_;
	std::vector<std::byte> task_;
};

/* Tests. */

/* A sample of the encoders checked against assembler output (clang
 * -march=armv9-a+sve2), and each word against its encoding-table row. */
void encodings_match_the_assembler()
{
	struct Sample final {
		std::uint32_t ours;
		std::uint32_t assembled;
		std::uint16_t form;
	};
	const std::array samples{
		Sample{while_compare(While::lo, 0, 0, 1, 0, true), 0x2521'1c00U,
			941},
		Sample{while_compare(While::le, 1, 0, 1, 2, false), 0x25a1'0411U,
			938},
		Sample{while_compare(While::gt, 2, 3, 4, 3, true), 0x25e4'1072U,
			929},
		Sample{while_conflict(false, 3, 1, 2, 1), 0x2562'3023U, 949},
		Sample{while_conflict(true, 4, 1, 2, 2), 0x25a2'3034U, 948},
		Sample{incp(3, 5, 3, false, false), 0x25ec'88a3U, 287},
		Sample{incp(3, 5, 1, true, true), 0x256d'80a3U, 130},
		Sample{cntp(2, 1, 2, 2), 0x25a0'8442U, 120},
		Sample{addvl(31, 3, -2, false), 0x0423'57dfU, 16},
		Sample{addvl(4, 31, 31, true), 0x047f'53e4U, 10},
		Sample{rdvl(5, -32), 0x04bf'5405U, 550},
		Sample{inc(2, 0, 7, 3, false, false), 0x0432'e0e2U, 285},
		Sample{inc(1, 3, 31, 16, true, true), 0x04ff'c7e1U, 128},
		Sample{ptrue(7, 1, 30, true), 0x2559'e3c7U, 541},
		Sample{rdffr_z(1, 2, true), 0x2558'f041U, 549},
		Sample{predicate_logical(0x25c0'4210U, 3, 4, 5, 6), 0x25c6'52b3U,
			490},
		Sample{predicate_logical(0x2500'4210U, 1, 2, 3, 4), 0x2504'4a71U,
			585},
	};
	for (const auto &sample : samples) {
		require(sample.ours == sample.assembled);
		const auto *row = arm64::detail::find_sme_encoding(sample.ours);
		require(row != nullptr && row->form == sample.form);
		require((row->non_streaming_requirement &
			arm64::detail::sme_feature_sve) != 0U);
		require(gadgets::sve::row_forms[static_cast<std::size_t>(
			row - arm64::detail::sme_encodings.data())] !=
			gadgets::sve::no_form);
	}
}

/* PTEST against the reference for every governing predicate, each with
 * itself, its complement, nothing and a random result. */
void predicate_test_matches_the_reference()
{
	const std::array code{ptest(1, 2), svc};
	Block block{code};
	Random random{1};
	arm64::CpuState cpu{};
	for (unsigned mask = 0; mask <= 0xffffU; ++mask) {
		for (const auto result : {static_cast<std::uint16_t>(mask),
			     static_cast<std::uint16_t>(~mask),
			     std::uint16_t{0},
			     static_cast<std::uint16_t>(random.next())}) {
			set_pred(cpu, 1, static_cast<std::uint16_t>(mask));
			set_pred(cpu, 2, result);
			cpu.nzcv = nzcv_v;
			block.run(cpu);
			require(cpu.nzcv == pred_test(
				static_cast<std::uint16_t>(mask), result, 1));
		}
	}
}

/* PTRUE and PTRUES at every element size and pattern; PFALSE. */
void predicate_constants()
{
	for (unsigned size = 0; size < 4U; ++size) {
		const unsigned esize = 1U << size;
		for (unsigned pattern = 0; pattern < 32U; ++pattern) {
			const std::array code{ptrue(4, size, pattern, true),
				ptrue(3, size, pattern), svc};
			Block block{code};
			arm64::CpuState cpu{};
			set_pred(cpu, 3, 0xa5a5);
			set_pred(cpu, 4, 0x5a5a);
			block.run(cpu);
			const auto expected = leading(
				pattern_count(pattern, esize), esize);
			require(pred(cpu, 3) == expected);
			require(pred(cpu, 4) == expected);
			/* PTRUES tests the result under itself; PTRUE
			 * leaves NZCV alone. */
			require(cpu.nzcv == pred_test(expected, expected,
				esize));
		}
	}
	const auto cpu = after(0x2518'e400U | 5U, [](arm64::CpuState &state) {
		set_pred(state, 5, 0xffff);
		state.nzcv = 0x9000'0000U;
	});
	require(pred(cpu, 5) == 0U && cpu.nzcv == 0x9000'0000U);
}

/* The predicate logical operations, with and without flags, separate
 * and aliased registers. */
void predicate_logical_operations()
{
	struct Operation final {
		std::uint32_t base;
		bool flags;
		std::uint16_t (*apply)(std::uint16_t, std::uint16_t,
			std::uint16_t);
	};
	using P = std::uint16_t;
	const std::array operations{
		Operation{0x2500'4000U, false, [](P g, P n, P m) -> P {
			return g & n & m; }},
		Operation{0x2540'4000U, true, [](P g, P n, P m) -> P {
			return g & n & m; }},
		Operation{0x2500'4010U, false, [](P g, P n, P m) -> P {
			return g & n & static_cast<P>(~m); }},
		Operation{0x2540'4010U, true, [](P g, P n, P m) -> P {
			return g & n & static_cast<P>(~m); }},
		Operation{0x2500'4200U, false, [](P g, P n, P m) -> P {
			return g & (n ^ m); }},
		Operation{0x2540'4200U, true, [](P g, P n, P m) -> P {
			return g & (n ^ m); }},
		Operation{0x2500'4210U, false, [](P g, P n, P m) -> P {
			return static_cast<P>((g & n) | (~g & m)); }},
		Operation{0x2580'4000U, false, [](P g, P n, P m) -> P {
			return g & (n | m); }},
		Operation{0x25c0'4000U, true, [](P g, P n, P m) -> P {
			return g & (n | m); }},
		Operation{0x2580'4010U, false, [](P g, P n, P m) -> P {
			return g & (n | static_cast<P>(~m)); }},
		Operation{0x25c0'4010U, true, [](P g, P n, P m) -> P {
			return g & (n | static_cast<P>(~m)); }},
		Operation{0x2580'4200U, false, [](P g, P n, P m) -> P {
			return g & static_cast<P>(~(n | m)); }},
		Operation{0x25c0'4200U, true, [](P g, P n, P m) -> P {
			return g & static_cast<P>(~(n | m)); }},
		Operation{0x2580'4210U, false, [](P g, P n, P m) -> P {
			return g & static_cast<P>(~(n & m)); }},
		Operation{0x25c0'4210U, true, [](P g, P n, P m) -> P {
			return g & static_cast<P>(~(n & m)); }},
	};
	Random random{2};
	for (const auto &operation : operations) {
		const std::array separate{predicate_logical(operation.base,
			0, 1, 2, 3), svc};
		const std::array aliased{predicate_logical(operation.base,
			2, 2, 2, 5), svc};
		Block first{separate};
		Block second{aliased};
		for (int sample = 0; sample < 2000; ++sample) {
			const auto g = random.predicate();
			const auto n = random.predicate();
			const auto m = random.predicate();
			arm64::CpuState cpu{};
			set_pred(cpu, 0, random.predicate());
			set_pred(cpu, 1, g);
			set_pred(cpu, 2, n);
			set_pred(cpu, 3, m);
			cpu.nzcv = nzcv_v;
			first.run(cpu);
			const auto result = operation.apply(g, n, m);
			require(pred(cpu, 0) == result);
			require(pred(cpu, 1) == g && pred(cpu, 2) == n &&
				pred(cpu, 3) == m);
			require(cpu.nzcv == (operation.flags ?
				pred_test(g, result, 1) : nzcv_v));

			set_pred(cpu, 5, m);
			cpu.nzcv = nzcv_v;
			second.run(cpu);
			const auto alias = operation.apply(n, n, m);
			require(pred(cpu, 2) == alias);
			require(cpu.nzcv == (operation.flags ?
				pred_test(n, alias, 1) : nzcv_v));
		}
	}
}

/* CNTP, INCP and DECP at every element size; XZR discards the count. */
void predicate_counts()
{
	Random random{3};
	for (unsigned size = 0; size < 4U; ++size) {
		const unsigned esize = 1U << size;
		const std::array code{cntp(2, 1, 3, size),
			incp(4, 3, size, false, false),
			incp(5, 1, size, true, false), cntp(31, 1, 3, size),
			incp(31, 3, size, false, false), svc};
		Block block{code};
		for (int sample = 0; sample < 500; ++sample) {
			arm64::CpuState cpu{};
			const auto g = random.predicate();
			const auto n = random.predicate();
			set_pred(cpu, 1, g);
			set_pred(cpu, 3, n);
			cpu.registers[4] = random.next();
			cpu.registers[5] = random.next();
			auto registers = cpu.registers;
			block.run(cpu);
			registers[2] = count_active(
				static_cast<std::uint16_t>(g & n), esize);
			registers[4] += count_active(n, esize);
			registers[5] -= count_active(g, esize);
			require(cpu.registers == registers);
		}
		if (size == 0U)
			continue;
		/* The vector forms have no byte elements. */
		const std::array vector{incp(6, 1, size, false, true),
			incp(7, 3, size, true, true), svc};
		Block vectors{vector};
		for (int sample = 0; sample < 200; ++sample) {
			arm64::CpuState cpu{};
			const auto g = random.predicate();
			const auto n = random.predicate();
			set_pred(cpu, 1, g);
			set_pred(cpu, 3, n);
			auto six = random.bytes();
			auto seven = random.bytes();
			set_vec(cpu, 6, six);
			set_vec(cpu, 7, seven);
			vectors.run(cpu);
			const auto bits = esize * 8U;
			const auto mask = bits == 64U ? ~std::uint64_t{0} :
				(std::uint64_t{1} << bits) - 1U;
			for (unsigned index = 0; index < 16U / esize; ++index) {
				set_element(six, esize, index,
					(element(six, esize, index) +
					 count_active(g, esize)) & mask);
				set_element(seven, esize, index,
					(element(seven, esize, index) -
					 count_active(n, esize)) & mask);
			}
			require(vec(cpu, 6) == six && vec(cpu, 7) == seven);
		}
	}
}

/* CNT*, INC* and DEC* at every pattern, RDVL, ADDVL and ADDPL, SP
 * included. */
void element_counts()
{
	for (unsigned size = 0; size < 4U; ++size) {
		const unsigned esize = 1U << size;
		for (unsigned pattern = 0; pattern < 32U; ++pattern) {
			for (const unsigned mul : {1U, 3U, 16U}) {
				const auto count = std::uint64_t{
					pattern_count(pattern, esize)} * mul;
				std::vector<std::uint32_t> code{
					cnt(0, size, pattern, mul),
					inc(1, size, pattern, mul, false, false),
					inc(2, size, pattern, mul, true, false),
					cnt(31, size, pattern, mul),
					inc(31, size, pattern, mul, false,
						false)};
				if (size != 0U)
					code.insert(code.end(), {
						inc(3, size, pattern, mul, false,
							true),
						inc(4, size, pattern, mul, true,
							true)});
				code.push_back(svc);
				Block block{code};
				arm64::CpuState cpu{};
				cpu.registers[0] = 0xdead;
				cpu.registers[1] = 0xffff'ffff'ffff'fff0ULL;
				cpu.registers[2] = 7;
				const Bytes three{0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
					0xff, 0xff, 1, 2, 3, 4, 5, 6, 7, 8};
				set_vec(cpu, 3, three);
				set_vec(cpu, 4, three);
				auto registers = cpu.registers;
				block.run(cpu);
				registers[0] = count;
				registers[1] += count;
				registers[2] -= count;
				require(cpu.registers == registers);
				if (size == 0U)
					continue;
				auto up = three;
				auto down = three;
				const auto bits = esize * 8U;
				const auto mask = bits == 64U ? ~std::uint64_t{0} :
					(std::uint64_t{1} << bits) - 1U;
				for (unsigned index = 0; index < 16U / esize;
				     ++index) {
					set_element(up, esize, index,
						(element(three, esize, index) +
						 count) & mask);
					set_element(down, esize, index,
						(element(three, esize, index) -
						 count) & mask);
				}
				require(vec(cpu, 3) == up && vec(cpu, 4) == down);
			}
		}
	}
	for (int imm = -32; imm < 32; ++imm) {
		const std::array code{rdvl(0, imm), addvl(1, 2, imm, false),
			addvl(31, 31, imm, true), addvl(3, 31, imm, false),
			addvl(31, 4, imm, true), addvl(5, 5, imm, false),
			rdvl(31, imm), svc};
		Block block{code};
		arm64::CpuState cpu{};
		cpu.registers[2] = 0x1000;
		cpu.registers[4] = 0x9000;
		cpu.registers[5] = 0x3000;
		cpu.sp = 0x8000;
		auto registers = cpu.registers;
		block.run(cpu);
		const auto vl = static_cast<std::uint64_t>(imm * 16);
		const auto pl = static_cast<std::uint64_t>(imm * 2);
		registers[0] = vl;
		registers[1] = 0x1000 + vl;
		registers[3] = 0x8000 + pl + vl;
		registers[5] = 0x3000 + vl;
		require(cpu.registers == registers);
		/* SP += PL, then SP := X4 + PL. */
		require(cpu.sp == 0x9000 + pl);
	}
}

/* SETFFR, RDFFR in both forms, RDFFRS and WRFFR. */
void first_fault_register()
{
	{
		const std::array code{setffr, rdffr(1), svc};
		Block block{code};
		arm64::CpuState cpu{};
		cpu.nzcv = nzcv_v;
		block.run(cpu);
		require(ffr(cpu) == 0xffffU && pred(cpu, 1) == 0xffffU);
		require(cpu.nzcv == nzcv_v);
	}
	const std::array code{wrffr(2), rdffr_z(3, 4, false),
		rdffr_z(5, 6, true), svc};
	Block block{code};
	Random random{4};
	for (int sample = 0; sample < 2000; ++sample) {
		arm64::CpuState cpu{};
		const auto value = random.predicate();
		const auto g = random.predicate();
		const auto h = random.predicate();
		set_pred(cpu, 2, value);
		set_pred(cpu, 4, g);
		set_pred(cpu, 6, h);
		block.run(cpu);
		require(ffr(cpu) == value);
		require(pred(cpu, 3) == (value & g));
		require(pred(cpu, 5) == (value & h));
		require(cpu.nzcv == pred_test(h,
			static_cast<std::uint16_t>(value & h), 1));
	}
}

/* Element `index` of `esize` bytes active in Pg selects `source`, an
 * inactive one keeps `inactive`. */
[[nodiscard]] Bytes merge(const Bytes &inactive, const Bytes &source,
	std::uint16_t g, unsigned esize)
{
	auto result = inactive;
	for (unsigned index = 0; index < 16U / esize; ++index)
		if (active(g, esize, index))
			set_element(result, esize, index,
				element(source, esize, index));
	return result;
}

[[nodiscard]] Bytes replicate(std::uint64_t value, unsigned esize)
{
	Bytes result{};
	for (unsigned index = 0; index < 16U / esize; ++index)
		set_element(result, esize, index, value);
	return result;
}

/* MOVPRFX, DUP, DUPM, FDUP, CPY, FCPY, SEL and the unpredicated logical
 * forms. */
void vector_moves()
{
	Random random{5};
	const Bytes zero{};
	for (unsigned size = 0; size < 4U; ++size) {
		const unsigned esize = 1U << size;
		const auto s = size << 22;
		const std::array code{
			0x0420'bc00U | (2U << 5) | 1U,		  // movprfx z1, z2
			0x0411'2000U | s | (3U << 10) | (4U << 5) | 5U, // /m
			0x0410'2000U | s | (3U << 10) | (4U << 5) | 6U, // /z
			0x0520'3800U | s | (7U << 5) | 8U,	  // dup z8, x7
			0x0520'3800U | s | (31U << 5) | 9U,	  // dup z9, sp
			0x0528'a000U | s | (3U << 10) | (7U << 5) | 10U,
			0x0528'a000U | s | (3U << 10) | (31U << 5) | 11U,
			0x0520'8000U | s | (3U << 10) | (4U << 5) | 12U,
			0x0520'c000U | s | (14U << 16) | (3U << 10) | (4U << 5) |
				13U,				  // sel
			svc};
		Block block{code};
		for (int sample = 0; sample < 300; ++sample) {
			arm64::CpuState cpu{};
			const auto g = random.predicate();
			set_pred(cpu, 3, g);
			std::array<Bytes, 16> before{};
			for (unsigned n = 0; n < 16U; ++n) {
				before[n] = random.bytes();
				set_vec(cpu, n, before[n]);
			}
			cpu.registers[7] = random.next();
			cpu.sp = random.next();
			block.run(cpu);
			require(vec(cpu, 1) == before[2]);
			require(vec(cpu, 5) == merge(before[5], before[4], g,
				esize));
			require(vec(cpu, 6) == merge(zero, before[4], g, esize));
			require(vec(cpu, 8) == replicate(cpu.registers[7], esize));
			require(vec(cpu, 9) == replicate(cpu.sp, esize));
			require(vec(cpu, 10) == merge(before[10],
				replicate(cpu.registers[7], esize), g, esize));
			require(vec(cpu, 11) == merge(before[11],
				replicate(cpu.sp, esize), g, esize));
			require(vec(cpu, 12) == merge(before[12],
				replicate(element(before[4], esize, 0), esize), g,
				esize));
			require(vec(cpu, 13) == merge(before[14], before[4], g,
				esize));
		}
		/* DUP and CPY of an immediate, shifted where the size has
		 * room. */
		for (const unsigned shift : {0U, 1U}) {
			if (size == 0U && shift == 1U)
				continue;
			for (const unsigned imm : {0x00U, 0x01U, 0x7fU, 0x80U,
				     0xffU, 0x5aU}) {
				const auto field = (shift << 13) | (imm << 5);
				const std::array immediates{
					0x2538'c000U | s | field | 1U,
					0x0510'4000U | s | (3U << 16) | field | 2U,
					0x0510'0000U | s | (3U << 16) | field | 4U,
					svc};
				Block constant{immediates};
				arm64::CpuState cpu{};
				const auto g = random.predicate();
				set_pred(cpu, 3, g);
				const auto two = random.bytes();
				set_vec(cpu, 2, two);
				set_vec(cpu, 4, random.bytes());
				constant.run(cpu);
				const auto value = static_cast<std::uint64_t>(
					static_cast<std::int64_t>(
						static_cast<std::int8_t>(imm)) <<
					(shift * 8U));
				const auto replicated = replicate(value, esize);
				require(vec(cpu, 1) == replicated);
				require(vec(cpu, 2) == merge(two, replicated, g,
					esize));
				require(vec(cpu, 4) == merge(zero, replicated, g,
					esize));
			}
		}
	}
	/* Assembler vectors for DUPM, FDUP and FCPY. */
	struct Constant final {
		std::uint32_t word;
		unsigned esize;
		std::uint64_t value;
	};
	for (const auto &constant : {
		     Constant{0x05c0'00e0U, 4, 0xff},
		     Constant{0x05c0'81e1U, 4, 0xffff'0000U},
		     Constant{0x05c0'0402U, 2, 0x1},
		     Constant{0x05c0'3663U, 1, 0x3c},
		     Constant{0x05c0'40c4U, 4, 0x7f00'0000U},
		     Constant{0x2579'cc05U, 2, 0x3800},
		     Constant{0x25f9'd006U, 8, 0xc000'0000'0000'0000ULL}}) {
		const auto cpu = after(constant.word, [](arm64::CpuState &) {});
		require(vec(cpu, constant.word & 31U) ==
			replicate(constant.value, constant.esize));
	}
	{
		/* fmov z2.d, p3/m, #1.0 */
		Bytes two{};
		std::ranges::fill(two, 0x11);
		const auto cpu = after(0x05d3'ce02U, [&](arm64::CpuState &state) {
			set_pred(state, 3, 0x0100);
			set_vec(state, 2, two);
		});
		auto expected = two;
		set_element(expected, 8, 1, 0x3ff0'0000'0000'0000ULL);
		require(vec(cpu, 2) == expected);
	}
	/* DUP Zd.T, Zn.T[imm] for every element size and index; past the
	 * vector, zero. */
	for (unsigned field = 1; field < 128U; ++field) {
		if ((field & 31U) == 0U)
			continue;
		const unsigned lowest = static_cast<unsigned>(
			std::countr_zero(field & 31U));
		const unsigned esize = 1U << lowest;
		const unsigned index = field >> (lowest + 1U);
		const auto word = 0x0520'2000U | ((field >> 5) << 22) |
			((field & 31U) << 16) | (2U << 5) | 1U;
		const auto source = Random{field}.bytes();
		const auto cpu = after(word, [&](arm64::CpuState &state) {
			set_vec(state, 2, source);
		});
		Bytes expected{};
		if ((index + 1U) * esize <= 16U)
			for (unsigned byte = 0; byte < 16U; ++byte)
				expected[byte] = source[index * esize +
					byte % esize];
		require(vec(cpu, 1) == expected);
	}
	/* ORR, AND, EOR and BIC (vectors). */
	const std::array logical{0x0460'3000U | (3U << 16) | (2U << 5) | 1U,
		0x0420'3000U | (3U << 16) | (2U << 5) | 4U,
		0x04a0'3000U | (3U << 16) | (2U << 5) | 5U,
		0x04e0'3000U | (3U << 16) | (2U << 5) | 6U, svc};
	Block block{logical};
	for (int sample = 0; sample < 100; ++sample) {
		arm64::CpuState cpu{};
		const auto a = random.bytes();
		const auto b = random.bytes();
		set_vec(cpu, 2, a);
		set_vec(cpu, 3, b);
		block.run(cpu);
		for (unsigned byte = 0; byte < 16U; ++byte) {
			require(vec(cpu, 1)[byte] == (a[byte] | b[byte]));
			require(vec(cpu, 4)[byte] == (a[byte] & b[byte]));
			require(vec(cpu, 5)[byte] == (a[byte] ^ b[byte]));
			require(vec(cpu, 6)[byte] ==
				static_cast<std::uint8_t>(a[byte] & ~b[byte]));
		}
	}
}

/* The WHILE* pseudocode: operand1 steps towards operand2 in rsize bits,
 * wrapping, and the elements are active until the comparison first
 * fails; the decrementing forms fill from the last element. */
[[nodiscard]] std::uint16_t while_reference(While op, std::uint64_t x,
	std::uint64_t y, unsigned esize, bool sf)
{
	const unsigned rsize = sf ? 64U : 32U;
	const auto mask = sf ? ~std::uint64_t{0} : 0xffff'ffffULL;
	const auto signed_value = [&](std::uint64_t value) {
		return sf ? static_cast<std::int64_t>(value) :
			static_cast<std::int64_t>(static_cast<std::int32_t>(
				static_cast<std::uint32_t>(value)));
	};
	const bool is_signed = op == While::ge || op == While::gt ||
		op == While::lt || op == While::le;
	const bool decrement = op == While::ge || op == While::gt ||
		op == While::hs || op == While::hi;
	auto operand1 = x & mask;
	const auto operand2 = y & mask;
	const auto holds = [&] {
		if (is_signed) {
			const auto a = signed_value(operand1);
			const auto b = signed_value(operand2);
			switch (op) {
			case While::ge: return a >= b;
			case While::gt: return a > b;
			case While::lt: return a < b;
			default: return a <= b;
			}
		}
		switch (op) {
		case While::hs: return operand1 >= operand2;
		case While::hi: return operand1 > operand2;
		case While::lo: return operand1 < operand2;
		default: return operand1 <= operand2;
		}
	};
	(void)rsize;
	const unsigned elements = 16U / esize;
	std::uint16_t result = 0;
	bool last = true;
	for (unsigned step = 0; step < elements; ++step) {
		const unsigned index = decrement ? elements - 1U - step : step;
		last = last && holds();
		if (last)
			result = static_cast<std::uint16_t>(result |
				(1U << (index * esize)));
		operand1 = (decrement ? operand1 - 1U : operand1 + 1U) & mask;
	}
	return result;
}

[[nodiscard]] std::uint16_t conflict_reference(bool read_write,
	std::uint64_t x, std::uint64_t y, unsigned esize)
{
	/* diff, rounded towards minus infinity: (y - x) for WHILEWR, |y - x|
	 * for WHILERW, in elements. */
	bool all = false;
	std::uint64_t distance = 0;
	if (y >= x) {
		distance = (y - x) / esize;
		all = distance == 0U;
	} else if (read_write) {
		distance = (x - y) / esize;
		all = distance == 0U;
	} else {
		all = true;
	}
	std::uint16_t result = 0;
	for (unsigned index = 0; index < 16U / esize; ++index)
		if (all || index < distance)
			result = static_cast<std::uint16_t>(result |
				(1U << (index * esize)));
	return result;
}

/* WHILE* at every size, both widths, across the wrap and the extremes. */
void while_forms()
{
	std::vector<std::uint64_t> values{0, 1, 2, 3, 7, 8, 15, 16, 17, 100,
		0x7fff'fffeU, 0x7fff'ffffU, 0x8000'0000U, 0x8000'0001U,
		0xffff'fff0U, 0xffff'fffeU, 0xffff'ffffU,
		0x7fff'ffff'ffff'fff0ULL, 0x7fff'ffff'ffff'fffeULL,
		0x7fff'ffff'ffff'ffffULL, 0x8000'0000'0000'0000ULL,
		0x8000'0000'0000'0001ULL, 0xffff'ffff'ffff'fff0ULL,
		0xffff'ffff'ffff'fffeULL, 0xffff'ffff'ffff'ffffULL};
	Random random{6};
	for (int sample = 0; sample < 12; ++sample)
		values.push_back(random.next());
	/* Upper halves the 32-bit forms ignore. */
	const auto garbage = [&](std::uint64_t value, bool sf) {
		return sf ? value : (value & 0xffff'ffffU) |
			(random.next() << 32);
	};
	for (unsigned op = 0; op < 8U; ++op) {
		for (const bool sf : {false, true}) {
			for (unsigned size = 0; size < 4U; ++size) {
				const unsigned esize = 1U << size;
				const std::array code{while_compare(
					static_cast<While>(op), 1, 2, 3, size, sf),
					svc};
				Block block{code};
				for (const auto x : values) {
					for (auto y : values) {
						for (const int delta : {0, 1, -1,
							     5, -5}) {
							y += static_cast<std::uint64_t>(
								delta);
							arm64::CpuState cpu{};
							cpu.registers[2] =
								garbage(x, sf);
							cpu.registers[3] =
								garbage(y, sf);
							block.run(cpu);
							const auto expected =
								while_reference(
								static_cast<While>(
									op), x, y,
								esize, sf);
							require(pred(cpu, 1) ==
								expected);
							require(cpu.nzcv ==
								pred_test(0xffff,
								expected, esize));
							y -= static_cast<std::uint64_t>(
								delta);
						}
					}
				}
			}
		}
	}
	for (const bool read_write : {false, true}) {
		for (unsigned size = 0; size < 4U; ++size) {
			const unsigned esize = 1U << size;
			const std::array code{while_conflict(read_write, 1, 2,
				3, size), svc};
			Block block{code};
			for (const auto x : values) {
				for (const std::int64_t delta : {0LL, 1LL, -1LL,
					     7LL, -7LL, 8LL, -8LL, 15LL, -15LL,
					     16LL, -16LL, 63LL, 64LL, 127LL,
					     128LL, -128LL, 1000LL}) {
					const auto y = x + static_cast<std::uint64_t>(
						delta);
					arm64::CpuState cpu{};
					cpu.registers[2] = x;
					cpu.registers[3] = y;
					block.run(cpu);
					const auto expected = conflict_reference(
						read_write, x, y, esize);
					require(pred(cpu, 1) == expected);
					require(cpu.nzcv == pred_test(0xffff,
						expected, esize));
				}
			}
		}
	}
}

/* Laid out as the executor's link record, as the link tests do it. */
struct LinkTarget final {
	explicit LinkTarget(std::uint64_t pc) noexcept
		: fallback{address(uml_arm64_gadget_link_exit)},
		  link{.stream = &fallback, .instruction_count = 0,
			  .guest_pc = pc}
	{
	}

	std::uintptr_t fallback;
	gadgets::BlockLink link;
};

/* WHILE* followed by B.cond on one NZCV bit runs as one entry whose
 * fall-through is translated inline; the other conditions branch as
 * before. Both outcomes of each, with the retired count. */
void fused_while_branch()
{
	struct Condition final {
		unsigned code;
		bool fused;
		bool (*taken)(std::uint32_t);
	};
	const std::array conditions{
		Condition{0x0, true, [](std::uint32_t f) {
			return (f & 0x4000'0000U) != 0U; }},
		Condition{0x1, true, [](std::uint32_t f) {
			return (f & 0x4000'0000U) == 0U; }},
		Condition{0x4, true, [](std::uint32_t f) {
			return (f & 0x8000'0000U) != 0U; }},
		Condition{0x5, true, [](std::uint32_t f) {
			return (f & 0x8000'0000U) == 0U; }},
		Condition{0x2, false, [](std::uint32_t f) {
			return (f & 0x2000'0000U) != 0U; }},
		Condition{0x3, false, [](std::uint32_t f) {
			return (f & 0x2000'0000U) == 0U; }},
	};
	const std::uint64_t target = code_base + 0x400;
	for (const auto &condition : conditions) {
		for (const bool sf : {false, true}) {
			const auto word = while_compare(While::lo, 1, 2, 3, 2, sf);
			/* B.cond from code_base + 4 to target. */
			const std::array code{word,
				0x5400'0000U | (0xffU << 5) | condition.code,
				movz(5, 1), svc};
			LinkTarget link{target};
			const auto built = build(code, sve_host(), 0,
				[&](std::uint64_t pc) {
					require(pc == target);
					return &link.link;
				});
			const auto *row = arm64::detail::find_sme_encoding(word);
			const auto &form = gadgets::sve::forms[
				gadgets::sve::row_forms[static_cast<std::size_t>(
					row - arm64::detail::sme_encodings.data())]];
			auto variant = sf ? 1U : 0U;
			if (condition.fused)
				variant += 2U + 2U * static_cast<unsigned>(
					std::ranges::find(
						gadgets::sve::fused_conditions,
						condition.code) -
					gadgets::sve::fused_conditions.begin());
			require(variant < form.variants);
			require(built.program.words()[0] == emitted_entry(
				uml_arm64_gadget_sve_generated_entries[
					form.first_entry + variant]));
			for (const auto [x, y] : {std::pair{0ULL, 0ULL},
				     std::pair{0ULL, 2ULL}, std::pair{0ULL, 100ULL},
				     std::pair{5ULL, 3ULL}}) {
				arm64::CpuState cpu{};
				cpu.registers[2] = x;
				cpu.registers[3] = y;
				cpu.pc = code_base;
				uml::memory::Tlb tlb;
				std::size_t executed = 0;
				const auto event = gadgets::execute(built.program,
					cpu, tlb, executed);
				const auto expected = while_reference(While::lo,
					x, y, 4, sf);
				const auto flags = pred_test(0xffff, expected, 4);
				require(pred(cpu, 1) == expected);
				require(cpu.nzcv == flags);
				if (condition.taken(flags)) {
					require(cpu.pc == target);
					require(executed == 2U);
					require(cpu.registers[5] == 0U);
					require(event != EngineEvent::system_call);
				} else {
					require(event == EngineEvent::system_call);
					require(executed == code.size());
					require(cpu.registers[5] == 1U);
				}
			}
		}
	}
}

/* A program holding an SVE word accesses the FP state and marks the task
 * live, entered from C++ or linked to; one without leaves it alone. */
void live_marker_and_fp_access()
{
	std::array<arm64::VectorRegister, 32> deferred{};
	for (std::size_t n = 0; n < deferred.size(); ++n)
		std::ranges::fill(deferred[n].bytes, static_cast<std::byte>(n + 1));
	{
		const std::array code{ptrue(0, 0, 31), svc};
		const auto built = build(code, sve_host());
		require(built.program.holds_sve() &&
			built.program.accesses_fp_state());
		arm64::CpuState cpu{};
		cpu.defer_fp_load(deferred.data(), 0, 0,
			arm64::fpcr_writable_mask);
		uml::memory::Tlb tlb;
		require(gadgets::execute(built.program, cpu, tlb) ==
			EngineEvent::system_call);
		require(cpu.sve_live == 1U && cpu.fp_loaded && cpu.fp_dirty);
		require(cpu.vectors[7].bytes[0] == std::byte{8});
		require(pred(cpu, 0) == 0xffffU);
	}
	{
		const std::array code{movz(0, 1), svc};
		const auto built = build(code, sve_host());
		require(!built.program.holds_sve());
		arm64::CpuState cpu{};
		uml::memory::Tlb tlb;
		require(gadgets::execute(built.program, cpu, tlb) ==
			EngineEvent::system_call);
		require(cpu.sve_live == 0U);
	}
	{
		/* The SVE link entry, in front of a stream as the executor
		 * stores it, reached through a branch's link. */
		const std::uint64_t target = code_base + 0x100;
		const std::array successor_code{ptrue(2, 0, 31), svc};
		gadgets::BuildOptions options{.host_capabilities = sve_host()};
		const auto successor = gadgets::Builder{}.build(target,
			successor_code, {}, std::pmr::get_default_resource(), {},
			options);
		require(successor.program.holds_sve());
		std::vector<std::uintptr_t> stream{
			address(uml_arm64_gadget_sve_link_entry)};
		const auto words = successor.program.words();
		stream.insert(stream.end(), words.begin(), words.end());
		gadgets::BlockLink link{.stream = stream.data(),
			.instruction_count = successor.program.instruction_count(),
			.guest_pc = target};
		const std::array code{0x1400'0040U /* b .+0x100 */};
		const auto first = build(code, sve_host(), 0,
			[&](std::uint64_t) { return &link; });
		require(!first.program.holds_sve());
		arm64::CpuState cpu{};
		cpu.defer_fp_load(deferred.data(), 0, 0,
			arm64::fpcr_writable_mask);
		uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			EngineEvent::system_call);
		require(executed == 3U);
		require(cpu.sve_live == 1U && cpu.fp_loaded && cpu.fp_dirty);
		require(cpu.vectors[9].bytes[3] == std::byte{10});
		require(pred(cpu, 2) == 0xffffU);
	}
	{
		/* Through the executor, which stores the block behind its
		 * link entry: once the branch is linked, the block holding
		 * the SVE word is entered from the gadgets, not from C++. */
		const std::array code{0x1400'0002U /* b .+8 */, svc,
			ptrue(3, 0, 31), svc};
		Machine machine{code, sve_host()};
		for (int round = 0; round < 3; ++round) {
			machine.cpu.sve_live = 0;
			set_pred(machine.cpu, 3, 0);
			require(machine.run() == EngineEvent::system_call);
			require(machine.cpu.sve_live == 1U);
			require(pred(machine.cpu, 3) == 0xffffU);
			require(machine.retired == 3U);
		}
		require(machine.blocks == 1U);
	}
}

/* Guest memory for a reference run of the library that touches none. */
class NoMemory final : public arm64::detail::SveMemory {
public:
	std::byte *translate(std::uint64_t,
		uml::memory::MemoryAccess) noexcept override
	{
		return nullptr;
	}
	void stored(std::byte *, std::size_t) noexcept override {}
};

/* The state the semantic library leaves for `word` on `cpu`. */
[[nodiscard]] arm64::CpuState library_after(arm64::CpuState cpu,
	std::uint32_t word, arm64::detail::SveEvent expected =
		arm64::detail::SveEvent::retired,
	std::uint32_t *syndrome = nullptr)
{
	const auto *row = arm64::detail::decode_sve_non_streaming(word,
		arm64::detail::implemented_sve_features(sve_host()));
	require(row != nullptr);
	NoMemory memory;
	const auto result = arm64::detail::execute_sve(cpu, word, *row, memory);
	require(result.event == expected);
	if (syndrome != nullptr)
		*syndrome = result.syndrome;
	return cpu;
}

void require_same_registers(const arm64::CpuState &actual,
	const arm64::CpuState &expected)
{
	for (unsigned n = 0; n < 32; ++n)
		require(actual.vectors[n].bytes == expected.vectors[n].bytes);
	require(actual.sve_p == expected.sve_p);
	require(actual.sve_ffr == expected.sve_ffr);
	require(actual.nzcv == expected.nzcv);
}

/* Words no generated gadget lowers run through the semantic library, in
 * the middle of a block: the state after them is the library's on the
 * same state, NZCV included, and behind a native FP gadget the cumulative
 * FPSR flags of both carry over, and an FPCR write in the block governs
 * the library's rounding. A load reaches guest memory through the
 * TLB on a page and across two, and reports a fault at its own PC. While
 * an FPCR trap enable is set the executor runs the word through the
 * library itself, which decides the trap. */
void library_words()
{
	Random random{0x5ec7'0001U};
	const auto randomize = [&](arm64::CpuState &cpu) {
		for (unsigned n = 0; n < 8; ++n) {
			Bytes bytes{};
			for (auto &byte : bytes)
				byte = static_cast<std::uint8_t>(random.next());
			set_vec(cpu, n, bytes);
		}
		set_pred(cpu, 1, static_cast<std::uint16_t>(random.next()));
		cpu.nzcv = nzcv_v;
	};
	for (const auto word : {0x0422'0020U /* add z0.b, z1.b, z2.b */,
		     0x2403'a440U /* cmpeq p0.b, p1/z, z2.b, z3.b */}) {
		for (int round = 0; round < 64; ++round) {
			arm64::CpuState initial{};
			randomize(initial);
			const auto cpu = after(word, [&](arm64::CpuState &state) {
				state = initial;
			});
			require_same_registers(cpu,
				library_after(initial, word));
		}
	}

	/* fadd s4, s4, s5 activates the native FP environment and raises
	 * IXC; fadd z0.s, z1.s, z2.s then raises OFC through the library. */
	{
		const std::array code{0x1e25'2884U, 0x6582'0020U, svc};
		Block block{code};
		arm64::CpuState cpu{};
		const auto one = std::bit_cast<std::uint32_t>(1.0F);
		const auto tiny = std::bit_cast<std::uint32_t>(0x1p-30F);
		const auto large = std::bit_cast<std::uint32_t>(0x1.fffffep127F);
		Bytes s4{}, s5{}, big{};
		std::memcpy(s4.data(), &one, 4);
		std::memcpy(s5.data(), &tiny, 4);
		for (unsigned lane = 0; lane < 4; ++lane)
			std::memcpy(big.data() + 4 * lane, &large, 4);
		set_vec(cpu, 4, s4);
		set_vec(cpu, 5, s5);
		set_vec(cpu, 1, big);
		set_vec(cpu, 2, big);
		block.run(cpu);
		constexpr std::uint32_t ofc = 1U << 2;
		constexpr std::uint32_t ixc = 1U << 4;
		require((cpu.fpsr & (ofc | ixc)) == (ofc | ixc));
		for (unsigned lane = 0; lane < 4; ++lane) {
			std::uint32_t value = 0;
			std::memcpy(&value, cpu.vectors[0].bytes.data() + 4 * lane,
				4);
			require(value == 0x7f80'0000U);
		}
	}

	/* msr fpcr, x1 to round toward zero behind an active native FP
	 * environment: fadd z0.s through the library rounds 1 + 0.75 ULP
	 * down, where the reset rounding would round it up. */
	{
		const std::array code{0x1e25'2884U, 0xd51b'4401U, 0x6582'0020U,
			svc};
		Block block{code};
		arm64::CpuState cpu{};
		const auto one = std::bit_cast<std::uint32_t>(1.0F);
		const auto small = std::bit_cast<std::uint32_t>(0x1.8p-24F);
		Bytes ones{}, smalls{};
		for (unsigned lane = 0; lane < 4; ++lane) {
			std::memcpy(ones.data() + 4 * lane, &one, 4);
			std::memcpy(smalls.data() + 4 * lane, &small, 4);
		}
		set_vec(cpu, 4, ones);
		set_vec(cpu, 5, smalls);
		set_vec(cpu, 1, ones);
		set_vec(cpu, 2, smalls);
		cpu.registers[1] = 3U << 22;
		block.run(cpu);
		require(cpu.fpcr == 3U << 22);
		for (unsigned lane = 0; lane < 4; ++lane) {
			std::uint32_t value = 0;
			std::memcpy(&value, cpu.vectors[0].bytes.data() + 4 * lane,
				4);
			require(value == one);
		}
		std::uint32_t s4 = 0;
		std::memcpy(&s4, cpu.vectors[4].bytes.data(), 4);
		require(s4 == one + 1U);
	}

	/* ld1b {z0.b}, p0/z, [x1]: within a page, across two, and at an
	 * unmapped address. */
	constexpr std::uint64_t page = DARWIN_UML_GUEST_PAGE_SIZE;
	for (const std::uint64_t offset : {std::uint64_t{0x800}, page - 8U}) {
		const std::array code{ptrue(0, 0, 31), 0xa400'a020U, svc};
		Machine machine{code, sve_host()};
		machine.put64(offset, 0x0706'0504'0302'0100U);
		machine.put64(offset + 8U, 0x0f0e'0d0c'0b0a'0908U);
		machine.cpu.registers[1] = code_base + offset;
		require(machine.run() == EngineEvent::system_call);
		require(machine.retired == 3U);
		for (unsigned index = 0; index < 16; ++index)
			require(machine.cpu.vectors[0].bytes[index] ==
				std::byte{static_cast<std::uint8_t>(index)});
	}
	{
		const std::array code{ptrue(0, 0, 31), 0xa400'a020U, svc};
		Machine machine{code, sve_host()};
		const std::uint64_t unmapped = code_base + 0x40'0000U;
		machine.cpu.registers[1] = unmapped;
		require(machine.run() == EngineEvent::memory_fault);
		require(machine.cpu.pc == code_base + 4U);
		require(machine.fault_address() == unmapped);
		require(pred(machine.cpu, 0) == 0xffffU);
	}

	/* fadd z0.s, z1.s, z2.s with FPCR.IOE and a signalling NaN: the
	 * library's trap, which leaves Z0 and FPSR as they were. */
	{
		const std::array code{0x6582'0020U, svc};
		Machine machine{code, sve_host()};
		Bytes nan{}, zero{};
		const std::uint32_t signalling = 0x7f80'0001U;
		for (unsigned lane = 0; lane < 4; ++lane)
			std::memcpy(nan.data() + 4 * lane, &signalling, 4);
		set_vec(machine.cpu, 1, nan);
		set_vec(machine.cpu, 2, zero);
		set_vec(machine.cpu, 0, nan);
		machine.cpu.fpcr = 1U << 8;
		std::uint32_t syndrome = 0;
		const auto expected = library_after(machine.cpu, 0x6582'0020U,
			arm64::detail::SveEvent::floating_point_trap, &syndrome);
		require(machine.run() == EngineEvent::floating_point_trap);
		require(machine.cpu.pc == code_base);
		require(machine.floating_point_syndrome() == syndrome);
		require(machine.cpu.vectors[0].bytes ==
			expected.vectors[0].bytes);
		require(machine.cpu.fpsr == expected.fpsr);
	}
}

/* A random element value for a differential run: raw bits, a normal FP16,
 * FP32 or FP64 value near one, a small integer, or an extreme. */
void fill_lanes(Random &random, Bytes &bytes)
{
	const auto kind = random.next() % 6U;
	if (kind == 0U) {
		bytes = random.bytes();
		return;
	}
	static constexpr unsigned sizes[]{2, 2, 4, 8, 1};
	const unsigned esize = sizes[kind - 1U];
	for (unsigned index = 0; index < 16U / esize; ++index) {
		std::uint64_t value = random.next();
		switch (kind) {
		case 1: /* FP16 with exponent 13-17. */
			value = (value & 0x83ffU) | ((13U + value % 5U) << 10);
			break;
		case 2: /* FP16 integers and extremes. */
			value = std::array<std::uint64_t, 6>{0x0000U, 0x8000U,
				0x3c00U, 0x7bffU, 0x0001U, 0xc500U}[value % 6U];
			break;
		case 3: /* FP32 with exponent 124-130. */
			value = (value & 0x807f'ffffU) |
				((124U + value % 7U) << 23);
			break;
		case 4: /* FP64 with exponent 1020-1026. */
			value = (value & 0x800f'ffff'ffff'ffffULL) |
				((1020ULL + value % 7U) << 52);
			break;
		default: /* Small integers and byte extremes. */
			value = std::array<std::uint64_t, 6>{0x00U, 0x01U, 0x7fU,
				0x80U, 0xffU, value % 9U}[value % 6U];
			break;
		}
		for (unsigned byte = 0; byte < esize; ++byte)
			bytes[index * esize + byte] =
				static_cast<std::uint8_t>(value >> (8U * byte));
	}
}

/* Random architectural state for a differential run. FPCR takes a random
 * rounding mode, FZ, DN and FZ16, with no trap enable. */
[[nodiscard]] arm64::CpuState random_state(Random &random)
{
	arm64::CpuState cpu{};
	for (auto &value : cpu.registers) {
		const auto raw = random.next();
		switch (raw % 4U) {
		case 0:
			value = raw >> 58;
			break;
		case 1:
			value = ~(raw >> 58);
			break;
		case 2:
			value = (raw & 1U) != 0U ? 0x7fff'ffff'ffff'ffffULL - (raw >> 60) :
				0x8000'0000'0000'0000ULL + (raw >> 60);
			break;
		default:
			value = raw;
			break;
		}
	}
	cpu.sp = random.next() & ~std::uint64_t{15};
	for (unsigned n = 0; n < 32; ++n) {
		Bytes bytes{};
		fill_lanes(random, bytes);
		set_vec(cpu, n, bytes);
	}
	for (unsigned n = 0; n < 16; ++n)
		set_pred(cpu, n, random.predicate());
	cpu.sve_ffr = arm64::sve_predicate_mask(random.predicate());
	cpu.sve_live = 1;
	cpu.nzcv = static_cast<std::uint32_t>(random.next() & 0xfU) << 28;
	cpu.fpcr = static_cast<std::uint32_t>(random.next()) &
		((3U << 22) | (1U << 24) | (1U << 25) | (1U << 19));
	cpu.fpsr = static_cast<std::uint32_t>(random.next()) & 0x0800'009fU;
	return cpu;
}

/* Whether the gadget engine leaves `expected` after running `code`, which
 * ends in an SVC, from `initial`; reports `word` otherwise. */
void require_library_state(std::span<const std::uint32_t> code,
	const arm64::CpuState &initial, const arm64::CpuState &expected,
	std::uint32_t word, std::size_t index, bool fp_active)
{
	const auto host = sve_host();
	const auto built = build(code, host);
	arm64::CpuState cpu = initial;
	cpu.pc = code_base;
	uml::memory::Tlb tlb;
	std::size_t executed = 0;
	const auto event = gadgets::execute(built.program, cpu, tlb, executed);
	gadgets::restore_native_fp_environment(cpu);
	bool same = event == EngineEvent::system_call &&
		executed == code.size() &&
		cpu.registers == expected.registers &&
		cpu.sp == expected.sp && cpu.sve_p == expected.sve_p &&
		cpu.sve_ffr == expected.sve_ffr && cpu.nzcv == expected.nzcv &&
		cpu.fpsr == expected.fpsr && cpu.fpcr == expected.fpcr &&
		cpu.sve_live == expected.sve_live;
	unsigned differing = 32;
	for (unsigned n = 0; n < 32 && same; ++n)
		if (cpu.vectors[n].bytes != expected.vectors[n].bytes) {
			differing = n;
			same = false;
		}
	if (same)
		return;
	const auto &row = arm64::detail::sme_encodings[index];
	std::fprintf(stderr, "word %08x (row %zu, SVE-%04u%s): the gadget "
		"leaves another state than the library (event %d, executed "
		"%zu, Z%u, nzcv %08x/%08x, fpsr %08x/%08x, fpcr %08x)\n", word,
		index, row.form & 0x7fffU, fp_active ? ", FP active" : "",
		static_cast<int>(event), executed, differing, cpu.nzcv,
		expected.nzcv, cpu.fpsr, expected.fpsr, initial.fpcr);
	if (differing < 32U) {
		std::fprintf(stderr, "  Z%u gadget ", differing);
		for (const auto byte : cpu.vectors[differing].bytes)
			std::fprintf(stderr, "%02x", static_cast<unsigned>(byte));
		std::fprintf(stderr, "\n  Z%u library ", differing);
		for (const auto byte : expected.vectors[differing].bytes)
			std::fprintf(stderr, "%02x", static_cast<unsigned>(byte));
		std::fprintf(stderr, "\n");
	}
	require(false);
}

/* Every generated form agrees with the semantic library: random words of
 * each register-only row a generated gadget runs, each alone in a block on
 * a random state, leave the library's registers, P, FFR, NZCV and FPSR;
 * and so does each behind an FP instruction that leaves the guest's
 * environment active in the host's FP registers. */
void generated_forms_match_the_library()
{
	/* fadd s31, s31, s31 on 1.0: exact, so it raises nothing. */
	constexpr std::uint32_t activate = 0x1e3f'2bffU;
	Random random{0x5ec7'0002U};
	const auto host = sve_host();
	std::size_t runs = 0;
	std::size_t rows = 0;
	for (std::size_t index = 0; index < arm64::detail::sme_encodings.size();
	     ++index) {
		const auto &row = arm64::detail::sme_encodings[index];
		if (gadgets::sve::row_forms[index] == gadgets::sve::no_form ||
		    row.memory != 0U)
			continue;
		++rows;
		for (int sample = 0; sample < 48; ++sample) {
			const auto word = row.value |
				(static_cast<std::uint32_t>(random.next()) &
				 ~row.mask);
			const auto decoded = gadgets::decode_sve_word(word, host,
				0);
			if (decoded.kind != gadgets::SveWord::implemented ||
			    decoded.row != &row)
				continue;
			auto initial = random_state(random);
			const std::array alone{word, svc};
			require_library_state(alone, initial,
				library_after(initial, word), word, index, false);
			Bytes one{}, two{};
			const auto one_bits = std::bit_cast<std::uint32_t>(1.0F);
			const auto two_bits = std::bit_cast<std::uint32_t>(2.0F);
			std::memcpy(one.data(), &one_bits, 4);
			std::memcpy(two.data(), &two_bits, 4);
			set_vec(initial, 31, one);
			auto activated = initial;
			set_vec(activated, 31, two);
			const std::array behind{activate, word, svc};
			require_library_state(behind, initial,
				library_after(activated, word), word, index, true);
			runs += 2;
		}
	}
	require(rows > 0U && runs > rows);
	std::printf("%zu generated-form runs over %zu rows match the "
		"library\n", runs, rows);
}

/* Guest memory for a reference run of the library: the machine's mapped
 * pages, copied. */
class CopiedMemory final : public arm64::detail::SveMemory {
public:
	explicit CopiedMemory(std::vector<std::byte> &bytes) : bytes_{bytes} {}

	std::byte *translate(std::uint64_t address,
		uml::memory::MemoryAccess) noexcept override
	{
		if (address < code_base || address - code_base >= bytes_.size())
			return nullptr;
		return bytes_.data() + (address - code_base);
	}
	void stored(std::byte *, std::size_t) noexcept override {}

private:
	std::vector<std::byte> &bytes_;
};

/* Every generated memory form agrees with the semantic library: random
 * words of each contiguous row, each alone in a block on a two-page mapping
 * of random bytes and again behind an FP instruction that leaves the
 * guest's environment active, from bases inside a page, at a page's end,
 * across into the unmapped page after them, and tagged under TBI, leave
 * the library's state, event, fault address and memory. Predicates are
 * all-true half the time, so both a store's fast path and the library's
 * partial store run. */
void memory_forms_match_the_library()
{
	constexpr std::size_t page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::size_t mapped = mapped_pages * page;
	/* fadd s31, s31, s31 on 1.0, as in generated_forms_match_the_library. */
	constexpr std::uint32_t activate = 0x1e3f'2bffU;
	Random random{0x5ec7'0003U};
	const auto host = sve_host();
	std::size_t runs = 0;
	std::size_t faults = 0;
	std::size_t rows = 0;
	for (std::size_t index = 0; index < arm64::detail::sme_encodings.size();
	     ++index) {
		const auto &row = arm64::detail::sme_encodings[index];
		if (gadgets::sve::row_forms[index] == gadgets::sve::no_form ||
		    row.memory == 0U)
			continue;
		const auto &memory = arm64::detail::sme_encoding_memory[
			row.memory - 1U];
		const auto *const operands =
			arm64::detail::sme_encoding_operands_of(row);
		++rows;
		for (int sample = 0; sample < 12; ++sample) {
			const auto word = row.value |
				(static_cast<std::uint32_t>(random.next()) &
				 ~row.mask);
			const auto decoded = gadgets::decode_sve_word(word, host,
				0);
			if (decoded.kind != gadgets::SveWord::implemented ||
			    decoded.row != &row)
				continue;
			std::vector<std::byte> bytes(mapped);
			for (std::size_t offset = 0; offset < mapped; offset += 8) {
				const auto value = random.next();
				std::memcpy(bytes.data() + offset, &value, 8);
			}
			auto initial = random_state(random);
			for (unsigned n = 0; n < 16; ++n)
				if (random.next() % 2U == 0U)
					set_pred(initial, n, 0xffffU);
			/* The base: mostly inside the second page, sometimes at
			 * the end of a page, or unmapped. */
			std::uint64_t base = code_base + page +
				random.next() % (page - 64U);
			switch (random.next() % 8U) {
			case 0:
				base = code_base + page - random.next() % 48U;
				break;
			case 1:
				base = code_base + mapped - random.next() % 48U;
				break;
			case 2:
				base = code_base + mapped + random.next() % page;
				break;
			default:
				break;
			}
			const auto base_register = arm64::detail::sme_operand_number(
				operands[memory.base_operand], word);
			if (memory.index_operand != arm64::detail::sme_no_operand) {
				const auto index_register =
					arm64::detail::sme_operand_number(
						operands[memory.index_operand], word);
				initial.registers[index_register] =
					random.next() % 4U;
			}
			if (random.next() % 4U == 0U) {
				initial.tbi_enabled = true;
				base |= (random.next() & 0xffU) << 56;
			}
			if (base_register == 31U)
				initial.sp = base & ~std::uint64_t{15};
			else
				initial.registers[base_register] = base;
			Bytes one{}, two{};
			const auto one_bits = std::bit_cast<std::uint32_t>(1.0F);
			const auto two_bits = std::bit_cast<std::uint32_t>(2.0F);
			std::memcpy(one.data(), &one_bits, 4);
			std::memcpy(two.data(), &two_bits, 4);
			set_vec(initial, 31, one);

			for (const bool behind : {false, true}) {
				const std::array alone{word, svc};
				const std::array activated{activate, word, svc};
				const std::span<const std::uint32_t> code = behind ?
					std::span<const std::uint32_t>{activated} :
					std::span<const std::uint32_t>{alone};
				std::vector<std::byte> before = bytes;
				std::memcpy(before.data(), code.data(),
					code.size_bytes());
				Machine machine{code, host};
				machine.put(0, before);
				machine.cpu = initial;
				const auto event = machine.run();
				std::vector<std::byte> after(mapped);
				machine.get(0, after);

				auto expected = initial;
				if (behind)
					set_vec(expected, 31, two);
				CopiedMemory reference{before};
				const auto result = arm64::detail::execute_sve(
					expected, word, row, reference);
				const bool faulted = result.event ==
					arm64::detail::SveEvent::memory_fault;
				const auto at = code_base + (behind ? 4U : 0U);
				faults += faulted ? 1U : 0U;
				bool same = event == (faulted ?
						EngineEvent::memory_fault :
						EngineEvent::system_call) &&
					(!faulted || (machine.cpu.pc == at &&
						machine.fault_address() ==
							result.fault_address)) &&
					result.event !=
						arm64::detail::SveEvent::fatal &&
					machine.cpu.registers == expected.registers &&
					machine.cpu.sp == expected.sp &&
					machine.cpu.sve_p == expected.sve_p &&
					machine.cpu.sve_ffr == expected.sve_ffr &&
					machine.cpu.nzcv == expected.nzcv &&
					machine.cpu.fpsr == expected.fpsr &&
					after == before;
				for (unsigned n = 0; n < 32 && same; ++n)
					same = machine.cpu.vectors[n].bytes ==
						expected.vectors[n].bytes;
				if (!same) {
					std::fprintf(stderr, "word %08x (row %zu, "
						"SVE-%04u%s) at %#llx: the gadget "
						"leaves another state than the "
						"library (event %d, library %d, "
						"fault %#llx/%#llx, fpsr %08x/%08x, "
						"memory %s)\n", word, index,
						row.form & 0x7fffU,
						behind ? ", FP active" : "",
						static_cast<unsigned long long>(base),
						static_cast<int>(event),
						static_cast<int>(result.event),
						static_cast<unsigned long long>(
							machine.fault_address()),
						static_cast<unsigned long long>(
							result.fault_address),
						machine.cpu.fpsr, expected.fpsr,
						after == before ? "same" : "differs");
					require(false);
				}
				++runs;
			}
		}
	}
	require(rows > 0U && runs > rows && faults > 0U);
	std::printf("%zu memory-form runs over %zu rows match the library, "
		"%zu of them faulting\n", runs, rows, faults);
}

/* Words the virtual CPU does not implement end the block: without SVE; a
 * row whose feature the host lacks; a reserved immediate, of a generated
 * form (DUPM, DUP) and of one the library runs (AND); a word of no row,
 * and an SME row, without SME. */
void unimplemented_words()
{
	struct Case final {
		std::uint32_t word;
		HostCapabilities host;
	};
	/* A word of the SVE space no row matches. */
	std::uint32_t unallocated = 0;
	for (std::uint32_t low = 0; low < 0x0200'0000U; low += 0x1001U) {
		const auto word = 0x0400'0000U | low;
		if (arm64::detail::find_sme_encoding(word) == nullptr) {
			unallocated = word;
			break;
		}
	}
	require(unallocated != 0U);
	const std::array cases{
		Case{ptrue(0, 0, 31), sve_host(false)},
		Case{0x4522'e000U /* aese z0.b, z0.b, z0.b */, sve_host()},
		Case{0x05c0'0000U | (0x103fU << 5) /* dupm, reserved */,
			sve_host()},
		Case{0x0580'0000U | (0x103fU << 5) /* and, reserved */,
			sve_host()},
		Case{0x2538'e000U /* dup z0.b, #0, lsl #8 */, sve_host()},
		Case{unallocated, sve_host()},
		Case{0xc008'00ffU /* zero {za} */, sve_host()},
	};
	for (const auto &item : cases) {
		const std::array code{movz(0, 1), item.word, svc};
		arm64::CpuState cpu{};
		std::size_t executed = 0;
		require(run_illegal(code, item.host, executed, cpu) ==
			EngineEvent::illegal_instruction);
		require(cpu.pc == code_base + 4U);
		require(cpu.registers[0] == 1U);
		require(cpu.sve_live == 0U);
	}
}

/* A word of a translated program that names an entry outside its mode:
 * for SVCR.SM = 1 one of the SVE part, for SM = 0 a streaming SVE entry or
 * a streaming twin. The entry is its offset from the SVE part, which no
 * address-space slide changes. */
struct EntryFinding final {
	unsigned host;
	unsigned mode;
	std::uint32_t word;
	std::size_t index;
	std::uintptr_t offset;
	auto operator<=>(const EntryFinding &) const = default;
};

struct EntryScan final {
	std::set<EntryFinding> findings;
	std::size_t selected_sve{};
};

/* Every such word of the programs translated from sampled words on hosts
 * with SVE alone and with SVE and SME, in each SVCR mode the host has, and
 * how many words at SM = 0 name the SVE part. Program words are untyped:
 * an operand word, such as a packed immediate or an ADRP result, can
 * equal an entry's address under this process's slide, and is found too;
 * under another slide it names another offset. */
[[nodiscard]] EntryScan scan_entries()
{
	const auto sve_begin = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_sve_section_begin);
	const auto sve_end = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_sve_section_end);
	const auto twins_begin = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_sm_twins_begin);
	const auto twins_end = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_sm_twins_end);
	std::vector<std::uintptr_t> streaming{
		uml_arm64_gadget_sm_generated_entries,
		uml_arm64_gadget_sm_generated_entries_end};
	streaming.insert(streaming.end(), uml_arm64_gadget_sm_memory_entries,
		uml_arm64_gadget_sm_memory_entries_end);
	std::ranges::sort(streaming);
	require(!streaming.empty() && sve_end > sve_begin);
	const std::span sve_entries{uml_arm64_gadget_sve_generated_entries,
		uml_arm64_gadget_sve_generated_entries_end};
	require(sve_entries.size() == gadgets::sve::entry_count);
	for (const auto entry : sve_entries)
		require(entry >= sve_begin && entry < sve_end);

	std::vector<std::uint32_t> words;
	Random random{7};
	for (int sample = 0; sample < 20'000; ++sample)
		words.push_back(static_cast<std::uint32_t>(random.next()));
	for (const auto &row : arm64::detail::sme_encodings)
		for (int sample = 0; sample < 3; ++sample)
			words.push_back(row.value |
				(static_cast<std::uint32_t>(random.next()) &
				 ~row.mask));
	gadgets::BlockLink link{};
	gadgets::IndirectLink site{};
	EntryScan scan;
	unsigned host_index = 0;
	for (const auto &host : {sve_host(), sve_sme_host()}) {
		const bool sme = arm64::implements_sme(host);
		for (const std::uint8_t mode : {0, 1, 2, 3}) {
			if (!sme && mode != 0U)
				continue;
			gadgets::BuildOptions options{.host_capabilities = host};
			options.svcr = mode;
			const bool streaming_mode = (mode & arm64::svcr_sm) != 0U;
			for (const auto word : words) {
				const std::array window{word};
				const auto built = gadgets::Builder{}.build(
					code_base, window,
					[&](std::uint64_t) { return &link; },
					std::pmr::get_default_resource(),
					[&](std::uint64_t) { return &site; },
					options);
				require(!streaming_mode ||
					!built.program.holds_sve());
				const auto stream = built.program.words();
				for (std::size_t index = 0; index < stream.size();
				     ++index) {
					const auto entry = stream[index];
					const bool in_sve = entry >= sve_begin &&
						entry < sve_end;
					const bool outside = streaming_mode ?
						in_sve :
						std::ranges::binary_search(
							streaming, entry) ||
							(entry >= twins_begin &&
							 entry < twins_end);
					if (outside)
						scan.findings.insert({host_index,
							mode, word, index,
							entry - sve_begin});
					if (!streaming_mode && in_sve)
						++scan.selected_sve;
				}
			}
		}
		++host_index;
	}
	return scan;
}

constexpr const char *scan_variable = "UML_GADGET_SVE_ENTRY_SCAN";

/* The findings of this test binary run in a new process, which has its own
 * address-space slide, and that slide's SVE part address. */
[[nodiscard]] std::pair<std::set<EntryFinding>, std::uintptr_t>
findings_in_another_process()
{
	std::array<char, 4096> path{};
	auto size = static_cast<std::uint32_t>(path.size());
	require(_NSGetExecutablePath(path.data(), &size) == 0);
	std::array<int, 2> pipe_ends{};
	require(pipe(pipe_ends.data()) == 0);
	posix_spawn_file_actions_t actions;
	require(posix_spawn_file_actions_init(&actions) == 0);
	require(posix_spawn_file_actions_adddup2(&actions, pipe_ends[1],
			STDOUT_FILENO) == 0);
	require(posix_spawn_file_actions_addclose(&actions, pipe_ends[0]) == 0);
	std::vector<std::string> variables;
	for (char **entry = *_NSGetEnviron(); *entry != nullptr; ++entry)
		variables.emplace_back(*entry);
	variables.push_back(std::string{scan_variable} + "=1");
	std::vector<char *> environment;
	for (auto &variable : variables)
		environment.push_back(variable.data());
	environment.push_back(nullptr);
	char *const arguments[] = {path.data(), nullptr};
	pid_t child = 0;
	require(posix_spawn(&child, path.data(), &actions, nullptr, arguments,
			environment.data()) == 0);
	posix_spawn_file_actions_destroy(&actions);
	close(pipe_ends[1]);
	std::string output;
	std::array<char, 4096> buffer{};
	for (ssize_t got; (got = read(pipe_ends[0], buffer.data(),
				  buffer.size())) != 0;) {
		if (got < 0 && errno == EINTR)
			continue;
		require(got > 0);
		output.append(buffer.data(), static_cast<std::size_t>(got));
	}
	close(pipe_ends[0]);
	int status = 0;
	while (waitpid(child, &status, 0) < 0)
		require(errno == EINTR);
	require(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	std::set<EntryFinding> findings;
	std::uintptr_t base = 0;
	std::istringstream lines{output};
	for (std::string line; std::getline(lines, line);) {
		unsigned long long a = 0, b = 0, c = 0, d = 0, e = 0;
		if (std::sscanf(line.c_str(), "sve %llx", &a) == 1)
			base = a;
		else if (std::sscanf(line.c_str(),
				 "finding %llu %llu %llx %llu %llx", &a, &b, &c,
				 &d, &e) == 5)
			findings.insert({static_cast<unsigned>(a),
				static_cast<unsigned>(b),
				static_cast<std::uint32_t>(c),
				static_cast<std::size_t>(d),
				static_cast<std::uintptr_t>(e)});
	}
	require(base != 0);
	return {findings, base};
}

/* No program translated for SVCR.SM = 1 names an entry of the SVE part,
 * and no program for SM = 0 a streaming SVE entry or twin, at any position
 * of its stream, on hosts with SVE alone and with SVE and SME; some SM = 0
 * programs do select the SVE part. A finding counts only when a process
 * with another slide makes it too: an entry has the same offset from the
 * SVE part under every slide, and an operand word does not. */
void streaming_safety()
{
	const auto scan = scan_entries();
	require(scan.selected_sve > 0U);
	std::printf("%zu SVE part entries selected at SM = 0\n",
		scan.selected_sve);
	if (scan.findings.empty())
		return;
	const auto base = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_sve_section_begin);
	for (int attempt = 0; attempt < 4; ++attempt) {
		const auto [other, other_base] = findings_in_another_process();
		if (other_base == base)
			continue;
		bool failed = false;
		for (const auto &finding : scan.findings) {
			if (!other.contains(finding))
				continue;
			std::fprintf(stderr, "host %u SVCR %u word %08x names "
				"SVE-part offset %#lx at word %zu, outside its "
				"mode's entries\n", finding.host, finding.mode,
				finding.word,
				static_cast<unsigned long>(finding.offset),
				finding.index);
			failed = true;
		}
		require(!failed);
		std::printf("%zu operand words matched entries under this "
			"slide only\n", scan.findings.size());
		return;
	}
	std::fputs("no process with another slide\n", stderr);
	require(false);
}

/* The sweep alone, for streaming_safety() in the parent process. */
int report_entry_findings()
{
	std::printf("sve %#lx\n", static_cast<unsigned long>(
		reinterpret_cast<std::uintptr_t>(
			uml_arm64_gadget_sve_section_begin)));
	for (const auto &finding : scan_entries().findings)
		std::printf("finding %u %u %08x %zu %#lx\n", finding.host,
			finding.mode, finding.word, finding.index,
			static_cast<unsigned long>(finding.offset));
	return 0;
}

/* An SM change zeroes P and FFR: only where the host runs streaming
 * programs. */
void streaming_mode_changes_zero_predicates()
{
	auto host = apple::probe_host_capabilities(apple::GuestSmePolicy::on);
	if (!host.sme_hardware || !arm64::implements_sme(host)) {
		std::puts("SM changes need FEAT_SME in hardware: not run");
		return;
	}
	for (const auto feature : {ArmFeature::sve, ArmFeature::sve2})
		host.supported.set(static_cast<std::size_t>(feature));
	const std::array code{ptrue(1, 0, 31), setffr, smstart_sm, smstop_sm,
		cntp(0, 1, 1, 0), svc};
	Machine machine{code, host};
	machine.cpu.registers[0] = 99;
	require(machine.run(64) == EngineEvent::system_call);
	require(pred(machine.cpu, 1) == 0U && ffr(machine.cpu) == 0U);
	require(machine.cpu.registers[0] == 0U);
	require(machine.cpu.svcr == 0U);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	if (std::getenv(scan_variable) != nullptr)
		return report_entry_findings();
	std::printf("gadget SVE support: %s\n",
		gadgets::sve_support == arm64::SveSupport::none ? "none" :
		gadgets::sve_support == arm64::SveSupport::control ? "control" :
								     "complete");
	encodings_match_the_assembler();
	predicate_test_matches_the_reference();
	predicate_constants();
	predicate_logical_operations();
	predicate_counts();
	element_counts();
	first_fault_register();
	vector_moves();
	while_forms();
	fused_while_branch();
	live_marker_and_fp_access();
	library_words();
	generated_forms_match_the_library();
	memory_forms_match_the_library();
	unimplemented_words();
	streaming_safety();
	streaming_mode_changes_zero_predicates();
	return 0;
}
