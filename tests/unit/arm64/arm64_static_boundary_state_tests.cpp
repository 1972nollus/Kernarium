// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_guest.h"
#include "uml_darwin_cpu.h"
#include "uml_darwin_static_mmu.h"
#include "runtime/execution_engine.hpp"
#include "scratch_directory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <source_location>
#include <sys/mman.h>
#include <unistd.h>

namespace {

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

class TemporaryBacking final {
public:
	TemporaryBacking()
	{
		auto path = uml::testing::scratch_template(
			"uml-static-boundary.XXXXXX");
		descriptor_ = mkstemp(path.data());
		require(descriptor_ >= 0);
		require(unlink(path.c_str()) == 0);
		require(ftruncate(descriptor_, DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	}

	TemporaryBacking(const TemporaryBacking &) = delete;
	TemporaryBacking &operator=(const TemporaryBacking &) = delete;

	~TemporaryBacking()
	{
		if (descriptor_ >= 0)
			close(descriptor_);
	}

	void put32(std::size_t offset, std::uint32_t value)
	{
		put(offset, value);
	}

	void put64(std::size_t offset, std::uint64_t value)
	{
		put(offset, value);
	}

	[[nodiscard]] std::uint64_t get64(std::size_t offset) const
	{
		std::uint64_t value{};
		require(pread(descriptor_, &value, sizeof(value), offset) ==
			static_cast<ssize_t>(sizeof(value)));
		return value;
	}

	[[nodiscard]] int descriptor() const noexcept { return descriptor_; }

private:
	template<typename Value>
	void put(std::size_t offset, Value value)
	{
		require(pwrite(descriptor_, &value, sizeof(value), offset) ==
			static_cast<ssize_t>(sizeof(value)));
	}

	int descriptor_{-1};
};

class Mapping final {
public:
	Mapping(unsigned long generation, std::uint64_t base,
		TemporaryBacking &backing)
		: generation_{generation}
	{
		require(darwin_uml_static_vm_activate(generation) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(base), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE,
			PROT_READ | PROT_WRITE | PROT_EXEC, 1, 1) == 0);
	}

	Mapping(const Mapping &) = delete;
	Mapping &operator=(const Mapping &) = delete;

	~Mapping()
	{
		require(darwin_uml_static_vm_destroy(generation_) == 0);
	}

private:
	unsigned long generation_;
};

[[nodiscard]] constexpr std::uint32_t pstate(
	unsigned immediate, unsigned op2)
{
	return 0xd500'401fU | (3U << 16) |
		((immediate & 15U) << 8) | ((op2 & 7U) << 5);
}

int enter(darwin_uml_guest_registers &registers,
	darwin_uml_guest_fp_registers &fp, unsigned long generation)
{
	unsigned long tls = 0x1234'5678UL;
	return darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
}

using GuestEntry = int (*)(
	void *, void *, unsigned long, unsigned long, unsigned long *);

int enter_with(GuestEntry entry, darwin_uml_guest_registers &registers,
	darwin_uml_guest_fp_registers &fp, unsigned long generation)
{
	unsigned long tls = 0x1234'5678UL;
	return entry(&registers, &fp, sizeof(fp), generation, &tls);
}

void fpmr_survives_boundary()
{
	constexpr unsigned long generation = 200;
	constexpr std::uint64_t base = 0x50'0000;
	constexpr std::uint64_t source = 0xffff'ffff'ffff'ffffULL;
	constexpr std::uint64_t expected = 0x0000'003f'ff7f'c1ffULL;
	TemporaryBacking backing;
	backing.put32(0, 0xd51b'4440U);  // msr fpmr, x0
	backing.put32(4, 0xd400'0001U);  // svc #0
	backing.put32(8, 0xd53b'4441U);  // mrs x1, fpmr
	backing.put32(12, 0xd400'0001U); // svc #0
	Mapping mapping{generation, base, backing};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	registers.x[0] = source;

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 8);
	require(registers.fpmr == expected);
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 16);
	require(registers.x[1] == expected);
}

void pstate_and_svcr_survive_boundary()
{
	constexpr unsigned long generation = 201;
	constexpr std::uint64_t base = 0x51'0000;
	TemporaryBacking backing;
	backing.put32(0, pstate(1, 1));  // msr ssbs, #1
	backing.put32(4, pstate(1, 2));  // msr dit, #1
	backing.put32(8, pstate(1, 4));  // msr tco, #1
	backing.put32(12, pstate(7, 3)); // smstart smza
	backing.put32(16, 0xd400'0001U); // svc #0
	backing.put32(20, pstate(0, 1)); // msr ssbs, #0
	backing.put32(24, 0xd400'0001U); // svc #0
	Mapping mapping{generation, base, backing};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 20);
	require((registers.pstate & 0x0300'1000U) == 0x0300'1000U);
	require(registers.svcr == 3);
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 28);
	require((registers.pstate & 0x0000'1000U) == 0);
	require((registers.pstate & 0x0300'0000U) == 0x0300'0000U);
	require(registers.svcr == 3);
}

void exclusive_monitor_is_cleared_at_boundary()
{
	constexpr unsigned long generation = 202;
	constexpr std::uint64_t base = 0x52'0000;
	constexpr std::uint64_t data = base + 0x800;
	constexpr std::uint64_t original = 0x1122'3344'5566'7788ULL;
	constexpr std::uint64_t replacement = 0x8877'6655'4433'2211ULL;
	TemporaryBacking backing;
	backing.put32(0, 0xc85f'7c22U);  // ldxr x2, [x1]
	backing.put32(4, 0xd400'0001U);  // svc #0
	backing.put32(8, 0xc805'fc23U);  // stlxr w5, x3, [x1]
	backing.put32(12, 0xd400'0001U); // svc #0
	backing.put64(0x800, original);
	Mapping mapping{generation, base, backing};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	registers.x[1] = data;
	registers.x[3] = replacement;

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[2] == original);
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[5] == 1);
	require(backing.get64(0x800) == original);
}

void deferred_fp_source_is_refreshed_at_boundary()
{
	constexpr unsigned long generation = 203;
	constexpr std::uint64_t base = 0x53'0000;
	TemporaryBacking backing;
	backing.put32(0, 0x4f00'041fU); // movi v31.4s, #0
	backing.put32(4, 0xd400'0001U); // svc #0
	backing.put32(8, 0xd400'0001U); // svc #0
	Mapping mapping{generation, base, backing};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	std::memset(fp.vectors[31], 0xff, sizeof(fp.vectors[31]));

	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	for (const auto byte : fp.vectors[31])
		require(byte == 0);
	std::memset(fp.vectors[31], 0x5a, sizeof(fp.vectors[31]));
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	for (const auto byte : fp.vectors[31])
		require(byte == 0x5a);
}

void deferred_fp_source_is_materialized_on_linked_target()
{
	constexpr unsigned long generation = 204;
	constexpr std::uint64_t base = 0x54'0000;
	constexpr std::uint64_t first_value = 0x0123'4567'89ab'cdefULL;
	constexpr std::uint64_t second_value = 0xfedc'ba98'7654'3210ULL;
	TemporaryBacking backing;
	backing.put32(0, 0x1400'0004U);  // b base + 16
	backing.put32(16, 0x9e66'0000U); // fmov x0, d0
	backing.put32(20, 0xd400'0001U); // svc #0
	Mapping mapping{generation, base, backing};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};

	/* Translate and activate the FP target before its integer predecessor. */
	registers.pc = base + 16;
	std::memcpy(fp.vectors[0], &first_value, sizeof(first_value));
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == first_value);

	/* A new entry defers a different kernel frame. The cached predecessor now
	 * chains through the FP prefix and must observe this new source. */
	registers.pc = base;
	std::memcpy(fp.vectors[0], &second_value, sizeof(second_value));
	require(enter(registers, fp, generation) == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == second_value);
	require(registers.pc == base + 24);
}

void guest_keys_survive_every_engine_boundary()
{
	struct Engine final {
		GuestEntry enter;
		unsigned long generation;
		std::uint64_t base;
		};
		const std::array engines{
			Engine{&darwin_uml_static_enter_guest, 206, 0x56'0000},
			Engine{&darwin_uml_jit_enter_guest, 207, 0x57'0000},
		};
		require(uml::runtime::execution_engine_available(
			uml::runtime::ExecutionEngine::jit));
		for (const auto engine : engines) {
			TemporaryBacking backing;
			/*
			 * The production profile deliberately does not advertise
			 * pointer authentication. Dedicated feature-enabled engine
			 * tests cover its instructions; this boundary test only needs
			 * two ordinary entries to prove that the guest-owned keys are
			 * initialized once and then preserved.
			 */
			backing.put32(0, 0xd280'0022U);  // movz x2, #1
			backing.put32(4, 0xd400'0001U);  // svc #0
			backing.put32(8, 0xd280'0043U);  // movz x3, #2
			backing.put32(12, 0xd400'0001U); // svc #0
		Mapping mapping{engine.generation, engine.base, backing};
		darwin_uml_guest_registers registers{};
		darwin_uml_guest_fp_registers fp{};
		constexpr std::uint64_t pointer = 0x0000'1234'5678'9abcULL;
		registers.pc = engine.base;
		registers.x[0] = pointer;
		registers.x[1] = 0xfeed'face'0123'4567ULL;

		require(enter_with(
			engine.enter, registers, fp, engine.generation) ==
			DARWIN_UML_EVENT_SYSCALL);
		require(registers.pc == engine.base + 8);
		bool initialized = false;
		for (const auto &key : registers.pointer_auth_keys)
			initialized |= key[0] != 0 || key[1] != 0;
		require(initialized);
		std::array<std::array<std::uint64_t, 2>,
			DARWIN_UML_POINTER_AUTH_KEY_COUNT> keys{};
		std::memcpy(keys.data(), registers.pointer_auth_keys,
			sizeof(registers.pointer_auth_keys));

		require(enter_with(
			engine.enter, registers, fp, engine.generation) ==
			DARWIN_UML_EVENT_SYSCALL);
		require(registers.pc == engine.base + 16);
		require(registers.x[0] == pointer);
		require(std::memcmp(
			keys.data(), registers.pointer_auth_keys,
			sizeof(registers.pointer_auth_keys)) == 0);
	}
}

} // namespace

int main()
{
	fpmr_survives_boundary();
	pstate_and_svcr_survive_boundary();
	exclusive_monitor_is_cleared_at_boundary();
	deferred_fp_source_is_refreshed_at_boundary();
	deferred_fp_source_is_materialized_on_linked_target();
	guest_keys_survive_every_engine_boundary();
}
