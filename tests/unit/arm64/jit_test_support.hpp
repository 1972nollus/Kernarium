// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_guest.h"
#include "uml_darwin_cpu.h"
#include "uml_darwin_static_mmu.h"
#include "runtime/execution_engine.hpp"
#include "scratch_backing.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <source_location>
#include <span>
#include <sys/mman.h>
#include <unistd.h>

namespace uml::emulator::tests {

[[nodiscard]] inline bool jit_available() noexcept
{
	return ::uml::runtime::execution_engine_available(
		::uml::runtime::ExecutionEngine::jit);
}

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

/* Guest pages of scratch backing for the JIT tests. */
class JitBacking final : public ::uml::testing::ScratchBacking {
public:
	explicit JitBacking(std::size_t pages)
		: ScratchBacking{"uml-jit-test.XXXXXX",
			  pages * DARWIN_UML_GUEST_PAGE_SIZE}
	{
	}
};

class JitMapping final {
public:
	JitMapping(unsigned long generation, std::uint64_t base,
		   const ::uml::testing::ScratchBacking &backing,
		   std::size_t pages,
		   int protection = PROT_READ | PROT_WRITE | PROT_EXEC)
		: generation_{generation}
		, protection_{protection}
	{
		require(darwin_uml_static_vm_activate(generation) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(base), backing.descriptor(), 0,
			pages * DARWIN_UML_GUEST_PAGE_SIZE, protection, 1,
			(protection & PROT_WRITE) != 0) == 0);
	}

	/* Put the first page of another backing behind one mapped page, as a
	 * walker does after Linux replaces the page a PTE names. */
	void replace_page(std::uint64_t address,
		const ::uml::testing::ScratchBacking &backing)
	{
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(address), backing.descriptor(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE, protection_, 1,
			(protection_ & PROT_WRITE) != 0) == 0);
	}

	~JitMapping()
	{
		require(darwin_uml_static_vm_destroy(generation_) == 0);
	}

	JitMapping(const JitMapping &) = delete;
	JitMapping &operator=(const JitMapping &) = delete;

private:
	unsigned long generation_;
	int protection_;
};

inline int enter_jit(darwin_uml_guest_registers &registers,
		     darwin_uml_guest_fp_registers &fp,
		     unsigned long generation)
{
	unsigned long tls = 0;
	return darwin_uml_jit_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
}

} // namespace uml::emulator::tests
