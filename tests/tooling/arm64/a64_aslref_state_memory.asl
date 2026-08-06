/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Deterministic memory-and-ordering boundary for per-form A64 ASLRef tests.
 *
 * This is a finite single-PE oracle state, not a whole-system memory model.
 * The bridge retains the input harness's ordered, non-overlapping regions in
 * a bounded flat store.  Only configured bytes and tags are initialized or
 * observed; reset work is never proportional to unused bridge capacity.
 * Accesses outside the configured regions, permission failures, injected
 * failures, and alignment failures are retained as typed PhysMemRetStatus
 * outcomes.  A harness must treat a non-Fault_None outcome as terminal and
 * ignore any subsequently computed instruction result.  Barriers and ordered
 * accesses advance an explicit sequence and retain their typed kind instead
 * of being invisible no-ops.
 */

/* The current C++ harness has at most three regions.  The separately gated
 * full-extent CPY fixture needs 96 KiB of backing in 16 KiB-page mode; its
 * payload is generated compactly and the fixed array is not source-unrolled.
 * Eight region slots leave a reviewed finite margin.  The tag bound includes
 * one rounded granule per slot, so non-granule-sized regions cannot overflow. */
constant A64Oracle_MemoryByteCount : integer{} = 4096;
constant A64Oracle_MemoryTagCount : integer{} = 256;
constant A64Oracle_MaximumMemoryRegionCount : integer{} = 8;
constant A64Oracle_MaximumMemoryByteCount : integer{} = 98304;
constant A64Oracle_MaximumMemoryTagCount : integer{} = 6152;
constant A64Oracle_MaximumMemoryByteChunkCount : integer{} = 6144;
constant A64Oracle_MaximumMemoryTagChunkCount : integer{} = 385;
constant A64Oracle_AddressSpaceSize : integer{} = 18446744073709551616;

type MemAttrHints of record {
    attrs : bits(2),
    hints : bits(2),
    transient : boolean
};

type PrefetchHint of enumeration {
    Prefetch_READ,
    Prefetch_WRITE,
    Prefetch_IR,
    Prefetch_EXEC
};

type A64OracleMemoryEvent of enumeration {
    A64OracleMemoryEvent_None,
    A64OracleMemoryEvent_Load,
    A64OracleMemoryEvent_Store,
    A64OracleMemoryEvent_TagLoad,
    A64OracleMemoryEvent_TagStore,
    A64OracleMemoryEvent_DataMemoryBarrier,
    A64OracleMemoryEvent_DataSynchronizationBarrier,
    A64OracleMemoryEvent_StoreBarrierPA,
    A64OracleMemoryEvent_StoreBarrierVA,
    A64OracleMemoryEvent_Prefetch,
    A64OracleMemoryEvent_RangePrefetch,
    A64OracleMemoryEvent_StopInstructionPrefetch
};

type A64OracleMemoryAccessClass of enumeration {
    A64OracleMemoryAccessClass_None,
    A64OracleMemoryAccessClass_DataRead,
    A64OracleMemoryAccessClass_DataWrite,
    A64OracleMemoryAccessClass_TagRead,
    A64OracleMemoryAccessClass_TagWrite
};

type A64OracleMemoryAddressDomain of enumeration {
    A64OracleMemoryAddressDomain_None,
    A64OracleMemoryAddressDomain_Virtual,
    A64OracleMemoryAddressDomain_Physical
};

type A64OracleExclusiveMonitor of record {
    valid : boolean,
    paddress : FullAddress,
    processorid : integer,
    size : integer,
    rawaddress : bits(64),
    key : bits(64),
    value : bits(64),
    value2 : bits(64),
    version : bits(64)
};

var A64Oracle_Memory : array [[6144]] of bits(128);
var A64Oracle_MemoryTags : array [[385]] of bits(64);
var A64Oracle_MemoryRegionCount : integer;
var A64Oracle_MemoryTotalBytes : integer;
var A64Oracle_MemoryTotalTags : integer;
var A64Oracle_MemoryRegionsFinalized : boolean;
var A64Oracle_MemoryRegionAddress : array [[8]] of bits(64);
var A64Oracle_MemoryRegionSize : array [[8]] of integer;
var A64Oracle_MemoryRegionPASpace : array [[8]] of PASpace;
var A64Oracle_MemoryRegionByteOffset : array [[8]] of integer;
var A64Oracle_MemoryRegionTagOffset : array [[8]] of integer;
var A64Oracle_MemoryRegionTagCount : array [[8]] of integer;
var A64Oracle_MemoryRegionInitializedBytes : array [[8]] of integer;
var A64Oracle_MemoryRegionInitializedTags : array [[8]] of integer;
var A64Oracle_MemoryRegionReadable : array [[8]] of boolean;
var A64Oracle_MemoryRegionWritable : array [[8]] of boolean;
var A64Oracle_MemoryRegionExecutable : array [[8]] of boolean;
var A64Oracle_MemoryRegionGuestCode : array [[8]] of boolean;
var A64Oracle_MemoryRegionGuestWritable : array [[8]] of boolean;
var A64Oracle_LegacyMemoryBytesAreZero : boolean;
var A64Oracle_LegacyMemoryTagsAreZero : boolean;
/* Legacy aliases for the first region. */
var A64Oracle_MemoryBase : bits(64);
var A64Oracle_MemoryPASpace : PASpace;
var A64Oracle_InjectedMemoryFault : Fault;
var A64Oracle_LastMemoryStatus : PhysMemRetStatus;
var A64Oracle_LastMemoryEvent : A64OracleMemoryEvent;
var A64Oracle_MemorySequence : integer;
var A64Oracle_LastBarrierTypes : MBReqTypes;
var A64Oracle_LastBarrierScope : MBMaintenanceScope;
var A64Oracle_LastBarrierNXS : boolean;
var A64Oracle_LastAccessAcquire : boolean;
var A64Oracle_LastAccessRelease : boolean;
var A64Oracle_LastAccessExclusive : boolean;
var A64Oracle_LastAccessPresent : boolean;
var A64Oracle_LastAccessClass : A64OracleMemoryAccessClass;
var A64Oracle_LastAccessDomain : A64OracleMemoryAddressDomain;
var A64Oracle_LastAccessPASpace : PASpace;
var A64Oracle_LastAccessAddress : bits(64);
var A64Oracle_LastAccessByteCount : integer;
var A64Oracle_LastAccessStatus : PhysMemRetStatus;
var A64Oracle_LastFaultPresent : boolean;
var A64Oracle_LastFaultDomain : A64OracleMemoryAddressDomain;
var A64Oracle_LastFaultPASpace : PASpace;
var A64Oracle_LastFaultAddress : bits(64);
var A64Oracle_LastFaultStatus : PhysMemRetStatus;
var A64Oracle_LastPrefetchAddress : bits(64);
var A64Oracle_LastPrefetchHint : PrefetchHint;
var A64Oracle_LastPrefetchTarget : integer;
var A64Oracle_LastPrefetchStream : boolean;
var A64Oracle_LastRangeLength : integer;
var A64Oracle_LastRangeStride : integer;
var A64Oracle_LastRangeCount : integer;
var A64Oracle_LastRangeReuse : integer;
var A64Oracle_LastRangeOperation : bits(6);
var A64Oracle_InstructionPrefetchStopped : boolean;
var A64Oracle_LS64Supported : boolean;
var A64Oracle_CurrentProcessorID : integer;
var A64Oracle_LocalMonitor : A64OracleExclusiveMonitor;
var A64Oracle_GlobalMonitor : A64OracleExclusiveMonitor;
var A64Oracle_LastExclusiveStatus : bit;

func A64Oracle_MakeMemoryStatus(statuscode : Fault) => PhysMemRetStatus
begin
    var status : PhysMemRetStatus;
    status.statuscode = statuscode;
    status.extflag = '0';
    status.merrorstate = ErrorState_CE;
    status.store64bstatus = Zeros{64};
    return status;
end;

readonly func A64Oracle_AddressRangeDoesNotWrap(address : bits(64),
                                                bytes : integer) => boolean
begin
    let first : integer = UInt(address);
    return bytes > 0 && first <= A64Oracle_AddressSpaceSize - bytes;
end;

func A64Oracle_FindVirtualMemoryRegion(address : bits(64)) => integer
begin
    var found : integer = -1;
    if A64Oracle_MemoryRegionsFinalized then
        let value : integer = UInt(address);
        for index = 0 to A64Oracle_MemoryRegionCount - 1 do
            let first : integer = UInt(A64Oracle_MemoryRegionAddress[[index]]);
            let limit : integer = first + A64Oracle_MemoryRegionSize[[index]];
            if value >= first && value < limit then
                found = index;
            end;
        end;
    end;
    return found;
