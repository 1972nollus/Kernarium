// SPDX-License-Identifier: GPL-2.0-or-later
/* Wine ARM64EC front-end for the in-emulator x86 gadget CPU service. */

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winnt.h>
#include <winternl.h>

#include "wine_cpu_protocol.h"

#define UML_ARM64EC_STATE_PAGE_SIZE 4096U
#define UML_ARM64EC_X86_RETURN_BYTES 1U

struct uml_arm64ec_thread_state
{
    struct uml_wine_cpu_request request;
    uint64_t original_emulator_stack_limit;
};

struct uml_arm64ec_exception_stack
{
    ARM64_NT_CONTEXT context;
    uint64_t sve_padding[4];
    EXCEPTION_RECORD record;
    uint64_t alignment;
    uint64_t redzone[2];
};

_Static_assert( offsetof( struct uml_arm64ec_thread_state, request ) == 0,
                "request must lead the per-thread state" );
_Static_assert( sizeof(struct uml_arm64ec_thread_state) <=
                UML_ARM64EC_STATE_PAGE_SIZE,
                "the bridge state must fit its reserved stack page" );
_Static_assert( sizeof(struct uml_wine_cpu_request) == 960,
                "host and guest protocol layouts differ" );

extern void uml_arm64ec_hostcall( struct uml_wine_cpu_request *request );
extern void uml_arm64ec_enter_loaded( struct uml_arm64ec_thread_state *state )
    __attribute__((noreturn));
extern void uml_arm64ec_jump_stack( void *target, uintptr_t stack )
    __attribute__((noreturn));

void *uml_arm64ec_x86_return;

static void *native_exception_dispatcher;
static uint64_t service_capabilities;

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        LdrDisableThreadCalloutsForDll( instance );
    return TRUE;
}

static void zero_bytes( void *destination, size_t size )
{
    volatile unsigned char *bytes = destination;
    while (size--) *bytes++ = 0;
}

static void copy_bytes( void *destination, const void *source, size_t size )
{
    unsigned char *output = destination;
    const unsigned char *input = source;
    while (size--) *output++ = *input++;
}

static CHPE_V2_CPU_AREA_INFO *cpu_area(void)
{
    return NtCurrentTeb()->ChpeV2CpuAreaInfo;
}

static struct uml_arm64ec_thread_state *thread_state(void)
{
    CHPE_V2_CPU_AREA_INFO *area = cpu_area();
    return area ? area->EmulatorData[0] : NULL;
}

static void initialise_request( struct uml_wine_cpu_request *request )
{
    zero_bytes( request, sizeof(*request) );
    request->magic = UML_WINE_CPU_PROTOCOL_MAGIC;
    request->version = UML_WINE_CPU_PROTOCOL_VERSION;
    request->size = sizeof(*request);
    request->capabilities = service_capabilities;
    request->context.rflags = 0x202;
    request->context.mxcsr = 0x1f80;
    request->context.x87_control = 0x027f;
    request->context.x87_tag = 0xffff;
    request->context.segment_selectors[0] = 0x2b;
    request->context.segment_selectors[1] = 0x33;
    request->context.segment_selectors[2] = 0x2b;
    request->context.segment_selectors[3] = 0x2b;
    request->context.segment_selectors[4] = 0x53;
    request->context.segment_selectors[5] = 0x2b;
}

static void initialise_arm64ec_context( ARM64EC_NT_CONTEXT *context )
{
    zero_bytes( context, sizeof(*context) );
    context->ContextFlags = CONTEXT_CONTROL | CONTEXT_SEGMENTS |
                            CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;
    context->AMD64_SegCs = 0x33;
    context->AMD64_SegDs = 0x2b;
    context->AMD64_SegEs = 0x2b;
    context->AMD64_SegFs = 0x53;
    context->AMD64_SegGs = 0x2b;
    context->AMD64_SegSs = 0x2b;
    context->AMD64_EFlags = 0x202;
    context->AMD64_MxCsr = 0x1f80;
    context->AMD64_MxCsr_copy = 0x1f80;
    context->AMD64_ControlWord = 0x027f;
}

