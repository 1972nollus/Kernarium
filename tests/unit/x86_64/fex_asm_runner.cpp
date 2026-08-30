/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "memory/guest_address_space.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t page_size = 4096;
constexpr std::uint64_t code_address = 0x1'0000;
constexpr std::uint64_t scratch_address = 0xe000'0000;
constexpr std::uint64_t sib_address = 0xe800'0000;
constexpr std::size_t instruction_limit = 1'000'000;

struct Region final {
	std::uint64_t begin{};
	std::uint64_t end{};
};

class SparseMemory final : public uml::memory::GuestAddressSpace {
public:
	[[nodiscard]] bool map(std::uint64_t address, std::uint64_t size)
	{
		if (size == 0 || address >
			std::numeric_limits<std::uint64_t>::max() - size)
			return false;
		regions_.push_back({address, address + size});
		std::sort(regions_.begin(), regions_.end(),
			[](const Region &left, const Region &right) {
				return left.begin < right.begin;
			});
		std::vector<Region> merged;
		for (const auto &region : regions_) {
			if (merged.empty() || region.begin > merged.back().end)
				merged.push_back(region);
			else
				merged.back().end = std::max(merged.back().end, region.end);
		}
		regions_ = std::move(merged);
		return true;
	}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		return transfer_read(address, destination,
			uml::memory::MemoryAccess::read, true);
	}

	[[nodiscard]] bool fetch(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		return transfer_read(address, destination,
			uml::memory::MemoryAccess::execute, true);
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) override
	{
		if (!mapped(address, source.size())) {
			set_fault(address, uml::memory::MemoryAccess::write);
			return false;
		}
		std::size_t offset{};
		while (offset < source.size()) {
			const auto current = address + offset;
			const auto page_number = current / page_size;
			const auto page_offset = static_cast<std::size_t>(
				current % page_size);
			const auto count = std::min(source.size() - offset,
				static_cast<std::size_t>(page_size) - page_offset);
			auto [iterator, inserted] = pages_.try_emplace(page_number);
			if (inserted)
				iterator->second.fill(std::byte{});
			std::memcpy(iterator->second.data() + page_offset,
				source.data() + offset, count);
			offset += count;
		}
		return true;
	}

	[[nodiscard]] uml::memory::MemoryFault last_fault() const noexcept override
	{
		return fault_;
	}

	[[nodiscard]] bool has_fault() const noexcept override { return faulted_; }

	[[nodiscard]] std::vector<std::byte> snapshot(
		std::uint64_t address, std::size_t size) const
	{
		std::vector<std::byte> bytes;
		bytes.reserve(size);
		for (std::size_t offset = 0; offset < size; ++offset) {
			if (!mapped(address + offset, 1))
				break;
			const auto current = address + offset;
			const auto iterator = pages_.find(current / page_size);
			bytes.push_back(iterator == pages_.end() ? std::byte{} :
				iterator->second[static_cast<std::size_t>(current % page_size)]);
		}
		return bytes;
	}

private:
	using Page = std::array<std::byte, page_size>;

	[[nodiscard]] bool mapped(std::uint64_t address,
		std::size_t size) const noexcept
	{
		if (size == 0)
			return true;
		if (address > std::numeric_limits<std::uint64_t>::max() - size)
			return false;
		const auto end = address + size;
		return std::any_of(regions_.begin(), regions_.end(),
			[address, end](const Region &region) {
				return address >= region.begin && end <= region.end;
			});
	}

	[[nodiscard]] bool transfer_read(std::uint64_t address,
		std::span<std::byte> destination, uml::memory::MemoryAccess access,
		bool record_fault)
	{
		if (!mapped(address, destination.size())) {
			if (record_fault)
				set_fault(address, access);
			return false;
		}
		std::size_t offset{};
		while (offset < destination.size()) {
			const auto current = address + offset;
			const auto page_number = current / page_size;
			const auto page_offset = static_cast<std::size_t>(
				current % page_size);
			const auto count = std::min(destination.size() - offset,
				static_cast<std::size_t>(page_size) - page_offset);
			const auto iterator = pages_.find(page_number);
			if (iterator == pages_.end())
				std::fill_n(destination.data() + offset, count, std::byte{});
			else
				std::memcpy(destination.data() + offset,
					iterator->second.data() + page_offset, count);
			offset += count;
		}
		return true;
	}

	void set_fault(std::uint64_t address,
		uml::memory::MemoryAccess access) noexcept
	{
		fault_ = {address, access};
		faulted_ = true;
	}

	std::vector<Region> regions_;
	std::map<std::uint64_t, Page> pages_;
	uml::memory::MemoryFault fault_{};
	bool faulted_{};
};

