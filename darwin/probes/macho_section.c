/* SPDX-License-Identifier: GPL-2.0 */
/* Verify how the Darwin assembler treats Linux-style ELF section names. */
__attribute__((section(".init.text"))) int uml_init_section_probe(void)
{
	return 0;
}