static void refresh_bitmap( struct uml_wine_cpu_request *request )
{
    PEB *peb = NtCurrentTeb()->Peb;
    request->ec_bitmap = (uintptr_t)peb->EcCodeBitMap;
    if (request->ec_bitmap) request->flags |= UML_WINE_CPU_FLAG_EC_BITMAP_VALID;
    else request->flags &= ~UML_WINE_CPU_FLAG_EC_BITMAP_VALID;
}

static uint16_t full_x87_tag( uint8_t abridged )
{
    uint16_t result = 0;
    unsigned int index;
    for (index = 0; index < 8; ++index)
        result |= (abridged & (1U << index) ? 0U : 3U) << (index * 2U);
    return result;
}

static void load_context( struct uml_wine_x86_context *target,
                          const ARM64EC_NT_CONTEXT *source )
{
    static const size_t source_gpr_offsets[16] =
    {
        offsetof( ARM64EC_NT_CONTEXT, X8 ),
        offsetof( ARM64EC_NT_CONTEXT, X0 ),
        offsetof( ARM64EC_NT_CONTEXT, X1 ),
        offsetof( ARM64EC_NT_CONTEXT, X27 ),
        offsetof( ARM64EC_NT_CONTEXT, Sp ),
        offsetof( ARM64EC_NT_CONTEXT, Fp ),
        offsetof( ARM64EC_NT_CONTEXT, X25 ),
        offsetof( ARM64EC_NT_CONTEXT, X26 ),
        offsetof( ARM64EC_NT_CONTEXT, X2 ),
        offsetof( ARM64EC_NT_CONTEXT, X3 ),
        offsetof( ARM64EC_NT_CONTEXT, X4 ),
        offsetof( ARM64EC_NT_CONTEXT, X5 ),
        offsetof( ARM64EC_NT_CONTEXT, X19 ),
        offsetof( ARM64EC_NT_CONTEXT, X20 ),
        offsetof( ARM64EC_NT_CONTEXT, X21 ),
        offsetof( ARM64EC_NT_CONTEXT, X22 ),
    };
    const unsigned char *bytes = (const unsigned char *)source;
    const uint16_t x16_words[4] =
    {
        source->X16_0, source->X16_1, source->X16_2, source->X16_3,
    };
    const uint16_t x17_words[4] =
    {
        source->X17_0, source->X17_1, source->X17_2, source->X17_3,
    };
    unsigned int index;

    zero_bytes( target, sizeof(*target) );
    for (index = 0; index < 16; ++index)
        copy_bytes( &target->gpr[index], bytes + source_gpr_offsets[index],
                    sizeof(target->gpr[index]) );
    target->rip = source->Pc;
    target->rflags = source->AMD64_EFlags | 2U;
    target->gs_base = (uintptr_t)NtCurrentTeb();
    for (index = 0; index < 16; ++index)
        copy_bytes( target->vectors[index], &source->V[index], 16 );

    target->x87_significand[0] = target->mmx[0] = source->Lr;
    target->x87_significand[1] = target->mmx[1] = source->X6;
    target->x87_significand[2] = target->mmx[2] = source->X7;
    target->x87_significand[3] = target->mmx[3] = source->X9;
    target->x87_significand[4] = target->mmx[4] = source->X10;
    target->x87_significand[5] = target->mmx[5] = source->X11;
    target->x87_significand[6] = target->mmx[6] = source->X12;
    target->x87_significand[7] = target->mmx[7] = source->X15;
    for (index = 0; index < 4; ++index)
    {
        target->x87_sign_exponent[index] = x16_words[index];
        target->x87_sign_exponent[index + 4] = x17_words[index];
    }
    target->segment_selectors[0] = source->AMD64_SegEs;
    target->segment_selectors[1] = source->AMD64_SegCs;
    target->segment_selectors[2] = source->AMD64_SegSs;
    target->segment_selectors[3] = source->AMD64_SegDs;
    target->segment_selectors[4] = source->AMD64_SegFs;
    target->segment_selectors[5] = source->AMD64_SegGs;
    target->mxcsr = source->AMD64_MxCsr;
    target->x87_control = source->AMD64_ControlWord;
    target->x87_status = source->AMD64_StatusWord;
    target->x87_tag = full_x87_tag( source->AMD64_TagWord );
    target->x87_top = (source->AMD64_StatusWord >> 11) & 7U;
}

