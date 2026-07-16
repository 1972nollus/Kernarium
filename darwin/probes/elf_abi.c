/* SPDX-License-Identifier: GPL-2.0 */
/* Built as ELF against Darwin headers, then run by the Mach-O payload loader. */
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv, char **envp)
{
	static const char message[] = "AArch64 ELF called Darwin write()\n";

	(void)argc;
	(void)argv;
	(void)envp;
	if (write(STDOUT_FILENO, message, strlen(message)) != sizeof(message) - 1)
		return 1;
	return 37;
}
