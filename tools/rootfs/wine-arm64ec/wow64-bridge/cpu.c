// SPDX-License-Identifier: GPL-2.0-or-later
/* Wine i386 WoW64 front-end for the in-emulator x86 gadget CPU service. */

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winnt.h>
#include <winternl.h>
#include <rtlsupportapi.h>

#include "wine_cpu_protocol.h"

#define UML_WOW64_STUB_PAGE_SIZE 4096U
#define UML_WOW64_UNIX_STUB_OFFSET 16U
#define UML_WOW64_WINDOWS_TLS_INDEX 4U
#define UML_WOW64_TLS_IN_CPU_SIMULATION 2U

typedef NTSTATUS (WINAPI *unix_call_dispatcher_t)( uint64_t handle,
                                                   unsigned int code,
                                                   void *arguments );

extern void uml_wow64_hostcall( struct uml_wine_cpu_request *request );
extern NTSTATUS WINAPI Wow64SystemServiceEx( UINT number, UINT *arguments );

static BYTE DECLSPEC_ALIGN(UML_WOW64_STUB_PAGE_SIZE)
    code_buffer[UML_WOW64_STUB_PAGE_SIZE];
static uint64_t service_capabilities;
static unix_call_dispatcher_t unix_call_dispatcher;

_Static_assert( sizeof(struct uml_wine_cpu_request) == 960,
                "host and guest protocol layouts differ" );

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

static TEB32 *current_teb32(void)
{
    TEB *teb = NtCurrentTeb();
    return (TEB32 *)((char *)teb + teb->WowTebOffset);
}

static WOW64_CPURESERVED *cpu_reserved(void)
{
    return NtCurrentTeb()->TlsSlots[WOW64_TLS_CPURESERVED];
}

static I386_CONTEXT *current_context(void)
{
    void *context = NULL;
    NTSTATUS status = RtlWow64GetCurrentCpuArea( NULL, &context, NULL );
    if (status) RtlRaiseStatus( status );
    return context;
}

static uint16_t full_x87_tag( const XSAVE_FORMAT *save )
{
    unsigned int index;
    unsigned int stack_top = (save->StatusWord >> 11) & 7;
    uint16_t result = 0;

    for (index = 0; index < 8; ++index)
    {
        unsigned int tag;
        const M128A *reg;
        if (!(save->TagWord & (1U << index))) tag = 3;
        else
        {
            reg = &save->FloatRegisters[(index - stack_top) & 7];
            if ((reg->High & 0x7fff) == 0x7fff) tag = 2;
            else if (!(reg->High & 0x7fff)) tag = reg->Low ? 2 : 1;
            else tag = reg->Low >> 63 ? 0 : 2;
        }
        result |= tag << (2U * index);
    }
    return result;
}

static uint8_t abridged_x87_tag( uint16_t tag )
{
    unsigned int index;
    uint8_t result = 0;
    for (index = 0; index < 8; ++index)
        if (((tag >> (2U * index)) & 3U) != 3U) result |= 1U << index;
    return result;
}

static void initialise_request( struct uml_wine_cpu_request *request )
{
    zero_bytes( request, sizeof(*request) );
    request->magic = UML_WINE_CPU_PROTOCOL_MAGIC;
    request->version = UML_WINE_CPU_PROTOCOL_VERSION;
    request->size = sizeof(*request);
    request->capabilities = service_capabilities;
    request->context.execution_mode = 1;
    request->context.rflags = 0x202;
    request->context.mxcsr = 0x1f80;
    request->context.x87_control = 0x027f;
    request->context.x87_tag = 0xffff;
}

