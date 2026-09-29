/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/execution_types.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "memory/guest_address_space.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
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
/* The address-space generation cached runs share their code-page registry
 * under; each case's executor releases it before the next case starts. */
constexpr std::uint64_t cached_generation = 1;

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

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, uml::memory::MemoryAccess access) override
	{
		if (size == 0 || size > page_size - (address % page_size) ||
			!mapped(address, size)) {
			set_fault(address, access);
			return nullptr;
		}
		const auto page_number = address / page_size;
		auto [iterator, inserted] = pages_.try_emplace(page_number);
		if (inserted)
			iterator->second.fill(std::byte{});
		return iterator->second.data() +
			static_cast<std::size_t>(address % page_size);
	}

	/* Repository cases run on the product's code-page model: every mapped
	 * page may hold code, as under READ_IMPLIES_EXEC, and only a code
	 * publication moves a page's stamp and the address space's code-change
	 * sequence, so a cached translation survives exactly the stores nobody
	 * published. */
	void model_code_pages(std::uint64_t *code_changes) noexcept
	{
		code_changes_ = code_changes;
	}

	[[nodiscard]] bool is_executable(std::uint64_t address) noexcept override
	{
		return code_changes_ != nullptr && mapped(address, 1);
	}

	[[nodiscard]] bool code_page_stamp(std::uint64_t address, void *,
		uml::memory::CodePageStamp &stamp) noexcept override
	{
		if (code_changes_ == nullptr || !mapped(address, 1))
			return false;
		const auto page_number = address / page_size;
		const auto version = code_versions_.find(page_number);
		stamp = {page_number + 1U,
			version == code_versions_.end() ? 0 : version->second};
		return true;
	}

	void invalidate_code(std::uint64_t address) override
	{
		if (code_changes_ == nullptr)
			return;
		++code_versions_[address / page_size];
		++*code_changes_;
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
	struct alignas(page_size) Page final {
		[[nodiscard]] std::byte *data() noexcept { return bytes.data(); }
		[[nodiscard]] const std::byte *data() const noexcept
		{
			return bytes.data();
		}
		[[nodiscard]] std::byte &operator[](std::size_t index) noexcept
		{
			return bytes[index];
		}
		[[nodiscard]] const std::byte &operator[](
			std::size_t index) const noexcept
		{
			return bytes[index];
		}
		void fill(std::byte value) noexcept { bytes.fill(value); }

		std::array<std::byte, page_size> bytes{};
	};

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
	std::map<std::uint64_t, std::uint64_t> code_versions_;
	std::uint64_t *code_changes_{};
	uml::memory::MemoryFault fault_{};
	bool faulted_{};
};

/* How a case reaches its blocks. Upstream FEX programs build each block
 * afresh; repository cases dispatch through the executor's program cache on
 * the product's code-page model, so they can also hold the engine to its
 * code-publication contract. */
enum class Dispatch {
	fresh,
	cached,
};

struct CodeSequences final {
	std::uint64_t mapping_changes{1};
	std::uint64_t code_changes{1};
};

[[nodiscard]] uml::cpu::x86_64::gadgets::ProgramCacheSequences
read_code_sequences(void *opaque) noexcept
{
	const auto &sequences = *static_cast<const CodeSequences *>(opaque);
	return {
		.mapping_changes = sequences.mapping_changes,
		.code_changes = sequences.code_changes,
		.interrupted = false,
	};
}

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
	uml::cpu::x86_64::ExecutionMode mode{
		uml::cpu::x86_64::ExecutionMode::long64};
};

[[nodiscard]] bool range_valid(std::span<const std::byte> bytes,
	std::size_t offset, std::size_t size) noexcept
{
	return offset <= bytes.size() && size <= bytes.size() - offset;
}

/* Reads a little-endian `Value` at `offset`, or fails when it would extend
 * past the end of `bytes`. */
