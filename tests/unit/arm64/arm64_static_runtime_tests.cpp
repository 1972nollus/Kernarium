// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_guest.h"
#include "uml_darwin_cpu.h"
#include "uml_darwin_host.h"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <source_location>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

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
	explicit TemporaryBacking(std::size_t size)
	{
		char path[] = "/private/tmp/uml-static-mmu.XXXXXX";
		fd_ = mkstemp(path);
		require(fd_ >= 0);
		require(unlink(path) == 0);
		require(ftruncate(fd_, static_cast<off_t>(size)) == 0);
	}

	TemporaryBacking(const TemporaryBacking &) = delete;
	TemporaryBacking &operator=(const TemporaryBacking &) = delete;

	~TemporaryBacking()
	{
		if (fd_ >= 0)
			close(fd_);
	}

	[[nodiscard]] int fd() const noexcept { return fd_; }

	void put32(std::size_t offset, std::uint32_t instruction)
	{
		std::array<std::byte, 4> encoded{};
		for (std::size_t index = 0; index < encoded.size(); ++index)
			encoded[index] = static_cast<std::byte>(instruction >> (index * 8));
		require(pwrite(fd_, encoded.data(), encoded.size(),
				offset) == static_cast<ssize_t>(encoded.size()));
	}

	void put64(std::size_t offset, std::uint64_t value)
	{
		require(pwrite(fd_, &value, sizeof(value), offset) ==
			static_cast<ssize_t>(sizeof(value)));
	}

	template<std::size_t Size>
	void put_bytes(std::size_t offset, const std::array<std::byte, Size> &bytes)
	{
		require(pwrite(fd_, bytes.data(), bytes.size(), offset) ==
			static_cast<ssize_t>(bytes.size()));
	}

private:
	int fd_{-1};
};

static void uml_static_entry_bridge()
{
	constexpr unsigned long generation = 77;
	constexpr std::uint64_t base = 0x1'0000;
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xd280'00a0U);  // movz x0, #5
	backing.put32(4, 0x9100'1c01U);  // add x1, x0, #7
	backing.put32(8, 0xf900'07e1U);  // str x1, [sp, #8]
	backing.put32(12, 0xd400'0001U); // svc #0

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		1, 1) == 0);
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	registers.sp = base + 0x800;
	unsigned long tls = 0x1234;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 5);
	require(registers.x[1] == 12);
	require(registers.pc == base + 16);
	void *stored = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, registers.sp + 8, DARWIN_UML_STATIC_READ,
		&stored) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t value = 0;
	std::memcpy(&value, stored, sizeof(value));
	require(value == 12);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