end;

func A64Oracle_FindPhysicalMemoryRegion(paddress : FullAddress) => integer
begin
    var found : integer = -1;
    if A64Oracle_MemoryRegionsFinalized then
        let value : integer = UInt(paddress.address);
        for index = 0 to A64Oracle_MemoryRegionCount - 1 do
            let first : integer = UInt(A64Oracle_MemoryRegionAddress[[index]]);
            let limit : integer = first + A64Oracle_MemoryRegionSize[[index]];
            if paddress.paspace == A64Oracle_MemoryRegionPASpace[[index]] &&
               value >= first && value < limit then
                found = index;
            end;
        end;
    end;
    return found;
end;

func A64Oracle_VirtualAddressValid(address : bits(64),
                                   bytes : integer) => boolean
begin
    if !A64Oracle_AddressRangeDoesNotWrap(address, bytes) then
        return FALSE;
    end;
    var valid : boolean = TRUE;
    for offset = 0 to bytes - 1 do
        if A64Oracle_FindVirtualMemoryRegion(address + offset) < 0 then
            valid = FALSE;
        end;
    end;
    return valid;
end;

func A64Oracle_PhysicalAddressValid(paddress : FullAddress,
                                    bytes : integer) => boolean
begin
    let first : integer = UInt(paddress.address);
    let physical_address_space_size : integer = 2 ^ NUM_PABITS;
    if bytes <= 0 || first > physical_address_space_size - bytes then
        return FALSE;
    end;
    var valid : boolean = TRUE;
    for offset = 0 to bytes - 1 do
        var current : FullAddress = paddress;
        current.address = paddress.address + offset;
        if A64Oracle_FindPhysicalMemoryRegion(current) < 0 then
            valid = FALSE;
        end;
    end;
    return valid;
end;

func A64Oracle_VirtualByteOffset(address : bits(64)) => integer
begin
    let region : integer = A64Oracle_FindVirtualMemoryRegion(address);
    assert region >= 0;
    return A64Oracle_MemoryRegionByteOffset[[region]] +
           (UInt(address) -
            UInt(A64Oracle_MemoryRegionAddress[[region]]));
end;

func A64Oracle_PhysicalByteOffset(paddress : FullAddress) => integer
begin
    let region : integer = A64Oracle_FindPhysicalMemoryRegion(paddress);
    assert region >= 0;
    return A64Oracle_MemoryRegionByteOffset[[region]] +
           (UInt(paddress.address) -
            UInt(A64Oracle_MemoryRegionAddress[[region]]));
end;

func A64Oracle_VirtualTagOffset(address : bits(64)) => integer
begin
    let region : integer = A64Oracle_FindVirtualMemoryRegion(address);
    assert region >= 0;
    return A64Oracle_MemoryRegionTagOffset[[region]] +
           (UInt(address) -
            UInt(A64Oracle_MemoryRegionAddress[[region]])) DIVRM 16;
end;

func A64Oracle_PhysicalTagOffset(paddress : FullAddress) => integer
begin
    let region : integer = A64Oracle_FindPhysicalMemoryRegion(paddress);
    assert region >= 0;
    return A64Oracle_MemoryRegionTagOffset[[region]] +
           (UInt(paddress.address) -
            UInt(A64Oracle_MemoryRegionAddress[[region]])) DIVRM 16;
end;

func A64Oracle_VirtualPermissionValid(address : bits(64),
                                      bytes : integer,
                                      write : boolean,
                                      accdesc : AccessDescriptor) => boolean
begin
    /* Match the harness mapping contract: host R/W/X protect data accesses;
     * execution additionally requires guest-code classification.  The
     * guest-writable bit is retained losslessly for mapping reconstruction,
     * but it is not a second store-permission gate. */
    var permitted : boolean = TRUE;
    for offset = 0 to bytes - 1 do
        let region : integer =
            A64Oracle_FindVirtualMemoryRegion(address + offset);
        if region < 0 then
            permitted = FALSE;
        elsif write then
            if !A64Oracle_MemoryRegionWritable[[region]] then
                permitted = FALSE;
            end;
        elsif accdesc.acctype == AccessType_IFETCH then
            if !A64Oracle_MemoryRegionExecutable[[region]] ||
               !A64Oracle_MemoryRegionGuestCode[[region]] then
                permitted = FALSE;
            end;
        elsif !A64Oracle_MemoryRegionReadable[[region]] then
            permitted = FALSE;
        end;
    end;
    return permitted;
end;

func A64Oracle_PhysicalPermissionValid(paddress : FullAddress,
                                       bytes : integer,
                                       write : boolean,
                                       accdesc : AccessDescriptor) => boolean
begin
    var permitted : boolean = TRUE;
    for offset = 0 to bytes - 1 do
        var current : FullAddress = paddress;
        current.address = paddress.address + offset;
        let region : integer = A64Oracle_FindPhysicalMemoryRegion(current);
        if region < 0 then
            permitted = FALSE;
        elsif write then
            if !A64Oracle_MemoryRegionWritable[[region]] then
                permitted = FALSE;
            end;
        elsif accdesc.acctype == AccessType_IFETCH then
            if !A64Oracle_MemoryRegionExecutable[[region]] ||
               !A64Oracle_MemoryRegionGuestCode[[region]] then
                permitted = FALSE;
            end;
        elsif !A64Oracle_MemoryRegionReadable[[region]] then
            permitted = FALSE;
        end;
    end;
    return permitted;
end;

func A64Oracle_EffectiveVirtualMemoryStatus(
    valid : boolean, aligned : boolean
) => PhysMemRetStatus
begin
    if !aligned then
        return A64Oracle_MakeMemoryStatus(Fault_Alignment);
    elsif !valid then
        return A64Oracle_MakeMemoryStatus(Fault_Translation);
    else
        return A64Oracle_MakeMemoryStatus(A64Oracle_InjectedMemoryFault);
    end;
end;

func A64Oracle_EffectivePhysicalMemoryStatus(
    valid : boolean, aligned : boolean
) => PhysMemRetStatus
begin
    if !aligned || !valid ||
       A64Oracle_InjectedMemoryFault != Fault_None then
        return A64Oracle_MakeMemoryStatus(Fault_SyncExternal);
    else
        return A64Oracle_MakeMemoryStatus(Fault_None);
    end;
end;

func A64Oracle_PhysicalMemoryStatus(desc : AddressDescriptor,
                                    valid : boolean,
                                    aligned : boolean) => PhysMemRetStatus
begin
    if desc.fault.statuscode != Fault_None then
        return A64Oracle_MakeMemoryStatus(Fault_SyncExternal);
    else
        return A64Oracle_EffectivePhysicalMemoryStatus(valid, aligned);
    end;
end;

func A64Oracle_VirtualAccessStatus(address : bits(64),
                                   bytes : integer,
                                   aligned : boolean,
                                   write : boolean,
                                   accdesc : AccessDescriptor) => PhysMemRetStatus
begin
    let valid : boolean = A64Oracle_VirtualAddressValid(address, bytes);
    if !aligned then
        return A64Oracle_MakeMemoryStatus(Fault_Alignment);
    elsif !valid then
        return A64Oracle_MakeMemoryStatus(Fault_Translation);
    elsif !A64Oracle_VirtualPermissionValid(
        address, bytes, write, accdesc
    ) then
        return A64Oracle_MakeMemoryStatus(Fault_Permission);
    else
        return A64Oracle_MakeMemoryStatus(A64Oracle_InjectedMemoryFault);
    end;
end;

/* The official AArch64 access path consumes translation/access faults from
 * AddressDescriptor.fault and reserves PhysMemRetStatus for failures reported
 * by physical memory.  Project the harness's virtual mapping contract at the
 * translation boundary so translation and permission failures take the
 * architectural AArch64_Abort route rather than HandleExternalAbort. */
func A64Oracle_ProjectVirtualTranslationFault(
    address : bits(64), bytes : integer, accdesc : AccessDescriptor
) => FaultRecord
begin
    assert bytes > 0;
    let write : boolean = !accdesc.read && accdesc.write;
    var fault : FaultRecord = NoFault(accdesc, address);
    if !A64Oracle_VirtualAddressValid(address, bytes) then
        fault.statuscode = Fault_Translation;
        var found : boolean = FALSE;
        for offset = 0 to bytes - 1 do
            if !found &&
               A64Oracle_FindVirtualMemoryRegion(address + offset) < 0 then
                fault.vaddress = address + offset;
                found = TRUE;
            end;
        end;
    elsif !A64Oracle_VirtualPermissionValid(
        address, bytes, write, accdesc
    ) then
        fault.statuscode = Fault_Permission;
        var found : boolean = FALSE;
        for offset = 0 to bytes - 1 do
            if !found && !A64Oracle_VirtualPermissionValid(
                address + offset, 1, write, accdesc
            ) then
                fault.vaddress = address + offset;
                found = TRUE;
            end;
        end;
    end;
    if fault.statuscode != Fault_None then
        fault.level = 0;
        fault.write = write;
        fault.extflag = '0';
        fault.merrorstate = ErrorState_CE;
    end;
    return fault;
