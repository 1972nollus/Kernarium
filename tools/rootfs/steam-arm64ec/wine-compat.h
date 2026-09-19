// SPDX-License-Identifier: LGPL-2.1-or-later
/* Wine 11 removed these private names from winternl.h. The x86
 * SYSTEM_CPU_INFORMATION flag values used by the shared CPU bridges remain
 * unchanged. Keep this source compatibility adapter local to the modules. */
#ifndef UML_PROTON_WINE_COMPAT_H
#define UML_PROTON_WINE_COMPAT_H

#ifndef CPU_FEATURE_TSC
#define CPU_FEATURE_TSC  0x00000002
#define CPU_FEATURE_CMOV 0x00000008
#define CPU_FEATURE_CX8  0x00000080
#define CPU_FEATURE_MMX  0x00000100
#define CPU_FEATURE_X86  0x00000200
#define CPU_FEATURE_FXSR 0x00000800
#define CPU_FEATURE_SSE  0x00002000
#define CPU_FEATURE_SSE2 0x00010000
#endif

#endif
