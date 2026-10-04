// SPDX-License-Identifier: GPL-2.0-or-later
/* The gadget engine's SME control core (gadgets::sme_support = control):
 * the SVCR writes and reads, TPIDR2_EL0, RDSVL/ADDSVL/ADDSPL, and the ZA and
 * ZT0 loads, stores and zeroings, with the translation cache keyed by the
 * guest's SVCR mode. Everything outside streaming mode runs on any host
 * through an injected SME profile, at several streaming vector lengths;
 * streaming programs need FEAT_SME in hardware and run at the host's SVL.
 * A sweep checks that no program translated for SVCR.SM = 1 selects an entry
 * outside the streaming set. */
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/arm64/gadgets/program_builder_decode.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "platform/apple/host_features.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <vector>

extern "C" {
extern const char uml_arm64_gadget_bank_begin[];
extern const char uml_arm64_gadget_sme_section_end[];
void uml_arm64_gadget_sm_za_vector_load();
void uml_arm64_gadget_sm_za_vector_store();
void uml_arm64_gadget_sm_zt0_load();
void uml_arm64_gadget_sm_zt0_store();
void uml_arm64_gadget_sm_zt0_zero();
void uml_arm64_gadget_sm_za_zero_table();
void uml_arm64_gadget_sm_za_zero_table_end();
}

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
namespace apple = uml::platform::apple;
using uml::cpu::ArmFeature;
using uml::cpu::EngineEvent;
using uml::memory::StaticGuestMemory;
using namespace uml::emulator::tests;

namespace {

constexpr std::uint64_t code_base = 0x82'0000;
constexpr std::size_t page = DARWIN_UML_GUEST_PAGE_SIZE;
/* Code in page 0, data in pages 1 and 2; page 3 is not mapped. */
constexpr std::uint64_t data_base = code_base + page;
constexpr std::size_t mapped_pages = 3;

constexpr std::uint32_t svc = 0xd400'0001U;
constexpr std::uint32_t smstart = 0xd503'477fU;
constexpr std::uint32_t smstop = 0xd503'467fU;
constexpr std::uint32_t smstart_sm = 0xd503'437fU;
constexpr std::uint32_t smstop_sm = 0xd503'427fU;
constexpr std::uint32_t smstart_za = 0xd503'457fU;
constexpr std::uint32_t smstop_za = 0xd503'447fU;

[[nodiscard]] constexpr std::uint32_t rdsvl(unsigned rd, int imm) noexcept
{
	return 0x04bf'5800U | ((static_cast<unsigned>(imm) & 63U) << 5) | rd;
}

[[nodiscard]] constexpr std::uint32_t addsvl(unsigned rd, unsigned rn,
	int imm, bool predicate = false) noexcept
{
	return (predicate ? 0x0460'5800U : 0x0420'5800U) | (rn << 16) |
		((static_cast<unsigned>(imm) & 63U) << 5) | rd;
}

/* LDR or STR ZA[W(12 + v), #off], [Xn|SP, #off, MUL VL] */
[[nodiscard]] constexpr std::uint32_t za_vector(bool store, unsigned v,
	unsigned rn, unsigned off) noexcept
{
	return (store ? 0xe120'0000U : 0xe100'0000U) | (v << 13) | (rn << 5) |
		off;
}

[[nodiscard]] constexpr std::uint32_t zt0(bool store, unsigned rn) noexcept
{
	return (store ? 0xe13f'8000U : 0xe11f'8000U) | (rn << 5);
}

/* The host's own SME profile, whatever the guest-SME policy, or none. */
[[nodiscard]] const uml::platform::HostCapabilities &native_host()
{
	static const auto host =
		apple::probe_host_capabilities(apple::GuestSmePolicy::on);
	return host;
}

[[nodiscard]] bool streaming_hardware()
{
	return native_host().sme_hardware && arm64::implements_sme(native_host());
}

/* The probed host with an SME profile injected at `svl`, as a test builds
 * one by hand: it never claims the hardware. */
[[nodiscard]] uml::platform::HostCapabilities injected_host(unsigned svl,
	bool sme2)
{
	auto host = ::uml::cpu::host_capabilities();
	for (const auto feature : {ArmFeature::sme, ArmFeature::sme_f64f64,
		     ArmFeature::sme_i16i64})
		host.supported.set(static_cast<std::size_t>(feature));
	host.supported.set(static_cast<std::size_t>(ArmFeature::sme2), sme2);
	host.sme_svl_bytes = static_cast<std::uint16_t>(svl);
	host.sme_hardware = false;
	return host;
}

[[nodiscard]] uml::platform::HostCapabilities without_sme()
{
	auto host = ::uml::cpu::host_capabilities();
	for (const auto feature : {ArmFeature::sme, ArmFeature::sme2,
		     ArmFeature::sme_f64f64, ArmFeature::sme_i16i64})
		host.supported.reset(static_cast<std::size_t>(feature));
	host.sme_svl_bytes = 0;
	return host;
}

/* One guest address space with `code` at code_base, an executor for `host`
 * and a vCPU with a register file and, unless told otherwise, task storage. */
class Machine final {
public:
	Machine(std::span<const std::uint32_t> code,
		const uml::platform::HostCapabilities &host, bool storage = true)
		: generation_{next_generation_++}, backing_{mapped_pages + 1},
		  mapping_{generation_, code_base, backing_, mapped_pages,
			  PROT_READ | PROT_WRITE | PROT_EXEC},
		  executor_{gadgets::BuildOptions{.host_capabilities = host}},
		  file_{arm64::SmeRegisterFile::for_host(host)}
	{
		for (std::size_t index = 0; index < code.size(); ++index)
			backing_.put32(index * sizeof(std::uint32_t), code[index]);
		cpu.pc = code_base;
		if (file_.data() != nullptr) {
			cpu.bind_sme_registers(file_);
			task_.assign(arm64::sme_storage_size(file_.svl()),
				std::byte{0});
		}
		cpu.defer_sme_load(storage && file_.data() != nullptr ?
			task_.data() : nullptr, 0, 0);
	}

