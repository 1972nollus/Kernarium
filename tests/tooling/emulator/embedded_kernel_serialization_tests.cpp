// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml/emulator.h"
#include "uml_elf64.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
/* Match the real Darwin UML kernel link window. A lower arbitrary MAP_FIXED
 * address can collide with unrelated host allocations and does not exercise
 * the production loader contract safely. */
constexpr std::uint64_t linked_address = 0x6000'0000'0000ULL;

struct Interleave final {
	std::mutex mutex;
	std::condition_variable changed;
	bool first_entered{};
	bool second_started{};
	bool second_entered{};
	bool release_first{};
} interleave;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

int first_kernel(int, char *const[], char *const[])
{
	std::unique_lock lock{interleave.mutex};
	interleave.first_entered = true;
	interleave.changed.notify_all();
	if (!interleave.changed.wait_for(lock, 5s, [] {
		return interleave.release_first;
	}))
		return 91;
	return 17;
}

int second_kernel(int, char *const[], char *const[])
{
	std::lock_guard lock{interleave.mutex};
	interleave.second_entered = true;
	interleave.changed.notify_all();
	return 23;
}

std::vector<std::byte> embedded_image(uml_emulator_kernel_entry entry)
{
	constexpr std::size_t payload_offset = 256;
	constexpr std::uint32_t load_callback = 0x5800'0050U;
	constexpr std::uint32_t branch_callback = 0xd61f'0200U;
	std::vector<std::byte> image(payload_offset + 16);
	auto *header = reinterpret_cast<uml_elf64_ehdr *>(image.data());
	header->ident[0] = UML_ELFMAG0;
	header->ident[1] = UML_ELFMAG1;
	header->ident[2] = UML_ELFMAG2;
	header->ident[3] = UML_ELFMAG3;
	header->ident[4] = UML_ELFCLASS64;
	header->ident[5] = UML_ELFDATA2LSB;
	header->type = UML_ET_DYN;
	header->machine = UML_EM_AARCH64;
	header->version = 1;
	header->entry = linked_address;
	header->phoff = sizeof(*header);
	header->ehsize = sizeof(*header);
	header->phentsize = sizeof(uml_elf64_phdr);
	header->phnum = 1;
	auto *program = reinterpret_cast<uml_elf64_phdr *>(
		image.data() + header->phoff);
	program->type = UML_PT_LOAD;
	program->flags = UML_PF_R | UML_PF_X;
	program->offset = payload_offset;
	program->vaddr = linked_address;
	program->paddr = linked_address;
	program->filesz = 16;
	program->memsz = 16;
	program->align = 0x4000;
	std::memcpy(image.data() + payload_offset, &load_callback,
		sizeof(load_callback));
	std::memcpy(image.data() + payload_offset + 4, &branch_callback,
		sizeof(branch_callback));
	const auto callback = reinterpret_cast<std::uintptr_t>(entry);
	std::memcpy(image.data() + payload_offset + 8, &callback,
		sizeof(callback));
	return image;
}

using Emulator = std::unique_ptr<uml_emulator, decltype(&uml_emulator_destroy)>;

Emulator make_emulator(const std::vector<std::byte> &image)
{
	const uml_emulator_config config{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.rootfs_path = "/tmp/embedded-kernel-serialization.img",
		.kernel_image = image.data(),
		.kernel_image_size = image.size(),
	};
	uml_emulator *created = nullptr;
	require(uml_emulator_create(&config, &created) == UML_EMULATOR_OK,
		"embedded-kernel emulator creation failed");
	return {created, uml_emulator_destroy};
}

void test_embedded_handles_are_process_serialized()
{
	const auto image_a = embedded_image(first_kernel);
	const auto image_b = embedded_image(second_kernel);
	auto emulator_a = make_emulator(image_a);
	auto emulator_b = make_emulator(image_b);
	const char *arguments[]{"embedded-kernel-serialization"};
	int status_a = -1;
	int status_b = -1;
	std::thread first{[&] {
		status_a = uml_emulator_run(
			emulator_a.get(), 1, arguments, nullptr);
	}};
	bool first_entered = false;
	{
		std::unique_lock lock{interleave.mutex};
		first_entered = interleave.changed.wait_for(lock, 5s, [] {
			return interleave.first_entered;
		});
	}
	std::thread second;
	if (first_entered) {
		second = std::thread{[&] {
			{
				std::lock_guard lock{interleave.mutex};
				interleave.second_started = true;
			}
			interleave.changed.notify_all();
			status_b = uml_emulator_run(
				emulator_b.get(), 1, arguments, nullptr);
		}};
	}
	bool overlapped = false;
	{
		std::unique_lock lock{interleave.mutex};
		if (first_entered && interleave.changed.wait_for(lock, 5s, [] {
			return interleave.second_started;
		}))
			overlapped = interleave.changed.wait_for(lock, 250ms, [] {
				return interleave.second_entered;
			});
		interleave.release_first = true;
	}
	interleave.changed.notify_all();
	first.join();
	if (second.joinable())
		second.join();
	if (!first_entered || overlapped || status_a != 17 || status_b != 23 ||
	    !interleave.second_entered)
		std::fprintf(stderr,
			"serialization diagnostics first-entered=%d second-started=%d "
			"second-entered=%d overlapped=%d status-a=%d status-b=%d\n",
			first_entered, interleave.second_started,
			interleave.second_entered, overlapped, status_a, status_b);
	require(first_entered, "first embedded kernel did not enter");
	require(!overlapped,
		"two embedded kernels occupied the linked window concurrently");
	require(status_a == 17 && status_b == 23 && interleave.second_entered,
		"serialized embedded kernels did not both complete");
}

} // namespace

int main()
{
	try {
		test_embedded_handles_are_process_serialized();
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "embedded-kernel serialization test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
}