end;

func A64Oracle_RecordProjectedVirtualFault(
    address : bits(64), bytes : integer, accdesc : AccessDescriptor,
    fault : FaultRecord)
begin
    assert fault.statuscode IN {Fault_Translation, Fault_Permission};
    let write : boolean = !accdesc.read && accdesc.write;
    let access_class : A64OracleMemoryAccessClass =
        if write then A64OracleMemoryAccessClass_DataWrite
        else A64OracleMemoryAccessClass_DataRead;
    let event : A64OracleMemoryEvent =
        if write then A64OracleMemoryEvent_Store
        else A64OracleMemoryEvent_Load;
    let status : PhysMemRetStatus =
        A64Oracle_MakeMemoryStatus(fault.statuscode);
    A64Oracle_RecordAccessDescriptor(accdesc);
    A64Oracle_RecordVirtualAccess(
        access_class, fault.vaddress, bytes, status
    );
    A64Oracle_RecordMemoryEvent(event, status);
end;

func A64Oracle_PhysicalAccessStatus(desc : AddressDescriptor,
                                    bytes : integer,
                                    aligned : boolean,
                                    write : boolean,
                                    accdesc : AccessDescriptor) => PhysMemRetStatus
begin
    let valid : boolean =
        A64Oracle_PhysicalAddressValid(desc.paddress, bytes);
    if desc.fault.statuscode != Fault_None then
        return A64Oracle_MakeMemoryStatus(Fault_SyncExternal);
    elsif !aligned || !valid then
        return A64Oracle_MakeMemoryStatus(Fault_SyncExternal);
    elsif !A64Oracle_PhysicalPermissionValid(
        desc.paddress, bytes, write, accdesc
    ) then
        return A64Oracle_MakeMemoryStatus(Fault_SyncExternal);
    elsif A64Oracle_InjectedMemoryFault != Fault_None then
        return A64Oracle_MakeMemoryStatus(Fault_SyncExternal);
    else
        return A64Oracle_MakeMemoryStatus(Fault_None);
    end;
end;

func A64Oracle_AccessAligned(address : bits(64),
                             bytes : integer,
                             accdesc : AccessDescriptor) => boolean
begin
    if bytes <= 0 || (accdesc.ispair && bytes MOD 2 != 0) then
        return FALSE;
    end;
    let pair_element_bytes : integer =
        if accdesc.ispair && !accdesc.exclusive then bytes DIV 2 else bytes;
    let naturally_aligned : boolean =
        UInt(address) MOD pair_element_bytes == 0;
    if accdesc.acctype == AccessType_GCS || accdesc.rcw || accdesc.rcws ||
       accdesc.ls64 || accdesc.exclusive || accdesc.atomicop then
        return naturally_aligned;
    elsif accdesc.acqsc || accdesc.acqpc || accdesc.relsc then
        if accdesc.ispair then
            let half : integer = bytes DIV 2;
            let first : integer = UInt(address);
            return first MOD 16 + half <= 16 &&
                   (first + half) MOD 16 + half <= 16;
        else
            return UInt(address) MOD 16 + bytes <= 16;
        end;
    else
        return TRUE;
    end;
end;

func A64Oracle_RecordMemoryEvent(event : A64OracleMemoryEvent,
                                 status : PhysMemRetStatus)
begin
    A64Oracle_LastMemoryEvent = event;
    A64Oracle_LastMemoryStatus = status;
    A64Oracle_MemorySequence = A64Oracle_MemorySequence + 1;
end;

func A64Oracle_RecordOrderingEvent(event : A64OracleMemoryEvent)
begin
    A64Oracle_LastMemoryEvent = event;
    A64Oracle_MemorySequence = A64Oracle_MemorySequence + 1;
end;

func A64Oracle_RecordAccessDescriptor(accdesc : AccessDescriptor)
begin
    A64Oracle_LastAccessAcquire = accdesc.acqsc || accdesc.acqpc;
    A64Oracle_LastAccessRelease = accdesc.relsc;
    A64Oracle_LastAccessExclusive = accdesc.exclusive;
end;

func A64Oracle_RecordAccessFault(
    domain : A64OracleMemoryAddressDomain,
    paspace : PASpace,
    address : bits(64),
    status : PhysMemRetStatus)
begin
    if status.statuscode != Fault_None then
        A64Oracle_LastFaultPresent = TRUE;
        A64Oracle_LastFaultDomain = domain;
        A64Oracle_LastFaultPASpace = paspace;
        A64Oracle_LastFaultAddress = address;
        A64Oracle_LastFaultStatus = status;
    end;
end;

func A64Oracle_RecordVirtualAccess(
    access_class : A64OracleMemoryAccessClass,
    address : bits(64),
    bytes : integer,
    status : PhysMemRetStatus)
begin
    assert bytes > 0;
    A64Oracle_LastAccessPresent = TRUE;
    A64Oracle_LastAccessClass = access_class;
    A64Oracle_LastAccessDomain = A64OracleMemoryAddressDomain_Virtual;
    A64Oracle_LastAccessPASpace = PAS_NonSecure;
    A64Oracle_LastAccessAddress = address;
    A64Oracle_LastAccessByteCount = bytes;
    A64Oracle_LastAccessStatus = status;
    A64Oracle_RecordAccessFault(
        A64OracleMemoryAddressDomain_Virtual,
        PAS_NonSecure,
        address,
        status
    );
end;

func A64Oracle_RecordPhysicalAccess(
    access_class : A64OracleMemoryAccessClass,
    paddress : FullAddress,
    bytes : integer,
    status : PhysMemRetStatus)
begin
    assert bytes > 0;
    let address : bits(64) =
        Zeros{64 - NUM_PABITS} :: paddress.address;
    A64Oracle_LastAccessPresent = TRUE;
    A64Oracle_LastAccessClass = access_class;
    A64Oracle_LastAccessDomain = A64OracleMemoryAddressDomain_Physical;
    A64Oracle_LastAccessPASpace = paddress.paspace;
    A64Oracle_LastAccessAddress = address;
    A64Oracle_LastAccessByteCount = bytes;
    A64Oracle_LastAccessStatus = status;
    A64Oracle_RecordAccessFault(
        A64OracleMemoryAddressDomain_Physical,
        paddress.paspace,
        address,
        status
    );
end;

func A64Oracle_ClearMonitor(monitor : A64OracleExclusiveMonitor) => A64OracleExclusiveMonitor
begin
    var cleared : A64OracleExclusiveMonitor = monitor;
    cleared.valid = FALSE;
    cleared.processorid = 0;
    cleared.size = 0;
    cleared.paddress.paspace = PAS_NonSecure;
    cleared.paddress.address = Zeros{NUM_PABITS};
    cleared.rawaddress = Ones{64};
    cleared.key = Zeros{64};
    cleared.value = Zeros{64};
    cleared.value2 = Zeros{64};
    cleared.version = Zeros{64};
    return cleared;
end;

func A64Oracle_MonitorContains(monitor : A64OracleExclusiveMonitor,
                               paddress : FullAddress,
                               processorid : integer,
                               size : integer) => boolean
begin
    let monitor_first : integer = UInt(monitor.paddress.address);
    let requested_first : integer = UInt(paddress.address);
    return monitor.valid && monitor.processorid == processorid && size > 0 &&
           monitor.paddress.paspace == paddress.paspace &&
           requested_first >= monitor_first &&
           requested_first + size <= monitor_first + monitor.size;
end;

func A64Oracle_MonitorOverlaps(monitor : A64OracleExclusiveMonitor,
                               paddress : FullAddress,
                               size : integer) => boolean
begin
    let monitor_first : integer = UInt(monitor.paddress.address);
    let requested_first : integer = UInt(paddress.address);
    return monitor.valid && monitor.size > 0 && size > 0 &&
           monitor.paddress.paspace == paddress.paspace &&
           monitor_first < requested_first + size &&
           requested_first < monitor_first + monitor.size;
end;

func A64Oracle_MaterializeLegacyMemoryBytes()
begin
    if A64Oracle_LegacyMemoryBytesAreZero then
        for index = 0 to A64Oracle_MemoryByteCount DIVRM 16 - 1 do
            A64Oracle_Memory[[index]] = Zeros{128};
        end;
        A64Oracle_LegacyMemoryBytesAreZero = FALSE;
    end;