static uint64_t mxcsr_to_fpcsr( uint32_t mxcsr )
{
    uint32_t fpcr = 0, fpsr = 0;
    if (mxcsr & 0x0001) fpsr |= 0x0001;
    if (mxcsr & 0x0002) fpsr |= 0x0080;
    if (mxcsr & 0x0004) fpsr |= 0x0002;
    if (mxcsr & 0x0008) fpsr |= 0x0004;
    if (mxcsr & 0x0010) fpsr |= 0x0008;
    if (mxcsr & 0x0020) fpsr |= 0x0010;
    if (mxcsr & 0x0040) fpcr |= 0x00080000;
    if (!(mxcsr & 0x0080)) fpcr |= 0x00000100;
    if (!(mxcsr & 0x0100)) fpcr |= 0x00008000;
    if (!(mxcsr & 0x0200)) fpcr |= 0x00000200;
    if (!(mxcsr & 0x0400)) fpcr |= 0x00000400;
    if (!(mxcsr & 0x0800)) fpcr |= 0x00000800;
    if (!(mxcsr & 0x1000)) fpcr |= 0x00001000;
    if (mxcsr & 0x2000) fpcr |= 0x00800000;
    if (mxcsr & 0x4000) fpcr |= 0x00400000;
    if (mxcsr & 0x8000) fpcr |= 0x01000000;
    return fpcr | ((uint64_t)fpsr << 32);
}

static void store_arm_context( ARM64_NT_CONTEXT *target,
                               const struct uml_wine_x86_context *source )
{
    uint64_t fpcsr;
    unsigned int index;

    zero_bytes( target, sizeof(*target) );
    target->ContextFlags = CONTEXT_ARM64_FULL;
    target->X8 = source->gpr[0];
    target->X0 = source->gpr[1];
    target->X1 = source->gpr[2];
    target->X27 = source->gpr[3];
    target->Sp = source->gpr[4];
    target->Fp = source->gpr[5];
    target->X25 = source->gpr[6];
    target->X26 = source->gpr[7];
    target->X2 = source->gpr[8];
    target->X3 = source->gpr[9];
    target->X4 = source->gpr[10];
    target->X5 = source->gpr[11];
    target->X19 = source->gpr[12];
    target->X20 = source->gpr[13];
    target->X21 = source->gpr[14];
    target->X22 = source->gpr[15];
    target->Pc = source->rip;
    target->Lr = source->x87_significand[0];
    target->X6 = source->x87_significand[1];
    target->X7 = source->x87_significand[2];
    target->X9 = source->x87_significand[3];
    target->X10 = source->x87_significand[4];
    target->X11 = source->x87_significand[5];
    target->X12 = source->x87_significand[6];
    target->X15 = source->x87_significand[7];
    for (index = 0; index < 4; ++index)
    {
        target->X16 |= (uint64_t)source->x87_sign_exponent[index] <<
                       (index * 16U);
        target->X17 |= (uint64_t)source->x87_sign_exponent[index + 4] <<
                       (index * 16U);
    }
    for (index = 0; index < 16; ++index)
        copy_bytes( &target->V[index], source->vectors[index], 16 );
    if (source->rflags & 0x0001) target->Cpsr |= 0x20000000;
    if (source->rflags & 0x0040) target->Cpsr |= 0x40000000;
    if (source->rflags & 0x0080) target->Cpsr |= 0x80000000;
    if (source->rflags & 0x0100) target->Cpsr |= 0x00200000;
    if (source->rflags & 0x0800) target->Cpsr |= 0x10000000;
    fpcsr = mxcsr_to_fpcsr( source->mxcsr );
    target->Fpcr = (uint32_t)fpcsr;
    target->Fpsr = fpcsr >> 32;
}