template<std::unsigned_integral Value>
[[nodiscard]] bool read_le(std::span<const std::byte> bytes,
	std::size_t offset, Value &value) noexcept
{
	if (!range_valid(bytes, offset, sizeof(value)))
		return false;
	value = 0;
	for (std::size_t index = 0; index < sizeof(value); ++index)
		value |= static_cast<Value>(static_cast<Value>(
			std::to_integer<std::uint8_t>(bytes[offset + index]))
			<< (index * 8U));
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
	std::uint32_t environment_offset{};
	std::uint32_t environment_count{};
	if (!read_le(bytes, 36, mode) || !read_le(bytes, 44, region_offset) ||
		!read_le(bytes, 48, region_count) ||
		!read_le(bytes, 52, register_offset) ||
		!read_le(bytes, 56, register_count) ||
		!read_le(bytes, 60, memory_offset) ||
		!read_le(bytes, 64, memory_count) ||
		!read_le(bytes, 68, environment_offset) ||
		!read_le(bytes, 72, environment_count)) {
		error = "configuration header fields are truncated";
		return false;
	}
	if (mode > 1) {
		error = "configuration has an unsupported FEX execution mode";
		return false;
	}
	configuration.mode = mode == 0 ?
		uml::cpu::x86_64::ExecutionMode::compat32 :
		uml::cpu::x86_64::ExecutionMode::long64;

	auto cursor = static_cast<std::size_t>(region_offset);
	for (std::uint32_t index = 0; index < region_count; ++index) {
		Region region{};
		std::uint64_t size{};
		if (!read_le(bytes, cursor, region.begin) ||
			!read_le(bytes, cursor + 8, size) || size == 0 ||
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
		if (!read_le(bytes, cursor, value_count) ||
			!read_le(bytes, cursor + 4, expectation.key) ||
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
			if (!read_le(bytes, cursor, value)) {
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
		if (!read_le(bytes, cursor, data.address) ||
			!read_le(bytes, cursor + 8, size) ||
			!range_valid(bytes, cursor + 12, size)) {
			error = "invalid memory-data record";
			return false;
		}
		data.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor + 12),
			bytes.begin() + static_cast<std::ptrdiff_t>(cursor + 12 + size));
		configuration.memory_data.push_back(std::move(data));
		cursor += 12 + size;
	}

	cursor = static_cast<std::size_t>(environment_offset);
	for (std::uint32_t index = 0; index < environment_count; ++index) {
		if (cursor >= bytes.size()) {
			error = "environment data is truncated";
			return false;
		}
		const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(cursor);
		const auto key_end = std::find(begin, bytes.end(), std::byte{});
		if (key_end == bytes.end()) {
			error = "truncated environment key";
			return false;
		}
		const auto value_begin = key_end + 1;
		const auto value_end = std::find(value_begin, bytes.end(), std::byte{});
		if (value_end == bytes.end()) {
			error = "truncated environment value";
			return false;
		}
		const std::string key(reinterpret_cast<const char *>(
			bytes.data() + cursor),
			static_cast<std::size_t>(key_end - begin));
		const std::string value(reinterpret_cast<const char *>(&*value_begin),
			static_cast<std::size_t>(value_end - value_begin));
		/* FEX's reduced-precision x87 mode computes in doubles; the
		 * engine has only full 80-bit x87, so a case asking for it
		 * could not mean what it says. */
		if (key == "FEX_X87REDUCEDPRECISION" && value != "0") {
			error = "FEX_X87REDUCEDPRECISION selects a reduced-precision "
				"x87 mode the engine does not implement";
			return false;
		}
		cursor = static_cast<std::size_t>(value_end - bytes.begin()) + 1U;
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
	std::string_view detail, std::string_view test = {})
{
	std::cout << "status=" << status << '\t';
	if (!test.empty())
		std::cout << "test=" << test << '\t';
	std::cout
		<< "retired=" << retired << '\t'
		<< "address=0x" << std::hex << address << std::dec << '\t'
		<< "opcode=" << hex_bytes(opcode) << '\t'
		<< "detail=" << detail << '\n';
}

/* FEX expectation keys, K-trace register masks and register_name() all index
 * the general registers in this order, and the K-trace and compact RFLAGS
 * encodings index the flags in this one. */
constexpr std::array<uml::cpu::x86_64::Register, 16> pack_registers{
	uml::cpu::x86_64::Register::rax, uml::cpu::x86_64::Register::rbx,
	uml::cpu::x86_64::Register::rcx, uml::cpu::x86_64::Register::rdx,
	uml::cpu::x86_64::Register::rsi, uml::cpu::x86_64::Register::rdi,
	uml::cpu::x86_64::Register::rbp, uml::cpu::x86_64::Register::rsp,
	uml::cpu::x86_64::Register::r8, uml::cpu::x86_64::Register::r9,
	uml::cpu::x86_64::Register::r10, uml::cpu::x86_64::Register::r11,
	uml::cpu::x86_64::Register::r12, uml::cpu::x86_64::Register::r13,
	uml::cpu::x86_64::Register::r14, uml::cpu::x86_64::Register::r15};
constexpr std::array<std::uint64_t, 7> pack_flags{
	uml::cpu::x86_64::rflags_carry, uml::cpu::x86_64::rflags_parity,
	uml::cpu::x86_64::rflags_auxiliary, uml::cpu::x86_64::rflags_zero,
	uml::cpu::x86_64::rflags_sign, uml::cpu::x86_64::rflags_direction,
	uml::cpu::x86_64::rflags_overflow};

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
				cpu.gpr(pack_registers[register_index]) !=
					expectation.values.front()) {
				status = "result_mismatch";
				std::ostringstream mismatch;
				mismatch << register_name(register_index) <<
					" expected=0x" << std::hex <<
					(expectation.values.empty() ? 0 :
					 expectation.values.front()) << " actual=0x" <<
					cpu.gpr(pack_registers[register_index]);
				detail = mismatch.str();
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
			bool matched = false;
			if (expectation.values.size() == 1) {
				matched = cpu.mmx[mmx_index] == expectation.values.front();
			} else if (expectation.values.size() == 2) {
				matched = cpu.x87[mmx_index].significand ==
					expectation.values[0] &&
					cpu.x87[mmx_index].sign_exponent ==
					static_cast<std::uint16_t>(expectation.values[1]);
			}
			if (!matched) {
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
	case uml::cpu::EngineEvent::floating_point_trap:
		return "floating_point_trap";
	case uml::cpu::EngineEvent::pc_alignment: return "pc_alignment";
	case uml::cpu::EngineEvent::overflow: return "overflow";
	case uml::cpu::EngineEvent::general_protection: return "general_protection";
	case uml::cpu::EngineEvent::trap_flag: return "trap_flag";
	}
	return "unknown";
}

} // namespace

