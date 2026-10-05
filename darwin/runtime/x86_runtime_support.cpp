// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/x86_runtime_support.hpp"

#if DARWIN_UML_X86_64_GUEST

#include "cpu/x86_64/decoder.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

namespace uml::runtime {

namespace x86_64 = ::uml::cpu::x86_64;
using ::uml::memory::StaticGuestMemory;

namespace {

class MmSharedLog final {
public:
	MmSharedLog()
	{
		const char *path = std::getenv("UML_X86_MM_SHARED_LOG");
		if (path != nullptr && path[0] != '\0')
			file_ = std::fopen(path, "ae");
	}

	~MmSharedLog()
	{
		if (file_ != nullptr)
			std::fclose(file_);
	}

	MmSharedLog(const MmSharedLog &) = delete;
	MmSharedLog &operator=(const MmSharedLog &) = delete;

	void note(unsigned long generation, std::uint64_t rip) noexcept
	{
		if (file_ == nullptr)
			return;
		const std::lock_guard lock{mutex_};
		try {
			if (!generations_.insert(generation).second)
				return;
		} catch (...) {
			return;
		}
		std::fprintf(file_, "uml-x86 mm-shared generation=%lu rip=%#llx\n",
			generation, static_cast<unsigned long long>(rip));
		std::fflush(file_);
	}

private:
	std::FILE *file_{};
	std::mutex mutex_;
	std::unordered_set<unsigned long> generations_;
};

} // namespace

void note_x86_mm_shared(unsigned long generation, std::uint64_t rip) noexcept
{
	static MmSharedLog log;
	log.note(generation, rip);
}

std::uint32_t x86_noncanonical_vector(StaticGuestMemory &memory,
	const x86_64::CpuState &cpu) noexcept
{
	const auto decoded = x86_64::decode(memory, cpu.rip, cpu.execution_mode);
	if (decoded.status != x86_64::DecodeStatus::decoded)
		return x86_general_protection;
	const auto &instruction = decoded.instruction;
	using x86_64::Operation;
	switch (instruction.operation) {
	case Operation::push:
	case Operation::pop:
	case Operation::push_all:
	case Operation::pop_all:
	case Operation::push_flags:
	case Operation::pop_flags:
	case Operation::enter:
	case Operation::call:
	case Operation::call_indirect:
	case Operation::far_call:
	case Operation::ret:
	case Operation::far_ret:
	case Operation::interrupt_ret:
	case Operation::segment_stack:
		if (!x86_canonical(cpu.gpr(x86_64::Register::rsp)))
			return x86_stack_segment_fault;
		break;
	case Operation::leave:
		if (!x86_canonical(cpu.gpr(x86_64::Register::rbp)))
			return x86_stack_segment_fault;
		break;
	default:
		break;
	}
	for (const auto *operand : {&instruction.destination,
		&instruction.source, &instruction.auxiliary}) {
		const auto &memory_operand = operand->memory;
		if (operand->kind == x86_64::OperandKind::memory &&
			!memory_operand.rip_relative &&
			memory_operand.segment == x86_64::Segment::none &&
			(memory_operand.base == 4 || memory_operand.base == 5))
			return x86_stack_segment_fault;
	}
	return x86_general_protection;
}

} // namespace uml::runtime

#endif