static void *resolve_native_ntdll_export( const char *name )
{
    LDR_DATA_TABLE_ENTRY *entry;
    IMAGE_LOAD_CONFIG_DIRECTORY *load_config;
    IMAGE_ARM64EC_METADATA *metadata;
    IMAGE_ARM64EC_REDIRECTION_ENTRY *map;
    void *function;
    ULONG size;
    uintptr_t base, rva;
    int minimum, maximum;

    if (LdrFindEntryForAddress( (const void *)NtClose, &entry )) return NULL;
    base = (uintptr_t)entry->DllBase;
    load_config = RtlImageDirectoryEntryToData( entry->DllBase, TRUE,
                                                IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG,
                                                &size );
    if (!load_config || size <= offsetof( IMAGE_LOAD_CONFIG_DIRECTORY,
                                          CHPEMetadataPointer )) return NULL;
    metadata = (IMAGE_ARM64EC_METADATA *)(uintptr_t)
        load_config->CHPEMetadataPointer;
    function = RtlFindExportedRoutineByName( entry->DllBase, name );
    if (!metadata || !function || !metadata->RedirectionMetadataCount)
        return NULL;
    rva = (uintptr_t)function - base;
    map = (IMAGE_ARM64EC_REDIRECTION_ENTRY *)(base +
                                               metadata->RedirectionMetadata);
    minimum = 0;
    maximum = (int)metadata->RedirectionMetadataCount - 1;
    while (minimum <= maximum)
    {
        int position = minimum + (maximum - minimum) / 2;
        if (map[position].Source == rva)
            return (void *)(base + map[position].Destination);
        if (map[position].Source < rva) minimum = position + 1;
        else maximum = position - 1;
    }
    return NULL;
}

static NTSTATUS query_service(void)
{
    struct uml_wine_cpu_request request;
    initialise_request( &request );
    request.operation = UML_WINE_CPU_OPERATION_QUERY;
    request.status = UML_WINE_CPU_STATUS_PENDING;
    uml_arm64ec_hostcall( &request );
    if (request.status != UML_WINE_CPU_STATUS_OK ||
        !(request.capabilities & UML_WINE_CPU_CAP_X86_64) ||
        !(request.capabilities & UML_WINE_CPU_CAP_ARM64EC_BOUNDARY) ||
        !(request.capabilities & UML_WINE_CPU_CAP_STRICT_TSO))
        return STATUS_NOT_SUPPORTED;
    service_capabilities = request.capabilities;
    return STATUS_SUCCESS;
}

static NTSTATUS allocate_x86_return(void)
{
    void *address = NULL;
    void *protect_address;
    SIZE_T size = UML_ARM64EC_STATE_PAGE_SIZE;
    SIZE_T protect_size;
    ULONG old_protection;
    NTSTATUS status;

    status = NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0, &size,
                                      MEM_COMMIT | MEM_RESERVE,
                                      PAGE_READWRITE );
    if (status) return status;
    *(unsigned char *)address = 0xc3;
    protect_address = address;
    protect_size = size;
    status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_address,
                                     &protect_size, PAGE_EXECUTE_READ,
                                     &old_protection );
    if (status)
    {
        size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &address, &size, MEM_RELEASE );
        return status;
    }
    uml_arm64ec_x86_return = address;
    return STATUS_SUCCESS;
}