static void load_context( struct uml_wine_x86_context *target,
                          const I386_CONTEXT *source )
{
    const XSAVE_FORMAT *save = (const XSAVE_FORMAT *)source->ExtendedRegisters;
    uintptr_t teb32 = (uintptr_t)current_teb32();
    unsigned int index;

    zero_bytes( target, sizeof(*target) );
    target->gpr[0] = source->Eax;
    target->gpr[1] = source->Ecx;
    target->gpr[2] = source->Edx;
    target->gpr[3] = source->Ebx;
    target->gpr[4] = source->Esp;
    target->gpr[5] = source->Ebp;
    target->gpr[6] = source->Esi;
    target->gpr[7] = source->Edi;
    target->rip = source->Eip;
    target->rflags = source->EFlags | 2U;
    target->fs_base = teb32;
    target->compat_tls_bases[UML_WOW64_WINDOWS_TLS_INDEX] = (uint32_t)teb32;
    target->compat_tls_present = 1U << UML_WOW64_WINDOWS_TLS_INDEX;
    target->segment_selectors[0] = source->SegEs;
    target->segment_selectors[1] = source->SegCs;
    target->segment_selectors[2] = source->SegSs;
    target->segment_selectors[3] = source->SegDs;
    target->segment_selectors[4] = source->SegFs;
    target->segment_selectors[5] = source->SegGs;
    target->execution_mode = 1;

    target->mxcsr = save->MxCsr ? save->MxCsr : 0x1f80;
    target->x87_control = save->ControlWord ? save->ControlWord :
                          source->FloatSave.ControlWord ?
                          (uint16_t)source->FloatSave.ControlWord : 0x027f;
    target->x87_status = save->StatusWord;
    target->x87_tag = full_x87_tag( save );
    target->x87_top = (save->StatusWord >> 11) & 7U;
    for (index = 0; index < 8; ++index)
    {
        target->x87_significand[index] = save->FloatRegisters[index].Low;
        target->x87_sign_exponent[index] =
            (uint16_t)save->FloatRegisters[index].High;
        target->mmx[index] = save->FloatRegisters[index].Low;
    }
    for (index = 0; index < 8; ++index)
        copy_bytes( target->vectors[index], &save->XmmRegisters[index], 16 );
}

static void store_context( I386_CONTEXT *target,
                           const struct uml_wine_x86_context *source )
{
    XSAVE_FORMAT *save = (XSAVE_FORMAT *)target->ExtendedRegisters;
    unsigned int index;

    target->Eax = (DWORD)source->gpr[0];
    target->Ecx = (DWORD)source->gpr[1];
    target->Edx = (DWORD)source->gpr[2];
    target->Ebx = (DWORD)source->gpr[3];
    target->Esp = (DWORD)source->gpr[4];
    target->Ebp = (DWORD)source->gpr[5];
    target->Esi = (DWORD)source->gpr[6];
    target->Edi = (DWORD)source->gpr[7];
    target->Eip = (DWORD)source->rip;
    target->EFlags = (DWORD)source->rflags | 2U;
    target->SegEs = source->segment_selectors[0];
    target->SegCs = source->segment_selectors[1];
    target->SegSs = source->segment_selectors[2];
    target->SegDs = source->segment_selectors[3];
    target->SegFs = source->segment_selectors[4];
    target->SegGs = source->segment_selectors[5];

    save->ControlWord = source->x87_control;
    save->StatusWord = source->x87_status;
    save->TagWord = abridged_x87_tag( source->x87_tag );
    save->MxCsr = source->mxcsr;
    for (index = 0; index < 8; ++index)
    {
        save->FloatRegisters[index].Low = source->x87_significand[index];
        save->FloatRegisters[index].High = source->x87_sign_exponent[index];
        copy_bytes( &target->FloatSave.RegisterArea[index * 10],
                    &source->x87_significand[index], 8 );
        copy_bytes( &target->FloatSave.RegisterArea[index * 10 + 8],
                    &source->x87_sign_exponent[index], 2 );
    }
    for (index = 0; index < 8; ++index)
        copy_bytes( &save->XmmRegisters[index], source->vectors[index], 16 );
    target->FloatSave.ControlWord = source->x87_control;
    target->FloatSave.StatusWord = source->x87_status;
    target->FloatSave.TagWord = source->x87_tag;
    target->FloatSave.Cr0NpxState = source->x87_status | 0xffff0000U;
}