end;

func A64Oracle_MaterializeLegacyMemoryTags()
begin
    if A64Oracle_LegacyMemoryTagsAreZero then
        for index = 0 to A64Oracle_MemoryTagCount DIVRM 16 - 1 do
            A64Oracle_MemoryTags[[index]] = Zeros{64};
        end;
        A64Oracle_LegacyMemoryTagsAreZero = FALSE;
    end;
end;

readonly func A64Oracle_ReadMemoryByte(storage : integer) => bits(8)
begin
    assert storage >= 0 && storage < A64Oracle_MemoryTotalBytes;
    if A64Oracle_LegacyMemoryBytesAreZero &&
       storage < A64Oracle_MemoryByteCount then
        return Zeros{8};
    else
        return A64Oracle_Memory[[storage DIVRM 16]][(storage MOD 16) * 8 +: 8];
    end;
end;

readonly func A64Oracle_ReadMemoryTag(storage : integer) => bits(4)
begin
    assert storage >= 0 && storage < A64Oracle_MemoryTotalTags;
    if A64Oracle_LegacyMemoryTagsAreZero &&
       storage < A64Oracle_MemoryTagCount then
        return Zeros{4};
    else
        return A64Oracle_MemoryTags[[storage DIVRM 16]][(storage MOD 16) * 4 +: 4];
    end;
end;

func A64Oracle_WriteMemoryByte(storage : integer, value : bits(8))
begin
    assert storage >= 0 && storage < A64Oracle_MemoryTotalBytes;
    A64Oracle_MaterializeLegacyMemoryBytes();
    A64Oracle_Memory[[storage DIVRM 16]][(storage MOD 16) * 8 +: 8] = value;
end;

func A64Oracle_WriteMemoryTag(storage : integer, value : bits(4))
begin
    assert storage >= 0 && storage < A64Oracle_MemoryTotalTags;
    A64Oracle_MaterializeLegacyMemoryTags();
    A64Oracle_MemoryTags[[storage DIVRM 16]][(storage MOD 16) * 4 +: 4] = value;
end;

func A64Oracle_ResetMemoryState()
begin
    /* Preserve the original observer contract as one initialized 4 KiB
     * region.  The remaining 94,208 bytes of the 98,304-byte bridge capacity
     * are untouched. */
    A64Oracle_MemoryRegionCount = 1;
    A64Oracle_MemoryTotalBytes = A64Oracle_MemoryByteCount;
    A64Oracle_MemoryTotalTags = A64Oracle_MemoryTagCount;
    A64Oracle_MemoryRegionsFinalized = TRUE;
    A64Oracle_MemoryRegionAddress[[0]] = Zeros{64};
    A64Oracle_MemoryRegionSize[[0]] = A64Oracle_MemoryByteCount;
    A64Oracle_MemoryRegionPASpace[[0]] = PAS_NonSecure;
    A64Oracle_MemoryRegionByteOffset[[0]] = 0;
    A64Oracle_MemoryRegionTagOffset[[0]] = 0;
    A64Oracle_MemoryRegionTagCount[[0]] = A64Oracle_MemoryTagCount;
    A64Oracle_MemoryRegionInitializedBytes[[0]] =
        A64Oracle_MemoryByteCount;
    A64Oracle_MemoryRegionInitializedTags[[0]] =
        A64Oracle_MemoryTagCount;
    A64Oracle_MemoryRegionReadable[[0]] = TRUE;
    A64Oracle_MemoryRegionWritable[[0]] = TRUE;
    A64Oracle_MemoryRegionExecutable[[0]] = FALSE;
    A64Oracle_MemoryRegionGuestCode[[0]] = FALSE;
    A64Oracle_MemoryRegionGuestWritable[[0]] = TRUE;
    A64Oracle_LegacyMemoryBytesAreZero = TRUE;
    A64Oracle_LegacyMemoryTagsAreZero = TRUE;
    A64Oracle_MemoryBase = Zeros{64};
    A64Oracle_MemoryPASpace = PAS_NonSecure;
    A64Oracle_InjectedMemoryFault = Fault_None;
    A64Oracle_LastMemoryStatus = A64Oracle_MakeMemoryStatus(Fault_None);
    A64Oracle_LastMemoryEvent = A64OracleMemoryEvent_None;
    A64Oracle_MemorySequence = 0;
    A64Oracle_LastBarrierTypes = MBReqTypes_All;
    A64Oracle_LastBarrierScope = MBMaintenanceScope_None;
    A64Oracle_LastBarrierNXS = FALSE;
    A64Oracle_LastAccessAcquire = FALSE;
    A64Oracle_LastAccessRelease = FALSE;
    A64Oracle_LastAccessExclusive = FALSE;
    A64Oracle_LastAccessPresent = FALSE;
    A64Oracle_LastAccessClass = A64OracleMemoryAccessClass_None;
    A64Oracle_LastAccessDomain = A64OracleMemoryAddressDomain_None;
    A64Oracle_LastAccessPASpace = PAS_NonSecure;
    A64Oracle_LastAccessAddress = Zeros{64};
    A64Oracle_LastAccessByteCount = 0;
    A64Oracle_LastAccessStatus = A64Oracle_MakeMemoryStatus(Fault_None);
    A64Oracle_LastFaultPresent = FALSE;
    A64Oracle_LastFaultDomain = A64OracleMemoryAddressDomain_None;
    A64Oracle_LastFaultPASpace = PAS_NonSecure;
    A64Oracle_LastFaultAddress = Zeros{64};
    A64Oracle_LastFaultStatus = A64Oracle_MakeMemoryStatus(Fault_None);
    A64Oracle_LastPrefetchAddress = Zeros{64};
    A64Oracle_LastPrefetchHint = Prefetch_READ;
    A64Oracle_LastPrefetchTarget = 0;
    A64Oracle_LastPrefetchStream = FALSE;
    A64Oracle_LastRangeLength = 0;
    A64Oracle_LastRangeStride = 0;
    A64Oracle_LastRangeCount = 0;
    A64Oracle_LastRangeReuse = 0;
    A64Oracle_LastRangeOperation = Zeros{6};
    A64Oracle_InstructionPrefetchStopped = FALSE;
    A64Oracle_LS64Supported = FALSE;
    A64Oracle_CurrentProcessorID = 0;
    A64Oracle_LocalMonitor = A64Oracle_ClearMonitor(A64Oracle_LocalMonitor);
    A64Oracle_GlobalMonitor = A64Oracle_ClearMonitor(A64Oracle_GlobalMonitor);
    A64Oracle_LastExclusiveStatus = '0';
end;

func A64Oracle_BeginMemoryRegions()
begin
    A64Oracle_MemoryRegionCount = 0;
    A64Oracle_MemoryTotalBytes = 0;
    A64Oracle_MemoryTotalTags = 0;
    A64Oracle_MemoryRegionsFinalized = FALSE;
    A64Oracle_LegacyMemoryBytesAreZero = FALSE;
    A64Oracle_LegacyMemoryTagsAreZero = FALSE;
    A64Oracle_MemoryBase = Zeros{64};
    A64Oracle_MemoryPASpace = PAS_NonSecure;
end;

func A64Oracle_ConfigureMemoryRegion(index : integer,
                                     address : bits(64),
                                     size : integer,
                                     paspace : PASpace,
                                     readable : boolean,
                                     writable : boolean,
                                     executable : boolean,
                                     guest_code : boolean,
                                     guest_writable : boolean)
