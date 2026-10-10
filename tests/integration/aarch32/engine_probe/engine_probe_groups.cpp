// SPDX-License-Identifier: GPL-2.0-or-later
/* The groups of the AArch32 engine probe, in the order they run, with the
 * least level of the profile each needs. */
#include "engine_probe.hpp"

namespace probe {

const Group groups[] = {
	{"basic", 0, group_basic},
	{"interwork", 0, group_interwork},
	{"signals", 0, group_signals},
	{"kuser", 0, group_kuser},
	{"tls", 0, group_tls},
	{"cacheflush", 0, group_cacheflush},
	{"faults", 0, group_faults},
	{"ill", 0, group_ill},
	{"ptrace", 0, group_ptrace},
	{"atomics", 0, group_atomics},
	{"absent", 0, group_absent},
	{"simd", 1, group_simd},
	{"complete", 2, group_complete},
};
const unsigned group_count = sizeof(groups) / sizeof(groups[0]);

} // namespace probe