static void invalidate_code( const void *address, SIZE_T size )
{
    struct uml_wine_cpu_request request;
    if (!service_capabilities || !size) return;
    initialise_request( &request );
    request.operation = UML_WINE_CPU_OPERATION_INVALIDATE;
    request.invalidate_address = (uintptr_t)address;
    request.invalidate_size = size;
    request.status = UML_WINE_CPU_STATUS_PENDING;
    uml_arm64ec_hostcall( &request );
}

NTSTATUS WINAPI ProcessInit(void)
{
    NTSTATUS status = query_service();
    if (status) return status;
    native_exception_dispatcher =
        resolve_native_ntdll_export( "KiUserExceptionDispatcher" );
    if (!native_exception_dispatcher)
    {
        service_capabilities = 0;
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    status = allocate_x86_return();
    if (status) service_capabilities = 0;
    return status;
}

void WINAPI ProcessTerm( HANDLE process, BOOL after, NTSTATUS status )
{
    /* NtTerminateProcess(NULL) leaves the calling thread alive to run DLL
     * shutdown callbacks. Keep the bridge and return thunk usable until the
     * process address space is destroyed. */
    (void)process;
    (void)after;
    (void)status;
}

NTSTATUS WINAPI ThreadInit(void)
{
    CHPE_V2_CPU_AREA_INFO *area = cpu_area();
    struct uml_arm64ec_thread_state *state;
    uintptr_t limit, area_end, context_end;

    if (!area || !area->ContextAmd64 || !service_capabilities)
        return STATUS_INVALID_PARAMETER;
    if (area->EmulatorData[0]) return STATUS_SUCCESS;
    limit = area->EmulatorStackLimit;
    /* Wine may place the CPU area and its inline context at StackLimit.
     * Reserve bridge state after either structure that lies in this stack. */
    area_end = (uintptr_t)area + sizeof(*area);
    context_end = (uintptr_t)area->ContextAmd64 + sizeof(*area->ContextAmd64) +
                  sizeof(CONTEXT_EX);
    if ((uintptr_t)area < area->EmulatorStackBase && area_end > limit)
        limit = area_end;
    if ((uintptr_t)area->ContextAmd64 < area->EmulatorStackBase &&
        context_end > limit)
        limit = context_end;
    limit = (limit + 15U) & ~(uintptr_t)15U;
    if (limit > area->EmulatorStackBase ||
        UML_ARM64EC_STATE_PAGE_SIZE > area->EmulatorStackBase - limit)
        return STATUS_NO_MEMORY;
    state = (struct uml_arm64ec_thread_state *)limit;
    zero_bytes( state, sizeof(*state) );
    state->original_emulator_stack_limit = area->EmulatorStackLimit;
    area->EmulatorStackLimit = limit + UML_ARM64EC_STATE_PAGE_SIZE;
    initialise_request( &state->request );
    initialise_arm64ec_context( area->ContextAmd64 );
    refresh_bitmap( &state->request );
    load_context( &state->request.context, area->ContextAmd64 );
    area->EmulatorData[0] = state;
    area->EmulatorData[2] = uml_arm64ec_enter_loaded;
    area->EmulatorData[3] = uml_arm64ec_enter_loaded;
    return STATUS_SUCCESS;
}

void WINAPI ThreadTerm( HANDLE thread, LONG exit_code )
{
    CHPE_V2_CPU_AREA_INFO *area = cpu_area();
    struct uml_arm64ec_thread_state *state;
    (void)thread;
    (void)exit_code;
    if (!area || !(state = area->EmulatorData[0])) return;
    area->EmulatorStackLimit = state->original_emulator_stack_limit;
    area->EmulatorData[0] = NULL;
    area->EmulatorData[2] = NULL;
    area->EmulatorData[3] = NULL;
}

void WINAPI BeginSimulation(void)
{
    CHPE_V2_CPU_AREA_INFO *area = cpu_area();
    struct uml_arm64ec_thread_state *state = thread_state();
    if (!area || !state || !area->ContextAmd64) __builtin_trap();
    load_context( &state->request.context, area->ContextAmd64 );
    refresh_bitmap( &state->request );
    area->InSimulation = 1;
    uml_arm64ec_enter_loaded( state );
}

void WINAPI BTCpu64FlushInstructionCache( const void *address, SIZE_T size )
{
    invalidate_code( address, size );
}

void WINAPI BTCpu64NotifyMemoryDirty( void *address, SIZE_T size )
{
    invalidate_code( address, size );
}

void WINAPI BTCpu64NotifyReadFile( HANDLE handle, void *address, SIZE_T size,
                                   BOOL after, NTSTATUS status )
{
    (void)handle;
    if (after && !status) invalidate_code( address, size );
}

void WINAPI FlushInstructionCacheHeavy( const void *address, SIZE_T size )
{
    invalidate_code( address, size );
}

void WINAPI NotifyMemoryAlloc( void *address, SIZE_T size, ULONG type,
                               ULONG protection, BOOL after, NTSTATUS status )
{
    (void)type;
    (void)protection;
    if (after && !status) invalidate_code( address, size );
}

void WINAPI NotifyMemoryFree( void *address, SIZE_T size, ULONG type,
                              BOOL after, NTSTATUS status )
{
    (void)type;
    if (!after && !status) invalidate_code( address, size );
}

void WINAPI NotifyMemoryProtect( void *address, SIZE_T size, ULONG protection,
                                 BOOL after, NTSTATUS status )
{
    (void)protection;
    if (after && !status) invalidate_code( address, size );
}

NTSTATUS WINAPI NotifyMapViewOfSection( void *unknown1, void *address,
                                        void *unknown2, SIZE_T size,
                                        ULONG allocation_type,
                                        ULONG protection )
{
    (void)unknown1;
    (void)unknown2;
    (void)allocation_type;
    (void)protection;
    invalidate_code( address, size );
    return STATUS_SUCCESS;
}

void WINAPI NotifyUnmapViewOfSection( void *address, BOOL after,
                                      NTSTATUS status )
{
    if (!after && !status) invalidate_code( address, UML_ARM64EC_STATE_PAGE_SIZE );
}

void WINAPI ResetToConsistentState( EXCEPTION_RECORD *record, CONTEXT *guest,
                                    ARM64_NT_CONTEXT *native )
{
    struct uml_arm64ec_thread_state *state = thread_state();
    (void)record;
    if (!state || !guest) return;
    load_context( &state->request.context,
                  (const ARM64EC_NT_CONTEXT *)guest );
    if (native)
    {
        store_arm_context( native, &state->request.context );
        native->Pc = (uintptr_t)BeginSimulation;
        native->Sp = cpu_area()->EmulatorStackBase;
    }
}

BOOLEAN WINAPI BTCpu64IsProcessorFeaturePresent( UINT feature )
{
    switch (feature)
    {
    case PF_COMPARE_EXCHANGE_DOUBLE:
    case PF_MMX_INSTRUCTIONS_AVAILABLE:
    case PF_RDTSC_INSTRUCTION_AVAILABLE:
    case PF_XMMI_INSTRUCTIONS_AVAILABLE:
    case PF_XMMI64_INSTRUCTIONS_AVAILABLE:
    case PF_NX_ENABLED:
    case PF_SSE_DAZ_MODE_AVAILABLE:
    case PF_SSE3_INSTRUCTIONS_AVAILABLE:
    case PF_COMPARE_EXCHANGE128:
    case PF_COMPARE64_EXCHANGE128:
    case PF_RDWRFSGSBASE_AVAILABLE:
    case PF_XSAVE_ENABLED:
    case PF_FASTFAIL_AVAILABLE:
    case PF_RDRAND_INSTRUCTION_AVAILABLE:
    case PF_RDTSCP_INSTRUCTION_AVAILABLE:
    case PF_SSSE3_INSTRUCTIONS_AVAILABLE:
    case PF_SSE4_1_INSTRUCTIONS_AVAILABLE:
    case PF_SSE4_2_INSTRUCTIONS_AVAILABLE:
    case PF_AVX_INSTRUCTIONS_AVAILABLE:
    case PF_AVX2_INSTRUCTIONS_AVAILABLE:
    case PF_BMI2_INSTRUCTIONS_AVAILABLE:
        return TRUE;
    default:
        return FALSE;
    }
}

void WINAPI UpdateProcessorInformation( SYSTEM_CPU_INFORMATION *information )
{
    if (!information) return;
    information->ProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
    information->ProcessorLevel = 6;
    information->ProcessorRevision = 0x03c3;
    information->ProcessorFeatureBits |= CPU_FEATURE_TSC | CPU_FEATURE_CMOV |
        CPU_FEATURE_CX8 | CPU_FEATURE_MMX | CPU_FEATURE_X86 |
        CPU_FEATURE_FXSR | CPU_FEATURE_SSE | CPU_FEATURE_SSE2;
}

void uml_arm64ec_raise_stop( struct uml_arm64ec_thread_state *state )
{
    struct uml_arm64ec_exception_stack *stack;
    struct uml_wine_cpu_request *request = &state->request;
    uintptr_t guest_stack = request->context.gpr[4] & ~(uintptr_t)63U;
    ULONG_PTR access;

    /* Windows syscall thunks may select INT 2e instead of SYSCALL. The
     * Linux-oriented CPU service leaves this software interrupt to us;
     * dispatch it through Wine with the following instruction as the return
     * address, preserving RCX and R11 as INT does. */
    if (request->stop_reason == UML_WINE_CPU_STOP_ILLEGAL_INSTRUCTION)
    {
        unsigned char opcode[2];
        SIZE_T read = 0;
        if (!NtReadVirtualMemory( NtCurrentProcess(),
                                  (void *)(uintptr_t)request->context.rip,
                                  opcode, sizeof(opcode), &read ) &&
            read == sizeof(opcode) && opcode[0] == 0xcd && opcode[1] == 0x2e)
        {
            request->context.rip += sizeof(opcode);
            request->stop_reason = UML_WINE_CPU_STOP_SYSTEM_CALL;
        }
    }

    stack = (struct uml_arm64ec_exception_stack *)guest_stack - 1;
    zero_bytes( stack, sizeof(*stack) );
    store_arm_context( &stack->context, &request->context );
    stack->record.ExceptionAddress = (void *)(uintptr_t)request->context.rip;
    switch (request->stop_reason)
    {
    case UML_WINE_CPU_STOP_SYSTEM_CALL:
        stack->record.ExceptionCode = STATUS_EMULATION_SYSCALL;
        break;
    case UML_WINE_CPU_STOP_MEMORY_FAULT:
        stack->record.ExceptionCode = STATUS_ACCESS_VIOLATION;
        stack->record.NumberParameters = 2;
        access = request->fault_access == UML_WINE_CPU_ACCESS_WRITE ? 1 :
                 request->fault_access == UML_WINE_CPU_ACCESS_EXECUTE ? 8 : 0;
        stack->record.ExceptionInformation[0] = access;
        stack->record.ExceptionInformation[1] = request->fault_address;
        break;
    case UML_WINE_CPU_STOP_BREAKPOINT:
        stack->record.ExceptionCode = STATUS_BREAKPOINT;
        if (stack->context.Pc) --stack->context.Pc;
        stack->record.ExceptionAddress = (void *)(uintptr_t)stack->context.Pc;
        break;
    case UML_WINE_CPU_STOP_FLOATING_POINT:
        stack->record.ExceptionCode = STATUS_FLOAT_INVALID_OPERATION;
        break;
    case UML_WINE_CPU_STOP_ILLEGAL_INSTRUCTION:
    default:
        stack->record.ExceptionCode = STATUS_ILLEGAL_INSTRUCTION;
        break;
    }
    cpu_area()->InSimulation = 0;
    uml_arm64ec_jump_stack( native_exception_dispatcher, (uintptr_t)stack );
}