begin
    assert !A64Oracle_MemoryRegionsFinalized;
    assert index == A64Oracle_MemoryRegionCount;
    assert index >= 0 && index < A64Oracle_MaximumMemoryRegionCount;
    assert A64Oracle_AddressRangeDoesNotWrap(address, size);
    assert size <= A64Oracle_MaximumMemoryByteCount -
                   A64Oracle_MemoryTotalBytes;
    let tag_count : integer = (size + 15) DIVRM 16;
    assert tag_count <= A64Oracle_MaximumMemoryTagCount -
                        A64Oracle_MemoryTotalTags;
    let first : integer = UInt(address);
    let limit : integer = first + size;
    for previous = 0 to index - 1 do
        let previous_first : integer =
            UInt(A64Oracle_MemoryRegionAddress[[previous]]);
        let previous_limit : integer = previous_first +
            A64Oracle_MemoryRegionSize[[previous]];
        assert limit <= previous_first || previous_limit <= first;
    end;
    A64Oracle_MemoryRegionAddress[[index]] = address;
    A64Oracle_MemoryRegionSize[[index]] = size;
    A64Oracle_MemoryRegionPASpace[[index]] = paspace;
    A64Oracle_MemoryRegionByteOffset[[index]] =
        A64Oracle_MemoryTotalBytes;
    A64Oracle_MemoryRegionTagOffset[[index]] =
        A64Oracle_MemoryTotalTags;
    A64Oracle_MemoryRegionTagCount[[index]] = tag_count;
    A64Oracle_MemoryRegionInitializedBytes[[index]] = 0;
    A64Oracle_MemoryRegionInitializedTags[[index]] = 0;
    A64Oracle_MemoryRegionReadable[[index]] = readable;
    A64Oracle_MemoryRegionWritable[[index]] = writable;
    A64Oracle_MemoryRegionExecutable[[index]] = executable;
    A64Oracle_MemoryRegionGuestCode[[index]] = guest_code;
    A64Oracle_MemoryRegionGuestWritable[[index]] = guest_writable;
    A64Oracle_MemoryTotalBytes = A64Oracle_MemoryTotalBytes + size;
    A64Oracle_MemoryTotalTags = A64Oracle_MemoryTotalTags + tag_count;
    A64Oracle_MemoryRegionCount = A64Oracle_MemoryRegionCount + 1;
    if index == 0 then
        A64Oracle_MemoryBase = address;
        A64Oracle_MemoryPASpace = paspace;
    end;
end;

func A64Oracle_SetMemoryRegionByteChunk(index : integer,
                                        offset : integer,
                                        byte_count : integer,
                                        value : bits(128))
begin
    assert !A64Oracle_MemoryRegionsFinalized;
    assert index >= 0 && index < A64Oracle_MemoryRegionCount;
    assert byte_count > 0 && byte_count <= 16;
    assert offset == A64Oracle_MemoryRegionInitializedBytes[[index]];
    assert offset + byte_count <= A64Oracle_MemoryRegionSize[[index]];
    let storage : integer =
        A64Oracle_MemoryRegionByteOffset[[index]] + offset;
    if byte_count == 16 && storage MOD 16 == 0 then
        A64Oracle_Memory[[storage DIVRM 16]] = value;
    else
        for byte = 0 to byte_count - 1 do
            let current : integer = storage + byte;
            A64Oracle_Memory[[current DIVRM 16]][(current MOD 16) * 8 +: 8] =
                value[byte * 8 +: 8];
        end;
    end;
    A64Oracle_MemoryRegionInitializedBytes[[index]] =
        offset + byte_count;
end;

func A64Oracle_SetMemoryRegionTagChunk(index : integer,
                                       offset : integer,
                                       tag_count : integer,
                                       value : bits(64))
begin
    assert !A64Oracle_MemoryRegionsFinalized;
    assert index >= 0 && index < A64Oracle_MemoryRegionCount;
    assert tag_count > 0 && tag_count <= 16;
    assert offset == A64Oracle_MemoryRegionInitializedTags[[index]];
    assert offset + tag_count <= A64Oracle_MemoryRegionTagCount[[index]];
    let storage : integer =
        A64Oracle_MemoryRegionTagOffset[[index]] + offset;
    if tag_count == 16 && storage MOD 16 == 0 then
        A64Oracle_MemoryTags[[storage DIVRM 16]] = value;
    else
        for tag = 0 to tag_count - 1 do
            let current : integer = storage + tag;
            A64Oracle_MemoryTags[[current DIVRM 16]][(current MOD 16) * 4 +: 4] =
                value[tag * 4 +: 4];
        end;
    end;
    A64Oracle_MemoryRegionInitializedTags[[index]] = offset + tag_count;
end;

func A64Oracle_FinalizeMemoryRegions()
begin
    assert !A64Oracle_MemoryRegionsFinalized;
    for index = 0 to A64Oracle_MemoryRegionCount - 1 do
        assert A64Oracle_MemoryRegionInitializedBytes[[index]] ==
               A64Oracle_MemoryRegionSize[[index]];
        assert A64Oracle_MemoryRegionInitializedTags[[index]] ==
               A64Oracle_MemoryRegionTagCount[[index]];
    end;
    A64Oracle_MemoryRegionsFinalized = TRUE;
end;

readonly func A64Oracle_GetMemoryRegionCount() => integer
begin
    assert A64Oracle_MemoryRegionsFinalized;
    return A64Oracle_MemoryRegionCount;
end;

readonly func A64Oracle_GetMemoryRegionAddress(index : integer) => bits(64)
begin
    assert A64Oracle_MemoryRegionsFinalized;
    assert index >= 0 && index < A64Oracle_MemoryRegionCount;
    return A64Oracle_MemoryRegionAddress[[index]];
end;

readonly func A64Oracle_GetMemoryRegionSize(index : integer) => integer
begin
    assert A64Oracle_MemoryRegionsFinalized;
    assert index >= 0 && index < A64Oracle_MemoryRegionCount;
    return A64Oracle_MemoryRegionSize[[index]];
end;

readonly func A64Oracle_GetMemoryRegionPASpace(index : integer) => PASpace
begin
    assert A64Oracle_MemoryRegionsFinalized;
    assert index >= 0 && index < A64Oracle_MemoryRegionCount;
    return A64Oracle_MemoryRegionPASpace[[index]];
end;

readonly func A64Oracle_GetMemoryRegionReadable(index : integer) => boolean
begin
    assert A64Oracle_MemoryRegionsFinalized;
    assert index >= 0 && index < A64Oracle_MemoryRegionCount;
    return A64Oracle_MemoryRegionReadable[[index]];
end;

readonly func A64Oracle_GetMemoryRegionWritable(index : integer) => boolean
begin
    assert A64Oracle_MemoryRegionsFinalized;
    assert index >= 0 && index < A64Oracle_MemoryRegionCount;
    return A64Oracle_MemoryRegionWritable[[index]];
end;

readonly func A64Oracle_GetMemoryRegionExecutable(index : integer) => boolean
begin
    assert A64Oracle_MemoryRegionsFinalized;
    assert index >= 0 && index < A64Oracle_MemoryRegionCount;
    return A64Oracle_MemoryRegionExecutable[[index]];
end;

readonly func A64Oracle_GetMemoryRegionGuestCode(index : integer) => boolean
begin
    assert A64Oracle_MemoryRegionsFinalized;
    assert index >= 0 && index < A64Oracle_MemoryRegionCount;
    return A64Oracle_MemoryRegionGuestCode[[index]];
end;

readonly func A64Oracle_GetMemoryRegionGuestWritable(index : integer) => boolean
begin
    assert A64Oracle_MemoryRegionsFinalized;
    assert index >= 0 && index < A64Oracle_MemoryRegionCount;
    return A64Oracle_MemoryRegionGuestWritable[[index]];
end;

readonly func A64Oracle_GetMemoryRegionByteChunk(
    index : integer, offset : integer, byte_count : integer
) => bits(128)
begin
    assert A64Oracle_MemoryRegionsFinalized;
    assert index >= 0 && index < A64Oracle_MemoryRegionCount;
    assert byte_count > 0 && byte_count <= 16;
    assert offset >= 0 &&
           offset + byte_count <= A64Oracle_MemoryRegionSize[[index]];
    var value : bits(128) = Zeros{128};
    let storage : integer =
        A64Oracle_MemoryRegionByteOffset[[index]] + offset;
    if byte_count == 16 && storage MOD 16 == 0 then
        value = A64Oracle_Memory[[storage DIVRM 16]];
    else
        for byte = 0 to byte_count - 1 do
            value[byte * 8 +: 8] =
                A64Oracle_ReadMemoryByte(storage + byte);
        end;
    end;
    return value;
end;

readonly func A64Oracle_GetMemoryRegionTagChunk(
    index : integer, offset : integer, tag_count : integer
) => bits(64)
begin
    assert A64Oracle_MemoryRegionsFinalized;
    assert index >= 0 && index < A64Oracle_MemoryRegionCount;
    assert tag_count > 0 && tag_count <= 16;
    assert offset >= 0 &&
           offset + tag_count <= A64Oracle_MemoryRegionTagCount[[index]];
    var value : bits(64) = Zeros{64};
    let storage : integer =
        A64Oracle_MemoryRegionTagOffset[[index]] + offset;
    if tag_count == 16 && storage MOD 16 == 0 then
        value = A64Oracle_MemoryTags[[storage DIVRM 16]];
    else
        for tag = 0 to tag_count - 1 do
            value[tag * 4 +: 4] = A64Oracle_ReadMemoryTag(storage + tag);
        end;
    end;
    return value;
end;

