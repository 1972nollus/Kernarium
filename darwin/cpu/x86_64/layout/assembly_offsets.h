/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* Assembly-visible x86-64 CPU state. Keep synchronized with cpu_state.hpp;
 * assembly_layout.hpp verifies every offset, and every event code a gadget
 * returns against EngineEvent, from C++.
 */
#define UML_X86_64_CPU_REGISTERS 0
#define UML_X86_64_CPU_VECTORS 128
#define UML_X86_64_CPU_MMX 640
#define UML_X86_64_CPU_X87 704
#define UML_X86_64_CPU_SEGMENT_SELECTORS 832
#define UML_X86_64_CPU_COMPAT_TLS_BASES 844
#define UML_X86_64_CPU_COMPAT_TLS_PRESENT 864
#define UML_X86_64_CPU_COMPAT_TLS_FIRST 865
#define UML_X86_64_CPU_RIP 872
#define UML_X86_64_CPU_RFLAGS 880
#define UML_X86_64_CPU_SEGMENT_ZERO 888
#define UML_X86_64_CPU_FS_BASE 896
#define UML_X86_64_CPU_GS_BASE 904
#define UML_X86_64_CPU_MXCSR 912
#define UML_X86_64_CPU_X87_CONTROL 916
#define UML_X86_64_CPU_X87_STATUS 918
#define UML_X86_64_CPU_X87_TAG 920
#define UML_X86_64_CPU_X87_TOP 922
#define UML_X86_64_CPU_SYSTEM_CALL_32BIT 924
#define UML_X86_64_CPU_EXECUTION_MODE 925
#define UML_X86_64_CPU_TIMESTAMP_FAULTS 926
#define UML_X86_64_CPU_DEFERRED_LEFT 928
#define UML_X86_64_CPU_DEFERRED_RIGHT 936
#define UML_X86_64_CPU_DEFERRED_RESULT 944
#define UML_X86_64_CPU_DEFERRED_OPERATION 952
#define UML_X86_64_CPU_DEFERRED_WIDTH 953
#define UML_X86_64_CPU_DEFERRED_CARRY 954
#define UML_X86_64_CPU_DEFERRED_PENDING 955
#define UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK 956
#define UML_X86_64_CPU_DEFERRED_OVERRIDE_VALUE 958
#define UML_X86_64_CPU_CARRY_SLOT 960

#define UML_X86_64_TLB_READ_PAGE 0
#define UML_X86_64_TLB_READ_DATA_MINUS_ADDRESS 8
#define UML_X86_64_TLB_WRITE_PAGE 16
#define UML_X86_64_TLB_WRITE_DATA_MINUS_ADDRESS 24

#define UML_X86_64_TLB_WRITE_CODE 2
#define UML_X86_64_TLB_WRITE_CACHED_CODE 4
#define UML_X86_64_WRITE_CODE_PENDING 1
#define UML_X86_64_WRITE_CODE_PUBLISHED 2
/* No page: guest pages are 4 KiB aligned, so this is none of them. */
#define UML_X86_64_CODE_PAGE_NONE 1

/* Stream-descriptor bit: the effective address is the sum of base, scaled
 * index and displacement with nothing after it -- long-mode arithmetic and no
 * segment base to add.
 */
#define UML_X86_64_MEMORY_PLAIN_ADDRESS 54

#define UML_X86_64_CONTEXT_MEMORY 0
#define UML_X86_64_CONTEXT_TLB 8
#define UML_X86_64_CONTEXT_PROGRAM_BEGIN 16
#define UML_X86_64_CONTEXT_PROGRAM_END 24
#define UML_X86_64_CONTEXT_FAULT_ADDRESS 32
#define UML_X86_64_CONTEXT_WRITE_CODE 48
#define UML_X86_64_CONTEXT_UNALIGNED_EXCLUSIVES 49
#define UML_X86_64_CONTEXT_SPLIT_LOCK 50
#define UML_X86_64_CONTEXT_INSTRUCTION_LIMIT 56
#define UML_X86_64_CONTEXT_INDIRECT_MISS 64
#define UML_X86_64_CONTEXT_TARGET_LINKS 72
#define UML_X86_64_CONTEXT_STACK_ENTRY 128
#define UML_X86_64_CONTEXT_CODE_PAGE 136
#define UML_X86_64_TARGET_LINK_FOLD 12
#define UML_X86_64_TARGET_LINK_MASK 4095

#define UML_X86_64_EVENT_CONTINUED 0
#define UML_X86_64_EVENT_SYSCALL 1
#define UML_X86_64_EVENT_MEMORY_FAULT 2
#define UML_X86_64_EVENT_BREAKPOINT 5
#define UML_X86_64_EVENT_FLOATING_POINT_TRAP 7
#define UML_X86_64_EVENT_OVERFLOW 9
#define UML_X86_64_EVENT_GENERAL_PROTECTION 10
#define UML_X86_64_EVENT_TRAP_FLAG 11
#define UML_X86_64_EVENT_DEBUG_TRAP 12
#define UML_X86_64_EVENT_BOUND_RANGE 13
/* Not an EngineEvent: a repeated string instruction spent its repeat budget
 * and stopped between elements, with RIP still naming it. `execute` reports
 * it as continued and sets ExecuteResult::repeat_yielded. */
#define UML_X86_64_EVENT_REPEAT_YIELD 64

/* The work a repeated string instruction does before it stops between
 * elements: elements in its element loop, 64-byte blocks in a page-bounded
 * run, and one for each run. */
#define UML_X86_64_STRING_REPEAT_BUDGET 4096
