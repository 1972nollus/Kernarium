// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_guest.h"
#include "uml_darwin_cpu.h"
#include "uml_darwin_static_mmu.h"
#include "runtime/execution_engine.hpp"
#include "scratch_directory.hpp"

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

class JitBacking final {
public:
	explicit JitBacking(std::size_t pages)
	{
		auto path = ::uml::testing::scratch_template(
			"uml-jit-test.XXXXXX");
		descriptor_ = mkstemp(path.data());
		require(descriptor_ >= 0);
		require(unlink(path.c_str()) == 0);
		require(ftruncate(descriptor_,
			static_cast<off_t>(
				pages * DARWIN_UML_GUEST_PAGE_SIZE)) == 0);
	}

	~JitBacking()
	{
		if (descriptor_ >= 0)
			(void)close(descriptor_);
	}

	JitBacking(const JitBacking &) = delete;
	JitBacking &operator=(const JitBacking &) = delete;

	void put32(std::size_t offset, std::uint32_t value)
	{
		require(pwrite(descriptor_, &value, sizeof(value), offset) ==
			static_cast<ssize_t>(sizeof(value)));
	}

	void put64(std::size_t offset, std::uint64_t value)
	{
		require(pwrite(descriptor_, &value, sizeof(value), offset) ==
			static_cast<ssize_t>(sizeof(value)));
	}

	void put(std::size_t offset, std::span<const std::byte> bytes)
	{
		require(pwrite(descriptor_, bytes.data(), bytes.size(), offset) ==
			static_cast<ssize_t>(bytes.size()));
	}

	[[nodiscard]] std::uint64_t get64(std::size_t offset) const
	{
		std::uint64_t value{};
		require(pread(descriptor_, &value, sizeof(value), offset) ==
			static_cast<ssize_t>(sizeof(value)));
		return value;
	}

	void get(std::size_t offset, std::span<std::byte> bytes) const
	{
		require(pread(descriptor_, bytes.data(), bytes.size(), offset) ==
			static_cast<ssize_t>(bytes.size()));
	}

	[[nodiscard]] int descriptor() const noexcept { return descriptor_; }

private:
	int descriptor_{-1};
};

class JitMapping final {
public:
	JitMapping(unsigned long generation, std::uint64_t base,
		   JitBacking &backing, std::size_t pages,
		   int protection = PROT_READ | PROT_WRITE | PROT_EXEC)
		: generation_{generation}
	{
		require(darwin_uml_static_vm_activate(generation) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(base), backing.descriptor(), 0,
			pages * DARWIN_UML_GUEST_PAGE_SIZE, protection, 1,
			(protection & PROT_WRITE) != 0) == 0);
	}

	~JitMapping()
	{
		require(darwin_uml_static_vm_destroy(generation_) == 0);
	}

	JitMapping(const JitMapping &) = delete;
	JitMapping &operator=(const JitMapping &) = delete;

private:
	unsigned long generation_;
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