func A64Oracle_SetMemoryBase(address : bits(64))
begin
    assert UInt(address) MOD 16 == 0;
    assert A64Oracle_MemoryRegionCount > 0;
    assert A64Oracle_AddressRangeDoesNotWrap(
        address, A64Oracle_MemoryRegionSize[[0]]
    );
    let first : integer = UInt(address);
    let limit : integer = first + A64Oracle_MemoryRegionSize[[0]];
    for other = 1 to A64Oracle_MemoryRegionCount - 1 do
        let other_first : integer =
            UInt(A64Oracle_MemoryRegionAddress[[other]]);
        let other_limit : integer = other_first +
            A64Oracle_MemoryRegionSize[[other]];
        assert limit <= other_first || other_limit <= first;
    end;
    A64Oracle_MemoryBase = address;
    A64Oracle_MemoryRegionAddress[[0]] = address;
end;

func A64Oracle_SetMemoryPASpace(paspace : PASpace)
begin
    assert A64Oracle_MemoryRegionCount > 0;
    A64Oracle_MemoryPASpace = paspace;
    A64Oracle_MemoryRegionPASpace[[0]] = paspace;
end;

func A64Oracle_SetMemoryFault(statuscode : Fault)
begin
    A64Oracle_InjectedMemoryFault = statuscode;
end;

func A64Oracle_SetLS64Supported(supported : boolean)
begin
    A64Oracle_LS64Supported = supported;
end;

func A64Oracle_SetProcessorID(processorid : integer)
begin
    assert processorid >= 0;
    A64Oracle_CurrentProcessorID = processorid;
end;

func A64Oracle_SetExclusiveMonitorsStatus(status : bit)
begin
    A64Oracle_LastExclusiveStatus = status;
end;

func A64Oracle_SetMemoryByte(offset : integer, value : bits(8))
begin
    assert offset >= 0 && offset < A64Oracle_MemoryByteCount;
    assert A64Oracle_MemoryRegionCount > 0 &&
           A64Oracle_MemoryRegionSize[[0]] >= A64Oracle_MemoryByteCount;
    A64Oracle_WriteMemoryByte(
        A64Oracle_MemoryRegionByteOffset[[0]] + offset, value
    );
end;

readonly func A64Oracle_GetMemoryByte(offset : integer) => bits(8)
begin
    assert offset >= 0 && offset < A64Oracle_MemoryByteCount;
    assert A64Oracle_MemoryRegionCount > 0 &&
           A64Oracle_MemoryRegionSize[[0]] >= A64Oracle_MemoryByteCount;
    return A64Oracle_ReadMemoryByte(
        A64Oracle_MemoryRegionByteOffset[[0]] + offset
    );
end;

func A64Oracle_SetMemoryTag(offset : integer, value : bits(4))
begin
    assert offset >= 0 && offset < A64Oracle_MemoryTagCount;
    assert A64Oracle_MemoryRegionCount > 0 &&
           A64Oracle_MemoryRegionTagCount[[0]] >= A64Oracle_MemoryTagCount;
    A64Oracle_WriteMemoryTag(
        A64Oracle_MemoryRegionTagOffset[[0]] + offset, value
    );
end;

readonly func A64Oracle_GetMemoryTag(offset : integer) => bits(4)
begin
    assert offset >= 0 && offset < A64Oracle_MemoryTagCount;
    assert A64Oracle_MemoryRegionCount > 0 &&
           A64Oracle_MemoryRegionTagCount[[0]] >= A64Oracle_MemoryTagCount;
    return A64Oracle_ReadMemoryTag(
        A64Oracle_MemoryRegionTagOffset[[0]] + offset
    );
end;

func A64Oracle_MakeExclusiveMonitorState(valid : boolean,
                                          paspace : PASpace,
                                          address : bits(64),
                                          processorid : integer,
                                          size : integer,
                                          key : bits(64),
                                          value : bits(64),
                                          value2 : bits(64),
                                          version : bits(64)) => A64OracleExclusiveMonitor
begin
    assert processorid >= 0;
    assert size >= 0 && size <= 255;
    if valid then
        assert size > 0;
        assert A64Oracle_AddressRangeDoesNotWrap(address, size);
    end;
    var monitor : A64OracleExclusiveMonitor;
    monitor.valid = valid;
    monitor.paddress.paspace = paspace;
    monitor.paddress.address = address[NUM_PABITS - 1:0];
    monitor.processorid = processorid;
    monitor.size = size;
    monitor.rawaddress = address;
    monitor.key = key;
    monitor.value = value;
    monitor.value2 = value2;
    monitor.version = version;
    return monitor;
end;

/* Local and global monitor inputs are intentionally independent.  The C++
 * CpuState payload carries only one reservation record, so an adapter must
 * not copy it into both monitors unless a reviewed contract supplies the
 * global state separately. */
func A64Oracle_SetLocalExclusiveMonitorState(valid : boolean,
                                              paspace : PASpace,
                                              address : bits(64),
                                              processorid : integer,
                                              size : integer,
                                              key : bits(64),
                                              value : bits(64),
                                              value2 : bits(64),
                                              version : bits(64))
begin
    A64Oracle_LocalMonitor = A64Oracle_MakeExclusiveMonitorState(
        valid, paspace, address, processorid, size, key, value, value2,
        version
    );
end;

func A64Oracle_SetGlobalExclusiveMonitorState(valid : boolean,
                                               paspace : PASpace,
                                               address : bits(64),
                                               processorid : integer,
                                               size : integer,
                                               key : bits(64),
                                               value : bits(64),
                                               value2 : bits(64),
                                               version : bits(64))
begin
    A64Oracle_GlobalMonitor = A64Oracle_MakeExclusiveMonitorState(
        valid, paspace, address, processorid, size, key, value, value2,
        version
    );
end;

readonly func A64Oracle_GetLocalExclusiveMonitorValid() => boolean
begin
    return A64Oracle_LocalMonitor.valid;
end;

readonly func A64Oracle_GetLocalExclusiveMonitorPASpace() => PASpace
begin
    return A64Oracle_LocalMonitor.paddress.paspace;
end;

readonly func A64Oracle_GetLocalExclusiveMonitorAddress() => bits(64)
begin
    return A64Oracle_LocalMonitor.rawaddress;
end;

readonly func A64Oracle_GetLocalExclusiveMonitorProcessorID() => integer
begin
    return A64Oracle_LocalMonitor.processorid;
end;

readonly func A64Oracle_GetLocalExclusiveMonitorSize() => integer
begin
    return A64Oracle_LocalMonitor.size;
end;

readonly func A64Oracle_GetLocalExclusiveMonitorKey() => bits(64)
begin
    return A64Oracle_LocalMonitor.key;
end;

readonly func A64Oracle_GetLocalExclusiveMonitorValue() => bits(64)
begin
    return A64Oracle_LocalMonitor.value;
end;

readonly func A64Oracle_GetLocalExclusiveMonitorValue2() => bits(64)
begin
    return A64Oracle_LocalMonitor.value2;
end;

readonly func A64Oracle_GetLocalExclusiveMonitorVersion() => bits(64)
begin
    return A64Oracle_LocalMonitor.version;
end;

readonly func A64Oracle_GetGlobalExclusiveMonitorValid() => boolean
begin
    return A64Oracle_GlobalMonitor.valid;
end;

readonly func A64Oracle_GetGlobalExclusiveMonitorPASpace() => PASpace
begin
    return A64Oracle_GlobalMonitor.paddress.paspace;
end;

readonly func A64Oracle_GetGlobalExclusiveMonitorAddress() => bits(64)
begin
    return A64Oracle_GlobalMonitor.rawaddress;
end;

readonly func A64Oracle_GetGlobalExclusiveMonitorProcessorID() => integer
begin
    return A64Oracle_GlobalMonitor.processorid;
end;

readonly func A64Oracle_GetGlobalExclusiveMonitorSize() => integer
begin
    return A64Oracle_GlobalMonitor.size;
end;

readonly func A64Oracle_GetGlobalExclusiveMonitorKey() => bits(64)
begin
    return A64Oracle_GlobalMonitor.key;
end;

readonly func A64Oracle_GetGlobalExclusiveMonitorValue() => bits(64)
begin
    return A64Oracle_GlobalMonitor.value;
end;

readonly func A64Oracle_GetGlobalExclusiveMonitorValue2() => bits(64)
begin
    return A64Oracle_GlobalMonitor.value2;
end;

readonly func A64Oracle_GetGlobalExclusiveMonitorVersion() => bits(64)
begin
    return A64Oracle_GlobalMonitor.version;
end;

readonly func A64Oracle_GetLastAccessPresent() => boolean
begin return A64Oracle_LastAccessPresent; end;