static NTSTATUS query_service(void)
{
    struct uml_wine_cpu_request request;
    initialise_request( &request );
    request.operation = UML_WINE_CPU_OPERATION_QUERY;
    request.status = UML_WINE_CPU_STATUS_PENDING;
    uml_wow64_hostcall( &request );
    if (request.status != UML_WINE_CPU_STATUS_OK ||
        !(request.capabilities & UML_WINE_CPU_CAP_COMPAT32) ||
        !(request.capabilities & UML_WINE_CPU_CAP_STRICT_TSO))
        return STATUS_NOT_SUPPORTED;
    service_capabilities = request.capabilities;
    return STATUS_SUCCESS;
}

static NTSTATUS initialise_dispatchers(void)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING( L"ntdll.dll" );
    void **dispatcher;
    HMODULE module;
    NTSTATUS status;

    status = LdrGetDllHandle( NULL, 0, &name, &module );
    if (status) return status;
    dispatcher = RtlFindExportedRoutineByName(
        module, "__wine_unix_call_dispatcher" );
    if (!dispatcher || !*dispatcher) return STATUS_PROCEDURE_NOT_FOUND;
    unix_call_dispatcher = (unix_call_dispatcher_t)*dispatcher;
    return STATUS_SUCCESS;
}

static NTSTATUS initialise_stubs(void)
{
    void *address = code_buffer;
    SIZE_T size = sizeof(code_buffer);
    ULONG old_protection;

    if ((uintptr_t)&code_buffer[sizeof(code_buffer) - 1] > UINT32_MAX)
        return STATUS_INVALID_ADDRESS;
    code_buffer[0] = 0xcd;
    code_buffer[1] = 0x80;
    code_buffer[UML_WOW64_UNIX_STUB_OFFSET] = 0xcd;
    code_buffer[UML_WOW64_UNIX_STUB_OFFSET + 1] = 0x80;
    return NtProtectVirtualMemory( NtCurrentProcess(), &address, &size,
                                   PAGE_EXECUTE_READ, &old_protection );
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
    uml_wow64_hostcall( &request );
}

static uint32_t load_u32( uint32_t address )
{
    uint32_t value;
    copy_bytes( &value, ULongToPtr(address), sizeof(value) );
    return value;
}

static uint64_t load_u64( uint32_t address )
{
    uint64_t value;
    copy_bytes( &value, ULongToPtr(address), sizeof(value) );
    return value;
}

static BOOL dispatch_system_boundary( I386_CONTEXT *context )
{
    uint32_t stack = context->Esp;
    uint32_t return_address;

    if (context->Eip == (uint32_t)(uintptr_t)&code_buffer[2])
    {
        return_address = load_u32( stack );
        context->Eip = return_address;
        context->Esp = stack + 4;
        context->Eax = Wow64SystemServiceEx(
            context->Eax, ULongToPtr(stack + 8) );
        return TRUE;
    }
    if (context->Eip == (uint32_t)(uintptr_t)
                           &code_buffer[UML_WOW64_UNIX_STUB_OFFSET + 2])
    {
        uint64_t handle = load_u64( stack + 4 );
        uint32_t code = load_u32( stack + 12 );
        void *arguments = ULongToPtr( load_u32(stack + 16) );
        return_address = load_u32( stack );
        context->Eip = return_address;
        context->Esp = stack + 20;
        context->Eax = unix_call_dispatcher( handle, code, arguments );
        return TRUE;
    }
    return FALSE;
}

