// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <source_location>
#include <sys/mman.h>
#include <unistd.h>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

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

class Mapping final {
public:
	Mapping()
	{
		char path[] = "/private/tmp/uml-nzcv-residency.XXXXXX";
		fd_ = mkstemp(path);
		require(fd_ >= 0);
		require(unlink(path) == 0);
		require(ftruncate(fd_, DARWIN_UML_GUEST_PAGE_SIZE) == 0);
		constexpr std::uint64_t value = 0x0123'4567'89ab'cdefULL;
		require(pwrite(fd_, &value, sizeof(value), 0) ==
			static_cast<ssize_t>(sizeof(value)));
		require(darwin_uml_static_vm_map_generation(generation,
				reinterpret_cast<void *>(guest), fd_, 0,
				DARWIN_UML_GUEST_PAGE_SIZE,
				PROT_READ | PROT_WRITE, 0, 1) == 0);
	}

	Mapping(const Mapping &) = delete;
	Mapping &operator=(const Mapping &) = delete;

	~Mapping()
	{
		require(darwin_uml_static_vm_destroy(generation) == 0);
		require(close(fd_) == 0);
	}

	static constexpr unsigned long generation = 412;
	static constexpr std::uint64_t guest = 0x1240'000;

private:
	int fd_{-1};
};

template<std::size_t Size>
arm64::CpuState execute(const std::array<std::uint32_t, Size> &instructions,
	arm64::CpuState cpu)
{
	const auto built = gadgets::Builder{}.build(0x9000, instructions);
	require(built.complete);
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(Mapping::generation,
		darwin_uml_static_vm_changes(Mapping::generation));
	require(gadgets::execute(built.program, cpu, *tlb) ==
		gadgets::Event::system_call);
	return cpu;
}

} // namespace

int main()
{
	Mapping mapping;

	{
		const std::array instructions{
			0xb100'0420U, // adds x0,x1,#1
			0xf940'0062U, // ldr x2,[x3] (TLB miss clobbers host PSTATE)
			0x9a1f'03e4U, // adc x4,xzr,xzr
			0x9a87'00c5U, // csel x5,x6,x7,eq
			0xd400'0001U, // svc #0
		};
		arm64::CpuState initial{};
		initial.registers[1] = ~std::uint64_t{};
		initial.registers[3] = Mapping::guest;
		initial.registers[6] = 0x66;
		initial.registers[7] = 0x77;
		const auto cpu = execute(instructions, initial);
		require(cpu.registers[0] == 0);
		require(cpu.registers[2] == 0x0123'4567'89ab'cdefULL);
		require(cpu.registers[4] == 1);
		require(cpu.registers[5] == 0x66);
		require(cpu.nzcv == 0x6000'0000U);
	}

	{
		const std::array instructions{
			0xd51b'4208U, // msr nzcv,x8
			0xf940'0062U, // ldr x2,[x3]
			0xd53b'4209U, // mrs x9,nzcv
			0x9a87'40c5U, // csel x5,x6,x7,mi
			0xd400'0001U, // svc #0
		};
		arm64::CpuState initial{};
		initial.registers[3] = Mapping::guest;
		initial.registers[6] = 0x66;
		initial.registers[7] = 0x77;
		initial.registers[8] = 0xa000'0000U;
		const auto cpu = execute(instructions, initial);
		require(cpu.registers[5] == 0x66);
		require(cpu.registers[9] == 0xa000'0000U);
		require(cpu.nzcv == 0xa000'0000U);
	}

	{
		const std::array instructions{
			0x1e61'2000U, // fcmp d0,d1
			0xf940'0062U, // ldr x2,[x3]
			0x9a87'00c5U, // csel x5,x6,x7,eq
			0xd400'0001U, // svc #0
		};
		arm64::CpuState initial{};
		initial.registers[3] = Mapping::guest;
		initial.registers[6] = 0x66;
		initial.registers[7] = 0x77;
		const auto one = std::bit_cast<std::uint64_t>(1.0);
		for (unsigned index : {0U, 1U})
			for (unsigned byte = 0; byte < 8; ++byte)
				initial.vectors[index].bytes[byte] =
					static_cast<std::byte>(one >> (byte * 8U));
		const auto cpu = execute(instructions, initial);
		require(cpu.registers[5] == 0x66);
		require(cpu.nzcv == 0x6000'0000U);
	}
}