readonly func A64Oracle_GetLastAccessClass() => A64OracleMemoryAccessClass
begin return A64Oracle_LastAccessClass; end;

readonly func A64Oracle_GetLastAccessDomain() => A64OracleMemoryAddressDomain
begin return A64Oracle_LastAccessDomain; end;

readonly func A64Oracle_GetLastAccessPASpace() => PASpace
begin return A64Oracle_LastAccessPASpace; end;

readonly func A64Oracle_GetLastAccessAddress() => bits(64)
begin return A64Oracle_LastAccessAddress; end;

readonly func A64Oracle_GetLastAccessWrite() => boolean
begin
    return A64Oracle_LastAccessClass IN {
        A64OracleMemoryAccessClass_DataWrite,
        A64OracleMemoryAccessClass_TagWrite
    };
end;

readonly func A64Oracle_GetLastAccessByteCount() => integer
begin return A64Oracle_LastAccessByteCount; end;

readonly func A64Oracle_GetLastAccessStatus() => PhysMemRetStatus
begin return A64Oracle_LastAccessStatus; end;

readonly func A64Oracle_GetLastAccessStatusCode() => Fault
begin return A64Oracle_LastAccessStatus.statuscode; end;

readonly func A64Oracle_GetLastAccessExtFlag() => bit
begin return A64Oracle_LastAccessStatus.extflag; end;

readonly func A64Oracle_GetLastAccessErrorState() => ErrorState
begin return A64Oracle_LastAccessStatus.merrorstate; end;

readonly func A64Oracle_GetLastAccessStore64BStatus() => bits(64)
begin return A64Oracle_LastAccessStatus.store64bstatus; end;

readonly func A64Oracle_GetLastFaultPresent() => boolean
begin return A64Oracle_LastFaultPresent; end;

readonly func A64Oracle_GetLastFaultDomain() => A64OracleMemoryAddressDomain
begin return A64Oracle_LastFaultDomain; end;

readonly func A64Oracle_GetLastFaultPASpace() => PASpace
begin return A64Oracle_LastFaultPASpace; end;

readonly func A64Oracle_GetLastFaultAddress() => bits(64)
begin return A64Oracle_LastFaultAddress; end;

readonly func A64Oracle_GetLastFaultStatus() => PhysMemRetStatus
begin return A64Oracle_LastFaultStatus; end;

readonly func A64Oracle_GetLastFaultStatusCode() => Fault
begin return A64Oracle_LastFaultStatus.statuscode; end;

readonly func A64Oracle_GetLastFaultExtFlag() => bit
begin return A64Oracle_LastFaultStatus.extflag; end;

readonly func A64Oracle_GetLastFaultErrorState() => ErrorState
begin return A64Oracle_LastFaultStatus.merrorstate; end;

readonly func A64Oracle_GetLastFaultStore64BStatus() => bits(64)
begin return A64Oracle_LastFaultStatus.store64bstatus; end;

func AArch64_IsExclusiveVA(address : bits(64),
                           processorid : integer,
                           size : integer) => boolean
begin
    return TRUE;
end;

accessor AArch64_MemTag(address : bits(64),
                        accdesc : AccessDescriptor) <=> value : bits(4)
begin
    getter
        let status : PhysMemRetStatus =
            if !accdesc.tagaccess then
                A64Oracle_MakeMemoryStatus(Fault_Permission)
            else
                A64Oracle_VirtualAccessStatus(
                    address, 1, TRUE, FALSE, accdesc
                );
        A64Oracle_RecordAccessDescriptor(accdesc);
        A64Oracle_RecordVirtualAccess(
            A64OracleMemoryAccessClass_TagRead, address, 1, status
        );
        A64Oracle_RecordMemoryEvent(A64OracleMemoryEvent_TagLoad, status);
        if status.statuscode == Fault_None then
            let offset : integer = A64Oracle_VirtualTagOffset(address);
            return A64Oracle_ReadMemoryTag(offset);
        else
            return Zeros{4};
        end;
    end;

    setter
        let aligned : boolean = UInt(address) MOD 16 == 0;
        let status : PhysMemRetStatus =
            if !accdesc.tagaccess then
                A64Oracle_MakeMemoryStatus(Fault_Permission)
            else
                A64Oracle_VirtualAccessStatus(
                    address, 16, aligned, TRUE, accdesc
                );
        A64Oracle_RecordAccessDescriptor(accdesc);
        A64Oracle_RecordVirtualAccess(
            A64OracleMemoryAccessClass_TagWrite, address, 16, status
        );
        A64Oracle_RecordMemoryEvent(A64OracleMemoryEvent_TagStore, status);
        if status.statuscode == Fault_None then
            let offset : integer = A64Oracle_VirtualTagOffset(address);
            A64Oracle_WriteMemoryTag(offset, value);
        end;
    end;
end;

func AddressInNaturallyAlignedBlock(address1 : bits(64),
                                    address2 : bits(64)) => boolean
begin
    return FALSE;
end;

func AddressSupportsLS64(paddress : bits(NUM_PABITS)) => boolean
begin
    return A64Oracle_LS64Supported;
end;

func ChooseRandomNonExcludedTag(exclude_in : bits(16)) => bits(4)
begin
    var chosen : bits(4) = Zeros{4};
    var found : boolean = FALSE;
    for index = 0 to 15 do
        if !found && exclude_in[index] == '0' then
            chosen = index[3:0];
            found = TRUE;
        end;
    end;
    return chosen;
end;

func ClearExclusiveByAddress(paddress : FullAddress,
                             processorid : integer,
                             size : integer)
begin
    if A64Oracle_GlobalMonitor.processorid != processorid &&
       A64Oracle_MonitorOverlaps(A64Oracle_GlobalMonitor, paddress, size) then
        A64Oracle_GlobalMonitor = A64Oracle_ClearMonitor(A64Oracle_GlobalMonitor);
    end;
end;

func ClearExclusiveLocal(processorid : integer)
begin
    if A64Oracle_LocalMonitor.processorid == processorid then
        A64Oracle_LocalMonitor = A64Oracle_ClearMonitor(A64Oracle_LocalMonitor);
    end;
end;

func DataMemoryBarrier(types : MBReqTypes)
begin
    A64Oracle_LastBarrierTypes = types;
    A64Oracle_RecordOrderingEvent(A64OracleMemoryEvent_DataMemoryBarrier);
end;

func DataSynchronizationBarrier(scope : MBMaintenanceScope,
                                types : MBReqTypes,
                                nXS : boolean)
begin
    A64Oracle_LastBarrierScope = scope;
    A64Oracle_LastBarrierTypes = types;
    A64Oracle_LastBarrierNXS = nXS;
    A64Oracle_RecordOrderingEvent(
        A64OracleMemoryEvent_DataSynchronizationBarrier
    );
end;

func ExclusiveMonitorsStatus() => bit
begin
    return A64Oracle_LastExclusiveStatus;
end;

func Hint_Prefetch(address : bits(64),
                   hint : PrefetchHint,
                   target : integer,
                   stream : boolean)
begin
    A64Oracle_LastPrefetchAddress = address;
    A64Oracle_LastPrefetchHint = hint;
    A64Oracle_LastPrefetchTarget = target;
    A64Oracle_LastPrefetchStream = stream;
    A64Oracle_RecordOrderingEvent(A64OracleMemoryEvent_Prefetch);
end;

func Hint_RangePrefetch(address : bits(64),
                        length : integer,
                        stride : integer,
                        count : integer,
                        reuse : integer,
                        operation : bits(6))
begin
    A64Oracle_LastPrefetchAddress = address;
    A64Oracle_LastRangeLength = length;
    A64Oracle_LastRangeStride = stride;
    A64Oracle_LastRangeCount = count;
    A64Oracle_LastRangeReuse = reuse;
    A64Oracle_LastRangeOperation = operation;
    A64Oracle_RecordOrderingEvent(A64OracleMemoryEvent_RangePrefetch);
end;

func IsConventionalMemory(addrdesc : AddressDescriptor) => boolean
begin
    return addrdesc.memattrs.memtype == MemType_Normal;
end;

func IsExclusiveGlobal(paddress : FullAddress,
                       processorid : integer,
                       size : integer) => boolean
begin
    return A64Oracle_MonitorContains(
        A64Oracle_GlobalMonitor, paddress, processorid, size
    );
end;

func IsExclusiveLocal(paddress : FullAddress,
                      processorid : integer,
                      size : integer) => boolean
begin
    return A64Oracle_MonitorContains(
        A64Oracle_LocalMonitor, paddress, processorid, size
    );
end;

func MarkExclusiveGlobal(paddress : FullAddress,
                         processorid : integer,
                         size : integer)
