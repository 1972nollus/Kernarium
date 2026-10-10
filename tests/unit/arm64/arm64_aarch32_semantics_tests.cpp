// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The AArch32 semantic library's own contract (cpu/arm64/aarch32/semantics.hpp):
 * what the corpus's model lane cannot pin because its oracle records the
 * state at an event and not the library's promises about it. Fault ordering,
 * the exclusive monitor, the IT state of the single-instruction entry, the
 * events, the library-exit counters and the coverage predicate.
 */
#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/semantics.hpp"
#include "require.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

#ifndef DARWIN_UML_GUEST_PAGE_SIZE
#error "the guest page profile sizes the page splits"
#endif

namespace {

using ::uml::memory::MemoryAccess;
using ::uml::testing::require;
using namespace ::uml::cpu::arm64;
using namespace ::uml::cpu::arm64::aarch32;

constexpr std::uint32_t page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::uint32_t code_base = 0x1000'0000;
/* Two data pages side by side, then the page after them. */
constexpr std::uint32_t data_base = 0x2000'0000;

/* Guest memory of separate host pages, so a library that assumed two pages
 * adjacent in the host would fail. */
class Machine final : public A32Memory {
public:
	std::byte *translate(std::uint64_t address, MemoryAccess access) noexcept override
	{
		const auto found = pages_.find(static_cast<std::uint32_t>(address / page));
		if (found == pages_.end() || address >> 32 != 0)
			return nullptr;
		Page &entry = found->second;
		if ((access == MemoryAccess::write && !entry.write) ||
			(access == MemoryAccess::execute && !entry.execute))
			return nullptr;
		++translations;
		return entry.bytes.get() + address % page;
	}

	void stored(std::byte *host, std::size_t size) noexcept override
	{
		stores.push_back({host, size});
	}

	void map(std::uint32_t address, bool write = true, bool execute = false)
	{
		Page entry;
		void *memory = std::aligned_alloc(page, page);
		require(memory != nullptr, "out of memory");
		std::memset(memory, 0, page);
		entry.bytes.reset(static_cast<std::byte *>(memory));
		entry.write = write;
		entry.execute = execute;
		pages_[address / page] = std::move(entry);
	}

	void unmap(std::uint32_t address) { pages_.erase(address / page); }

	template<typename T>
	void put(std::uint32_t address, T value)
	{
		std::memcpy(host(address), &value, sizeof value);
	}

	template<typename T>
	[[nodiscard]] T get(std::uint32_t address)
	{
		T value;
		std::memcpy(&value, host(address), sizeof value);
		return value;
	}

	std::vector<std::pair<std::byte *, std::size_t>> stores;
	std::size_t translations = 0;

private:
	struct Free final {
		void operator()(std::byte *pointer) const noexcept { std::free(pointer); }
	};
	struct Page final {
		std::unique_ptr<std::byte, Free> bytes;
		bool write{}, execute{};
	};

	std::byte *host(std::uint32_t address)
	{
		return pages_.at(address / page).bytes.get() + address % page;
	}

	std::map<std::uint32_t, Page> pages_;
};

struct Fixture final {
	Machine machine;
	CpuState cpu{};
	Profile profile = Profile::at(Level::base);

	Fixture()
	{
		machine.map(code_base, true, true);
		machine.map(data_base);
		machine.map(data_base + page);
		cpu.a32 = 1;
		cpu.a32_cp15ben = 1;
		set_pc(code_base, false);
	}

	void set_pc(std::uint32_t address, bool thumb)
	{
		cpu.pc = thumb ? t32_key(address) : a32_key(address);
	}

	[[nodiscard]] std::uint32_t pc() const { return key_pc(cpu.pc); }
	[[nodiscard]] std::uint32_t r(unsigned n) const
	{
		return static_cast<std::uint32_t>(cpu.registers[n]);
	}
	[[nodiscard]] std::uint64_t d(unsigned n) const
	{
		std::uint64_t value;
		std::memcpy(&value, cpu.vectors[n / 2].bytes.data() + 8U * (n & 1U), 8);
		return value;
	}
	void set_d(unsigned n, std::uint64_t value)
	{
		std::memcpy(cpu.vectors[n / 2].bytes.data() + 8U * (n & 1U), &value, 8);
	}
	[[nodiscard]] std::uint32_t s(unsigned n) const
	{
		std::uint32_t value;
		std::memcpy(&value, cpu.vectors[n / 4].bytes.data() + 4U * (n & 3U), 4);
		return value;
	}

	/* Places A32 words at the code page and steps the first. */
	StepResult step_a32_word(std::uint32_t word)
	{
		machine.put(code_base, word);
		set_pc(code_base, false);
		return step_a32(cpu, machine, profile);
	}