	/* Run blocks, as one kernel entry does, until an exit other than a
	 * continued block; `limit` blocks at most. */
	EngineEvent run(std::size_t limit = 16)
	{
		require(executor_.begin_execution(generation_, cpu.pc));
		EngineEvent event = EngineEvent::fatal;
		retired = 0;
		for (std::size_t block = 0; block < limit; ++block) {
			std::size_t executed = 0;
			event = executor_.run_block(cpu, memory_, generation_,
				executed);
			retired += executed;
			if (event != EngineEvent::continued &&
			    event != EngineEvent::interrupt)
				break;
		}
		executor_.end_execution();
		gadgets::restore_native_fp_environment(cpu);
		/* Whatever ran, host code runs out of streaming mode. */
		require(arm64::host_streaming_state_clear(native_host()));
		return event;
	}

	[[nodiscard]] std::byte *row(unsigned index) const noexcept
	{
		return file_.data() + arm64::sme_za_offset(file_.svl(), index);
	}

	[[nodiscard]] std::byte *zt0_bytes() const noexcept
	{
		return file_.data() + arm64::sme_zt0_offset(file_.svl());
	}

	[[nodiscard]] unsigned svl() const noexcept { return file_.svl(); }

	void put_data(std::uint64_t address, std::span<const std::byte> bytes)
	{
		backing_.put(address - code_base, bytes);
	}

	[[nodiscard]] std::vector<std::byte> data(std::uint64_t address,
		std::size_t size) const
	{
		std::vector<std::byte> bytes(size);
		backing_.get(address - code_base, bytes);
		return bytes;
	}

	[[nodiscard]] const gadgets::GadgetExecutor &executor() const noexcept
	{
		return executor_;
	}

