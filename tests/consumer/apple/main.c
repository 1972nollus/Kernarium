/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <uml/emulator.h>

#include <string.h>

static int native_kernel(int argc, char *const argv[], char *const envp[])
{
	(void)argc;
	(void)argv;
	(void)envp;
	return 0;
}

int main(void)
{
	struct uml_emulator *emulator = 0;
	const struct uml_emulator_config configuration = {
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(struct uml_emulator_config),
		.native_kernel_entry = native_kernel,
	};
	if (uml_emulator_create(&configuration, &emulator) != UML_EMULATOR_OK)
		return 1;
	struct uml_emulator_capabilities capabilities = {
		.struct_size = sizeof(struct uml_emulator_capabilities),
	};
	const int status = uml_emulator_get_capabilities(
		emulator, &capabilities);
	const char *const arguments[] = {"uml-package-consumer", "--help"};
	const int run_status = uml_emulator_run(emulator, 2, arguments, 0);
	uml_emulator_request_stop(emulator);
	uml_emulator_destroy(emulator);
	return status == UML_EMULATOR_OK &&
		run_status == UML_EMULATOR_OK &&
		capabilities.abi_version == UML_EMULATOR_ABI_VERSION &&
		capabilities.can_run && capabilities.kernel_is_native_macho &&
		capabilities.sandbox_safe && capabilities.platform != 0 &&
		strcmp(capabilities.platform, UML_EXPECTED_PLATFORM) == 0 ? 0 : 1;
}