#if DARWIN_UML_X86_64_GUEST
static void uml_static_x86_64_entry_bridge()
{
	constexpr unsigned long generation = 131;
	constexpr std::uint64_t base = 0x1'8000;
	constexpr std::array program{
		std::byte{0xb8}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
		std::byte{0x00},                         // mov eax, 1
		std::byte{0xbf}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
		std::byte{0x00},                         // mov edi, 1
		std::byte{0x48}, std::byte{0x8d}, std::byte{0x35}, std::byte{0x07},
		std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, // lea rsi, [rip+7]
		std::byte{0xba}, std::byte{0x05}, std::byte{0x00}, std::byte{0x00},
		std::byte{0x00},                         // mov edx, 5
		std::byte{0x0f}, std::byte{0x05},         // syscall
		std::byte{'x'}, std::byte{'8'}, std::byte{'6'}, std::byte{'!'},
		std::byte{'\n'},
	};
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put_bytes(0, program);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);

	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.padding = 2;
	registers.pc = base;
	registers.x[16] = 2;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 1); // rax
	require(registers.x[7] == 1); // rdi
	require(registers.x[6] == base + 24); // rsi
	require(registers.x[2] == 5); // rdx
	require(registers.pc == base + 24);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_x86_64_signal_return_bridge()
{
	constexpr unsigned long generation = 132;
	constexpr std::uint64_t base = 0x1'c000;
	constexpr std::uint64_t stack = base + 0x800;
	constexpr std::array program{std::byte{0xc3}}; // ret
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put_bytes(0, program);
	backing.put64(stack - base, DARWIN_UML_SIGRETURN_TRAMPOLINE);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		1, 1) == 0);

	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.padding = 2;
	registers.pc = base;
	registers.x[16] = 2;
	registers.x[4] = stack; // rsp
	registers.svcr = std::uint64_t{1} << 32;
	fp.reserved[1] = 0x5838'3634U;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 15);
	require(registers.x[4] == stack + sizeof(std::uint64_t));
	require(registers.pc == DARWIN_UML_SIGRETURN_TRAMPOLINE);
	require((registers.svcr & (std::uint64_t{1} << 32)) == 0);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_x86_64_x87_task_state()
{
	constexpr unsigned long generation = 133;
	constexpr std::uint64_t base = 0x2'0000;
	constexpr std::uint64_t result_address = base + 0x200;
	constexpr std::array first_task{
		std::byte{0xd9}, std::byte{0xe8},             // fld1
		std::byte{0x0f}, std::byte{0x05},             // syscall
		std::byte{0xdd}, std::byte{0x1d},             // fstp qword [rip+0x1f6]
		std::byte{0xf6}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
		std::byte{0x0f}, std::byte{0x05},             // syscall
	};
	constexpr std::array second_task{
		std::byte{0xdb}, std::byte{0xe3},             // fninit
		std::byte{0x0f}, std::byte{0x05},             // syscall
	};
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put_bytes(0, first_task);
	backing.put_bytes(0x100, second_task);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		1, 1) == 0);

	darwin_uml_guest_registers first_registers{};
	darwin_uml_guest_fp_registers first_fp{};
	first_registers.padding = 2;
	first_registers.pc = base;
	first_registers.x[16] = 2;
	darwin_uml_guest_registers second_registers{};
	darwin_uml_guest_fp_registers second_fp{};
	second_registers.padding = 2;
	second_registers.pc = base + 0x100;
	second_registers.x[16] = 2;
	unsigned long tls = 0;
	require(darwin_uml_static_enter_guest(&first_registers, &first_fp,
		sizeof(first_fp), generation, &tls) == DARWIN_UML_EVENT_SYSCALL);
	require(first_registers.pc == base + 4);
	require(darwin_uml_static_enter_guest(&second_registers, &second_fp,
		sizeof(second_fp), generation, &tls) == DARWIN_UML_EVENT_SYSCALL);
	require(darwin_uml_static_enter_guest(&first_registers, &first_fp,
		sizeof(first_fp), generation, &tls) == DARWIN_UML_EVENT_SYSCALL);
	void *stored = nullptr;
	require(darwin_uml_static_vm_translate(generation, result_address,
		DARWIN_UML_STATIC_READ, &stored) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t value{};
	std::memcpy(&value, stored, sizeof(value));
	require(value == 0x3ff0'0000'0000'0000ULL);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}
#endif

static void uml_static_descriptor_reuse_invalidation()
{
	constexpr unsigned long first_generation = 107;
	constexpr unsigned long second_generation = 108;
	constexpr std::uint64_t base = 0x4'0000;
	constexpr std::uint64_t first_value = 0x1111'2222'3333'4444ULL;
	constexpr std::uint64_t second_value = 0xaaaa'bbbb'cccc'ddddULL;
	char first_path[] = "/tmp/uml-static-fd-first-XXXXXX";
	int descriptor = mkstemp(first_path);
	require(descriptor >= 0);
	unlink(first_path);
	require(ftruncate(descriptor, DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	require(pwrite(descriptor, &first_value, sizeof(first_value), 0) ==
		static_cast<ssize_t>(sizeof(first_value)));
	require(darwin_uml_static_vm_activate(first_generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), descriptor, 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	const int reused_descriptor = descriptor;
	require(darwin_uml_close(descriptor) == 0);

	char second_path[] = "/tmp/uml-static-fd-second-XXXXXX";
	descriptor = mkstemp(second_path);
	require(descriptor >= 0);
	unlink(second_path);
	if (descriptor != reused_descriptor) {
		require(dup2(descriptor, reused_descriptor) == reused_descriptor);
		require(darwin_uml_close(descriptor) == 0);
		descriptor = reused_descriptor;
	}
	require(ftruncate(descriptor, DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	require(pwrite(descriptor, &second_value, sizeof(second_value), 0) ==
		static_cast<ssize_t>(sizeof(second_value)));
	require(darwin_uml_static_vm_activate(second_generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), descriptor, 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(
		second_generation, base, DARWIN_UML_STATIC_READ,
		&translated) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t actual = 0;
	std::memcpy(&actual, translated, sizeof(actual));
	require(actual == second_value);
	require(darwin_uml_close(descriptor) == 0);
	require(darwin_uml_static_vm_destroy(first_generation) == 0);
	require(darwin_uml_static_vm_destroy(second_generation) == 0);
}

static void uml_static_host_interrupt_sequence()
{
	using namespace std::chrono_literals;
	constexpr unsigned long generation = 106;
	constexpr std::uint64_t base = 0x3'c000;
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0x1400'0000U); // b .
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);

	unsigned int sequence = 0;
	require(darwin_uml_static_guest_interrupt_sequence(
		&sequence, sequence) == 0);
	std::thread interrupt{[&] {
		std::this_thread::sleep_for(10ms);
		__atomic_fetch_add(&sequence, 1U, __ATOMIC_RELEASE);
	}};
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	interrupt.join();
	require(darwin_uml_static_guest_interrupt_sequence(nullptr, 0) == 0);
	require(event == DARWIN_UML_EVENT_INTERRUPT);
	require(registers.pc == base);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_signal_return_bridge()
{
	constexpr std::uint64_t base = 0x1'4000;
	for (const auto [generation, fallback] :
	     std::array<std::pair<unsigned long, bool>, 2>{{{78, false}, {79, true}}}) {
		TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
		if (fallback) {
			backing.put32(0, 0x8b01'0000U); // add x0, x0, x1
			backing.put32(4, 0xd65f'03c0U); // ret
		} else {
			backing.put32(0, 0xd65f'03c0U); // ret
		}

		require(darwin_uml_static_vm_activate(generation) == 0);
		require(darwin_uml_static_vm_map(
			reinterpret_cast<void *>(base), backing.fd(), 0,
			DARWIN_UML_GUEST_PAGE_SIZE,
			PROT_READ | PROT_EXEC, 1, 0) == 0);
		darwin_uml_guest_registers registers{};
		darwin_uml_guest_fp_registers fp{};
		registers.pc = base;
		registers.lr = DARWIN_UML_SIGRETURN_TRAMPOLINE;
		registers.x[0] = 4;
		registers.x[1] = 5;
		unsigned long tls = 0;
		const int event = darwin_uml_static_enter_guest(
			&registers, &fp, sizeof(fp), generation, &tls);
		require(event == DARWIN_UML_EVENT_SYSCALL);
		require(registers.x[8] == DARWIN_UML_NR_RT_SIGRETURN);
		require(registers.pc == DARWIN_UML_SIGRETURN_TRAMPOLINE + 4);
		if (fallback)
			require(registers.x[0] == 9);
		require(darwin_uml_static_vm_destroy(generation) == 0);
	}
}

static void uml_static_permission_fault()
{
	constexpr unsigned long generation = 91;
	constexpr std::uint64_t base = 0x2'0000;
	TemporaryBacking backing{2 * DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0xd280'0aa0U);  // movz x0, #0x55
	backing.put32(4, 0xf900'0020U);  // str x0, [x1]
	backing.put32(8, 0xd400'0001U);  // svc #0

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base + DARWIN_UML_GUEST_PAGE_SIZE),
		backing.fd(), DARWIN_UML_GUEST_PAGE_SIZE,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ, 0, 1) == 0);
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	registers.x[1] = base + DARWIN_UML_GUEST_PAGE_SIZE;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_FAULT);
	require(registers.pc == base + 4);
	require(registers.fault.address == base + DARWIN_UML_GUEST_PAGE_SIZE);
	require(registers.fault.error_code == DARWIN_UML_FAULT_WRITE);
	require(registers.fault.trap_number == DARWIN_UML_FAULT_PAGE);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_illegal_instruction()
{
	constexpr unsigned long generation = 109;
	constexpr std::uint64_t base = 0x2'4000;
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	backing.put32(0, 0x0000'0000U); // udf #0

	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_EXEC, 1, 0) == 0);
	darwin_uml_guest_registers registers{};
	darwin_uml_guest_fp_registers fp{};
	registers.pc = base;
	unsigned long tls = 0;
	const int event = darwin_uml_static_enter_guest(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION);
	require(registers.pc == base);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_mapping_change_ranges()
{
	constexpr unsigned long generation = 97;
	constexpr std::uint64_t base = 0x2'4000;
	TemporaryBacking backing{2 * DARWIN_UML_GUEST_PAGE_SIZE};
	require(darwin_uml_static_vm_activate(generation) == 0);
	const auto initial = darwin_uml_static_vm_changes(generation);
	const auto initial_sequences =
		darwin_uml_static_vm_sequences(generation);
	require(initial_sequences.mapping_changes == initial);
	require(initial_sequences.code_changes ==
		darwin_uml_static_vm_code_changes(generation));
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		2 * DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	std::array<darwin_uml_static_mapping_change, 4> ranges{};
	std::uint64_t current = 0;
	require(darwin_uml_static_vm_mapping_changes(
		generation, initial, ranges.data(), ranges.size(), &current) == 1);
	require(ranges[0].address == base);
	require(ranges[0].length == 2 * DARWIN_UML_GUEST_PAGE_SIZE);
	require(current == darwin_uml_static_vm_changes(generation));
	const auto mapped_sequences =
		darwin_uml_static_vm_sequences(generation);
	require(mapped_sequences.mapping_changes == current);
	require(mapped_sequences.code_changes == initial_sequences.code_changes);

	/* Replaying an unchanged Linux PTE batch must not invalidate the TLB. */
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		2 * DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	require(darwin_uml_static_vm_changes(generation) == current);

	require(darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(base + DARWIN_UML_GUEST_PAGE_SIZE),
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ) == 0);
	const auto before_protect = current;
	require(darwin_uml_static_vm_mapping_changes(
		generation, before_protect, ranges.data(), ranges.size(), &current) == 1);
	require(ranges[0].address == base + DARWIN_UML_GUEST_PAGE_SIZE);
	require(ranges[0].length == DARWIN_UML_GUEST_PAGE_SIZE);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_partial_uaccess_copy_preserves_sibling_writes()
{
	constexpr unsigned long generation = 130;
	constexpr std::uint64_t base = 0x2'8000;
	constexpr std::size_t write_offset = 32;
	constexpr std::size_t concurrent_offset = 512;
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> stale_shadow{};
	stale_shadow.fill(std::byte{0x11});
	require(pwrite(backing.fd(), stale_shadow.data(), stale_shadow.size(), 0) ==
		static_cast<ssize_t>(stale_shadow.size()));
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(generation, base,
		DARWIN_UML_STATIC_WRITE, &translated) == DARWIN_UML_STATIC_TRANSLATED);
	auto *bytes = static_cast<std::byte *>(translated);
	bytes[concurrent_offset] = std::byte{0x7a};
	const std::array replacement{
		std::byte{0xa0}, std::byte{0xa1}, std::byte{0xa2}, std::byte{0xa3}};
	std::copy(replacement.begin(), replacement.end(),
		stale_shadow.begin() + write_offset);
	require(darwin_uml_static_vm_copy_from_buffer(
		base + write_offset, stale_shadow.data() + write_offset,
		replacement.size()) == 0);
	require(std::equal(replacement.begin(), replacement.end(),
		bytes + write_offset));
	require(bytes[concurrent_offset] == std::byte{0x7a});
	require(darwin_uml_static_vm_copy_from_buffer(base, nullptr, 0) == 0);
	require(darwin_uml_static_vm_copy_from_buffer(
		base + DARWIN_UML_GUEST_PAGE_SIZE - 1, stale_shadow.data(), 2) == -1);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_thread_local_generations()
{
	constexpr unsigned long first_generation = 101;
	constexpr unsigned long second_generation = 102;
	constexpr std::uint64_t base = 0x3'0000;
	constexpr std::uint64_t first_value = 0x1111'2222'3333'4444ULL;
	constexpr std::uint64_t second_value = 0xaaaa'bbbb'cccc'ddddULL;
	TemporaryBacking backing{2 * DARWIN_UML_GUEST_PAGE_SIZE};
	require(pwrite(backing.fd(), &first_value, sizeof(first_value), 0) ==
		static_cast<ssize_t>(sizeof(first_value)));
	require(pwrite(backing.fd(), &second_value, sizeof(second_value),
		DARWIN_UML_GUEST_PAGE_SIZE) ==
		static_cast<ssize_t>(sizeof(second_value)));
	std::barrier first_activated{2};
	std::barrier second_activated{2};
	std::thread first{[&] {
		require(darwin_uml_static_vm_activate(first_generation) == 0);
		first_activated.arrive_and_wait();
		second_activated.arrive_and_wait();
		require(darwin_uml_static_vm_map(reinterpret_cast<void *>(base),
			backing.fd(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
			PROT_READ | PROT_WRITE, 0, 1) == 0);
	}};
	std::thread second{[&] {
		first_activated.arrive_and_wait();
		require(darwin_uml_static_vm_activate(second_generation) == 0);
		second_activated.arrive_and_wait();
		require(darwin_uml_static_vm_map(reinterpret_cast<void *>(base),
			backing.fd(), DARWIN_UML_GUEST_PAGE_SIZE,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	}};
	first.join();
	second.join();
	void *first_host = nullptr;
	void *second_host = nullptr;
	require(darwin_uml_static_vm_translate(first_generation, base,
		DARWIN_UML_STATIC_READ, &first_host) == DARWIN_UML_STATIC_TRANSLATED);
	require(darwin_uml_static_vm_translate(second_generation, base,
		DARWIN_UML_STATIC_READ, &second_host) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t first_actual = 0;
	std::uint64_t second_actual = 0;
	std::memcpy(&first_actual, first_host, sizeof(first_actual));
	std::memcpy(&second_actual, second_host, sizeof(second_actual));
	require(first_actual == first_value);
	require(second_actual == second_value);
	const auto first_mapping_changes =
		darwin_uml_static_vm_changes(first_generation);
	const auto second_mapping_changes =
		darwin_uml_static_vm_changes(second_generation);
	const auto first_code_changes =
		darwin_uml_static_vm_code_changes(first_generation);
	const auto second_code_changes =
		darwin_uml_static_vm_code_changes(second_generation);
	const auto first_sequences =
		darwin_uml_static_vm_sequences(first_generation);
	const auto second_sequences =
		darwin_uml_static_vm_sequences(second_generation);
	require(first_sequences.mapping_changes == first_mapping_changes);
	require(first_sequences.code_changes == first_code_changes);
	require(second_sequences.mapping_changes == second_mapping_changes);
	require(second_sequences.code_changes == second_code_changes);
	require(darwin_uml_static_vm_activate(second_generation) == 0);
	require(darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(base), DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ) == 0);
	require(darwin_uml_static_vm_changes(first_generation) ==
		first_mapping_changes);
	require(darwin_uml_static_vm_changes(second_generation) >
		second_mapping_changes);
	require(darwin_uml_static_vm_code_changes(first_generation) ==
		first_code_changes);
	require(darwin_uml_static_vm_code_changes(second_generation) ==
		second_code_changes);
	darwin_uml_static_vm_invalidate_code(base);
	require(darwin_uml_static_vm_code_changes(first_generation) ==
		first_code_changes);
	require(darwin_uml_static_vm_code_changes(second_generation) >
		second_code_changes);
	require(darwin_uml_static_vm_destroy(first_generation) == 0);
	require(darwin_uml_static_vm_destroy(second_generation) == 0);
}

static void uml_static_concurrent_translation_readers()
{
	constexpr std::size_t reader_count = 16;
	constexpr std::size_t translation_count = 20'000;
	constexpr std::size_t protection_count = 4'000;
	constexpr unsigned long first_generation = 106;
	constexpr unsigned long writer_generation =
		first_generation + reader_count;
	constexpr std::uint64_t base = 0x3'0000;
	TemporaryBacking backing{(reader_count + 1) *
		DARWIN_UML_GUEST_PAGE_SIZE};

	for (std::size_t index = 0; index <= reader_count; ++index) {
		const std::uint64_t value = 0x5a5a'0000'0000'0000ULL + index;
		require(pwrite(backing.fd(), &value, sizeof(value),
			index * DARWIN_UML_GUEST_PAGE_SIZE) ==
			static_cast<ssize_t>(sizeof(value)));
		const auto generation = first_generation + index;
		require(darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(base), backing.fd(),
			index * DARWIN_UML_GUEST_PAGE_SIZE,
			DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
			0, 1) == 0);
	}

	std::barrier start{reader_count + 1};
	std::vector<std::thread> readers;
	readers.reserve(reader_count);
	for (std::size_t index = 0; index < reader_count; ++index) {
		readers.emplace_back([&, index] {
			const auto generation = first_generation + index;
			const std::uint64_t expected =
				0x5a5a'0000'0000'0000ULL + index;
			std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> page{};
			std::memcpy(page.data(), &expected, sizeof(expected));
			require(darwin_uml_static_vm_activate(generation) == 0);
			start.arrive_and_wait();
			for (std::size_t iteration = 0;
			     iteration < translation_count; ++iteration) {
				if (iteration % 1'000 == 0)
					require(darwin_uml_static_vm_copy_from_buffer(
						base, page.data(), page.size()) == 0);
				void *host = nullptr;
				require(darwin_uml_static_vm_translate(generation, base,
					DARWIN_UML_STATIC_READ, &host) ==
					DARWIN_UML_STATIC_TRANSLATED);
				std::uint64_t actual = 0;
				std::memcpy(&actual, host, sizeof(actual));
				require(actual == expected);
			}
		});
	}
	std::thread writer{[&] {
		require(darwin_uml_static_vm_activate(writer_generation) == 0);
		start.arrive_and_wait();
		for (std::size_t iteration = 0; iteration < protection_count;
		     ++iteration) {
			const int protection = iteration % 2 == 0 ? PROT_READ :
				PROT_READ | PROT_WRITE;
			require(darwin_uml_static_vm_protect(
				reinterpret_cast<void *>(base),
				DARWIN_UML_GUEST_PAGE_SIZE, protection) == 0);
		}
	}};
	for (auto &reader : readers)
		reader.join();
	writer.join();
	for (std::size_t index = 0; index <= reader_count; ++index)
		require(darwin_uml_static_vm_destroy(first_generation + index) == 0);
}

static void uml_static_tlb_shootdown_quiesces_execution()
{
	using namespace std::chrono_literals;
	constexpr unsigned long generation = 103;
	constexpr std::uint64_t base = 0x3'4000;
	TemporaryBacking backing{DARWIN_UML_GUEST_PAGE_SIZE};
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(reinterpret_cast<void *>(base),
		backing.fd(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);

	std::barrier execution_entered{2};
	std::barrier invalidation_started{2};
	std::atomic_bool release_execution{};
	std::atomic_bool invalidation_complete{};
	std::atomic<void *> context{};
	std::thread execution{[&] {
		void *execution_context =
			darwin_uml_static_vm_execution_context_create(generation);
		require(execution_context != nullptr);
		require(darwin_uml_static_vm_execution_context_begin(
			execution_context, base) == 0);
		context.store(execution_context, std::memory_order_release);
		execution_entered.arrive_and_wait();
		release_execution.wait(false, std::memory_order_acquire);
		require(darwin_uml_static_vm_execution_context_end(execution_context) == 0);
		darwin_uml_static_vm_execution_context_destroy(execution_context);
	}};
	execution_entered.arrive_and_wait();
	void *execution_context = context.load(std::memory_order_acquire);
	require(execution_context != nullptr);
	std::thread invalidation{[&] {
		invalidation_started.arrive_and_wait();
		require(darwin_uml_static_vm_unmap_generation(
			generation, reinterpret_cast<void *>(base),
			DARWIN_UML_GUEST_PAGE_SIZE) == 0);
		invalidation_complete.store(true, std::memory_order_release);
		invalidation_complete.notify_all();
	}};
	invalidation_started.arrive_and_wait();
	std::this_thread::sleep_for(20ms);
	require(!invalidation_complete.load(std::memory_order_acquire));
	const auto interrupted =
		darwin_uml_static_vm_execution_context_sequences(execution_context);
	require(interrupted.execution_interrupt != 0);
	release_execution.store(true, std::memory_order_release);
	release_execution.notify_all();
	execution.join();
	invalidation.join();
	require(invalidation_complete.load(std::memory_order_acquire));
	void *translated = nullptr;
	require(darwin_uml_static_vm_translate(generation, base,
		DARWIN_UML_STATIC_READ, &translated) == DARWIN_UML_STATIC_UNMAPPED);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

static void uml_static_batch_targets_explicit_generation()
{
	constexpr unsigned long active_generation = 104;
	constexpr unsigned long target_generation = 105;
	constexpr std::uint64_t base = 0x3'8000;
	constexpr std::uint64_t value = 0x1234'5678'9abc'def0ULL;
	constexpr std::uint64_t replacement = 0x0fed'cba9'8765'4321ULL;
	TemporaryBacking backing{2 * DARWIN_UML_GUEST_PAGE_SIZE};
	require(pwrite(backing.fd(), &value, sizeof(value), 0) ==
		static_cast<ssize_t>(sizeof(value)));
	require(pwrite(backing.fd(), &replacement, sizeof(replacement),
		DARWIN_UML_GUEST_PAGE_SIZE) ==
		static_cast<ssize_t>(sizeof(replacement)));
	require(darwin_uml_static_vm_activate(active_generation) == 0);

	/* Page-table synchronization may run on behalf of an mm other than the
	 * one active on this Darwin thread.  The batch and every operation inside
	 * it must target that explicit generation, and a mismatched nested batch
	 * must fail without disturbing the outer writer gate.
	 */
	require(darwin_uml_static_vm_batch_begin(target_generation) == 0);
	require(darwin_uml_static_vm_batch_begin(active_generation) == -1);
	require(darwin_uml_static_vm_map_generation(
		target_generation, reinterpret_cast<void *>(base), backing.fd(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE, PROT_READ | PROT_WRITE, 0, 1) == 0);
	void *target_host = nullptr;
	require(darwin_uml_static_vm_translate(target_generation, base,
		DARWIN_UML_STATIC_READ, &target_host) == DARWIN_UML_STATIC_UNMAPPED);
	require(darwin_uml_static_vm_batch_end() == 0);

	void *active_host = nullptr;
	require(darwin_uml_static_vm_translate(active_generation, base,
		DARWIN_UML_STATIC_READ, &active_host) == DARWIN_UML_STATIC_UNMAPPED);
	require(darwin_uml_static_vm_translate(target_generation, base,
		DARWIN_UML_STATIC_READ, &target_host) == DARWIN_UML_STATIC_TRANSLATED);
	std::uint64_t actual = 0;
	std::memcpy(&actual, target_host, sizeof(actual));
	require(actual == value);

	/* An invalidation which wins while a page-table snapshot is buffered
	 * makes that snapshot stale.  The commit must be rejected instead of
	 * resurrecting its old physical mapping.
	 */
	require(darwin_uml_static_vm_batch_begin(target_generation) == 0);
	require(darwin_uml_static_vm_map_generation(
		target_generation, reinterpret_cast<void *>(base), backing.fd(),
		DARWIN_UML_GUEST_PAGE_SIZE, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	std::thread invalidation{[&] {
		require(darwin_uml_static_vm_unmap_generation(
			target_generation, reinterpret_cast<void *>(base),
			DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	}};
	invalidation.join();
	require(darwin_uml_static_vm_batch_end() == -EAGAIN);
	target_host = nullptr;
	require(darwin_uml_static_vm_translate(target_generation, base,
		DARWIN_UML_STATIC_READ, &target_host) == DARWIN_UML_STATIC_UNMAPPED);
	require(darwin_uml_static_vm_destroy(active_generation) == 0);
	require(darwin_uml_static_vm_destroy(target_generation) == 0);
}

static void uml_static_walker_turnstile_blocks_host_thread()
{
	using namespace std::chrono_literals;
	constexpr unsigned long generation = 106;
	std::atomic_bool started{};
	std::atomic_bool acquired{};

	require(darwin_uml_static_vm_walker_lock(generation) == 0);
	require(darwin_uml_static_vm_walker_lock(generation) == -1);
	require(darwin_uml_static_vm_walker_unlock(generation + 1) == -1);
	std::thread sibling{[&] {
		started.store(true, std::memory_order_release);
		started.notify_all();
		require(darwin_uml_static_vm_walker_lock(generation) == 0);
		acquired.store(true, std::memory_order_release);
		require(darwin_uml_static_vm_walker_unlock(generation) == 0);
	}};
	started.wait(false, std::memory_order_acquire);
	std::this_thread::sleep_for(20ms);
	require(!acquired.load(std::memory_order_acquire));
	require(darwin_uml_static_vm_walker_unlock(generation) == 0);
	sibling.join();
	require(acquired.load(std::memory_order_acquire));
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

} // namespace

int main()
{
	uml_static_entry_bridge();
#if DARWIN_UML_X86_64_GUEST
	uml_static_x86_64_entry_bridge();
	uml_static_x86_64_signal_return_bridge();
	uml_static_x86_64_x87_task_state();
#endif
	uml_static_descriptor_reuse_invalidation();
	uml_static_host_interrupt_sequence();
	uml_static_signal_return_bridge();
	uml_static_permission_fault();
	uml_static_illegal_instruction();
	uml_static_mapping_change_ranges();
	uml_static_partial_uaccess_copy_preserves_sibling_writes();
	uml_static_thread_local_generations();
	uml_static_concurrent_translation_readers();
	uml_static_tlb_shootdown_quiesces_execution();
	uml_static_batch_targets_explicit_generation();
	uml_static_walker_turnstile_blocks_host_thread();
	return 0;
}