static void raise_emulation_exception(
    const struct uml_wine_cpu_request *request )
{
    EXCEPTION_RECORD record;
    ULONG_PTR access;

    zero_bytes( &record, sizeof(record) );
    record.ExceptionAddress = (void *)(uintptr_t)request->context.rip;
    switch (request->stop_reason)
    {
    case UML_WINE_CPU_STOP_MEMORY_FAULT:
        record.ExceptionCode = STATUS_ACCESS_VIOLATION;
        record.NumberParameters = 2;
        access = request->fault_access == UML_WINE_CPU_ACCESS_WRITE ? 1 :
                 request->fault_access == UML_WINE_CPU_ACCESS_EXECUTE ? 8 : 0;
        record.ExceptionInformation[0] = access;
        record.ExceptionInformation[1] = request->fault_address;
        break;
    case UML_WINE_CPU_STOP_BREAKPOINT:
        record.ExceptionCode = STATUS_BREAKPOINT;
        if (request->context.rip)
            record.ExceptionAddress =
                (void *)(uintptr_t)(request->context.rip - 1);
        break;
    case UML_WINE_CPU_STOP_FLOATING_POINT:
        record.ExceptionCode = STATUS_FLOAT_INVALID_OPERATION;
        break;
    case UML_WINE_CPU_STOP_ILLEGAL_INSTRUCTION:
    default:
        record.ExceptionCode = STATUS_ILLEGAL_INSTRUCTION;
        break;
    }
    RtlRaiseException( &record );
    RtlRaiseStatus( record.ExceptionCode );
}

NTSTATUS WINAPI BTCpuProcessInit(void)
{
    WOW64INFO *information = NtCurrentTeb()->TlsSlots[WOW64_TLS_WOW64INFO];
    NTSTATUS status;

    status = query_service();
    if (!status) status = initialise_dispatchers();
    if (!status) status = initialise_stubs();
    if (status)
    {
        service_capabilities = 0;
        unix_call_dispatcher = NULL;
        RtlRaiseStatus( status );
        return status;
    }
    information->CpuFlags |= WOW64_CPUFLAGS_SOFTWARE;
    return STATUS_SUCCESS;
}

void WINAPI BTCpuProcessTerm( HANDLE process, BOOL after, NTSTATUS status )
{
    struct uml_wine_cpu_request request;
    (void)process;
    (void)after;
    (void)status;
    if (!service_capabilities) return;
    initialise_request( &request );
    request.operation = UML_WINE_CPU_OPERATION_RESET;
    request.status = UML_WINE_CPU_STATUS_PENDING;
    uml_wow64_hostcall( &request );
    service_capabilities = 0;
    unix_call_dispatcher = NULL;
}

void *WINAPI BTCpuGetBopCode(void)
{
    return code_buffer;
}

void *WINAPI __wine_get_unix_opcode(void)
{
    return &code_buffer[UML_WOW64_UNIX_STUB_OFFSET];
}

NTSTATUS WINAPI BTCpuGetContext( HANDLE thread, HANDLE process, void *unknown,
                                 I386_CONTEXT *context )
{
    (void)process;
    (void)unknown;
    return RtlWow64GetThreadContext( thread, context );
}

NTSTATUS WINAPI BTCpuSetContext( HANDLE thread, HANDLE process, void *unknown,
                                 I386_CONTEXT *context )
{
    (void)process;
    (void)unknown;
    return RtlWow64SetThreadContext( thread, context );
}

void WINAPI BTCpuThreadInit(void)
{
}

void WINAPI BTCpuThreadTerm( HANDLE thread, LONG exit_code )
{
    (void)thread;
    (void)exit_code;
}