	/* Places T32 halfwords at the code page and steps the first instruction. */
	StepResult step_t32(std::initializer_list<std::uint16_t> halves)
	{
		std::uint32_t at = code_base;
		for (const auto half : halves) {
			machine.put(at, half);
			at += 2;
		}
		set_pc(code_base, true);
		return step_a32(cpu, machine, profile);
	}
};

constexpr std::uint32_t ldm_r0_r1_r3 = 0xe890000e; /* ldm r0, {r1, r2, r3} */
constexpr std::uint32_t stmia_wb_r0_r1_r3 = 0xe8a0000e; /* stm r0!, {r1, r2, r3} */
constexpr std::uint32_t ldr_r1_r0 = 0xe5901000;
constexpr std::uint32_t str_r1_r0 = 0xe5801000;
constexpr std::uint32_t ldrd_r2_r0 = 0xe1c020d0; /* ldrd r2, r3, [r0] */
constexpr std::uint32_t strd_r2_r0 = 0xe1c020f0;
constexpr std::uint32_t ldrex_r1_r0 = 0xe1901f9f;
constexpr std::uint32_t strex_r2_r3_r0 = 0xe1802f93; /* strex r2, r3, [r0] */
constexpr std::uint32_t ldrexd_r2_r0 = 0xe1b02f9f; /* ldrexd r2, r3, [r0] */
constexpr std::uint32_t strexd_r1_r4_r0 = 0xe1a01f94; /* strexd r1, r4, r5, [r0] */
constexpr std::uint32_t ldrexb_r2_r1 = 0xe1d12f9f;
constexpr std::uint32_t strexb_r3_r2_r1 = 0xe1c13f92;
constexpr std::uint32_t clrex = 0xf57ff01f;
constexpr std::uint32_t lda_r1_r0 = 0xe1901c9f;
constexpr std::uint32_t stl_r1_r0 = 0xe180fc91;
constexpr std::uint32_t ldaex_r1_r0 = 0xe1901e9f;
constexpr std::uint32_t stlex_r2_r1_r0 = 0xe1802e91;

[[nodiscard]] bool vector_memory_mnemonic(std::string_view name)
{
	return name == "VLDR" || name == "VSTR" || name == "VLDM" || name == "VLDMDB" ||
		name == "VSTM" || name == "VSTMDB" || name == "FLDMIAX" || name == "FLDMDBX" ||
		name == "FSTMIAX" || name == "FSTMDBX" ||
		(name.size() == 4 && (name.starts_with("VLD") || name.starts_with("VST")) &&
			name[3] >= '1' && name[3] <= '4');
}

void test_coverage()
{
	std::size_t covered = 0;
	for (std::size_t index = 0; index < kFormCount; ++index) {
		const auto form = static_cast<Form>(index);
		const FormInfo &info = kForms[index];
		const bool pseudo = form == Form::Undefined || form == Form::ReservedHint;
		/* Everything up to the library's level, and the VFP and SIMD memory
		 * forms that need no arithmetic. */
		const bool expected = pseudo || (info.availability == Availability::implemented &&
							(info.level <= a32_semantics_level() ||
								(info.level == Level::simd &&
									vector_memory_mnemonic(info.mnemonic))));
		require(a32_semantics_covers(form) == expected,
			"the coverage predicate follows the level and the memory group");
		covered += a32_semantics_covers(form) ? 1 : 0;
	}
	require(covered > 1000, "the library covers the base level and the SIMD memory group");
}

void test_events()
{
	Fixture f;
	/* A form above the library's level is fatal, and changes nothing. */
	f.cpu.registers[0] = 0x1234;
	const auto vadd = decode_a32(0xf2000d00, Profile::at(Level::complete));
	require(vadd.legal(), "vadd decodes at complete");
	f.machine.put(code_base, std::uint32_t{0xf2000d00});
	f.set_pc(code_base, false);
	auto result = execute_a32(f.cpu, vadd, f.machine);
	require(result.event == StepEvent::fatal && f.pc() == code_base && f.r(0) == 0x1234,
		"a form above the level is fatal and changes nothing");

	/* A word the decoder rejects is undefined. */
	f.cpu.a32_itstate = 0;
	result = f.step_a32_word(0xf2000d00);
	require(result.event == StepEvent::undefined && f.pc() == code_base,
		"a word above the profile's level is undefined at the PC");

	/* UDF. */
	result = f.step_a32_word(0xe7f000f0);
	require(result.event == StepEvent::undefined && f.pc() == code_base, "UDF is undefined");

	/* SVC advances the PC; BKPT stays and reports its immediate. */
	result = f.step_a32_word(0xef000005);
	require(result.event == StepEvent::supervisor_call && result.syndrome == 5 &&
			f.pc() == code_base + 4,
		"SVC leaves the PC past the instruction");
	result = f.step_a32_word(0xe1212374);
	require(result.event == StepEvent::breakpoint && result.syndrome == 0x1234 && f.pc() == code_base,
		"BKPT stays at the instruction and reports its immediate");
	/* Thumb BKPT and SVC. */
	result = f.step_t32({0xbe42});
	require(result.event == StepEvent::breakpoint && result.syndrome == 0x42 && f.pc() == code_base,
		"T16 BKPT");
	result = f.step_t32({0xdf07});
	require(result.event == StepEvent::supervisor_call && result.syndrome == 7 && f.pc() == code_base + 2,
		"T16 SVC leaves the PC past the instruction");

	/* A failed condition is a retired NOP. */
	f.cpu.nzcv = 0;
	f.cpu.registers[0] = 5;
	result = f.step_a32_word(0x02800001); /* addeq r0, r0, #1 */
	require(result.event == StepEvent::retired && f.r(0) == 5 && f.pc() == code_base + 4,
		"a failed condition retires with no effect");
	f.cpu.nzcv = 0x4000'0000;
	result = f.step_a32_word(0x02800001);
	require(result.event == StepEvent::retired && f.r(0) == 6, "a passed condition executes");

	/* The A32 target with bit 1 set is a PC alignment event at the next step. */
	f.cpu.registers[1] = code_base + 0x102;
	result = f.step_a32_word(0xe12fff11); /* bx r1 */
	require(result.event == StepEvent::retired && !key_is_canonical(f.cpu.pc),
		"BX to an unaligned A32 target leaves a non canonical key");
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::pc_alignment && result.fault_address == code_base + 0x102,
		"the next fetch is the PC alignment event");