[[nodiscard]] int run_fex_case(std::span<const std::byte> binary,
	std::span<const std::byte> config_bytes, std::string_view test,
	bool quiet_success = false, Dispatch dispatch = Dispatch::fresh)
{
	std::string error;
	if (binary.empty()) {
		print_result("malformed_input", 0, 0, {},
			"test binary is empty", test);
		return 2;
	}
	Configuration configuration;
	if (!parse_configuration(config_bytes, configuration, error)) {
		print_result("malformed_input", 0, 0, {}, error, test);
		return 2;
	}

	SparseMemory memory;
	CodeSequences sequences;
	if (dispatch == Dispatch::cached)
		memory.model_code_pages(&sequences.code_changes);
	const auto code_size = (binary.size() + page_size - 1U) &
		~(page_size - 1U);
	if (!memory.map(scratch_address, page_size * 10U) ||
		!memory.map(sib_address - page_size, page_size * 2U) ||
		!memory.map(code_address, code_size)) {
		print_result("malformed_input", 0, 0, {}, "invalid base mapping", test);
		return 2;
	}
	for (const auto &region : configuration.memory_regions) {
		if (!memory.map(region.begin, region.end - region.begin)) {
			print_result("malformed_input", 0, region.begin, {},
				"invalid configured mapping", test);
			return 2;
		}
	}
	if (!memory.write(code_address, binary)) {
		print_result("malformed_input", 0, code_address, {},
			"cannot load test code", test);
		return 2;
	}
	for (const auto &data : configuration.memory_data) {
		if (!memory.write(data.address, data.bytes)) {
			print_result("malformed_input", 0, data.address, {},
				"memory data targets an unmapped range", test);
			return 2;
		}
	}

	uml::cpu::x86_64::CpuState cpu{};
	uml::cpu::x86_64::gadgets::Executor executor;
	/* A cached run prepares the executor as the product's dispatch loop
	 * does: again whenever the code-change sequence has moved past the one
	 * it was prepared with. */
	std::uint64_t prepared_code_changes{};
	const auto prepare = [&]() {
		if (dispatch == Dispatch::fresh) {
			executor.prepare(memory);
			return;
		}
		executor.prepare(memory, cached_generation,
			sequences.mapping_changes, sequences.code_changes, &sequences,
			read_code_sequences);
		prepared_code_changes = sequences.code_changes;
	};
	prepare();
	cpu.rip = code_address;
	cpu.rflags = 0x202;
	cpu.execution_mode = configuration.mode;
	if (configuration.mode ==
		uml::cpu::x86_64::ExecutionMode::compat32) {
		cpu.segment_selectors[1] = 0x23;
		cpu.segment_selectors[2] = 0x2b;
	}
	std::size_t retired{};
	while (retired < instruction_limit) {
		const auto opcode = memory.snapshot(cpu.rip, 15);
		if (!opcode.empty() && opcode.front() == std::byte{0xf4}) {
			std::string status;
			std::string detail;
			const auto passed = compare_registers(
				configuration, cpu, status, detail);
			if (!passed || !quiet_success)
				print_result(status, retired, cpu.rip, opcode, detail, test);
			return passed ? 0 : 1;
		}

		uml::cpu::x86_64::gadgets::BuildResult built{};
		const uml::cpu::x86_64::gadgets::Program *program{};
		if (dispatch == Dispatch::cached) {
			if (prepared_code_changes != sequences.code_changes)
				prepare();
			const auto selected = executor.select_program(
				memory, cpu.rip, cpu.execution_mode);
			built.status = selected.status;
			program = selected.program;
		} else {
			built = uml::cpu::x86_64::gadgets::build(
				memory, cpu.rip, cpu.execution_mode);
			program = &built.program;
		}
		if (built.status != uml::cpu::x86_64::gadgets::BuildStatus::built ||
			program == nullptr) {
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
			print_result(status, retired, cpu.rip, opcode, detail, test);
			return 1;
		}
		const auto remaining = instruction_limit - retired;
		const auto result = uml::cpu::x86_64::gadgets::execute(
			*program, cpu, memory, executor, remaining);
		retired += result.retired_instructions;
		if (dispatch == Dispatch::cached)
			prepared_code_changes = executor.prepared_code_changes();
		/* One upstream decoder test uses Linux time(2) only to prove that
		 * SYSCALL reached a live boundary, and a repository case to read the
		 * flags SYSCALL saves in R11. Keep that test-local service in the
		 * adapter instead of teaching the CPU engine about Linux ABIs.
		 */
		if (result.event == uml::cpu::EngineEvent::system_call &&
			cpu.gpr(uml::cpu::x86_64::Register::rax) == 201 &&
			cpu.gpr(uml::cpu::x86_64::Register::rdi) == 0) {
			cpu.gpr(uml::cpu::x86_64::Register::rax) = 1;
			continue;
		}
		/* FEX's compatibility-ABI regression invokes 32-bit
		 * clock_gettime(2) through INT 80h. The test observes only the
		 * successful return; Linux ABI emulation remains outside the CPU.
		 */
		if (result.event == uml::cpu::EngineEvent::system_call &&
			cpu.system_call_32bit &&
			static_cast<std::uint32_t>(
				cpu.gpr(uml::cpu::x86_64::Register::rax)) == 265U &&
			static_cast<std::uint32_t>(
				cpu.gpr(uml::cpu::x86_64::Register::rbx)) == 0U) {
			cpu.gpr(uml::cpu::x86_64::Register::rax) = 0;
			continue;
		}
		/* FEX's inline-syscall decoder regression issues getrandom(2) with a
		 * zero-length buffer. It needs only the architecturally successful
		 * boundary; Linux syscall emulation remains outside this adapter.
		 */
		if (result.event == uml::cpu::EngineEvent::system_call &&
			cpu.system_call_32bit &&
			static_cast<std::uint32_t>(
				cpu.gpr(uml::cpu::x86_64::Register::rax)) == 355U &&
			static_cast<std::uint32_t>(
				cpu.gpr(uml::cpu::x86_64::Register::rbx)) == 0U &&
			static_cast<std::uint32_t>(
				cpu.gpr(uml::cpu::x86_64::Register::rcx)) == 0U &&
			static_cast<std::uint32_t>(
				cpu.gpr(uml::cpu::x86_64::Register::rdx)) == 0U) {
			cpu.gpr(uml::cpu::x86_64::Register::rax) = 0;
			continue;
		}
		/* HLT, the test's end marker, raises #GP at user privilege without
		 * retiring. The loop head recognizes it and compares the state. */
		if (result.event == uml::cpu::EngineEvent::general_protection) {
			const auto marker = memory.snapshot(cpu.rip, 1);
			if (!marker.empty() && marker.front() == std::byte{0xf4})
				continue;
		}
		if (result.event != uml::cpu::EngineEvent::continued) {
			const auto status = result.event ==
				uml::cpu::EngineEvent::memory_fault ?
				"memory_fault" : "unexpected_event";
			print_result(status, retired, cpu.rip,
				memory.snapshot(cpu.rip, 15), event_name(result.event), test);
			return 1;
		}
		if (result.instruction_budget_exhausted)
			break;
	}
	print_result("instruction_limit", retired, cpu.rip,
		memory.snapshot(cpu.rip, 15), "instruction retirement limit reached",
		test);
	return 1;
}