	arm64::CpuState cpu{};
	std::size_t retired{};

private:
	static inline unsigned long next_generation_ = 7700;
	unsigned long generation_;
	JitBacking backing_;
	JitMapping mapping_;
	StaticGuestMemory memory_;
	gadgets::GadgetExecutor executor_;
	arm64::SmeRegisterFile file_;
	std::vector<std::byte> task_;
};

[[nodiscard]] std::vector<std::byte> pattern(std::size_t size,
	unsigned seed)
{
	std::vector<std::byte> bytes(size);
	for (std::size_t index = 0; index < size; ++index)
		bytes[index] = static_cast<std::byte>(
			(index * 7U + seed * 31U + 1U) & 0xffU);
	return bytes;
}

[[nodiscard]] bool equal(const std::byte *a, std::span<const std::byte> b)
{
	return std::memcmp(a, b.data(), b.size()) == 0;
}

/* Without SME in the virtual CPU, every SME word stays the illegal exit. */
void sme_words_are_illegal_without_sme()
{
	for (const auto word : {smstart, smstart_za, rdsvl(0, 1),
		     0xd53b'4240U /* mrs x0, svcr */,
		     0xd53b'd0a0U /* mrs x0, tpidr2_el0 */,
		     0xd51b'd0a0U /* msr tpidr2_el0, x0 */,
		     za_vector(false, 0, 0, 0), 0xc008'00ffU, 0xc048'0001U}) {
		const std::array code{word, svc};
		Machine machine{code, without_sme()};
		require(machine.run() == EngineEvent::illegal_instruction);
		require(machine.cpu.pc == code_base);
		require(machine.retired == 0);
	}
}

void vector_lengths_read_at_run_time(unsigned svl)
{
	const std::array code{
		rdsvl(0, 1), rdsvl(1, -2), addsvl(2, 3, 3),
		addsvl(31, 31, -1, true), rdsvl(31, 5), svc};
	Machine machine{code, injected_host(svl, true)};
	machine.cpu.registers[3] = 0x1000;
	machine.cpu.sp = 0x8000;
	require(machine.run() == EngineEvent::system_call);
	require(machine.cpu.registers[0] == svl);
	require(machine.cpu.registers[1] ==
		static_cast<std::uint64_t>(-2 * static_cast<std::int64_t>(svl)));
	require(machine.cpu.registers[2] == 0x1000 + 3 * svl);
	require(machine.cpu.sp == 0x8000 - svl / 8);
	require(machine.retired == code.size());
}

void tpidr2_is_the_guest_register()
{
	const std::array code{
		0xd51b'd0a0U, /* msr tpidr2_el0, x0 */
		0xd53b'd0a1U, /* mrs x1, tpidr2_el0 */
		0xd51b'd0bfU, /* msr tpidr2_el0, xzr */
		0xd53b'd0a2U, /* mrs x2, tpidr2_el0 */
		0xd53b'4243U, /* mrs x3, svcr */
		svc};
	Machine machine{code, injected_host(64, true)};
	machine.cpu.registers[0] = 0x1234'5678'9abc'def0ULL;
	machine.cpu.registers[2] = 99;
	machine.cpu.registers[3] = 99;
	require(machine.run() == EngineEvent::system_call);
	require(machine.cpu.registers[1] == 0x1234'5678'9abc'def0ULL);
	require(machine.cpu.registers[2] == 0);
	require(machine.cpu.registers[3] == 0);
	require(machine.cpu.tpidr2 == 0);
}

/* SMSTART ZA enables ZA, zeroed; the next block runs translated for ZA = 1,
 * where the ZA loads, stores and zeroing work on the register file. */
void za_mode_memory(unsigned svl)
{
	const std::array code{
		smstart_za,
		0xd53b'4243U, /* mrs x3, svcr */
		za_vector(false, 0, 0, 0),  /* ldr za[w12, 0], [x0] */
		za_vector(false, 1, 1, 1),  /* ldr za[w13, 1], [x1, #1, mul vl] */
		za_vector(false, 3, 0, 0),  /* ldr za[w15, 0], [x0] */
		za_vector(true, 2, 2, 15),  /* str za[w14, 15], [x2, #15, mul vl] */
		0xc008'0005U,               /* zero {za0.d, za2.d} */
		svc};
	Machine machine{code, injected_host(svl, false)};
	std::memset(machine.row(0), 0x5a, std::size_t{svl} * svl);
	const auto first = pattern(svl, 1);
	const auto second = pattern(svl, 2);
	machine.put_data(data_base, first);
	machine.put_data(data_base + 2 * svl, second);
	machine.cpu.registers[0] = data_base;
	machine.cpu.registers[1] = data_base + svl;
	machine.cpu.registers[2] = data_base + 4 * svl;
	machine.cpu.registers[12] = 3;
	machine.cpu.registers[13] = svl + 6;       /* row 7, wrapped */
	machine.cpu.registers[14] = 2 * svl - 12;  /* row 3, wrapped */
	machine.cpu.registers[15] = 8;
	const auto event = machine.run();
	require(event == EngineEvent::system_call);
	require(machine.cpu.svcr == arm64::svcr_za);
	require(machine.cpu.registers[3] == arm64::svcr_za);
	require(machine.retired == code.size());
	/* Rows 3 and 7 hold the patterns; SMSTART ZA zeroed the rest, and ZERO
	 * cleared the rows 0 and 2 modulo 8, row 8 among them. */
	require(equal(machine.row(3), first));
	require(equal(machine.row(7), second));
	const std::vector<std::byte> zero(svl);
	require(equal(machine.row(1), zero));
	require(equal(machine.row(8), zero));
	require(machine.data(data_base + 19 * svl, svl) == first);
	/* SMSTOP ZA leaves it; ZA instructions are illegal again. */
	const std::array stop{smstop_za, za_vector(false, 0, 0, 0)};
	Machine after{stop, injected_host(svl, false)};
	after.cpu.defer_sme_load(after.cpu.sme_task, arm64::svcr_za, 0);
	require(after.run() == EngineEvent::illegal_instruction);
	require(after.cpu.svcr == 0);
	require(after.cpu.pc == code_base + 4);
	require(after.retired == 1);
}

/* With no task storage, a write that would set SM or ZA asks for it. */
void svcr_write_without_storage_asks_for_it()
{
	for (const auto word : {smstart, smstart_za, smstart_sm,
		     0xd51b'4240U /* msr svcr, x0 */}) {
		const std::array code{0xd280'00a5U /* movz x5, #5 */, word, svc};
		Machine machine{code, injected_host(64, true), false};
		machine.cpu.registers[0] = 3;
		require(machine.run() == EngineEvent::sme_access);
		require(machine.cpu.pc == code_base + 4);
		require(machine.retired == 1);
		require(machine.cpu.registers[5] == 5);
		require(machine.cpu.svcr == 0);
	}
	/* Writes that only clear bits need none. */
	for (const auto word : {smstop, smstop_za, smstop_sm}) {
		const std::array code{word, svc};
		Machine machine{code, injected_host(64, true), false};
		require(machine.run() == EngineEvent::system_call);
		require(machine.retired == 2);
	}
}

void za_instructions_need_za()
{
	for (const auto word : {za_vector(false, 0, 0, 0),
		     za_vector(true, 0, 0, 0), 0xc008'00ffU, zt0(false, 0),
		     zt0(true, 0), 0xc048'0001U}) {
		const std::array code{word, svc};
		Machine machine{code, injected_host(64, true)};
		require(machine.run() == EngineEvent::illegal_instruction);
		require(machine.cpu.pc == code_base);
		require(machine.retired == 0);
	}
}

void zt0_needs_sme2_and_works_with_za()
{
	const std::array code{zt0(false, 0), zt0(true, 1), 0xc048'0001U,
		zt0(true, 2), svc};
	{
		Machine machine{code, injected_host(32, false)};
		machine.cpu.defer_sme_load(machine.cpu.sme_task,
			arm64::svcr_za, 0);
		require(machine.run() == EngineEvent::illegal_instruction);
		require(machine.retired == 0);
	}
	Machine machine{code, injected_host(32, true)};
	machine.cpu.defer_sme_load(machine.cpu.sme_task, arm64::svcr_za, 0);
	const auto bytes = pattern(arm64::sme_zt0_bytes, 9);
	machine.put_data(data_base, bytes);
	machine.cpu.registers[0] = data_base;
	machine.cpu.registers[1] = data_base + 128;
	machine.cpu.registers[2] = data_base + 256;
	require(machine.run() == EngineEvent::system_call);
	const auto stored = machine.data(data_base + 128, bytes.size());
	require(stored == bytes);
	const auto zeroed = machine.data(data_base + 256, bytes.size());
	require(std::ranges::all_of(zeroed,
		[](std::byte b) { return b == std::byte{0}; }));
}

/* A store crossing into an unmapped page changes nothing and reports the
 * first byte of that page; an SP base must be 16-byte aligned. */
void za_faults()
{
	const unsigned svl = 64;
	const auto end = data_base + 2 * page;
	{
		const std::array code{0xd280'00a5U, za_vector(true, 0, 0, 0), svc};
		Machine machine{code, injected_host(svl, false)};
		machine.cpu.defer_sme_load(machine.cpu.sme_task,
			arm64::svcr_za, 0);
		std::memset(machine.row(0), 0x77, svl);
		const auto before = machine.data(end - 16, 16);
		machine.cpu.registers[0] = end - 16;
		require(machine.run() == EngineEvent::memory_fault);
		require(machine.cpu.pc == code_base + 4);
		require(machine.retired == 1);
		require(machine.executor().fault_address() == end);
		require(machine.executor().fault_access() ==
			uml::memory::Access::write);
		require(machine.data(end - 16, 16) == before);
	}
	{
		const std::array code{za_vector(false, 0, 0, 0), svc};
		Machine machine{code, injected_host(svl, false)};
		machine.cpu.defer_sme_load(machine.cpu.sme_task,
			arm64::svcr_za, 0);
		/* As if loaded already, so the row's contents stay. */
		machine.cpu.sme_loaded = arm64::svcr_za;
		std::memset(machine.row(0), 0x77, svl);
		machine.cpu.registers[0] = end - 16;
		require(machine.run() == EngineEvent::memory_fault);
		require(machine.executor().fault_address() == end);
		require(machine.row(0)[0] == std::byte{0x77});
	}
	{
		const std::array code{za_vector(false, 0, 31, 0), svc};
		Machine machine{code, injected_host(svl, false)};
		machine.cpu.defer_sme_load(machine.cpu.sme_task,
			arm64::svcr_za, 0);
		machine.cpu.sp = data_base + 8;
		require(machine.run() == EngineEvent::memory_fault);
		require(machine.executor().fault_alignment());
		require(machine.executor().fault_address() == data_base + 8);
	}
	{
		/* A same-page access through a crossing-free SP base works. */
		const std::array code{za_vector(false, 0, 31, 1), svc};
		Machine machine{code, injected_host(svl, false)};
		machine.cpu.defer_sme_load(machine.cpu.sme_task,
			arm64::svcr_za, 0);
		const auto bytes = pattern(svl, 4);
		machine.put_data(data_base + svl, bytes);
		machine.cpu.sp = data_base;
		require(machine.run() == EngineEvent::system_call);
		require(equal(machine.row(1), bytes));
	}
}

/* The same code is a different program in each mode. */
void translations_are_keyed_by_mode()
{
	const std::array code{za_vector(false, 0, 0, 0), svc};
	Machine machine{code, injected_host(64, false)};
	machine.cpu.registers[0] = data_base;
	require(machine.run() == EngineEvent::illegal_instruction);
	machine.cpu.defer_sme_load(machine.cpu.sme_task, arm64::svcr_za, 0);
	require(machine.run() == EngineEvent::system_call);
	machine.cpu.pc = code_base;
	machine.cpu.defer_sme_load(machine.cpu.sme_task, 0, 0);
	require(machine.run() == EngineEvent::illegal_instruction);
}

/* Streaming programs: only on a host with FEAT_SME, at its SVL. */
void streaming_control_core()
{
	const auto &host = native_host();
	const unsigned svl = host.sme_svl_bytes;
	const bool sme2 = host.has(ArmFeature::sme2);
	{
		/* SMSTART SM resets FPSR; integer code, branches and the SME
		 * control instructions run in streaming mode. */
		const std::array code{
			smstart_sm,
			rdsvl(0, 1),
			0xd53b'4241U,  /* mrs x1, svcr */
			0xf100'04a5U,  /* subs x5, x5, #1 */
			0x54ff'ffe1U,  /* b.ne .-4 */
			addsvl(2, 2, 2),
			smstop_sm,
			svc};
		Machine machine{code, host};
		machine.cpu.registers[5] = 1000;
		machine.cpu.access_fp_state();
		machine.cpu.fpsr = 0;
		require(machine.run(64) == EngineEvent::system_call);
		require(machine.cpu.registers[0] == svl);
		require(machine.cpu.registers[1] == arm64::svcr_sm);
		require(machine.cpu.registers[5] == 0);
		require(machine.cpu.registers[2] == 2 * svl);
		require(machine.cpu.svcr == 0);
		require(machine.cpu.fpsr == arm64::streaming_mode_change_fpsr);
	}
	{
		/* ZA resident in host ZA: native same-page accesses, a
		 * page-crossing one through the helper, the zeroing table, and
		 * ZT0; the leave sequence spills both to the register file. */
		std::vector<std::uint32_t> code{
			smstart,
			za_vector(false, 0, 0, 0),
			za_vector(false, 1, 1, 0),
			za_vector(true, 0, 2, 0),
			0xc008'0002U, /* zero {za1.d} */
		};
		if (sme2)
			code.insert(code.end(), {zt0(false, 3), zt0(true, 4)});
		code.push_back(svc);
		Machine machine{code, host};
		const auto first = pattern(svl, 5);
		const auto second = pattern(svl, 6);
		const auto end = data_base + 2 * page;
		machine.put_data(data_base, first);
		machine.put_data(data_base + page - svl / 2, second);
		const auto table = pattern(arm64::sme_zt0_bytes, 7);
		machine.put_data(data_base + 1024, table);
		machine.cpu.registers[0] = data_base;
		machine.cpu.registers[1] = data_base + page - svl / 2;
		machine.cpu.registers[2] = end - svl;
		machine.cpu.registers[3] = data_base + 1024;
		machine.cpu.registers[4] = data_base + 2048;
		machine.cpu.registers[12] = 2;
		machine.cpu.registers[13] = 9;
		require(machine.run() == EngineEvent::system_call);
		require(machine.cpu.svcr == arm64::svcr_mask);
		require(equal(machine.row(2), first));
		/* Row 9 is in ZA1.D, which ZERO cleared after the load. */
		const std::vector<std::byte> zero(svl);
		require(equal(machine.row(9), zero));
		require(machine.data(end - svl, svl) == first);
		if (sme2) {
			require(machine.data(data_base + 2048, table.size()) ==
				table);
			require(equal(machine.zt0_bytes(), table));
		}
		/* The crossing load, checked through a row ZERO left. */
		const std::array again{
			za_vector(false, 1, 1, 0), svc};
		Machine crossing{again, host};
		crossing.cpu.defer_sme_load(crossing.cpu.sme_task,
			arm64::svcr_mask, 0);
		crossing.put_data(data_base + page - svl / 2, second);
		crossing.cpu.registers[1] = data_base + page - svl / 2;
		crossing.cpu.registers[13] = 10;
		require(crossing.run() == EngineEvent::system_call);
		require(equal(crossing.row(10), second));
	}
	{
		/* AdvSIMD, and forms with no streaming twin yet, end the
		 * block at the illegal exit in streaming mode. */
		for (const auto word : {0x4ea2'8420U /* add v0.4s */,
			     0xf940'0005U /* ldr x5, [x0] */}) {
			const std::array code{word, svc};
			Machine machine{code, host};
			machine.cpu.defer_sme_load(machine.cpu.sme_task,
				arm64::svcr_sm, 0);
			machine.cpu.registers[0] = data_base;
			require(machine.run() == EngineEvent::illegal_instruction);
			require(machine.cpu.pc == code_base);
			require(machine.retired == 0);
		}
	}
}

/* Words to translate: uniformly sampled ones, which reach every decoder in
 * proportion to its share of the encoding space, and every FP&SIMD and
 * SVE/SME form of the shared table with sampled operand bits. */
[[nodiscard]] std::vector<std::uint32_t> sampled_words()
{
	std::vector<std::uint32_t> words;
	std::uint64_t state = 0x9e37'79b9'7f4a'7c15ULL;
	const auto next = [&state] {
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		return static_cast<std::uint32_t>(state >> 32);
	};
	for (int sample = 0; sample < 120'000; ++sample)
		words.push_back(next());
	for (const auto &row : arm64::detail::fp_simd_streaming_classes)
		for (int sample = 0; sample < 6; ++sample)
			words.push_back(row.value | (next() & ~row.mask));
	for (const auto &row : arm64::detail::sme_encodings)
		for (int sample = 0; sample < 2; ++sample)
			words.push_back(row.value | (next() & ~row.mask));
	return words;
}

/* No program translated for SVCR.SM = 1 names an entry outside the
 * streaming set, at any position of its stream; no program for SM = 0 names
 * a streaming-only one. */
void streaming_programs_select_streaming_entries()
{
	const auto begin = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_bank_begin);
	const auto end = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_sme_section_end);
	const auto zero_begin = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_sm_za_zero_table);
	const auto zero_end = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_sm_za_zero_table_end);
	const std::array streaming_only{
		reinterpret_cast<std::uintptr_t>(uml_arm64_gadget_sm_za_vector_load),
		reinterpret_cast<std::uintptr_t>(uml_arm64_gadget_sm_za_vector_store),
		reinterpret_cast<std::uintptr_t>(uml_arm64_gadget_sm_zt0_load),
		reinterpret_cast<std::uintptr_t>(uml_arm64_gadget_sm_zt0_store),
		reinterpret_cast<std::uintptr_t>(uml_arm64_gadget_sm_zt0_zero),
	};
	gadgets::BlockLink link{};
	gadgets::IndirectLink site{};
	const auto words = sampled_words();
	std::size_t streaming_programs = 0;
	for (const std::uint8_t mode : {0, 1, 2, 3}) {
		gadgets::BuildOptions options{.host_capabilities =
			injected_host(64, true)};
		options.svcr = mode;
		for (const auto word : words) {
			const std::array window{word};
			const auto built = gadgets::Builder{}.build(code_base,
				window, [&](std::uint64_t) { return &link; },
				std::pmr::get_default_resource(),
				[&](std::uint64_t) { return &site; }, options);
			const auto stream = std::span{built.program.words().data(),
				built.program.words().size()};
			for (const auto entry : stream) {
				if (entry < begin || entry >= end || (entry & 15U) != 0U)
					continue;
				if ((mode & arm64::svcr_sm) != 0U) {
					if (!gadgets::builder_detail::streaming_entry(entry)) {
						std::fprintf(stderr, "mode %u word %08x selects "
							"%#lx outside the streaming set\n",
							mode, word, static_cast<unsigned long>(
								entry - begin));
						require(false);
					}
				} else {
					require(std::ranges::find(streaming_only, entry) ==
						streaming_only.end());
					require(entry < zero_begin || entry >= zero_end);
				}
			}
			if ((mode & arm64::svcr_sm) != 0U)
				++streaming_programs;
		}
	}
	require(streaming_programs == 2 * words.size());
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	if constexpr (gadgets::sme_support == arm64::SmeSupport::none) {
		std::puts("gadget SME support: none; nothing to test");
		return 0;
	}
	sme_words_are_illegal_without_sme();
	streaming_programs_select_streaming_entries();
	for (const unsigned svl : {16U, 64U, 256U})
		vector_lengths_read_at_run_time(svl);
	tpidr2_is_the_guest_register();
	for (const unsigned svl : {16U, 64U, 128U})
		za_mode_memory(svl);
	svcr_write_without_storage_asks_for_it();
	za_instructions_need_za();
	zt0_needs_sme2_and_works_with_za();
	za_faults();
	translations_are_keyed_by_mode();
	if (streaming_hardware()) {
		streaming_control_core();
		std::printf("streaming programs ran at SVL %u\n",
			native_host().sme_svl_bytes);
	} else {
		std::puts("streaming programs need FEAT_SME in hardware: "
			"not run");
	}
	return 0;
}