void WINAPI BTCpuSimulate(void)
{
    struct uml_wine_cpu_request request;
    I386_CONTEXT *context = current_context();
    WOW64_CPURESERVED *cpu = cpu_reserved();

    initialise_request( &request );
    load_context( &request.context, context );
    cpu->Flags &= ~WOW64_CPURESERVED_FLAG_RESET_STATE;
    for (;;)
    {
        request.operation = UML_WINE_CPU_OPERATION_RUN;
        request.status = UML_WINE_CPU_STATUS_PENDING;
        NtCurrentTeb()->TlsSlots[UML_WOW64_TLS_IN_CPU_SIMULATION] =
            (void *)TRUE;
        uml_wow64_hostcall( &request );
        NtCurrentTeb()->TlsSlots[UML_WOW64_TLS_IN_CPU_SIMULATION] = NULL;
        if (request.status != UML_WINE_CPU_STATUS_OK)
            RtlRaiseStatus( request.status == UML_WINE_CPU_STATUS_UNSUPPORTED ?
                            STATUS_NOT_SUPPORTED : STATUS_INVALID_PARAMETER );
        if (request.stop_reason == UML_WINE_CPU_STOP_BUDGET ||
            request.stop_reason == UML_WINE_CPU_STOP_INTERRUPT)
            continue;
        break;
    }
    store_context( context, &request.context );
    if (request.stop_reason == UML_WINE_CPU_STOP_SYSTEM_CALL &&
        dispatch_system_boundary( context ))
        return;
    raise_emulation_exception( &request );
}

NTSTATUS WINAPI BTCpuResetToConsistentState( EXCEPTION_POINTERS *pointers )
{
    (void)pointers;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI BTCpuTurboThunkControl( ULONG enable )
{
    return enable ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
}

BOOLEAN WINAPI BTCpuIsProcessorFeaturePresent( UINT feature )
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

void WINAPI BTCpuUpdateProcessorInformation(
    SYSTEM_CPU_INFORMATION *information )
{
    if (!information) return;
    information->ProcessorArchitecture = PROCESSOR_ARCHITECTURE_INTEL;
    information->ProcessorLevel = 6;
    information->ProcessorRevision = 0x03c3;
    information->ProcessorFeatureBits |= CPU_FEATURE_TSC | CPU_FEATURE_CMOV |
        CPU_FEATURE_CX8 | CPU_FEATURE_MMX | CPU_FEATURE_X86 |
        CPU_FEATURE_FXSR | CPU_FEATURE_SSE | CPU_FEATURE_SSE2;
}

void WINAPI BTCpuFlushInstructionCache2( const void *address, SIZE_T size )
{
    invalidate_code( address, size );
}

void WINAPI BTCpuFlushInstructionCacheHeavy( const void *address, SIZE_T size )
{
    invalidate_code( address, size );
}

void WINAPI BTCpuNotifyMemoryDirty( void *address, SIZE_T size )
{
    invalidate_code( address, size );
}

void WINAPI BTCpuNotifyReadFile( HANDLE handle, void *address, SIZE_T size,
                                 BOOL after, NTSTATUS status )
{
    (void)handle;
    if (after && !status) invalidate_code( address, size );
}

void WINAPI BTCpuNotifyMemoryAlloc( void *address, SIZE_T size, ULONG type,
                                    ULONG protection, BOOL after,
                                    NTSTATUS status )
{
    (void)type;
    (void)protection;
    if (after && !status) invalidate_code( address, size );
}

void WINAPI BTCpuNotifyMemoryFree( void *address, SIZE_T size, ULONG type,
                                   BOOL after, NTSTATUS status )
{
    (void)type;
    if (!after && !status) invalidate_code( address, size );
}

void WINAPI BTCpuNotifyMemoryProtect( void *address, SIZE_T size,
                                      ULONG protection, BOOL after,
                                      NTSTATUS status )
{
    (void)protection;
    if (after && !status) invalidate_code( address, size );
}

NTSTATUS WINAPI BTCpuNotifyMapViewOfSection( void *unknown1, void *address,
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

void WINAPI BTCpuNotifyUnmapViewOfSection( void *address, BOOL after,
                                           NTSTATUS status )
{
    if (!after && !status)
        invalidate_code( address, UML_WOW64_STUB_PAGE_SIZE );
}