	/* A fetch from a page that cannot be executed. */
	f.set_pc(data_base, false);
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::memory_fault && result.access == MemoryAccess::execute &&
			result.fault_address == data_base && f.pc() == data_base,
		"an unfetchable PC faults for execute at the PC");
}

void test_it_state()
{
	Fixture f;
	/* it eq ; addeq r0, r0, #1 ; run as the architecture does: IT, then the
	 * block's instructions with ITAdvance after each. */
	f.cpu.registers[0] = 10;
	f.cpu.nzcv = 0x4000'0000; /* Z set */
	std::uint32_t at = code_base;
	for (const std::uint16_t half : {std::uint16_t{0xbf08}, std::uint16_t{0x1c40}}) {
		f.machine.put(at, half);
		at += 2;
	}
	f.set_pc(code_base, true);
	auto result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::retired && f.cpu.a32_itstate == 0x08, "IT starts the block");
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::retired && f.r(0) == 11 && f.cpu.a32_itstate == 0,
		"the conditional instruction executes and the block ends");
	/* The flag-setting 16-bit add inside an IT block does not set flags. */
	require(f.cpu.nzcv == 0x4000'0000, "an IT block's 16-bit add leaves the flags");

	/* The same entered in the middle: a state the engine resumes with. */
	f.cpu.nzcv = 0;
	f.cpu.registers[0] = 10;
	f.cpu.a32_itstate = 0x08;
	f.set_pc(code_base + 2, true);
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::retired && f.r(0) == 10 && f.cpu.a32_itstate == 0 &&
			f.pc() == code_base + 4,
		"a mid-IT entry honours the state and advances it");

	/* A fault inside a block reports the instruction's own ITSTATE. */
	f.cpu.registers[0] = data_base + 0x10;
	f.cpu.nzcv = 0x4000'0000;
	f.machine.put(code_base, std::uint16_t{0x6800}); /* ldr r0, [r0] */
	f.cpu.registers[0] = 0xffff'fffc;
	f.cpu.a32_itstate = 0x08;
	f.set_pc(code_base, true);
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::memory_fault && f.cpu.a32_itstate == 0x08 && f.pc() == code_base,
		"a fault leaves the instruction's own ITSTATE");
}

void test_load_multiple_writes_last()
{
	Fixture f;
	f.machine.unmap(data_base + page);
	for (unsigned n = 1; n < 4; ++n)
		f.cpu.registers[n] = 0xdead'0000 + n;
	/* r0 points at the last word of the first page: the block crosses into
	 * the unmapped page after one word. */
	f.cpu.registers[0] = data_base + page - 4;
	f.machine.put(data_base + page - 4, std::uint32_t{0x11});
	auto result = f.step_a32_word(ldm_r0_r1_r3);
	require(result.event == StepEvent::memory_fault && result.access == MemoryAccess::read &&
			result.fault_address == data_base + page && !result.alignment,
		"an LDM faults at the first word of the page that does not translate");
	require(f.r(1) == 0xdead'0001 && f.r(2) == 0xdead'0002 && f.pc() == code_base,
		"a faulting load changes no register");
}

void test_store_multiple_partial()
{
	Fixture f;
	f.machine.unmap(data_base + page);
	for (unsigned n = 1; n < 4; ++n)
		f.cpu.registers[n] = 0xaaaa'0000 + n;
	f.cpu.registers[0] = data_base + page - 8;
	/* stm r0!, {r1, r2, r3}: two words fit, the third faults. */
	auto result = f.step_a32_word(stmia_wb_r0_r1_r3);
	require(result.event == StepEvent::memory_fault && result.access == MemoryAccess::write &&
			result.fault_address == data_base + page,
		"an STM faults at the word that does not translate");
	require(f.machine.get<std::uint32_t>(data_base + page - 8) == 0xaaaa'0001 &&
			f.machine.get<std::uint32_t>(data_base + page - 4) == 0xaaaa'0002,
		"the words ahead of the fault are stored");
	require(f.r(0) == data_base + page - 8 && f.pc() == code_base,
		"the base register is unchanged");
}

