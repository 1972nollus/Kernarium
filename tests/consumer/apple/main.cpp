/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <uml/emulator.h>

#include <cstdint>
#include <cstring>

namespace {

static_assert(UML_EMULATOR_ABI_VERSION == 4U);
static_assert(UML_EMULATOR_ABI_FINGERPRINT == 0xd9a69804d3303044ULL);

int native_kernel(int, char *const[], char *const[])
{
	return 0;
}

} // namespace

int main()
{
	uml_emulator *emulator = nullptr;
	if (uml_emulator_create(nullptr, &emulator) !=
	    -UML_EMULATOR_INVALID_ARGUMENT)
		return 1;
	const uml_emulator_config configuration{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
		.native_kernel_entry = native_kernel,
	};
	if (uml_emulator_create(&configuration, &emulator) != UML_EMULATOR_OK)
		return 1;
	uml_emulator_capabilities capabilities{
		.struct_size = sizeof(uml_emulator_capabilities),
	};
	const int status = uml_emulator_get_capabilities(
		emulator, &capabilities);
	const char *const arguments[]{"uml-package-consumer", "--help"};
	const int run_status = uml_emulator_run(emulator, 2, arguments, nullptr);
	uml_emulator_request_stop(emulator);
	uml_emulator_destroy(emulator);
	return status == UML_EMULATOR_OK &&
		run_status == UML_EMULATOR_OK &&
		capabilities.abi_version == UML_EMULATOR_ABI_VERSION &&
		capabilities.can_run && capabilities.kernel_is_native_macho &&
		capabilities.sandbox_safe && capabilities.platform != nullptr &&
		std::strcmp(capabilities.platform, UML_EXPECTED_PLATFORM) == 0 ? 0 : 1;
}