struct RegisterExpectation final {
	std::uint64_t key{};
	std::vector<std::uint64_t> values;
};

struct MemoryData final {
	std::uint64_t address{};
	std::vector<std::byte> bytes;
};

struct Configuration final {
	std::vector<Region> memory_regions;
	std::vector<RegisterExpectation> registers;
	std::vector<MemoryData> memory_data;
};

[[nodiscard]] bool range_valid(std::span<const std::byte> bytes,
	std::size_t offset, std::size_t size) noexcept
{
	return offset <= bytes.size() && size <= bytes.size() - offset;
}

[[nodiscard]] bool read_u32(std::span<const std::byte> bytes,
	std::size_t offset, std::uint32_t &value) noexcept
{
	if (!range_valid(bytes, offset, sizeof(value)))
		return false;
	value = 0;
	for (unsigned index = 0; index < 4; ++index)
		value |= static_cast<std::uint32_t>(
			std::to_integer<std::uint8_t>(bytes[offset + index])) <<
			(index * 8U);
	return true;
}

[[nodiscard]] bool read_u64(std::span<const std::byte> bytes,
	std::size_t offset, std::uint64_t &value) noexcept
{
	if (!range_valid(bytes, offset, sizeof(value)))
		return false;
	value = 0;
	for (unsigned index = 0; index < 8; ++index)
		value |= static_cast<std::uint64_t>(
			std::to_integer<std::uint8_t>(bytes[offset + index])) <<
			(index * 8U);
	return true;
}

[[nodiscard]] bool parse_configuration(std::span<const std::byte> bytes,
	Configuration &configuration, std::string &error)
{
	constexpr std::size_t header_size = 76;
	if (bytes.size() < header_size) {
		error = "configuration header is truncated";
		return false;
	}
	std::uint32_t mode{};
	std::uint32_t region_offset{};
	std::uint32_t region_count{};
	std::uint32_t register_offset{};
	std::uint32_t register_count{};
	std::uint32_t memory_offset{};
	std::uint32_t memory_count{};
	if (!read_u32(bytes, 36, mode) || !read_u32(bytes, 44, region_offset) ||
		!read_u32(bytes, 48, region_count) ||
		!read_u32(bytes, 52, register_offset) ||
		!read_u32(bytes, 56, register_count) ||
		!read_u32(bytes, 60, memory_offset) ||
		!read_u32(bytes, 64, memory_count)) {
		error = "configuration header fields are truncated";
		return false;
	}
	if (mode != 1) {
		error = "configuration is not a 64-bit FEX test";
		return false;
	}

	auto cursor = static_cast<std::size_t>(region_offset);
	for (std::uint32_t index = 0; index < region_count; ++index) {
		Region region{};
		std::uint64_t size{};
		if (!read_u64(bytes, cursor, region.begin) ||
			!read_u64(bytes, cursor + 8, size) || size == 0 ||
			region.begin > std::numeric_limits<std::uint64_t>::max() - size) {
			error = "invalid memory-region record";
			return false;
		}
		region.end = region.begin + size;
		configuration.memory_regions.push_back(region);
		cursor += 16;
	}

	cursor = static_cast<std::size_t>(register_offset);
	for (std::uint32_t index = 0; index < register_count; ++index) {
		std::uint32_t value_count{};
		RegisterExpectation expectation{};
		if (!read_u32(bytes, cursor, value_count) ||
			!read_u64(bytes, cursor + 4, expectation.key) ||
			expectation.key == 0 ||
			(expectation.key & (expectation.key - 1U)) != 0 ||
			value_count == 0 || value_count > 4) {
			error = "invalid register-data record";
			return false;
		}
		cursor += 12;
		for (std::uint32_t value_index = 0;
			value_index < value_count; ++value_index) {
			std::uint64_t value{};
			if (!read_u64(bytes, cursor, value)) {
				error = "truncated register-data value";
				return false;
			}
			expectation.values.push_back(value);
			cursor += 8;
		}
		configuration.registers.push_back(std::move(expectation));
	}

	cursor = static_cast<std::size_t>(memory_offset);
	for (std::uint32_t index = 0; index < memory_count; ++index) {
		MemoryData data{};
		std::uint32_t size{};
		if (!read_u64(bytes, cursor, data.address) ||
			!read_u32(bytes, cursor + 8, size) ||
			!range_valid(bytes, cursor + 12, size)) {
			error = "invalid memory-data record";
			return false;
		}
		data.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor + 12),
			bytes.begin() + static_cast<std::ptrdiff_t>(cursor + 12 + size));
		configuration.memory_data.push_back(std::move(data));
		cursor += 12 + size;
	}
	return true;
}

