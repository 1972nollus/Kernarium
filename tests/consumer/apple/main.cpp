/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <uml/emulator.h>

#include <cstdint>

namespace {

static_assert(DARWIN_UML_ABI_VERSION == 3U);
static_assert(DARWIN_UML_ABI_FINGERPRINT == 0x4333b73bf8b2ea99ULL);

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
	uml_emulator_request_stop(emulator);
	uml_emulator_destroy(emulator);
	return status == UML_EMULATOR_OK &&
		capabilities.abi_version == UML_EMULATOR_ABI_VERSION ? 0 : 1;
}