begin
    A64Oracle_GlobalMonitor.valid = TRUE;
    A64Oracle_GlobalMonitor.paddress = paddress;
    A64Oracle_GlobalMonitor.processorid = processorid;
    A64Oracle_GlobalMonitor.size = size;
    A64Oracle_GlobalMonitor.rawaddress =
        Zeros{64 - NUM_PABITS} :: paddress.address;
end;

func MarkExclusiveLocal(paddress : FullAddress,
                        processorid : integer,
                        size : integer)
begin
    A64Oracle_LocalMonitor.valid = TRUE;
    A64Oracle_LocalMonitor.paddress = paddress;
    A64Oracle_LocalMonitor.processorid = processorid;
    A64Oracle_LocalMonitor.size = size;
    A64Oracle_LocalMonitor.rawaddress =
        Zeros{64 - NUM_PABITS} :: paddress.address;
end;

accessor Mem{size : integer{8, 16, 32, 64, 128, 256}}(
    address : bits(64), accdesc_in : AccessDescriptor
) <=> value_in : bits(size)
begin
    getter
        let bytes : integer = size DIV 8;
        let aligned : boolean = A64Oracle_AccessAligned(
            address, bytes, accdesc_in
        );
        let status : PhysMemRetStatus =
            A64Oracle_VirtualAccessStatus(
                address, bytes, aligned, FALSE, accdesc_in
            );
        var value : bits(size) = Zeros{size};
        A64Oracle_RecordAccessDescriptor(accdesc_in);
        A64Oracle_RecordVirtualAccess(
            A64OracleMemoryAccessClass_DataRead, address, bytes, status
        );
        A64Oracle_RecordMemoryEvent(A64OracleMemoryEvent_Load, status);
        if status.statuscode == Fault_None then
            for byte = 0 to bytes - 1 do
                let offset : integer =
                    A64Oracle_VirtualByteOffset(address + byte);
                value[byte * 8 +: 8] = A64Oracle_ReadMemoryByte(offset);
            end;
        end;
        return value;
    end;

    setter
        let bytes : integer = size DIV 8;
        let aligned : boolean = A64Oracle_AccessAligned(
            address, bytes, accdesc_in
        );
        let status : PhysMemRetStatus =
            A64Oracle_VirtualAccessStatus(
                address, bytes, aligned, TRUE, accdesc_in
            );
        A64Oracle_RecordAccessDescriptor(accdesc_in);
        A64Oracle_RecordVirtualAccess(
            A64OracleMemoryAccessClass_DataWrite, address, bytes, status
        );
        A64Oracle_RecordMemoryEvent(A64OracleMemoryEvent_Store, status);
        if status.statuscode == Fault_None then
            for byte = 0 to bytes - 1 do
                let offset : integer =
                    A64Oracle_VirtualByteOffset(address + byte);
                A64Oracle_WriteMemoryByte(
                    offset, value_in[byte * 8 +: 8]
                );
            end;
        end;
    end;
end;

func MemCpyParametersIllformedE(memcpy : CPYParams) => boolean
begin
    return FALSE;
end;

func MemCpyParametersIllformedM(memcpy : CPYParams) => boolean
begin
    return FALSE;
end;

func MemSetParametersIllformedE(memset : SETParams) => boolean
begin
    return FALSE;
end;

func MemSetParametersIllformedM(memset : SETParams) => boolean
begin
    return FALSE;
end;

func PhysMemRead{size : integer{8, 16, 32, 64, 128, 256, 512}}(
    desc : AddressDescriptor,
    accdesc : AccessDescriptor
) => (PhysMemRetStatus, bits(size))
begin
    let bytes : integer = size DIV 8;
    let status : PhysMemRetStatus =
        A64Oracle_PhysicalAccessStatus(desc, bytes, TRUE, FALSE, accdesc);
    var value : bits(size) = Zeros{size};
    A64Oracle_RecordAccessDescriptor(accdesc);
    A64Oracle_RecordPhysicalAccess(
        A64OracleMemoryAccessClass_DataRead,
        desc.paddress,
        bytes,
        status
    );
    A64Oracle_RecordMemoryEvent(A64OracleMemoryEvent_Load, status);
    if status.statuscode == Fault_None then
        for byte = 0 to bytes - 1 do
            var current : FullAddress = desc.paddress;
            current.address = desc.paddress.address + byte;
            let offset : integer = A64Oracle_PhysicalByteOffset(current);
            value[byte * 8 +: 8] = A64Oracle_ReadMemoryByte(offset);
        end;
    end;
    return (status, value);
end;

func PhysMemTagRead(desc : AddressDescriptor,
                    accdesc : AccessDescriptor) => (PhysMemRetStatus, bits(4))
begin
    let address : bits(NUM_PABITS) = desc.paddress.address;
    let aligned : boolean = UInt(address) MOD 16 == 0;
    let status : PhysMemRetStatus =
        if !accdesc.tagaccess then
            A64Oracle_MakeMemoryStatus(Fault_SyncExternal)
        else
            A64Oracle_PhysicalAccessStatus(
                desc, 16, aligned, FALSE, accdesc
            );
    var value : bits(4) = Zeros{4};
    A64Oracle_RecordAccessDescriptor(accdesc);
    A64Oracle_RecordPhysicalAccess(
        A64OracleMemoryAccessClass_TagRead,
        desc.paddress,
        16,
        status
    );
    A64Oracle_RecordMemoryEvent(A64OracleMemoryEvent_TagLoad, status);
    if status.statuscode == Fault_None then
        let offset : integer = A64Oracle_PhysicalTagOffset(desc.paddress);
        value = A64Oracle_ReadMemoryTag(offset);
    end;
    return (status, value);
end;

func PhysMemTagWrite(desc : AddressDescriptor,
                     accdesc : AccessDescriptor,
                     value : bits(4)) => PhysMemRetStatus
begin
    let address : bits(NUM_PABITS) = desc.paddress.address;
    let aligned : boolean = UInt(address) MOD 16 == 0;
    let status : PhysMemRetStatus =
        if !accdesc.tagaccess then
            A64Oracle_MakeMemoryStatus(Fault_SyncExternal)
        else
            A64Oracle_PhysicalAccessStatus(
                desc, 16, aligned, TRUE, accdesc
            );
    A64Oracle_RecordAccessDescriptor(accdesc);
    A64Oracle_RecordPhysicalAccess(
        A64OracleMemoryAccessClass_TagWrite,
        desc.paddress,
        16,
        status
    );
    A64Oracle_RecordMemoryEvent(A64OracleMemoryEvent_TagStore, status);
    if status.statuscode == Fault_None then
        let offset : integer = A64Oracle_PhysicalTagOffset(desc.paddress);
        A64Oracle_WriteMemoryTag(offset, value);
    end;
    return status;
end;

func PhysMemWrite{size : integer{8, 16, 32, 64, 128, 256, 512}}(
    desc : AddressDescriptor,
    accdesc : AccessDescriptor,
    value : bits(size)
) => PhysMemRetStatus
begin
    let bytes : integer = size DIV 8;
    let status : PhysMemRetStatus =
        A64Oracle_PhysicalAccessStatus(desc, bytes, TRUE, TRUE, accdesc);
    A64Oracle_RecordAccessDescriptor(accdesc);
    A64Oracle_RecordPhysicalAccess(
        A64OracleMemoryAccessClass_DataWrite,
        desc.paddress,
        bytes,
        status
    );
    A64Oracle_RecordMemoryEvent(A64OracleMemoryEvent_Store, status);
    if status.statuscode == Fault_None then
        for byte = 0 to bytes - 1 do
            var current : FullAddress = desc.paddress;
            current.address = desc.paddress.address + byte;
            let offset : integer = A64Oracle_PhysicalByteOffset(current);
            A64Oracle_WriteMemoryByte(offset, value[byte * 8 +: 8]);
        end;
        ClearExclusiveByAddress(desc.paddress, ProcessorID(), bytes);
    end;
    return status;
end;

func ProcessorID() => integer
begin
    return A64Oracle_CurrentProcessorID;
end;

func SpeculativeStoreBypassBarrierToPA()
begin
    A64Oracle_RecordOrderingEvent(A64OracleMemoryEvent_StoreBarrierPA);
end;

func SpeculativeStoreBypassBarrierToVA()
begin
    A64Oracle_RecordOrderingEvent(A64OracleMemoryEvent_StoreBarrierVA);
end;

func StopInstructionPrefetchAndEnableITR()
begin
    A64Oracle_InstructionPrefetchStopped = TRUE;
    A64Oracle_RecordOrderingEvent(
        A64OracleMemoryEvent_StopInstructionPrefetch
    );
end;