[[nodiscard]] bool load_file(const char *path, std::vector<std::byte> &bytes,
	std::string &error)
{
	std::ifstream stream{path, std::ios::binary | std::ios::ate};
	if (!stream) {
		error = "cannot open input file";
		return false;
	}
	const auto end = stream.tellg();
	if (end < 0) {
		error = "cannot determine input size";
		return false;
	}
	const auto size = static_cast<std::size_t>(end);
	bytes.resize(size);
	stream.seekg(0);
	if (size != 0 && !stream.read(reinterpret_cast<char *>(bytes.data()),
		static_cast<std::streamsize>(size))) {
		error = "cannot read input file";
		return false;
	}
	return true;
}

[[nodiscard]] std::string hex_bytes(std::span<const std::byte> bytes)
{
	std::ostringstream output;
	output << std::hex << std::setfill('0');
	for (const auto byte : bytes)
		output << std::setw(2)
			<< static_cast<unsigned>(std::to_integer<std::uint8_t>(byte));
	return output.str();
}

void print_result(std::string_view status, std::size_t retired,
	std::uint64_t address, std::span<const std::byte> opcode,
	std::string_view detail)
{
	std::cout << "status=" << status << '\t'
		<< "retired=" << retired << '\t'
		<< "address=0x" << std::hex << address << std::dec << '\t'
		<< "opcode=" << hex_bytes(opcode) << '\t'
		<< "detail=" << detail << '\n';
}

[[nodiscard]] const char *register_name(unsigned index) noexcept
{
	constexpr std::array<const char *, 16> names{
		"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP",
		"R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
	return index < names.size() ? names[index] : "unknown";
}

[[nodiscard]] bool compare_registers(const Configuration &configuration,
	const uml::cpu::x86_64::CpuState &cpu, std::string &status,
	std::string &detail)
{
	using uml::cpu::x86_64::Register;
	constexpr std::array<Register, 16> fex_registers{
		Register::rax, Register::rbx, Register::rcx, Register::rdx,
		Register::rsi, Register::rdi, Register::rbp, Register::rsp,
		Register::r8, Register::r9, Register::r10, Register::r11,
		Register::r12, Register::r13, Register::r14, Register::r15};
	for (const auto &expectation : configuration.registers) {
		const auto index = std::countr_zero(expectation.key);
		if (index == 0) {
			if (expectation.values.size() != 1 ||
				cpu.rip != expectation.values.front()) {
				status = "result_mismatch";
				detail = "RIP";
				return false;
			}
		} else if (index >= 1 && index <= 16) {
			const auto register_index = index - 1;
			if (expectation.values.size() != 1 ||
				cpu.gpr(fex_registers[register_index]) !=
					expectation.values.front()) {
				status = "result_mismatch";
				detail = register_name(register_index);
				return false;
			}
		} else if (index >= 17 && index <= 32) {
			const auto vector_index = index - 17;
			for (std::size_t lane = 0;
				lane < expectation.values.size(); ++lane) {
				std::uint64_t actual{};
				std::memcpy(&actual,
					cpu.vectors[vector_index].data() + lane * sizeof(actual),
					sizeof(actual));
				if (actual != expectation.values[lane]) {
					status = "result_mismatch";
					detail = "XMM" + std::to_string(vector_index);
					return false;
				}
			}
		} else if (index == 33 || index == 34) {
			const auto actual = index == 33 ? cpu.gs_base : cpu.fs_base;
			if (expectation.values.size() != 1 ||
				actual != expectation.values.front()) {
				status = "result_mismatch";
				detail = index == 33 ? "GS" : "FS";
				return false;
			}
		} else if (index >= 35 && index <= 42) {
			const auto mmx_index = index - 35;
			if (expectation.values.size() != 1 ||
				cpu.mmx[mmx_index] != expectation.values.front()) {
				status = "result_mismatch";
				detail = "MM" + std::to_string(mmx_index);
				return false;
			}
		} else {
			status = "malformed_input";
			detail = "unknown register key";
			return false;
		}
	}
	status = "passed";
	detail = "reached HLT with expected state";
	return true;
}

[[nodiscard]] const char *event_name(uml::cpu::EngineEvent event) noexcept
{
	switch (event) {
	case uml::cpu::EngineEvent::fatal: return "fatal";
	case uml::cpu::EngineEvent::continued: return "continued";
	case uml::cpu::EngineEvent::system_call: return "system_call";
	case uml::cpu::EngineEvent::memory_fault: return "memory_fault";
	case uml::cpu::EngineEvent::interrupt: return "interrupt";
	case uml::cpu::EngineEvent::illegal_instruction: return "illegal_instruction";
	case uml::cpu::EngineEvent::breakpoint: return "breakpoint";
	case uml::cpu::EngineEvent::gcs_fault: return "gcs_fault";
	case uml::cpu::EngineEvent::floating_point_trap:
		return "floating_point_trap";
	case uml::cpu::EngineEvent::pc_alignment: return "pc_alignment";
	}
	return "unknown";
}

} // namespace

