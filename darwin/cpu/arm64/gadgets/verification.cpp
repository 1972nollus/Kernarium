// SPDX-License-Identifier: GPL-2.0-or-later
#include "verification.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace uml::cpu::arm64::gadgets {

bool verification_enabled() noexcept
{
	static const bool enabled = [] {
		const auto *value = std::getenv("UML_STATIC_VERIFY_GADGETS");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}

VerificationMemory::VerificationMemory(
	::uml::memory::StaticGuestMemory &memory) noexcept
	: memory_{memory}
{
}

bool VerificationMemory::read(std::uint64_t address,
	std::span<std::byte> destination)
{
	if (!memory_.read(address, destination))
		return false;
	apply(address, destination);
	return true;
}

bool VerificationMemory::fetch(std::uint64_t address,
	std::span<std::byte> destination)
{
	if (!memory_.fetch(address, destination))
		return false;
	apply(address, destination);
	return true;
}

bool VerificationMemory::write(std::uint64_t address,
	std::span<const std::byte> source)
{
	if (!memory_.validate_write(address, source.size()))
		return false;
	for (std::size_t index = 0; index < source.size(); ++index)
		writes_.insert_or_assign(address + index, source[index]);
	return true;
}

bool VerificationMemory::validate_access(std::uint64_t address,
	std::size_t size, arm64::MemoryAccess access)
{
	return memory_.validate_access(address, size, access);
}

arm64::MemoryFault VerificationMemory::last_fault() const noexcept
{
	return memory_.last_fault();
}

bool VerificationMemory::matches_guest() const
{
	for (const auto &[address, expected] : writes_) {
		std::byte actual{};
		if (!memory_.read(address, std::span{&actual, 1}) ||
		    actual != expected) {
			std::fprintf(stderr,
				"Darwin static verify memory mismatch address=%#llx expected=%#x actual=%#x\n",
				static_cast<unsigned long long>(address),
				std::to_integer<unsigned>(expected),
				std::to_integer<unsigned>(actual));
			return false;
		}
	}
	return true;
}

void VerificationMemory::apply(std::uint64_t address,
	std::span<std::byte> destination) const
{
	for (std::size_t index = 0; index < destination.size(); ++index)
		if (const auto found = writes_.find(address + index);
		    found != writes_.end())
			destination[index] = found->second;
}

bool same_cpu_state(const arm64::CpuState &expected,
	const arm64::CpuState &actual, std::uint64_t pc)
{
	bool same = true;
	for (std::size_t index = 0; index < expected.registers.size(); ++index) {
		if (expected.registers[index] == actual.registers[index])
			continue;
		std::fprintf(stderr,
			"Darwin static verify register mismatch block=%#llx x%zu=%#llx/%#llx\n",
			static_cast<unsigned long long>(pc), index,
			static_cast<unsigned long long>(expected.registers[index]),
			static_cast<unsigned long long>(actual.registers[index]));
		same = false;
	}
	if (expected.sp != actual.sp || expected.pc != actual.pc ||
	    expected.nzcv != actual.nzcv || expected.tls != actual.tls ||
	    std::memcmp(expected.vectors.data(), actual.vectors.data(),
		    sizeof(expected.vectors)) != 0 ||
	    expected.fpcr != actual.fpcr || expected.fpsr != actual.fpsr) {
		std::fprintf(stderr,
			"Darwin static verify state mismatch block=%#llx sp=%#llx/%#llx pc=%#llx/%#llx nzcv=%#x/%#x tls=%#llx/%#llx\n",
			static_cast<unsigned long long>(pc),
			static_cast<unsigned long long>(expected.sp),
			static_cast<unsigned long long>(actual.sp),
			static_cast<unsigned long long>(expected.pc),
			static_cast<unsigned long long>(actual.pc), expected.nzcv,
			actual.nzcv, static_cast<unsigned long long>(expected.tls),
			static_cast<unsigned long long>(actual.tls));
		same = false;
	}
	return same;
}

} // namespace uml::cpu::arm64::gadgets
