/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <uml/emulator.h>

#include <string.h>

/* Packages are built without the embedded-kernel runner, so the consumer
 * checks the package's API on a handle without a kernel: it reports that it
 * cannot run and refuses a run. */
int main(void)
{
	struct uml_emulator *emulator = 0;
	const struct uml_emulator_config configuration = {
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(struct uml_emulator_config),
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
		run_status == -UML_EMULATOR_UNSUPPORTED &&
		capabilities.abi_version == UML_EMULATOR_ABI_VERSION &&
		!capabilities.can_run && capabilities.sandbox_safe &&
		capabilities.platform != 0 &&
		strcmp(capabilities.platform, UML_EXPECTED_PLATFORM) == 0 ? 0 : 1;
}