void test_single_store_is_all_or_nothing()
{
	Fixture f;
	f.machine.unmap(data_base + page);
	/* A word store that straddles into the unmapped page writes no byte. */
	f.cpu.registers[0] = data_base + page - 2;
	f.cpu.registers[1] = 0x8877'6655;
	auto result = f.step_a32_word(str_r1_r0);
	require(result.event == StepEvent::memory_fault && result.fault_address == data_base + page &&
			f.machine.get<std::uint16_t>(data_base + page - 2) == 0,
		"a straddling store writes nothing");
	/* STRD at a word-aligned address that straddles the same way. */
	f.cpu.registers[0] = data_base + page - 4;
	f.cpu.registers[2] = 1;
	f.cpu.registers[3] = 2;
	result = f.step_a32_word(strd_r2_r0);
	require(result.event == StepEvent::memory_fault && result.fault_address == data_base + page &&
			f.machine.get<std::uint32_t>(data_base + page - 4) == 0,
		"a straddling doubleword store writes nothing");
	require(f.machine.stores.empty(), "no store was reported");

	/* The same load straddling two mapped pages reads across the host gap. */
	f.machine.map(data_base + page);
	f.machine.put(data_base + page - 2, std::uint16_t{0x2211});
	f.machine.put(data_base + page, std::uint16_t{0x4433});
	f.cpu.registers[0] = data_base + page - 2;
	result = f.step_a32_word(ldr_r1_r0);
	require(result.event == StepEvent::retired && f.r(1) == 0x4433'2211,
		"an unaligned load reads across a page boundary");
	result = f.step_a32_word(str_r1_r0);
	require(result.event == StepEvent::retired && f.machine.stores.size() == 2 &&
			f.machine.get<std::uint16_t>(data_base + page) == 0x4433,
		"an unaligned store reports one range per page");
}

void test_alignment_faults()
{
	Fixture f;
	f.cpu.registers[0] = data_base + 2;
	auto result = f.step_a32_word(ldrd_r2_r0);
	require(result.event == StepEvent::memory_fault && result.alignment &&
			result.fault_address == data_base + 2 && result.access == MemoryAccess::read,
		"LDRD needs word alignment");
	result = f.step_a32_word(ldm_r0_r1_r3);
	require(result.event == StepEvent::memory_fault && result.alignment, "LDM needs word alignment");
	f.cpu.registers[0] = data_base + 1;
	result = f.step_a32_word(ldrex_r1_r0);
	require(result.event == StepEvent::memory_fault && result.alignment && result.fault_address == data_base + 1,
		"LDREX needs natural alignment");
	result = f.step_a32_word(lda_r1_r0);
	require(result.event == StepEvent::memory_fault && result.alignment, "LDA needs natural alignment");
	result = f.step_a32_word(stl_r1_r0);
	require(result.event == StepEvent::memory_fault && result.alignment &&
			result.access == MemoryAccess::write,
		"STL needs natural alignment");
	f.cpu.registers[0] = data_base + 4;
	result = f.step_a32_word(ldrexd_r2_r0);
	require(result.event == StepEvent::memory_fault && result.alignment, "LDREXD needs doubleword alignment");
	/* A plain word load does not. */
	f.cpu.registers[0] = data_base + 1;
	result = f.step_a32_word(ldr_r1_r0);
	require(result.event == StepEvent::retired, "LDR allows unaligned addresses");
	/* The address wraps at 4 GiB. */
	f.cpu.registers[0] = 0xffff'fffe;
	result = f.step_a32_word(ldr_r1_r0);
	require(result.event == StepEvent::memory_fault && result.fault_address == 0xffff'fffe,
		"an access at the top of the address space faults at its first byte");
}

