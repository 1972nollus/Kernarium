// SPDX-License-Identifier: GPL-2.0-or-later
/* The gadget engine's SME control core (gadgets::sme_support = control):
 * the SVCR writes and reads, TPIDR2_EL0, RDSVL/ADDSVL/ADDSPL, and the ZA and
 * ZT0 loads, stores and zeroings, with the translation cache keyed by the
 * guest's SVCR mode; and the generated register-only streaming forms.
 * Everything outside streaming mode runs on any host through an injected
 * SME profile, at several streaming vector lengths; streaming programs need
 * FEAT_SME in hardware and run at the host's SVL. A sweep checks that no
 * program translated for SVCR.SM = 1 selects an entry outside the streaming
 * set, the GPR memory instructions run the same through their streaming
 * twins as through their base entries, and every register-only form of the
 * encoding table selects its generated gadget, which names the guest's
 * registers in the guest's instruction. */
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/arm64/gadgets/program_builder_decode.hpp"
#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/floating_controls.hpp"
#include "cpu/arm64/layout/assembly_offsets.h"
#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "platform/apple/host_features.hpp"
#include "streaming_forms.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <compare>
#include <crt_externs.h>
#include <mach/mach.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach-o/dyld.h>
#include <optional>
#include <set>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
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
void uml_arm64_gadget_sm_twins_begin();
void uml_arm64_gadget_sm_twins_end();
void uml_arm64_gadget_sm_load64_unsigned_fast_r();
void uml_arm64_gadget_sm_store32_unsigned_fast_r_sp();
void uml_arm64_gadget_sysreg_fpcr_write_r();
void uml_arm64_gadget_sysreg_fpcr_write_z();
void uml_arm64_gadget_sm_sysreg_fpcr_write_r();
void uml_arm64_gadget_sm_sysreg_fpcr_write_r_afp();
void uml_arm64_gadget_sm_sysreg_fpcr_write_z();
void uml_arm64_gadget_illegal();
extern const std::uintptr_t uml_arm64_gadget_sm_generated_entries[];
extern const std::uintptr_t uml_arm64_gadget_sm_generated_entries_end[];
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
		/* Guest ZA survives SM changes while it is live: spilled when
		 * SMSTOP SM leaves streaming mode, used from the register file
		 * with SM = 0, and refilled when SMSTART SM re-enters it. */
		const std::array code{
			smstart,
			za_vector(false, 0, 0, 0),  /* mode 3: native load */
			smstop_sm,
			za_vector(true, 0, 1, 0),   /* mode 2: helper store */
			smstart_sm,
			za_vector(true, 0, 2, 0),   /* mode 3: native store */
			svc};
		Machine machine{code, host};
		const auto bytes = pattern(svl, 8);
		machine.put_data(data_base, bytes);
		machine.cpu.registers[0] = data_base;
		machine.cpu.registers[1] = data_base + 1024;
		machine.cpu.registers[2] = data_base + 2048;
		machine.cpu.registers[12] = 5;
		require(machine.run() == EngineEvent::system_call);
		require(machine.cpu.svcr == arm64::svcr_mask);
		require(machine.data(data_base + 1024, svl) == bytes);
		require(machine.data(data_base + 2048, svl) == bytes);
		require(equal(machine.row(5), bytes));
	}
	{
		/* SMSTART ZA in streaming mode enables a zeroed ZA there. */
		const std::array code{
			smstart_sm,
			smstart_za,
			za_vector(false, 0, 0, 0),  /* row 1 from x0 */
			0xd53b'4243U,               /* mrs x3, svcr */
			za_vector(true, 0, 1, 0),   /* row 1 to x1 */
			za_vector(true, 1, 2, 0),   /* row 0 to x2 */
			smstop,
			svc};
		Machine machine{code, host};
		const auto bytes = pattern(svl, 3);
		machine.put_data(data_base, bytes);
		machine.put_data(data_base + 2048, pattern(svl, 9));
		machine.cpu.registers[0] = data_base;
		machine.cpu.registers[1] = data_base + 1024;
		machine.cpu.registers[2] = data_base + 2048;
		machine.cpu.registers[12] = 1;
		machine.cpu.registers[13] = 0;
		require(machine.run() == EngineEvent::system_call);
		require(machine.cpu.registers[3] == arm64::svcr_mask);
		require(machine.cpu.svcr == 0);
		require(machine.data(data_base + 1024, svl) == bytes);
		require(machine.data(data_base + 2048, svl) ==
			std::vector<std::byte>(svl));
	}
	{
		/* AdvSIMD, and forms with no streaming twin yet, end the
		 * block at the illegal exit in streaming mode. */
		for (const auto word : {0x4ea2'8420U /* add v0.4s */,
			     0xfd40'0000U /* ldr d0, [x0] */}) {
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

/* Another thread that stops this one in the kernel every 200 us while it
 * lives, as preemption does at random: each stop is a kernel entry. */
class KernelStops final {
public:
	KernelStops()
		: target_{mach_thread_self()}, thread_{[this] { interrupt(); }}
	{
	}

	KernelStops(const KernelStops &) = delete;
	KernelStops &operator=(const KernelStops &) = delete;

	~KernelStops()
	{
		stop();
		mach_port_deallocate(mach_task_self(), target_);
	}

	/* Ends the stops and returns how many there were. */
	unsigned stop()
	{
		running_.store(false, std::memory_order_relaxed);
		if (thread_.joinable())
			thread_.join();
		return stops_.load();
	}

private:
	void interrupt()
	{
		while (running_.load(std::memory_order_relaxed)) {
			if (thread_suspend(target_) == KERN_SUCCESS) {
				/* Reading the state waits until the thread has
				 * stopped, so it entered the kernel before it
				 * resumes. */
				arm_thread_state64_t state{};
				mach_msg_type_number_t count =
					ARM_THREAD_STATE64_COUNT;
				if (thread_get_state(target_, ARM_THREAD_STATE64,
					    reinterpret_cast<thread_state_t>(
						    &state),
					    &count) == KERN_SUCCESS)
					stops_.fetch_add(1,
						std::memory_order_relaxed);
				thread_resume(target_);
			}
			std::this_thread::sleep_for(
				std::chrono::microseconds(200));
		}
	}

	mach_port_t target_;
	std::atomic<bool> running_{true};
	std::atomic<unsigned> stops_{0};
	std::thread thread_;
};

/*
 * XNU resets FPSR to 0x0800009F on return from any kernel entry while a
 * thread is in streaming mode. A streaming program therefore keeps the
 * guest's FPSR in CpuState and only its FPCR in the host register: both
 * survive a long streaming loop that another thread stops in the kernel
 * again and again, read back unchanged there, and are the guest's after the
 * run, which leaves the host's own FPCR in place.
 */
void fp_controls_survive_kernel_entries_in_streaming_mode()
{
	constexpr std::uint32_t written_fpsr = 0x0000'0011U; /* IXC, IOC */
	constexpr std::uint32_t written_fpcr = 0x0080'0000U; /* RMode -inf */
	const std::array code{
		smstart_sm,
		0xd51b'4421U, /* msr fpsr, x1 */
		0xd51b'4404U, /* msr fpcr, x4 */
		0xf100'0442U, /* subs x2, x2, #1 */
		0x54ff'ffe1U, /* b.ne .-4 */
		0xd53b'4423U, /* mrs x3, fpsr */
		0xd53b'4405U, /* mrs x5, fpcr */
		svc};
	Machine machine{code, native_host()};
	machine.cpu.registers[1] = written_fpsr;
	machine.cpu.registers[2] = 20'000'000;
	machine.cpu.registers[4] = written_fpcr;
	const auto host_fpcr = __builtin_arm_rsr64("fpcr");
	KernelStops interrupter;
	const auto event = machine.run(1'000'000);
	const auto stops = interrupter.stop();
	require(event == EngineEvent::system_call);
	std::fprintf(stderr, "streaming loop stopped in the kernel %u times\n",
		stops);
	require(stops >= 10U);
	require(machine.cpu.svcr == arm64::svcr_sm);
	require(machine.cpu.registers[3] == written_fpsr);
	require(machine.cpu.registers[5] == written_fpcr);
	require(machine.cpu.fpsr == written_fpsr);
	require(machine.cpu.fpcr == written_fpcr);
	require(__builtin_arm_rsr64("fpcr") == host_fpcr);
}

/* The streaming twin table: one row per base entry, each from an entry no
 * streaming program may select to one in the twins' row, and the builder
 * selects the twin. */
void streaming_twin_table()
{
	namespace detail = gadgets::builder_detail;
	const auto begin = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_sm_twins_begin);
	const auto end = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_sm_twins_end);
	std::set<std::uintptr_t> bases;
	/* The FPCR writes are safe in streaming mode but must also set the
	 * host's FPCR there; every other base entry is unsafe. */
	const std::set safe_bases{
		reinterpret_cast<std::uintptr_t>(
			uml_arm64_gadget_sysreg_fpcr_write_r),
		reinterpret_cast<std::uintptr_t>(
			uml_arm64_gadget_sysreg_fpcr_write_z)};
	for (const auto &row : detail::streaming_twins()) {
		require(row.base_end == row.base_begin + 4);
		require(row.twin_end == row.twin_begin + 4);
		require(!detail::streaming_entry(row.base_begin) ||
			safe_bases.contains(row.base_begin));
		require(row.twin_begin >= begin && row.twin_begin < end);
		require(detail::streaming_entry(row.twin_begin));
		require(detail::streaming_twin(row.base_begin) ==
			row.twin_begin);
		require(bases.insert(row.base_begin).second);
	}
	require(bases.size() >= 1500);
	/* A doubleword load of a register, and a word store of one with an
	 * SP base: both bake the register in the bank, and share one twin. */
	const auto twin_of = [](std::uint32_t word) {
		gadgets::BlockLink link{};
		gadgets::IndirectLink site{};
		gadgets::BuildOptions options{.host_capabilities =
			injected_host(64, true)};
		options.svcr = arm64::svcr_sm;
		const std::array window{word};
		const auto built = gadgets::Builder{}.build(code_base, window,
			[&](std::uint64_t) { return &link; },
			std::pmr::get_default_resource(),
			[&](std::uint64_t) { return &site; }, options);
		require(built.program.instruction_count() == 1);
		return built.program.words()[0];
	};
	for (const std::uint32_t rt : {0U, 5U, 30U}) {
		require(twin_of(0xf940'0000U | rt) == reinterpret_cast<
			std::uintptr_t>(uml_arm64_gadget_sm_load64_unsigned_fast_r));
		require(twin_of(0xb900'03e0U | rt) == reinterpret_cast<
			std::uintptr_t>(uml_arm64_gadget_sm_store32_unsigned_fast_r_sp));
	}
	/* MSR FPCR, whose base entry, or its FEAT_AFP twin, is safe in
	 * streaming mode but leaves the host's FPCR alone there. */
	const bool afp = arm64::implements_afp(injected_host(64, true));
	require(twin_of(0xd51b'4404U) == reinterpret_cast<std::uintptr_t>(afp ?
		uml_arm64_gadget_sm_sysreg_fpcr_write_r_afp :
		uml_arm64_gadget_sm_sysreg_fpcr_write_r));
	require(twin_of(0xd51b'441fU) == reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_sm_sysreg_fpcr_write_z));
}

/* One case for the twins: words ending in SVC, and the registers they need
 * beyond the common setup below. */
struct MemoryCase final {
	const char *name;
	std::vector<std::uint32_t> words;
	std::vector<std::pair<unsigned, std::uint64_t>> registers{};
	std::uint64_t sp = data_base + 2 * page - 64;
};

/* The architectural result of a run: what a twin must reproduce. */
struct MemoryResult final {
	EngineEvent event;
	std::array<std::uint64_t, 31> registers;
	std::uint64_t sp;
	std::uint64_t pc;
	std::uint32_t nzcv;
	std::vector<std::byte> data;
	std::uint64_t fault_address;
	bool fault_alignment;
	bool operator==(const MemoryResult &) const = default;
};

[[nodiscard]] MemoryResult run_memory_case(const MemoryCase &test,
	std::uint8_t svcr)
{
	auto code = test.words;
	code.push_back(svc);
	Machine machine{code, native_host()};
	machine.put_data(data_base, pattern(2 * page, 11));
	for (unsigned n = 0; n < 31; ++n)
		machine.cpu.registers[n] = 0x1111'1111'1111'1111ULL * (n % 15U);
	/* Bases: an aligned line, an LSE2-block misalignment, page ends that
	 * 2-, 4-, 8- and 16-byte accesses cross, and the unmapped page. */
	machine.cpu.registers[0] = data_base + page - 64;
	machine.cpu.registers[1] = data_base + page - 24;
	machine.cpu.registers[2] = data_base + page + 8;
	machine.cpu.registers[3] = data_base + 2 * page - 256;
	machine.cpu.registers[5] = data_base + page - 62;
	machine.cpu.registers[12] = data_base + 2 * page + 16;
	machine.cpu.registers[13] = data_base + page - 6;
	machine.cpu.registers[14] = data_base + page - 4;
	machine.cpu.registers[15] = data_base + page - 2;
	machine.cpu.registers[16] = 3;
	machine.cpu.registers[17] = 0xffff'fffeULL;
	machine.cpu.registers[18] = 5;
	for (const auto &[number, value] : test.registers)
		machine.cpu.registers[number] = value;
	machine.cpu.sp = test.sp;
	machine.cpu.defer_sme_load(machine.cpu.sme_task, svcr, 0);
	std::vector<std::byte> za;
	if ((svcr & arm64::svcr_za) != 0U) {
		/* As if loaded already: the rows must come back unchanged
		 * through every helper the twin leaves streaming mode for. */
		machine.cpu.sme_loaded = arm64::svcr_za;
		za = pattern(machine.svl() * machine.svl(), 23);
		std::memcpy(machine.row(0), za.data(), za.size());
	}
	MemoryResult result{};
	result.event = machine.run();
	if (!za.empty())
		require(equal(machine.row(0), za));
	std::copy(machine.cpu.registers.begin(), machine.cpu.registers.end(),
		result.registers.begin());
	result.sp = machine.cpu.sp;
	result.pc = machine.cpu.pc;
	result.nzcv = machine.cpu.nzcv;
	result.data = machine.data(data_base, 2 * page);
	if (result.event == EngineEvent::memory_fault) {
		result.fault_address = machine.executor().fault_address();
		result.fault_alignment = machine.executor().fault_alignment();
	}
	return result;
}

/* Every GPR memory family runs through its streaming twin exactly as through
 * its base entry: hot paths, TLB misses, page crossings, LSE2 and alignment
 * checks, and faults, with ZA off and resident. So does CSSC CNT. */
void streaming_twins()
{
	const std::uint64_t mops_size = 100;
	const std::vector<MemoryCase> cases{
		{"ldr x", {0xf940'0014U}},
		{"ldr x crossing", {0xf940'01b4U}},
		{"ldr w crossing", {0xb940'05b4U}},
		{"ldrb", {0x3940'04b4U}},
		{"ldrsb x", {0x3980'01d4U}},
		{"ldrh", {0x7940'01f4U}},
		{"ldursh x crossing", {0x7880'11f4U}},
		{"ldrsw", {0xb980'01d4U}},
		{"ldursw crossing", {0xb880'21d4U}},
		{"str x crossing", {0xf900'01b5U}},
		{"strb", {0x3900'05f5U}},
		{"sturh crossing", {0x7800'11f5U}},
		{"stur w crossing", {0xb800'21d5U}},
		{"str w", {0xb900'05d5U}},
		{"ldr pre-index", {0xf840'8c34U}},
		{"ldr post-index", {0xf840'8434U}},
		{"str pre-index", {0xf81f'8c55U}},
		{"ldr register lsl", {0xf870'7814U}},
		{"ldr register sxtw", {0xb871'd814U}},
		{"strh register", {0x7832'6815U}},
		{"ldp x crossing", {0xa940'55b4U}},
		{"ldp w crossing", {0x2940'55b4U}},
		{"ldpsw crossing", {0x6940'55b4U}},
		{"stp x crossing", {0xa900'5db6U}},
		{"stp w crossing", {0x2900'5db6U}},
		{"ldp pre-index crossing", {0xa9c1'5434U}},
		{"stp sp pre-index", {0xa9bf'5ff6U}},
		{"ldp sp post-index", {0xa8c1'7bfdU}},
		{"ldr sp", {0xf940'07f4U}},
		{"str xzr crossing", {0xf900'01bfU}},
		{"stp xzr crossing", {0xa900'5dbfU}},
		/* The literal follows a branch over it. */
		{"ldr literal", {0x5800'0054U, 0x1400'0003U, 0x8899'aabbU,
			0xccdd'eeffU}},
		{"ldrsw literal", {0x9800'0054U, 0x1400'0003U, 0x8899'aabbU,
			0xccdd'eeffU}},
		{"ldxr stxr", {0xc85f'7c14U, 0xc818'7c15U}},
		{"ldaxr stlxr", {0x885f'fc14U, 0x8818'fc15U}},
		{"ldxp stxp", {0xc87f'5414U, 0xc838'5c16U}},
		{"ldadd", {0xf835'0014U}},
		{"ldaddal", {0xb8f5'0014U}},
		{"swp", {0xf835'8014U}},
		{"cas", {0xc8b4'7c15U}},
		{"casp", {0x4834'7c16U}},
		{"ldar", {0xc8df'fc14U}},
		{"stlr", {0xc89f'fc15U}},
		{"ldapr", {0xf8bf'c014U}},
		{"ldapur", {0xd940'8014U}},
		{"stlur", {0x9900'4015U}},
		{"ldadd w in an LSE2 block", {0xb835'00b4U}},
		{"ldaddh in an LSE2 block", {0x7835'00b4U}},
		{"prfm", {0xf980'0000U}},
		{"dc zva", {0xd50b'7420U}},
		{"set", {0x19c2'0420U, 0x19c2'4420U, 0x19c2'8420U},
			{{1, mops_size}, {2, 0xab}}},
		{"cpyf", {0x1903'0420U, 0x1943'0420U, 0x1983'0420U},
			{{1, mops_size}}},
		{"cnt w", {0x5ac0'1eb4U}},
		{"cnt x", {0xdac0'1eb4U}},
		{"ldr unmapped", {0xf940'0194U}},
		{"str unmapped", {0xf900'0195U}},
		{"ldr misaligned sp", {0xf940'03f4U}, {},
			data_base + 2 * page - 56},
		{"ldadd misaligned sp", {0xf835'03f4U}, {},
			data_base + 2 * page - 56},
		{"stp misaligned sp", {0xa9bf'5ff6U}, {},
			data_base + 2 * page - 56},
	};
	for (const auto &test : cases) {
		const auto base = run_memory_case(test, 0);
		for (const std::uint8_t mode : {arm64::svcr_sm, arm64::svcr_mask}) {
			if (run_memory_case(test, mode) == base)
				continue;
			std::fprintf(stderr, "%s: SVCR %u differs from the base "
				"entry\n", test.name, mode);
			require(false);
		}
	}
}

namespace streaming = gadgets::streaming;
using arm64::detail::SmeOperand;
using arm64::detail::SmeOperandKind;

/* The program one word translates to for `svcr` on `host`. */
[[nodiscard]] std::vector<std::uintptr_t> translate(std::uint32_t word,
	const uml::platform::HostCapabilities &host, std::uint8_t svcr)
{
	gadgets::BlockLink link{};
	gadgets::IndirectLink site{};
	gadgets::BuildOptions options{.host_capabilities = host};
	options.svcr = svcr;
	const std::array window{word};
	const auto built = gadgets::Builder{}.build(code_base, window,
		[&](std::uint64_t) { return &link; },
		std::pmr::get_default_resource(),
		[&](std::uint64_t) { return &site; }, options);
	const auto &words = built.program.words();
	return {words.data(), words.data() + words.size()};
}

[[nodiscard]] std::uintptr_t illegal_entry() noexcept
{
	return reinterpret_cast<std::uintptr_t>(uml_arm64_gadget_illegal);
}

/* The offset a stream slot carries for register `number`: in the register
 * file for Z, V and P, in CpuState for a general register. */
[[nodiscard]] std::uint64_t slot_offset(SmeOperandKind kind, unsigned number,
	unsigned svl) noexcept
{
	switch (kind) {
	case SmeOperandKind::z:
	case SmeOperandKind::v:
		return arm64::sme_z_offset(svl, number);
	case SmeOperandKind::p:
		return arm64::sme_p_offset(svl, number);
	case SmeOperandKind::gpr:
		return number == 31U ? UML_GADGET_CPU_ZERO : number * 8U;
	default:
		return number * 8U;
	}
}

/* A word's field bits that name registers, which a gadget replaces with its
 * host registers'. */
[[nodiscard]] std::uint32_t register_bits(const SmeOperand *operands,
	unsigned count) noexcept
{
	std::uint32_t bits = 0;
	for (unsigned index = 0; index < count; ++index) {
		const auto &operand = operands[index];
		if (operand.kind == SmeOperandKind::za_tile)
			continue;
		for (unsigned segment = 0; segment < operand.segment_count;
		     ++segment) {
			const auto &part = operand.segments[segment];
			if (part.field)
				bits |= ((1U << part.width) - 1U) << part.low;
		}
	}
	return bits;
}

/*
 * One word a generated form takes, translated for SVCR.SM = 1: it selects
 * one of the form's entries. For a form that runs the guest's instruction
 * itself, the stream names the guest's registers in the register file's
 * layout at this SVL, and the gadget loads them, in stream order, into the
 * host registers its instruction names in their place, which differs from
 * the guest's word in no other bit. `ignored` bits are ones the instruction
 * ignores.
 */
void check_generated_word(std::uint32_t word, const streaming::Form &form,
	const SmeOperand *operands, const uml::platform::HostCapabilities &host,
	std::uint8_t svcr, std::uint32_t ignored = 0)
{
	const unsigned svl = host.sme_svl_bytes;
	const auto stream = translate(word, host, svcr);
	const auto entries = std::span{uml_arm64_gadget_sm_generated_entries,
		uml_arm64_gadget_sm_generated_entries_end};
	require(entries.size() == streaming::entry_count);
	const auto variants = entries.subspan(form.first_entry, form.variants);
	if (std::ranges::find(variants, stream[0]) == variants.end()) {
		std::fprintf(stderr, "word %08x at SVL %u selects %#lx, none of "
			"its form's %u entries\n", word, svl,
			static_cast<unsigned long>(stream[0]), form.variants);
		require(false);
	}
	if (form.lowering != streaming::Lowering::none)
		return;
	std::vector<std::pair<const SmeOperand *, unsigned>> slots;
	std::uint64_t offsets = 0;
	for (unsigned index = 0; index < form.operand_count; ++index) {
		const auto &operand = operands[index];
		if (operand.kind == SmeOperandKind::za_tile)
			continue;
		const auto number = arm64::detail::sme_operand_number(operand,
			word);
		const unsigned registers =
			operand.kind == SmeOperandKind::p ? 16U : 32U;
		for (unsigned member = 0; member < operand.count; ++member) {
			offsets |= slot_offset(operand.kind,
				(number + member * operand.stride) % registers,
				svl) << (16U * slots.size());
			slots.emplace_back(&operand, member);
		}
	}
	if (!slots.empty())
		require(stream.size() > 1 && stream[1] == offsets);
	const auto fields = register_bits(operands, form.operand_count) |
		ignored;
	const auto *code = reinterpret_cast<const std::uint32_t *>(stream[0]);
	std::vector<unsigned> hosts;
	std::optional<std::uint32_t> native;
	for (unsigned index = 0; index < 128U && code[index] != 0xd61f'0100U;
	     ++index) {
		const auto instruction = code[index];
		if ((instruction & 0xffc0'e000U) == 0x8580'4000U || /* ldr z */
		    (instruction & 0xffc0'e010U) == 0x8580'0000U || /* ldr p */
		    (instruction & 0xffe0'fc00U) == 0xf860'6800U)   /* ldr x */
			hosts.push_back(instruction & 31U);
		else if ((instruction & ~fields) == (word & ~fields))
			native = instruction;
	}
	if (!native.has_value() || hosts.size() != slots.size()) {
		std::fprintf(stderr, "word %08x: gadget %#lx has %s and %zu "
			"loads for %zu registers\n", word,
			static_cast<unsigned long>(stream[0]),
			native ? "its instruction" : "no instruction",
			hosts.size(), slots.size());
		require(false);
	}
	for (std::size_t slot = 0; slot < slots.size(); ++slot) {
		const auto [operand, member] = slots[slot];
		const unsigned registers =
			operand->kind == SmeOperandKind::p ? 16U : 32U;
		const auto first = arm64::detail::sme_operand_number(*operand,
			*native);
		if (hosts[slot] == (first + member * operand->stride) %
				registers)
			continue;
		std::fprintf(stderr, "word %08x: slot %zu loads host register "
			"%u, its instruction %08x names another\n", word, slot,
			hosts[slot], *native);
		require(false);
	}
}

/* Whether a word of a lowered form encodes a reserved immediate, which ends
 * the block at the illegal exit. */
[[nodiscard]] bool reserved_immediate(const streaming::Form &form,
	std::uint32_t word) noexcept
{
	return form.lowering == streaming::Lowering::logical_immediate &&
		!arm64::detail::decode_logical_immediate(true,
			(word >> 17) & 1U, (word >> 11) & 63U, (word >> 5) & 63U);
}

/* The FP&SIMD class a word belongs to, the first that matches. */
[[nodiscard]] std::size_t fp_simd_class(std::uint32_t word) noexcept
{
	namespace detail = arm64::detail;
	const std::uint32_t bucket = word >> detail::sme_encoding_bucket_shift;
	for (std::uint32_t slot = detail::fp_simd_streaming_buckets[bucket];
	     slot < detail::fp_simd_streaming_buckets[bucket + 1U]; ++slot) {
		const auto index = detail::fp_simd_streaming_bucket_rows[slot];
		const auto &row = detail::fp_simd_streaming_classes[index];
		if ((word & row.mask) == row.value)
			return index;
	}
	return detail::fp_simd_streaming_classes.size();
}

/*
 * Every register-only SVE, SVE2 and SME row of the SME1 profile that
 * executes in streaming mode has a generated form, as does every FP&SIMD
 * data-processing class that executes there, and each of its words selects
 * that form's gadget at every SVL (check_generated_word). An FP&SIMD word
 * the base decoder does not implement stays illegal, and one it implements
 * with an entry streaming mode may run keeps it.
 */
void generated_forms_match_the_table()
{
	namespace detail = arm64::detail;
	std::uint64_t state = 0x2545'f491'4f6c'dd1dULL;
	const auto next = [&state] {
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		return static_cast<std::uint32_t>(state >> 32);
	};
	std::size_t rows = 0;
	for (std::size_t index = 0; index < detail::sme_encodings.size();
	     ++index) {
		const auto &row = detail::sme_encodings[index];
		const bool control_core =
			(row.value & 0xffff'f000U) == 0x04bf'5000U ||
			(row.value & 0xffa0'f000U) == 0x0420'5000U;
		const bool register_only = row.requirement != 0U &&
			(row.requirement & detail::sme_feature_sme2) == 0U &&
			(row.mode & detail::sme_mode_streaming) != 0U &&
			row.memory == 0U && !control_core;
		require(register_only ==
			(streaming::sve_row_forms[index] != streaming::no_form));
		rows += register_only;
	}
	require(rows > 1000U);
	std::size_t classes = 0;
	for (std::size_t index = 0;
	     index < detail::fp_simd_streaming_classes.size(); ++index) {
		const auto &row = detail::fp_simd_streaming_classes[index];
		const bool data_processing = !row.advsimd &&
			(row.value & 0x0e00'0000U) == 0x0e00'0000U;
		require(data_processing ==
			(streaming::fp_class_forms[index] != streaming::no_form));
		classes += data_processing;
	}
	require(classes > 500U);
	std::size_t checked = 0;
	for (const unsigned svl : {16U, 32U, 64U, 128U, 256U}) {
		const auto host = injected_host(svl, false);
		const auto features = arm64::implemented_sme_features(host);
		for (std::size_t index = 0; index < detail::sme_encodings.size();
		     ++index) {
			if (streaming::sve_row_forms[index] == streaming::no_form)
				continue;
			const auto &row = detail::sme_encodings[index];
			const auto &form = streaming::forms[
				streaming::sve_row_forms[index]];
			const std::uint8_t svcr = arm64::svcr_sm |
				((row.mode & detail::sme_mode_za) != 0U ?
					arm64::svcr_za : 0U);
			for (int sample = 0; sample < 6; ++sample) {
				const auto word = row.value | (next() & ~row.mask);
				if (detail::decode_sme(word, features, svl) != &row)
					continue;
				if (reserved_immediate(form, word)) {
					require(translate(word, host, svcr)[0] ==
						illegal_entry());
					continue;
				}
				check_generated_word(word, form,
					detail::sme_encoding_operands_of(row), host,
					svcr);
				++checked;
			}
		}
		for (std::size_t index = 0;
		     index < detail::fp_simd_streaming_classes.size(); ++index) {
			if (streaming::fp_class_forms[index] == streaming::no_form)
				continue;
			const auto &row = detail::fp_simd_streaming_classes[index];
			const auto &form = streaming::forms[
				streaming::fp_class_forms[index]];
			const auto *operands = streaming::fp_operands.data() +
				form.operand_begin;
			/* FCMP and FCMPE with zero ignore their Rm field. */
			const std::uint32_t ignored = form.operand_count == 1U &&
				operands[0].segments[0].low == 5U ? 0x1f'0000U : 0U;
			for (int sample = 0; sample < 6; ++sample) {
				const auto word = row.value | (next() & ~row.mask);
				if (fp_simd_class(word) != index)
					continue;
				/* A word the base decoder does not implement, or
				 * implements with an entry streaming mode may run,
				 * as it does a write of XZR, keeps that entry. */
				const auto base = translate(word, host, 0)[0];
				if (base == illegal_entry() ||
				    gadgets::builder_detail::streaming_entry(
					    gadgets::builder_detail::streaming_twin(
						    base))) {
					require(translate(word, host,
						arm64::svcr_sm)[0] ==
						gadgets::builder_detail::streaming_twin(
							base));
					continue;
				}
				check_generated_word(word, form, operands, host,
					arm64::svcr_sm, ignored);
				++checked;
			}
		}
	}
	std::printf("%zu generated-form words checked for %zu rows and %zu "
		"FP&SIMD classes\n", checked, rows, classes);
	require(checked > 30'000U);
}

/*
 * Generated forms in a streaming program on the host: predicates,
 * broadcasts, lowered immediates and counts from the run-time SVL,
 * general-register results, NZCV written by SVE and FP comparisons and
 * read by base conditional selects, and FCCMP.
 */
void generated_forms_run()
{
	const auto &host = native_host();
	const unsigned svl = host.sme_svl_bytes;
	const std::uint64_t lanes = svl / 4U;
	const std::array code{
		smstart_sm,
		0x2598'e3e0U, /* ptrue p0.s */
		0x04be'4064U, /* index z4.s, #3, #-2 */
		0x25a0'd904U, /* add z4.s, z4.s, #200 */
		0x047f'9485U, /* lsr z5.s, z4.s, #1 */
		0x05a1'a08eU, /* lastb w14, p0, z4.s */
		0x05a1'a0afU, /* lastb w15, p0, z5.s */
		0x04a0'e3f0U, /* cntw x16 */
		0x04b1'f3f1U, /* sqincw x17, all, mul #2 */
		0x1e27'0020U, /* fmov s0, w1 */
		0x1e27'0041U, /* fmov s1, w2 */
		0x1e21'2000U, /* fcmp s0, s1 */
		0x1a9f'57eaU, /* cset w10, mi */
		0x1e20'4424U, /* fccmp s1, s0, #4, mi */
		0x1a9f'd7ebU, /* cset w11, gt */
		0x1e21'cc02U, /* fcsel s2, s0, s1, gt */
		0x1e26'004cU, /* fmov w12, s2 */
		0x25a3'1fe1U, /* whilelo p1.s, xzr, x3 */
		0x1a9f'57edU, /* cset w13, mi */
		svc};
	Machine machine{code, host};
	machine.cpu.registers[1] = 0x3f80'0000U; /* 1.0f */
	machine.cpu.registers[2] = 0x4000'0000U; /* 2.0f */
	machine.cpu.registers[3] = 2;
	machine.cpu.registers[17] = 5;
	require(machine.run() == EngineEvent::system_call);
	const auto last = static_cast<std::uint32_t>(203U - 2U * (lanes - 1U));
	require(machine.cpu.registers[14] == last);
	require(machine.cpu.registers[15] == last >> 1);
	require(machine.cpu.registers[16] == lanes);
	require(machine.cpu.registers[17] == 5U + 2U * lanes);
	require(machine.cpu.registers[10] == 1U);
	require(machine.cpu.registers[11] == 1U);
	require(machine.cpu.registers[12] == 0x3f80'0000U);
	require(machine.cpu.registers[13] == 1U);
	require(machine.cpu.svcr == arm64::svcr_sm);
}

/*
 * A streaming program's floating-point exceptions accumulate in the
 * guest's FPSR in CpuState, from SVE and scalar forms alike, and nothing
 * else does: a long loop of an inexact division and an exact addition that
 * another thread stops in the kernel again and again, each stop resetting
 * the host's FPSR to 0x0800009F, raises IXC alone.
 */
void fp_exceptions_accumulate_in_streaming_mode()
{
	const auto &host = native_host();
	{
		const std::array code{
			smstart_sm,
			0xd51b'443fU, /* msr fpsr, xzr */
			0x2598'e3e0U, /* ptrue p0.s */
			0x05a0'3820U, /* mov z0.s, w1 */
			0x05a0'3841U, /* mov z1.s, w2 */
			0x658d'8020U, /* fdiv z0.s, p0/m, z0.s, z1.s */
			0x05a1'a006U, /* lastb w6, p0, z0.s */
			0xd53b'4427U, /* mrs x7, fpsr */
			svc};
		Machine machine{code, host};
		machine.cpu.registers[1] = 0x3f80'0000U; /* 1.0f */
		machine.cpu.registers[2] = 0;
		require(machine.run() == EngineEvent::system_call);
		require(machine.cpu.registers[6] == 0x7f80'0000U); /* +inf */
		require(machine.cpu.registers[7] == 0x2U); /* DZC */
		require(machine.cpu.fpsr == 0x2U);
	}
	const std::array code{
		smstart_sm,
		0xd51b'443fU, /* msr fpsr, xzr */
		0x1e27'0062U, /* fmov s2, w3 */
		0x1e27'0083U, /* fmov s3, w4 */
		0x1e23'1845U, /* fdiv s5, s2, s3 */
		0x1e22'2846U, /* fadd s6, s2, s2 */
		0xf100'0442U, /* subs x2, x2, #1 */
		0x54ff'ffa1U, /* b.ne .-12 */
		0x1e26'00a5U, /* fmov w5, s5 */
		0xd53b'4427U, /* mrs x7, fpsr */
		svc};
	Machine machine{code, host};
	machine.cpu.registers[2] = 3'000'000;
	machine.cpu.registers[3] = 0x3f80'0000U; /* 1.0f */
	machine.cpu.registers[4] = 0x4040'0000U; /* 3.0f */
	KernelStops interrupter;
	const auto event = machine.run(1'000'000);
	const auto stops = interrupter.stop();
	require(event == EngineEvent::system_call);
	std::fprintf(stderr, "streaming FP loop stopped in the kernel %u "
		"times\n", stops);
	require(stops >= 10U);
	require(machine.cpu.registers[5] == 0x3eaa'aaabU); /* 1/3 */
	require(machine.cpu.registers[7] == 0x10U); /* IXC */
	require(machine.cpu.fpsr == 0x10U);
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

/* A word of a translated program that names an entry it must not: for
 * SVCR.SM = 1 one outside the streaming set, for SM = 0 a streaming-only
 * one. The entry is its offset from the bank, which no address-space slide
 * changes. */
struct EntryFinding final {
	unsigned mode;
	std::uint32_t word;
	std::size_t index;
	std::uintptr_t offset;
	auto operator<=>(const EntryFinding &) const = default;
};

/* Every such word of the programs translated from the sampled words in each
 * SVCR mode. A program's words are untyped, so an operand word that happens
 * to equal an entry's address under this process's slide is found too; such
 * a coincidence names a different offset, or none, under another slide. */
[[nodiscard]] std::set<EntryFinding> select_entries_outside_their_mode()
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
	std::set<EntryFinding> findings;
	std::size_t streaming_programs = 0;
	for (const std::uint8_t mode : {0, 1, 2, 3}) {
		gadgets::BuildOptions options{.host_capabilities =
			injected_host(64, true)};
		options.svcr = mode;
		const bool streaming = (mode & arm64::svcr_sm) != 0U;
		for (const auto word : words) {
			const std::array window{word};
			const auto built = gadgets::Builder{}.build(code_base,
				window, [&](std::uint64_t) { return &link; },
				std::pmr::get_default_resource(),
				[&](std::uint64_t) { return &site; }, options);
			const auto stream = std::span{built.program.words().data(),
				built.program.words().size()};
			for (std::size_t index = 0; index < stream.size(); ++index) {
				const auto entry = stream[index];
				if (entry < begin || entry >= end || (entry & 15U) != 0U)
					continue;
				const bool outside = streaming ?
					!gadgets::builder_detail::streaming_entry(entry) :
					std::ranges::find(streaming_only, entry) !=
							streaming_only.end() ||
						(entry >= zero_begin && entry < zero_end);
				if (outside)
					findings.insert({mode, word, index, entry - begin});
			}
			if (streaming)
				++streaming_programs;
		}
	}
	require(streaming_programs == 2 * words.size());
	return findings;
}

constexpr const char *scan_variable = "UML_GADGET_SME_ENTRY_SCAN";

/* The findings of this test binary run in a new process, which has its own
 * address-space slide, and that slide's bank address. */
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
	std::uintptr_t bank = 0;
	std::istringstream lines{output};
	for (std::string line; std::getline(lines, line);) {
		unsigned long long a = 0, b = 0, c = 0, d = 0;
		if (std::sscanf(line.c_str(), "bank %llx", &a) == 1)
			bank = a;
		else if (std::sscanf(line.c_str(), "finding %llu %llx %llu %llx",
				 &a, &b, &c, &d) == 4)
			findings.insert({static_cast<unsigned>(a),
				static_cast<std::uint32_t>(b),
				static_cast<std::size_t>(c),
				static_cast<std::uintptr_t>(d)});
	}
	require(bank != 0);
	return {findings, bank};
}

/* No program translated for SVCR.SM = 1 names an entry outside the
 * streaming set, at any position of its stream; no program for SM = 0 names
 * a streaming-only one. A finding counts only when a process with another
 * slide makes it too: a real entry has the same offset under every slide. */
void streaming_programs_select_streaming_entries()
{
	const auto findings = select_entries_outside_their_mode();
	if (findings.empty())
		return;
	const auto bank = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_bank_begin);
	for (int attempt = 0; attempt < 4; ++attempt) {
		const auto [other, other_bank] = findings_in_another_process();
		if (other_bank == bank)
			continue;
		bool failed = false;
		for (const auto &finding : findings) {
			if (!other.contains(finding))
				continue;
			std::fprintf(stderr, "mode %u word %08x selects %#lx at "
				"word %zu, outside its mode's entries\n",
				finding.mode, finding.word,
				static_cast<unsigned long>(finding.offset),
				finding.index);
			failed = true;
		}
		require(!failed);
		std::printf("%zu operand words matched entries under this "
			"slide only\n", findings.size());
		return;
	}
	std::fputs("no process with another slide\n", stderr);
	require(false);
}

/* The sweep alone, for streaming_programs_select_streaming_entries() in the
 * parent process. */
int report_entry_findings()
{
	std::printf("bank %#lx\n", static_cast<unsigned long>(
		reinterpret_cast<std::uintptr_t>(uml_arm64_gadget_bank_begin)));
	for (const auto &finding : select_entries_outside_their_mode())
		std::printf("finding %u %08x %zu %#lx\n", finding.mode,
			finding.word, finding.index,
			static_cast<unsigned long>(finding.offset));
	return 0;
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	if constexpr (gadgets::sme_support == arm64::SmeSupport::none) {
		std::puts("gadget SME support: none; nothing to test");
		return 0;
	}
	if (std::getenv(scan_variable) != nullptr)
		return report_entry_findings();
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
	streaming_twin_table();
	generated_forms_match_the_table();
	if (streaming_hardware()) {
		streaming_control_core();
		streaming_twins();
		fp_controls_survive_kernel_entries_in_streaming_mode();
		generated_forms_run();
		fp_exceptions_accumulate_in_streaming_mode();
		std::printf("streaming programs ran at SVL %u\n",
			native_host().sme_svl_bytes);
	} else {
		std::puts("streaming programs need FEAT_SME in hardware: "
			"not run");
	}
	return 0;
}