namespace {

constexpr std::array<std::byte, 8> corpus_magic{
	std::byte{'U'}, std::byte{'M'}, std::byte{'L'}, std::byte{'X'},
	std::byte{'8'}, std::byte{'6'}, std::byte{'T'}, std::byte{0}};
constexpr std::uint32_t corpus_version = 1;
/* FEX programs and repository cases share FEX's final-state configuration;
 * K records are per-instruction traces. */
constexpr std::uint8_t corpus_kind_fex = 1;
constexpr std::uint8_t corpus_kind_k = 2;
constexpr std::uint8_t corpus_kind_repository = 3;
constexpr std::uint64_t k_low_mapping_size = 0x1'0000'0000;

class ByteCursor final {
public:
	explicit ByteCursor(std::span<const std::byte> bytes) noexcept : bytes_{bytes}
	{
	}

	template<std::unsigned_integral Value>
	[[nodiscard]] bool read(Value &value) noexcept
	{
		if (!read_le(bytes_, offset_, value))
			return false;
		offset_ += sizeof(value);
		return true;
	}

	[[nodiscard]] bool take(std::size_t size,
		std::span<const std::byte> &value) noexcept
	{
		if (size > remaining())
			return false;
		value = bytes_.subspan(offset_, size);
		offset_ += size;
		return true;
	}