void test_exclusives()
{
	Fixture f;
	const std::uint32_t address = data_base + 0x40;
	f.machine.put(address, std::uint32_t{0x1111'1111});
	f.cpu.registers[0] = address;
	f.cpu.registers[3] = 0x2222'2222;

	auto result = f.step_a32_word(ldrex_r1_r0);
	require(result.event == StepEvent::retired && f.r(1) == 0x1111'1111 && f.cpu.exclusive_size == 4 &&
			f.cpu.exclusive_address == address && f.cpu.exclusive_value == 0x1111'1111 &&
			f.cpu.exclusive_key != 0,
		"LDREX records the monitor");
	result = f.step_a32_word(strex_r2_r3_r0);
	require(result.event == StepEvent::retired && f.r(2) == 0 &&
			f.machine.get<std::uint32_t>(address) == 0x2222'2222 && f.cpu.exclusive_size == 0 &&
			!f.machine.stores.empty(),
		"STREX stores after LDREX and clears the monitor");
	f.cpu.registers[3] = 0x3333'3333;
	f.machine.stores.clear();
	result = f.step_a32_word(strex_r2_r3_r0);
	require(result.event == StepEvent::retired && f.r(2) == 1 &&
			f.machine.get<std::uint32_t>(address) == 0x2222'2222 && f.machine.stores.empty(),
		"STREX without a monitor fails and stores nothing");

	/* The location changing between the pair fails the store. */
	(void)f.step_a32_word(ldrex_r1_r0);
	f.machine.put(address, std::uint32_t{0x4444'4444});
	result = f.step_a32_word(strex_r2_r3_r0);
	require(f.r(2) == 1 && f.machine.get<std::uint32_t>(address) == 0x4444'4444 && f.cpu.exclusive_size == 0,
		"STREX fails when the value changed and clears the monitor");

	/* CLREX. */
	(void)f.step_a32_word(ldrex_r1_r0);
	result = f.step_a32_word(clrex);
	require(result.event == StepEvent::retired && f.cpu.exclusive_size == 0, "CLREX clears the monitor");
	(void)f.step_a32_word(strex_r2_r3_r0);
	require(f.r(2) == 1, "STREX after CLREX fails");

	/* A store-exclusive to another address fails. */
	(void)f.step_a32_word(ldrex_r1_r0);
	f.cpu.registers[0] = address + 4;
	(void)f.step_a32_word(strex_r2_r3_r0);
	require(f.r(2) == 1 && f.machine.get<std::uint32_t>(address + 4) == 0, "STREX needs the monitored address");

	/* STREX to an unmapped page faults even without a monitor. */
	f.cpu.registers[0] = 0x3000'0000;
	result = f.step_a32_word(strex_r2_r3_r0);
	require(result.event == StepEvent::memory_fault && result.access == MemoryAccess::write &&
			result.fault_address == 0x3000'0000,
		"STREX translates its address first");

	/* Doubleword and byte forms. */
	f.cpu.registers[0] = address + 8;
	f.machine.put(address + 8, std::uint64_t{0x8877'6655'4433'2211});
	result = f.step_a32_word(ldrexd_r2_r0);
	require(f.r(2) == 0x4433'2211 && f.r(3) == 0x8877'6655 && f.cpu.exclusive_size == 8,
		"LDREXD loads the pair");
	f.cpu.registers[4] = 0xa1a2'a3a4;
	f.cpu.registers[5] = 0xb1b2'b3b4;
	result = f.step_a32_word(strexd_r1_r4_r0);
	require(f.r(1) == 0 && f.machine.get<std::uint64_t>(address + 8) == 0xb1b2'b3b4'a1a2'a3a4,
		"STREXD stores the pair");
	f.cpu.registers[1] = address + 0x11;
	f.machine.put(address + 0x11, std::uint8_t{0x5a});
	(void)f.step_a32_word(ldrexb_r2_r1);
	require(f.r(2) == 0x5a && f.cpu.exclusive_size == 1, "LDREXB");
	f.cpu.registers[2] = 0x1ff;
	(void)f.step_a32_word(strexb_r3_r2_r1);
	require(f.r(3) == 0 && f.machine.get<std::uint8_t>(address + 0x11) == 0xff, "STREXB stores a byte");

	/* LDAEX / STLEX. */
	f.cpu.registers[0] = address;
	f.machine.put(address, std::uint32_t{0x77});
	(void)f.step_a32_word(ldaex_r1_r0);
	f.cpu.registers[1] = 0x78;
	(void)f.step_a32_word(stlex_r2_r1_r0);
	require(f.r(2) == 0 && f.machine.get<std::uint32_t>(address) == 0x78, "STLEX after LDAEX");
	/* LDA and STL do not touch the monitor. */
	(void)f.step_a32_word(ldrex_r1_r0);
	(void)f.step_a32_word(lda_r1_r0);
	require(f.cpu.exclusive_size == 4, "LDA leaves the monitor");
	f.cpu.registers[1] = 0x99;
	(void)f.step_a32_word(stl_r1_r0);
	require(f.machine.get<std::uint32_t>(address) == 0x99, "STL stores");
}

void test_branches_and_stack()
{
	Fixture f;
	/* BL sets LR to the next instruction; BLX (immediate) switches to Thumb. */
	auto result = f.step_a32_word(0xeb000002);
	require(result.event == StepEvent::retired && f.pc() == code_base + 16 &&
			key_is_aarch32(f.cpu.pc) && !key_is_thumb(f.cpu.pc) && f.r(14) == code_base + 4,
		"BL branches and links");
	result = f.step_a32_word(0xfa000002);
	require(result.event == StepEvent::retired && f.pc() == code_base + 16 && key_is_thumb(f.cpu.pc) &&
			f.r(14) == code_base + 4,
		"BLX immediate changes to Thumb");
	/* BX with bit 0 set changes to Thumb and clears the bit. */
	f.cpu.registers[1] = code_base + 0x21;
	result = f.step_a32_word(0xe12fff11);
	require(result.event == StepEvent::retired && key_is_thumb(f.cpu.pc) && f.pc() == code_base + 0x20,
		"BX to an odd address enters Thumb");
	/* The Thumb BLX register form links with the Thumb bit. */
	f.cpu.registers[1] = code_base + 0x40;
	result = f.step_t32({0x4788});
	require(result.event == StepEvent::retired && !key_is_thumb(f.cpu.pc) && f.pc() == code_base + 0x40 &&
			f.r(14) == (code_base + 2 + 1),
		"T16 BLX register links with the Thumb bit");

	/* PUSH and POP with writeback; POP {pc} interworks. */
	f.cpu.registers[13] = data_base + 0x100;
	f.cpu.registers[4] = 0x44;
	f.cpu.registers[5] = 0x55;
	result = f.step_a32_word(0xe92d0030);
	require(result.event == StepEvent::retired && f.r(13) == data_base + 0xf8 &&
			f.machine.get<std::uint32_t>(data_base + 0xf8) == 0x44 &&
			f.machine.get<std::uint32_t>(data_base + 0xfc) == 0x55,
		"PUSH stores the lowest register at the lowest address");
	f.cpu.registers[4] = f.cpu.registers[5] = 0;
	result = f.step_a32_word(0xe8bd0030);
	require(result.event == StepEvent::retired && f.r(13) == data_base + 0x100 && f.r(4) == 0x44 &&
			f.r(5) == 0x55,
		"POP restores the registers and the stack pointer");
	f.machine.put(data_base + 0x100, std::uint32_t{code_base + 0x81});
	f.cpu.registers[0] = data_base + 0x100;
	result = f.step_a32_word(0xe8b08000); /* ldm r0!, {pc} */
	require(result.event == StepEvent::retired && key_is_thumb(f.cpu.pc) && f.pc() == code_base + 0x80 &&
			f.r(0) == data_base + 0x104,
		"LDM into the PC interworks and writes back");
}

void test_it_udf()
{
	Fixture f;
	/* A UDF in an IT block whose condition fails is a retired NOP; one
	 * that passes is undefined at its own ITSTATE. */
	f.machine.put(code_base, std::uint16_t{0xbf08}); /* it eq */
	f.machine.put(code_base + 2, std::uint16_t{0xde01}); /* udf #1 */
	f.cpu.nzcv = 0;
	f.set_pc(code_base, true);
	auto result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::retired, "IT");
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::retired && f.cpu.a32_itstate == 0 && f.pc() == code_base + 4,
		"a failed UDF retires and ends the block");
	f.cpu.nzcv = 0x4000'0000;
	f.set_pc(code_base, true);
	f.cpu.a32_itstate = 0;
	(void)step_a32(f.cpu, f.machine, f.profile);
	result = step_a32(f.cpu, f.machine, f.profile);
	require(result.event == StepEvent::undefined && f.cpu.a32_itstate == 0x08 && f.pc() == code_base + 2,
		"a UDF that passes is undefined at its own ITSTATE");
}

void test_vector_memory()
{
	Fixture f;
	f.profile = Profile::at(Level::complete);
	const std::uint32_t area = data_base + 0x100;
	f.cpu.registers[0] = area;
	f.machine.put(area + 8, std::uint64_t{0x1122'3344'5566'7788});
	f.machine.put(area + 4, std::uint32_t{0xcafe'f00d});

	/* VLDR / VSTR, D and S. */
	auto result = f.step_a32_word(0xed901b02); /* vldr d1, [r0, #8] */
	require(result.event == StepEvent::retired && f.d(1) == 0x1122'3344'5566'7788, "VLDR D");
	result = f.step_a32_word(0xedd01a01); /* vldr s3, [r0, #4] */
	require(result.event == StepEvent::retired && f.s(3) == 0xcafe'f00d && f.d(1) >> 32 == 0xcafe'f00d,
		"VLDR S writes only its word of the D register");
	f.machine.put(area + 8, std::uint64_t{0});
	result = f.step_a32_word(0xed801b02); /* vstr d1, [r0, #8] */
	require(result.event == StepEvent::retired &&
			f.machine.get<std::uint64_t>(area + 8) == 0xcafe'f00d'5566'7788,
		"VSTR D");
	result = f.step_a32_word(0xedc01a01); /* vstr s3, [r0, #4] */
	require(result.event == StepEvent::retired && f.machine.get<std::uint32_t>(area + 4) == 0xcafe'f00d,
		"VSTR S");

	/* VLDR (literal) reads from the word-aligned PC. */
	f.machine.put(code_base + 16, std::uint64_t{0x0102'0304'0506'0708});
	result = f.step_a32_word(0xed9f1b02); /* vldr d1, [pc, #8] */
	require(result.event == StepEvent::retired && f.d(1) == 0x0102'0304'0506'0708, "VLDR literal");

	/* MemA: a misaligned address is an alignment fault, with nothing changed. */
	f.cpu.registers[0] = area + 2;
	result = f.step_a32_word(0xed901b02);
	require(result.event == StepEvent::memory_fault && result.alignment &&
			result.fault_address == area + 10 && f.d(1) == 0x0102'0304'0506'0708,
		"VLDR needs word alignment");
	result = f.step_a32_word(0xec902b06); /* vldm r0, {d2-d4} */
	require(result.event == StepEvent::memory_fault && result.alignment && result.fault_address == area + 2,
		"VLDM needs word alignment");

	/* VLDM that runs into an unmapped page changes no register. */
	f.machine.unmap(data_base + page);
	f.cpu.registers[0] = data_base + page - 16;
	f.cpu.vectors[1].bytes.fill(std::byte{0x5a});
	result = f.step_a32_word(0xec902b06);
	require(result.event == StepEvent::memory_fault && !result.alignment &&
			result.access == MemoryAccess::read && result.fault_address == data_base + page &&
			f.d(2) == 0x5a5a'5a5a'5a5a'5a5a,
		"a faulting VLDM leaves the registers");

	/* VSTM stores the words ahead of the fault and leaves the base. */
	f.cpu.registers[0] = data_base + page - 12;
	f.set_d(2, 0x2121'2121'2121'2121);
	f.set_d(3, 0x3131'3131'3131'3131);
	f.set_d(4, 0x4141'4141'4141'4141);
	result = f.step_a32_word(0xeca02b06); /* vstmia r0!, {d2-d4} */
	require(result.event == StepEvent::memory_fault && result.access == MemoryAccess::write &&
			result.fault_address == data_base + page && f.r(0) == data_base + page - 12,
		"a faulting VSTM leaves the base");
	require(f.machine.get<std::uint32_t>(data_base + page - 12) == 0x2121'2121 &&
			f.machine.get<std::uint32_t>(data_base + page - 8) == 0x2121'2121 &&
			f.machine.get<std::uint32_t>(data_base + page - 4) == 0x3131'3131,
		"the words ahead of the fault are stored");
	f.machine.map(data_base + page);

	/* VLDM with writeback; FSTMX writes the doublewords of an odd count. */
	f.cpu.registers[0] = area;
	for (unsigned n = 0; n < 3; ++n)
		f.machine.put(area + 4 * n, std::uint32_t{0x100 + n});
	result = f.step_a32_word(0xecb02a03); /* vldmia r0!, {s4-s6} */
	require(result.event == StepEvent::retired && f.s(4) == 0x100 && f.s(5) == 0x101 &&
			f.s(6) == 0x102 && f.r(0) == area + 12,
		"VLDM single registers with writeback");
	f.cpu.registers[0] = area + 0x40;
	f.set_d(2, 0x7777'7777'7777'7777);
	f.set_d(3, 0x7777'7777'7777'7777);
	result = f.step_a32_word(0xec802b05); /* fstmiax r0, {d2, d3} */
	require(result.event == StepEvent::retired && f.r(0) == area + 0x40 &&
			f.machine.get<std::uint64_t>(area + 0x40) == 0x7777'7777'7777'7777 &&
			f.machine.get<std::uint64_t>(area + 0x48) == 0x7777'7777'7777'7777 &&
			f.machine.get<std::uint64_t>(area + 0x50) == 0,
		"FSTMX stores its doublewords");
}

void test_vector_structures()
{
	Fixture f;
	f.profile = Profile::at(Level::complete);
	const std::uint32_t area = data_base + 0x200;
	for (unsigned n = 0; n < 32; ++n)
		f.machine.put(area + n, static_cast<std::uint8_t>(n));
	f.cpu.registers[0] = area;
	f.cpu.registers[1] = 24;

	/* VLD1 multiple: consecutive. */
	auto result = f.step_a32_word(0xf4200a8d); /* vld1.32 {d0, d1}, [r0]! */
	require(result.event == StepEvent::retired && f.d(0) == 0x0706'0504'0302'0100 &&
			f.d(1) == 0x0f0e'0d0c'0b0a'0908 && f.r(0) == area + 16,
		"VLD1 loads consecutive doublewords and writes back");
	f.cpu.registers[0] = area;
	result = f.step_a32_word(0xf4200741); /* vld1.16 {d0}, [r0], r1 */
	require(result.event == StepEvent::retired && f.r(0) == area + 24, "post-indexed by a register");

	/* VLD2, VLD3 and VLD4 de-interleave. */
	f.cpu.registers[0] = area;
	result = f.step_a32_word(0xf420030f); /* vld2.8 {d0-d3}, [r0] */
	/* Four registers are two pairs (D0, D2) and (D1, D3). */
	require(result.event == StepEvent::retired && f.d(0) == 0x0e0c'0a08'0604'0200 &&
			f.d(2) == 0x0f0d'0b09'0705'0301 && f.d(1) == 0x1e1c'1a18'1614'1210 &&
			f.d(3) == 0x1f1d'1b19'1715'1311,
		"VLD2 de-interleaves pairs");
	result = f.step_a32_word(0xf420040f); /* vld3.8 {d0, d1, d2}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0x1512'0f0c'0906'0300 &&
			f.d(1) == 0x1613'100d'0a07'0401 && f.d(2) == 0x1714'110e'0b08'0502,
		"VLD3 de-interleaves triples");
	result = f.step_a32_word(0xf420000f); /* vld4.8 {d0-d3}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0x1c18'1410'0c08'0400 &&
			f.d(3) == 0x1f1b'1713'0f0b'0703,
		"VLD4 de-interleaves quadruples");

	/* VST4 interleaves it back. */
	f.cpu.registers[0] = area + 0x80;
	result = f.step_a32_word(0xf400000f); /* vst4.8 {d0-d3}, [r0] */
	require(result.event == StepEvent::retired);
	for (unsigned n = 0; n < 32; ++n)
		require(f.machine.get<std::uint8_t>(area + 0x80 + n) == n, "VST4 interleaves");
	result = f.step_a32_word(0xf400084f); /* vst2.16 {d0, d1}, [r0] */
	require(result.event == StepEvent::retired &&
			f.machine.get<std::uint16_t>(area + 0x80 + 2) == 0x0501,
		"VST2");

	/* Single lane: only that element changes. */
	f.cpu.registers[0] = area;
	f.cpu.vectors[0].bytes.fill(std::byte{0xee});
	result = f.step_a32_word(0xf4a0048f); /* vld1.16 {d0[2]}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0xeeee'0100'eeee'eeee,
		"VLD1 to one lane");
	f.cpu.registers[0] = area + 0x90;
	result = f.step_a32_word(0xf480088f); /* vst1.32 {d0[1]}, [r0] */
	require(result.event == StepEvent::retired &&
			f.machine.get<std::uint32_t>(area + 0x90) == 0xeeee'0100,
		"VST1 from one lane");
	f.cpu.registers[0] = area;
	f.cpu.vectors[0].bytes.fill(std::byte{0});
	f.cpu.vectors[1].bytes.fill(std::byte{0});
	result = f.step_a32_word(0xf4a00a8f); /* vld3.32 {d0[1], d1[1], d2[1]}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0x0302'0100'0000'0000 &&
			f.d(1) == 0x0706'0504'0000'0000 && f.d(2) == 0x0b0a'0908'0000'0000,
		"VLD3 to one lane of three registers");

	/* All lanes replicate. */
	result = f.step_a32_word(0xf4a00c0f); /* vld1.8 {d0[]}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0x0000'0000'0000'0000,
		"VLD1 to all lanes of byte zero");
	f.machine.put(area, std::uint16_t{0xa1b2});
	result = f.step_a32_word(0xf4a00c6f); /* vld1.16 {d0[], d1[]}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0xa1b2'a1b2'a1b2'a1b2 &&
			f.d(1) == 0xa1b2'a1b2'a1b2'a1b2,
		"VLD1 to all lanes of two registers");
	f.machine.put(area, std::uint32_t{0x04030201});
	f.machine.put(area + 4, std::uint32_t{0x08070605});
	result = f.step_a32_word(0xf4a00d0f); /* vld2.8 {d0[], d1[]}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0x0101'0101'0101'0101 &&
			f.d(1) == 0x0202'0202'0202'0202,
		"VLD2 to all lanes");
	result = f.step_a32_word(0xf4a00f4f); /* vld4.16 {d0[], d1[], d2[], d3[]}, [r0] */
	require(result.event == StepEvent::retired && f.d(0) == 0x0201'0201'0201'0201 &&
			f.d(3) == 0x0807'0807'0807'0807,
		"VLD4 to all lanes");

	/* The alignment qualifier faults when the address lacks it. */
	f.cpu.registers[0] = area + 8;
	result = f.step_a32_word(0xf4200aaf); /* vld1.32 {d0, d1}, [r0:128] */
	require(result.event == StepEvent::memory_fault && result.alignment && result.fault_address == area + 8,
		"an alignment hint is enforced");
	f.cpu.registers[0] = area + 16;
	result = f.step_a32_word(0xf4200aaf);
	require(result.event == StepEvent::retired, "an aligned hint passes");

	/* A structure access that crosses into an unmapped page changes nothing:
	 * a load leaves the registers, a store writes no byte. */
	f.machine.unmap(data_base + page);
	f.cpu.registers[0] = data_base + page - 4;
	f.cpu.vectors[0].bytes.fill(std::byte{0x33});
	result = f.step_a32_word(0xf420070f); /* vld1.8 {d0}, [r0] */
	require(result.event == StepEvent::memory_fault && result.fault_address == data_base + page &&
			f.d(0) == 0x3333'3333'3333'3333,
		"a faulting VLD1 leaves the register");
	result = f.step_a32_word(0xf400070f); /* vst1.8 {d0}, [r0] */
	require(result.event == StepEvent::memory_fault && result.access == MemoryAccess::write &&
			f.machine.get<std::uint32_t>(data_base + page - 4) == 0,
		"a faulting VST1 stores nothing");
}

void test_system()
{
	Fixture f;
	/* MSR APSR_nzcvq, r1 then MRS r0, apsr. */
	f.cpu.registers[1] = 0xf800'0000;
	auto result = f.step_a32_word(0xe128f001);
	require(result.event == StepEvent::retired && f.cpu.nzcv == 0xf000'0000 && f.cpu.a32_q == 1,
		"MSR writes N, Z, C, V and Q");
	f.cpu.a32_ge_mask = 0x00ff'00ff;
	result = f.step_a32_word(0xe10f0000);
	require(f.r(0) == 0xf805'0010, "MRS reads the flags with the User mode field");

	/* The CP15 barrier is a barrier when enabled and undefined otherwise. */
	result = f.step_a32_word(0xee070fba);
	require(result.event == StepEvent::retired, "the CP15 barrier executes when enabled");
	f.cpu.a32_cp15ben = 0;
	result = f.step_a32_word(0xee070fba);
	require(result.event == StepEvent::undefined && f.pc() == code_base,
		"the CP15 barrier is undefined when disabled");
}

void test_counters()
{
	Fixture f;
	reset_a32_library_exits();
	count_a32_library_exits(false);
	f.cpu.registers[0] = data_base;
	(void)f.step_a32_word(ldr_r1_r0);
	require(a32_library_exit_total() == 0, "counting is off by default");
	count_a32_library_exits(true);
	(void)f.step_a32_word(ldr_r1_r0);
	(void)f.step_a32_word(ldr_r1_r0);
	(void)f.step_a32_word(str_r1_r0);
	count_a32_library_exits(false);
	const auto ldr = decode_a32(ldr_r1_r0, Profile::at(Level::base)).form;
	const auto str = decode_a32(str_r1_r0, Profile::at(Level::base)).form;
	require(a32_library_exits(ldr) == 2 && a32_library_exits(str) == 1 && a32_library_exit_total() == 3,
		"every call counts against its form");
	reset_a32_library_exits();
	require(a32_library_exit_total() == 0 && a32_library_exits(ldr) == 0, "reset clears the counts");
}

} // namespace

int main()
{
	test_coverage();
	test_events();
	test_it_state();
	test_load_multiple_writes_last();
	test_store_multiple_partial();
	test_single_store_is_all_or_nothing();
	test_alignment_faults();
	test_exclusives();
	test_branches_and_stack();
	test_it_udf();
	test_vector_memory();
	test_vector_structures();
	test_system();
	test_counters();
	return 0;
}