int main(int argc, char **argv)
{
	if (argc != 3) {
		print_result("malformed_input", 0, 0, {},
			"usage: runner BINARY CONFIG");
		return 2;
	}
	std::vector<std::byte> binary;
	std::vector<std::byte> config_bytes;
	std::string error;
	if (!load_file(argv[1], binary, error) || binary.empty() ||
		!load_file(argv[2], config_bytes, error)) {
		print_result("malformed_input", 0, 0, {}, error);
		return 2;
	}
	Configuration configuration;
	if (!parse_configuration(config_bytes, configuration, error)) {
		print_result("malformed_input", 0, 0, {}, error);
		return 2;
	}

	SparseMemory memory;
	const auto code_size = (binary.size() + page_size - 1U) &
		~(page_size - 1U);
	if (!memory.map(scratch_address, page_size * 10U) ||
		!memory.map(sib_address - page_size, page_size * 2U) ||
		!memory.map(code_address, code_size)) {
		print_result("malformed_input", 0, 0, {}, "invalid base mapping");
		return 2;
	}
	for (const auto &region : configuration.memory_regions) {
		if (!memory.map(region.begin, region.end - region.begin)) {
			print_result("malformed_input", 0, region.begin, {},
				"invalid configured mapping");
			return 2;
		}
	}
	if (!memory.write(code_address, binary)) {
		print_result("malformed_input", 0, code_address, {},
			"cannot load test code");
		return 2;
	}
	for (const auto &data : configuration.memory_data) {
		if (!memory.write(data.address, data.bytes)) {
			print_result("malformed_input", 0, data.address, {},
				"memory data targets an unmapped range");
			return 2;
		}
	}

	uml::cpu::x86_64::CpuState cpu{};
	cpu.rip = code_address;
	cpu.rflags = 0x202;
	std::size_t retired{};
	while (retired < instruction_limit) {
		const auto opcode = memory.snapshot(cpu.rip, 15);
		if (!opcode.empty() && opcode.front() == std::byte{0xf4}) {
			std::string status;
			std::string detail;
			const auto passed = compare_registers(
				configuration, cpu, status, detail);
			print_result(status, retired, cpu.rip, opcode, detail);
			return passed ? 0 : 1;
		}

		const auto built = uml::cpu::x86_64::gadgets::build(memory, cpu.rip);
		if (built.status != uml::cpu::x86_64::gadgets::BuildStatus::built) {
			std::string_view status = "illegal_instruction";
			std::string_view detail = "decoder rejected opcode";
			if (built.status ==
				uml::cpu::x86_64::gadgets::BuildStatus::fetch_fault) {
				status = "memory_fault";
				detail = "instruction fetch fault";
			} else if (built.status ==
				uml::cpu::x86_64::gadgets::BuildStatus::allocation_failure) {
				status = "allocation_failure";
				detail = "gadget allocation failed";
			}
			print_result(status, retired, cpu.rip, opcode, detail);
			return 1;
		}
		const auto remaining = instruction_limit - retired;
		const auto result = uml::cpu::x86_64::gadgets::execute(
			built.program, cpu, memory, remaining);
		retired += result.retired_instructions;
		/* One upstream decoder test uses Linux time(2) only to prove that
		 * SYSCALL reached a live boundary. Keep that test-local service in
		 * the adapter instead of teaching the CPU engine about Linux ABIs.
		 */
		if (result.event == uml::cpu::EngineEvent::system_call &&
			cpu.gpr(uml::cpu::x86_64::Register::rax) == 201 &&
			cpu.gpr(uml::cpu::x86_64::Register::rdi) == 0) {
			cpu.gpr(uml::cpu::x86_64::Register::rax) = 1;
			continue;
		}
		if (result.event != uml::cpu::EngineEvent::continued) {
			const auto status = result.event ==
				uml::cpu::EngineEvent::memory_fault ?
				"memory_fault" : "unexpected_event";
			print_result(status, retired, cpu.rip,
				memory.snapshot(cpu.rip, 15), event_name(result.event));
			return 1;
		}
		if (result.instruction_budget_exhausted)
			break;
	}
	print_result("instruction_limit", retired, cpu.rip,
		memory.snapshot(cpu.rip, 15), "instruction retirement limit reached");
	return 1;
}