	[[nodiscard]] std::size_t remaining() const noexcept
	{
		return bytes_.size() - offset_;
	}

private:
	std::span<const std::byte> bytes_;
	std::size_t offset_{};
};

struct KExpectedState final {
	std::uint32_t rip_index{};
	std::uint16_t gpr_valid{};
	std::uint16_t vector_valid{};
	std::uint8_t flag_valid{};
	std::uint8_t flag_values{};
	std::uint8_t expected_event{};
	std::array<std::uint64_t, 16> gprs{};
	std::array<std::array<std::byte, 32>, 16> vectors{};
};

[[nodiscard]] bool read_k_state(ByteCursor &cursor, KExpectedState &state,
	std::string &error)
{
	std::uint16_t gpr_changed{};
	std::uint16_t vector_changed{};
	std::uint8_t flag_changed{};
	if (!cursor.read(state.rip_index) ||
		!cursor.read(state.gpr_valid) ||
		!cursor.read(gpr_changed) ||
		!cursor.read(state.vector_valid) ||
		!cursor.read(vector_changed) ||
		!cursor.read(state.flag_valid) ||
		!cursor.read(flag_changed) ||
		!cursor.read(state.flag_values) ||
		!cursor.read(state.expected_event)) {
		error = "truncated K state header";
		return false;
	}
	if (state.expected_event > 1 || (gpr_changed & ~state.gpr_valid) != 0 ||
		(vector_changed & ~state.vector_valid) != 0 ||
		(flag_changed & ~state.flag_valid) != 0 ||
		(state.flag_valid & 0x80U) != 0) {
		error = "invalid K state masks";
		return false;
	}
	for (unsigned index = 0; index < state.gprs.size(); ++index) {
		if ((gpr_changed & (1U << index)) != 0 &&
			!cursor.read(state.gprs[index])) {
			error = "truncated K general-register state";
			return false;
		}
	}
	for (unsigned index = 0; index < state.vectors.size(); ++index) {
		if ((vector_changed & (1U << index)) == 0)
			continue;
		std::span<const std::byte> bytes;
		if (!cursor.take(state.vectors[index].size(), bytes)) {
			error = "truncated K vector-register state";
			return false;
		}
		std::memcpy(state.vectors[index].data(), bytes.data(), bytes.size());
	}
	return true;
}

[[nodiscard]] std::uint8_t compact_flags(
	const uml::cpu::x86_64::CpuState &cpu) noexcept
{
	std::uint8_t value{};
	for (unsigned index = 0; index < pack_flags.size(); ++index)
		value |= ((cpu.rflags & pack_flags[index]) != 0) << index;
	return value;
}

void initialize_k_cpu(uml::cpu::x86_64::CpuState &cpu,
	const KExpectedState &expected, std::span<const std::uint32_t> offsets)
{
	for (unsigned index = 0; index < pack_registers.size(); ++index) {
		if ((expected.gpr_valid & (1U << index)) != 0)
			cpu.gpr(pack_registers[index]) = expected.gprs[index];
	}
	for (unsigned index = 0; index < cpu.vectors.size(); ++index) {
		if ((expected.vector_valid & (1U << index)) != 0)
			cpu.vectors[index] = expected.vectors[index];
	}
	cpu.rflags = uml::cpu::x86_64::rflags_reserved;
	for (unsigned index = 0; index < pack_flags.size(); ++index) {
		if ((expected.flag_valid & expected.flag_values & (1U << index)) != 0)
			cpu.rflags |= pack_flags[index];
	}
	cpu.rip = code_address + offsets[expected.rip_index];
}

[[nodiscard]] bool binary32_nan(std::uint32_t value) noexcept
{
	return (value & 0x7f80'0000U) == 0x7f80'0000U &&
		(value & 0x007f'ffffU) != 0;
}

[[nodiscard]] bool binary64_nan(std::uint64_t value) noexcept
{
	return (value & 0x7ff0'0000'0000'0000ULL) ==
			0x7ff0'0000'0000'0000ULL &&
		(value & 0x000f'ffff'ffff'ffffULL) != 0;
}

[[nodiscard]] bool equivalent_k_vector(
	const std::array<std::byte, 32> &expected,
	const std::array<std::byte, 32> &actual,
	bool nan_class_equivalence) noexcept
{
	if (expected == actual)
		return true;
	if (!nan_class_equivalence)
		return false;
	/* K's floating-point rules often select one concrete NaN payload where
	 * Intel either preserves an input payload or permits several quiet NaNs.
	 * Retain K as a class-level oracle for those lanes; native-oracle and FEX
	 * records remain bit-exact.
	 */
	for (unsigned lane = 0; lane < 4; ++lane) {
		std::uint64_t expected64{};
		std::uint64_t actual64{};
		std::memcpy(&expected64, expected.data() + lane * sizeof(expected64),
			sizeof(expected64));
		std::memcpy(&actual64, actual.data() + lane * sizeof(actual64),
			sizeof(actual64));
		if (expected64 == actual64 ||
			(binary64_nan(expected64) && binary64_nan(actual64)))
			continue;
		bool binary32_lanes_match = true;
		for (unsigned half = 0; half < 2; ++half) {
			std::uint32_t expected32{};
			std::uint32_t actual32{};
			const auto offset = lane * sizeof(expected64) +
				half * sizeof(expected32);
			std::memcpy(&expected32, expected.data() + offset,
				sizeof(expected32));
			std::memcpy(&actual32, actual.data() + offset,
				sizeof(actual32));
			if (expected32 != actual32 &&
				!(binary32_nan(expected32) && binary32_nan(actual32))) {
				binary32_lanes_match = false;
				break;
			}
		}
		if (!binary32_lanes_match)
			return false;
	}
	return true;
}

[[nodiscard]] bool compare_k_state(const uml::cpu::x86_64::CpuState &cpu,
	const KExpectedState &expected, std::span<const std::uint32_t> offsets,
	bool nan_class_equivalence, std::string &detail)
{
	const auto expected_rip = code_address + offsets[expected.rip_index];
	if (cpu.rip != expected_rip) {
		std::ostringstream mismatch;
		mismatch << "RIP expected=0x" << std::hex << expected_rip <<
			" actual=0x" << cpu.rip;
		detail = mismatch.str();
		return false;
	}
	for (unsigned index = 0; index < pack_registers.size(); ++index) {
		if ((expected.gpr_valid & (1U << index)) == 0 ||
			cpu.gpr(pack_registers[index]) == expected.gprs[index])
			continue;
		std::ostringstream mismatch;
		mismatch << register_name(index) << " expected=0x" << std::hex <<
			expected.gprs[index] << " actual=0x" << cpu.gpr(pack_registers[index]);
		detail = mismatch.str();
		return false;
	}
	for (unsigned index = 0; index < cpu.vectors.size(); ++index) {
		if ((expected.vector_valid & (1U << index)) != 0 &&
			!equivalent_k_vector(expected.vectors[index], cpu.vectors[index],
				nan_class_equivalence)) {
			for (unsigned lane = 0; lane < 4; ++lane) {
				std::uint64_t expected_lane{};
				std::uint64_t actual_lane{};
				std::memcpy(&expected_lane,
					expected.vectors[index].data() + lane * sizeof(expected_lane),
					sizeof(expected_lane));
				std::memcpy(&actual_lane,
					cpu.vectors[index].data() + lane * sizeof(actual_lane),
					sizeof(actual_lane));
				if (expected_lane == actual_lane)
					continue;
				std::ostringstream mismatch;
				mismatch << "YMM" << index << '[' << lane <<
					"] expected=0x" << std::hex << expected_lane <<
					" actual=0x" << actual_lane;
				detail = mismatch.str();
				break;
			}
			return false;
		}
	}
	const auto actual_flags = compact_flags(cpu);
	if (((actual_flags ^ expected.flag_values) & expected.flag_valid) != 0) {
		std::ostringstream mismatch;
		mismatch << "RFLAGS expected=0x" << std::hex <<
			static_cast<unsigned>(expected.flag_values) << " actual=0x" <<
			static_cast<unsigned>(actual_flags) << " mask=0x" <<
			static_cast<unsigned>(expected.flag_valid);
		detail = mismatch.str();
		return false;
	}
	return true;
}

[[nodiscard]] int run_k_trace(std::span<const std::byte> binary,
	std::span<const std::byte> payload, std::uint32_t expected_cases,
	std::string_view test, bool nan_class_equivalence)
{
	ByteCursor cursor{payload};
	std::uint32_t instruction_count{};
	std::uint32_t state_count{};
	std::uint32_t offset_count{};
	if (!cursor.read(instruction_count) || !cursor.read(state_count) ||
		!cursor.read(offset_count) || state_count < 2 ||
		offset_count != instruction_count + 1U ||
		expected_cases != state_count - 1U) {
		print_result("malformed_input", 0, 0, {},
			"invalid K trace header", test);
		return 2;
	}
	std::vector<std::uint32_t> offsets(offset_count);
	for (auto &offset : offsets) {
		if (!cursor.read(offset) || offset > binary.size()) {
			print_result("malformed_input", 0, 0, {},
				"invalid K instruction offset", test);
			return 2;
		}
	}
	if (!std::is_sorted(offsets.begin(), offsets.end()) ||
		std::adjacent_find(offsets.begin(), offsets.end()) != offsets.end()) {
		print_result("malformed_input", 0, 0, {},
			"unordered K instruction offsets", test);
		return 2;
	}

	SparseMemory memory;
	if (!memory.map(0, k_low_mapping_size) || !memory.write(code_address, binary)) {
		print_result("malformed_input", 0, code_address, {},
			"cannot map K test memory", test);
		return 2;
	}
	KExpectedState expected{};
	std::string error;
	if (!read_k_state(cursor, expected, error) ||
		expected.rip_index >= offsets.size()) {
		print_result("malformed_input", 0, 0, {}, error, test);
		return 2;
	}
	uml::cpu::x86_64::CpuState cpu{};
	initialize_k_cpu(cpu, expected, offsets);
	uml::cpu::x86_64::gadgets::Executor executor;
	executor.prepare(memory);
	for (std::uint32_t state_index = 1; state_index < state_count;
		++state_index) {
		const auto address = cpu.rip;
		const auto opcode = memory.snapshot(address, 15);
		if (!read_k_state(cursor, expected, error) ||
			expected.rip_index >= offsets.size()) {
			print_result("malformed_input", state_index, address, opcode,
				error, test);
			return 2;
		}
		const auto built = uml::cpu::x86_64::gadgets::build(
			memory, address, cpu.execution_mode);
		if (built.status != uml::cpu::x86_64::gadgets::BuildStatus::built) {
			const auto status = built.status ==
				uml::cpu::x86_64::gadgets::BuildStatus::fetch_fault ?
				"memory_fault" : "illegal_instruction";
			print_result(status, state_index - 1U, address, opcode,
				"K trace instruction did not build", test);
			return 1;
		}
		const auto result = uml::cpu::x86_64::gadgets::execute(
			built.program, cpu, memory, executor, 1);
		const auto expected_event = expected.expected_event == 0 ?
			uml::cpu::EngineEvent::continued :
			uml::cpu::EngineEvent::floating_point_trap;
		const auto expected_retired = expected.expected_event == 0 ? 1U : 0U;
		if (result.event != expected_event ||
			result.retired_instructions != expected_retired) {
			print_result("unexpected_event", state_index - 1U, address, opcode,
				event_name(result.event), test);
			return 1;
		}
		if (expected.expected_event != 0)
			cpu.rip = code_address + offsets[expected.rip_index];
		uml::cpu::x86_64::materialize_flags(cpu);
		std::string detail;
		if (!compare_k_state(cpu, expected, offsets,
			nan_class_equivalence, detail)) {
			print_result("result_mismatch", state_index, address, opcode,
				detail, test);
			return 1;
		}
	}
	if (cursor.remaining() != 0) {
		print_result("malformed_input", state_count - 1U, cpu.rip, {},
			"trailing K trace data", test);
		return 2;
	}
	return 0;
}

[[nodiscard]] int run_corpus(std::span<const std::byte> bytes)
{
	ByteCursor cursor{bytes};
	std::span<const std::byte> magic;
	std::uint32_t version{};
	std::uint32_t record_count{};
	if (!cursor.take(corpus_magic.size(), magic) ||
		!std::equal(magic.begin(), magic.end(), corpus_magic.begin()) ||
		!cursor.read(version) || version != corpus_version ||
		!cursor.read(record_count)) {
		print_result("malformed_input", 0, 0, {}, "invalid corpus header");
		return 2;
	}
	std::uint64_t cases{};
	std::uint32_t failures{};
	for (std::uint32_t record_index = 0; record_index < record_count;
		++record_index) {
		std::uint8_t kind{};
		std::uint8_t flags{};
		std::uint16_t reserved{};
		std::uint16_t name_size{};
		std::uint16_t source_size{};
		std::uint32_t code_size{};
		std::uint32_t payload_size{};
		std::uint32_t case_count{};
		if (!cursor.read(kind) || !cursor.read(flags) ||
			!cursor.read(reserved) || !cursor.read(name_size) ||
			!cursor.read(source_size) || !cursor.read(code_size) ||
			!cursor.read(payload_size) || !cursor.read(case_count) ||
			reserved != 0 || (flags & ~std::uint8_t{3}) != 0 ||
			kind < corpus_kind_fex || kind > corpus_kind_repository ||
			(kind != corpus_kind_k && (flags & 2U) != 0) ||
			(kind == corpus_kind_repository &&
				(flags != 0 || case_count != 1)) ||
			case_count == 0) {
			print_result("malformed_input", record_index, 0, {},
				"invalid corpus record header");
			return 2;
		}
		std::span<const std::byte> name_bytes;
		std::span<const std::byte> source_bytes;
		std::span<const std::byte> code;
		std::span<const std::byte> payload;
		if (!cursor.take(name_size, name_bytes) ||
			!cursor.take(source_size, source_bytes) ||
			!cursor.take(code_size, code) ||
			!cursor.take(payload_size, payload)) {
			print_result("malformed_input", record_index, 0, {},
				"truncated corpus record");
			return 2;
		}
		const std::string_view name{
			reinterpret_cast<const char *>(name_bytes.data()), name_bytes.size()};
		const auto result = kind == corpus_kind_k ?
			run_k_trace(code, payload, case_count, name,
				(flags & 2U) != 0) :
			run_fex_case(code, payload, name, true,
				kind == corpus_kind_repository ?
					Dispatch::cached : Dispatch::fresh);
		cases += case_count;
		failures += result != 0;
	}
	if (cursor.remaining() != 0) {
		print_result("malformed_input", record_count, 0, {},
			"trailing corpus bytes");
		return 2;
	}
	std::cout << "status=" << (failures == 0 ? "passed" : "failed") << '\t'
		<< "records=" << record_count << '\t' << "cases=" << cases << '\t'
		<< "failures=" << failures << '\n';
	return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
	std::vector<std::byte> first;
	std::string error;
	if (argc == 2) {
		if (!load_file(argv[1], first, error)) {
			print_result("malformed_input", 0, 0, {}, error);
			return 2;
		}
		return run_corpus(first);
	}
	const bool repository = argc == 4 &&
		std::string_view{argv[1]} == "--repository";
	if (argc == 3 || repository) {
		const auto *binary = argv[argc - 2];
		const auto *config = argv[argc - 1];
		std::vector<std::byte> second;
		if (!load_file(binary, first, error) ||
			!load_file(config, second, error)) {
			print_result("malformed_input", 0, 0, {}, error);
			return 2;
		}
		return run_fex_case(first, second, binary, false,
			repository ? Dispatch::cached : Dispatch::fresh);
	}
	print_result("malformed_input", 0, 0, {},
		"usage: runner CORPUS | runner [--repository] BINARY CONFIG");
	return 2;
}
