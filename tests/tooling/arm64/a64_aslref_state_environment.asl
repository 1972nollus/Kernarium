/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Deterministic EL0 environment adapter for selected Arm-generated A64 ASL.
 *
 * This file supplies test-oracle policy, not architectural truth.  Every
 * implementation-defined result below is an explicit, deterministic choice
 * made so that a specimen can be repeated.  A passing specimen is meaningful
 * only when its expected result is valid under the named policy; these choices
 * must never be cited as architecture-mandated results.
 *
 * Policy summary:
 *   - execution has FEAT_AA64 and FEAT_AA64EL0, with optional features off;
 *   - generic implementation-defined booleans/bits/integers are false/zero;
 *   - constrained-unpredictable booleans choose false, and integer choices
 *     choose Constraint_UNKNOWN with the lower bound;
 *   - implementation-selected data transforms use deterministic XOR folding;
 *   - branch-observation inputs report no misprediction and zero cycles;
 *   - copy/set staging uses no prologue or epilogue and small fixed blocks;
 *   - system-instruction write hooks are no-ops and result hooks write zero;
 *     synchronization, pending-error, and trace hooks have no effect;
 *   - typed registers unrelated to projected CpuState remain read-as-zero
 *     and write-ignored (RAZ/WI); the explicitly named projected-register
 *     subset below instead round-trips its corresponding EL0 state;
 *   - the current instruction is explicit adapter state and defaults to zero.
 *
 * The register type layouts are a mechanically selected subset of Arm's
 * 2026-06 A-profile System Register XML as extracted by the pinned ASLRef
 * v1.0~draft.2 bundler.  Their field layouts are architectural descriptions;
 * the RAZ/WI behavior attached here is solely the adapter policy above.
 * The extracted layouts retain Copyright 2022-2025 Arm Limited and/or its
 * affiliates and the bundler output's BSD-3-Clause terms.
 */

/* Current-instruction input owned by the test adapter. */
var A64Oracle_CurrentInstruction : bits(32) = Zeros{32};

/*
 * Exact scalar projection of the CpuState environment/system fields.  The
 * two fields which carry unused high bits in CpuState retain those bits in
 * shadow storage while official register operations update their
 * architecturally named low fields.  Feature availability remains a separate
 * allocation input; the projected profile booleans can only narrow it.
 */
var A64Oracle_ProjectedSystemStateInstalled : boolean;
var A64Oracle_ThreadPointer : bits(64);
var A64Oracle_ProjectedPSTATEDAIFHigh : bits(4);
var A64Oracle_DAIFAccess : boolean;
var A64Oracle_MTEEnabled : boolean;
var A64Oracle_MTEExclude : bits(16);
var A64Oracle_MTERandomTags : boolean;
var A64Oracle_MTESeed : bits(16);
var A64Oracle_MTETag : bits(8);
var A64Oracle_CPAAddEnabled : boolean;
var A64Oracle_CPAMultiplyEnabled : boolean;
var A64Oracle_ACCDATA : bits(32);
var A64Oracle_GCSPointer : bits(64);
var A64Oracle_GCSImplemented : boolean;
var A64Oracle_GCSPushEnabled : boolean;
var A64Oracle_GCSStoreEnabled : boolean;
var A64Oracle_SigningKeyLow : array [[5]] of bits(64);
var A64Oracle_SigningKeyHigh : array [[5]] of bits(64);
var A64Oracle_SigningEnabled : boolean;

/*
 * Explicit virtual-profile inputs for emulator-exposed non-CpuState system
 * registers.  Counter reads form a deterministic, saturating monotonic
 * sequence; no terminal expected state is consulted to produce a value.
 */
constant A64Oracle_DCZIDEL0Value : bits(64) =
    '0000000000000000000000000000000000000000000000000000000000000100';
constant A64Oracle_CTREl0Value : bits(64) =
    '0000000000000000000000000000000010000100010001001000000000000100';
var A64Oracle_CounterFrequency : bits(64);
var A64Oracle_CounterNext : bits(64);
var A64Oracle_CounterStep : bits(64);
var A64Oracle_CounterReadCount : integer;

/*
 * A specimen enables only the optional features named by its checked
 * allocation predicate.  The fixed bound is deliberately larger than the
 * maximum two-feature conjunction in the pinned 2026-06 allocation model.
 * It is an oracle input, not a claim about a host or architectural reset.
 */
constant A64Oracle_MaxEnabledFeatures : integer{} = 8;
var A64Oracle_EnabledFeatures : array [[8]] of Feature;
var A64Oracle_EnabledFeatureCount : integer;

func A64Oracle_SetCurrentInstruction(value : bits(32))
begin
    A64Oracle_CurrentInstruction = value;
end;

func A64Oracle_EnableFeature(feature : Feature)
begin
    var already_enabled : boolean = FALSE;
    var index : integer = 0;
    while index < A64Oracle_EnabledFeatureCount looplimit 8 do
        already_enabled = already_enabled ||
            A64Oracle_EnabledFeatures[[index]] == feature;
        index = index + 1;
    end;
    if !already_enabled then
        assert A64Oracle_EnabledFeatureCount < A64Oracle_MaxEnabledFeatures;
        A64Oracle_EnabledFeatures[[A64Oracle_EnabledFeatureCount]] = feature;
        A64Oracle_EnabledFeatureCount = A64Oracle_EnabledFeatureCount + 1;
    end;
end;

/*
 * PSTATE is the exact 2026-06 shared-pseudocode shape.  Initialization below
 * is the adapter's deterministic AArch64-EL0 choice, not an architectural
 * reset-state claim.
 */
var PSTATE : collection {
    N : bits(1),
    Z : bits(1),
    C : bits(1),
    V : bits(1),
    D : bits(1),
    A : bits(1),
    I : bits(1),
    F : bits(1),
    EXLOCK : bits(1),
    PAN : bits(1),
    UAO : bits(1),
    DIT : bits(1),
    TCO : bits(1),
    PM : bits(1),
    BTYPE : bits(2),
    PACM : bits(1),
    ZA : bits(1),
    SM : bits(1),
    ALLINT : bits(1),
    UINJ : bits(1),
    SS : bits(1),
    IL : bits(1),
    EL : bits(2),
    nRW : bits(1),
    SP : bits(1),
    Q : bits(1),
    GE : bits(4),
    SSBS : bits(1),
    IT : bits(8),
    J : bits(1),
    T : bits(1),
    E : bits(1),
    M : bits(5)
};

/*
 * Width/count choices required by the released pseudocode but not declared
 * by the instruction/shared XML archive.  Sixteen is the maximal ASID/VMID
 * width represented by the A-profile register layouts used by this adapter.
 * The PMU count is a finite typechecking choice; no PMU behavior is claimed.
 */
constant NUM_ASIDBITS : integer{} = 16;
constant NUM_VMIDBITS : integer{} = 16;
constant NUM_PMU_COUNTERS : integer{} = 31;

func A64Oracle_ResetStateEnvironment()
begin
    PSTATE.N = '0';
    PSTATE.Z = '0';
    PSTATE.C = '0';
    PSTATE.V = '0';
    PSTATE.D = '0';
    PSTATE.A = '0';
    PSTATE.I = '0';
    PSTATE.F = '0';
    PSTATE.EXLOCK = '0';
    PSTATE.PAN = '0';
    PSTATE.UAO = '0';
    PSTATE.DIT = '0';
    PSTATE.TCO = '0';
    PSTATE.PM = '0';
    PSTATE.BTYPE = '00';
    PSTATE.PACM = '0';
    PSTATE.ZA = '0';
    PSTATE.SM = '0';
    PSTATE.ALLINT = '0';
    PSTATE.UINJ = '0';
    PSTATE.SS = '0';
    PSTATE.IL = '0';
    PSTATE.EL = '00';
    PSTATE.nRW = '0';
    PSTATE.SP = '0';
    PSTATE.Q = '0';
    PSTATE.GE = '0000';
    PSTATE.SSBS = '0';
    PSTATE.IT = '00000000';
    PSTATE.J = '0';
    PSTATE.T = '0';
    PSTATE.E = '0';
    PSTATE.M = '00000';
    A64Oracle_CurrentInstruction = Zeros{32};
    A64Oracle_EnabledFeatureCount = 0;
    A64Oracle_ProjectedSystemStateInstalled = FALSE;
    A64Oracle_ThreadPointer = Zeros{64};
    A64Oracle_ProjectedPSTATEDAIFHigh = Zeros{4};
    A64Oracle_DAIFAccess = FALSE;
    A64Oracle_MTEEnabled = FALSE;
    A64Oracle_MTEExclude = Zeros{16};
    A64Oracle_MTERandomTags = FALSE;
    A64Oracle_MTESeed = '0000000000000001';
    A64Oracle_MTETag = Zeros{8};
    A64Oracle_CPAAddEnabled = FALSE;
    A64Oracle_CPAMultiplyEnabled = FALSE;
    A64Oracle_ACCDATA = Zeros{32};
    A64Oracle_GCSPointer = Zeros{64};
    A64Oracle_GCSImplemented = FALSE;
    A64Oracle_GCSPushEnabled = FALSE;
    A64Oracle_GCSStoreEnabled = FALSE;
    for key = 0 to 4 do
        A64Oracle_SigningKeyLow[[key]] = Zeros{64};
        A64Oracle_SigningKeyHigh[[key]] = Zeros{64};
    end;
    A64Oracle_SigningEnabled = TRUE;
    A64Oracle_CounterFrequency =
        '0000000000000000000000000000000000111011100110101100101000000000';
    A64Oracle_CounterNext = Zeros{64};
    A64Oracle_CounterStep = Zeros{63} :: '1';
    A64Oracle_CounterReadCount = 0;
end;

/* Keep the base adapter's portable nibble and architectural PSTATE in sync. */
func A64Oracle_SetArchitecturalNZCV(value : bits(4))
begin
    A64Oracle_SetNZCV(value);
    PSTATE.N = value[3];
    PSTATE.Z = value[2];
    PSTATE.C = value[1];
    PSTATE.V = value[0];
end;

readonly func A64Oracle_GetArchitecturalNZCV() => bits(4)
begin
    return PSTATE.N :: PSTATE.Z :: PSTATE.C :: PSTATE.V;
end;

readonly func A64Oracle_IsSigningFeature(feature : Feature) => boolean
begin
    return feature IN {
        FEAT_PAuth, FEAT_PAuth2, FEAT_PAuth_EnhCtl, FEAT_PAuth_LR,
        FEAT_CONSTPACFIELD, FEAT_FPAC, FEAT_FPACCOMBINE,
        FEAT_PACIMP, FEAT_PACQARMA3, FEAT_PACQARMA5
    };
end;

readonly func A64Oracle_IsAllocationFeatureEnabled(feature : Feature)
    => boolean
begin
    if feature IN {FEAT_AA64, FEAT_AA64EL0} then return TRUE; end;
    var index : integer = 0;
    while index < A64Oracle_EnabledFeatureCount looplimit 8 do
        if A64Oracle_EnabledFeatures[[index]] == feature then return TRUE; end;
        index = index + 1;
    end;
    return FALSE;
end;

/* Adapter choice: baseline AArch64 EL0 plus explicitly enabled features. */
readonly func IsFeatureImplemented(feature : Feature) => boolean
begin
    let allocated : boolean =
        A64Oracle_IsAllocationFeatureEnabled(feature);
    if !A64Oracle_ProjectedSystemStateInstalled then return allocated; end;
    if feature == FEAT_GCS then
        return allocated && A64Oracle_GCSImplemented;
    elsif A64Oracle_IsSigningFeature(feature) then
        return allocated && A64Oracle_SigningEnabled;
    else
        return allocated;
    end;
end;

/*
 * Reviewed implementation-defined projections for the pinned 2026-06 shared
 * pseudocode inventory.  Unknown descriptors assert instead of inheriting a
 * blanket false/zero choice.  The Python preflight independently verifies the
 * complete owner/context digest, the declared result ranges, and conservative
 * reachability from the generated batch main().
 */
readonly func ImpDefBool(description : string) => boolean
begin
    if description == "AT reports the HW update fault" then return FALSE; end;
    if description == "AT updates AF" then return FALSE; end;
    if description == "Apply effective shareability at stage 1" then return FALSE; end;
    if description == "BADDR expresses 52 bits for 64KB granule" then return FALSE; end;
    if description == "CPY in the forward direction" then return FALSE; end;
    if description == "CPY* instructions use Option A" then return FALSE; end;
    if description == "DCIMVAC generates watchpoint" then return FALSE; end;
    if description == "Device location supports unaligned access" then return FALSE; end;
    if description == "EDWAR is not valid on watchpoint debug event" then return FALSE; end;
    if description == "EL3 trap priority when SDD == '1'" then return FALSE; end;
    if description == "External Abort on HDBSS write reported in HDBSSPROD_EL2" then return FALSE; end;
    if description == "FAR is not valid on watchpoint exception" then return FALSE; end;
    if description == "FEAT_LSE2: access is atomic" then return FALSE; end;
    if description == "Fault on TxSZ value above maximum" then return FALSE; end;
    if description == "Fault on TxSZ value below minimum" then return FALSE; end;
    if description == "GPC Fault on DC operations" then return FALSE; end;
    if description == "GPC fault on HDBSSS write reported in HDBSSPROD_EL2" then return FALSE; end;
    if description == "GPF, GPC, Data SyncExternal Aborts, TagChecks prioritized over Watchpoints" then return FALSE; end;
    if description == "Generate access flag fault on IC/DC operations" then return FALSE; end;
    if description == "HCR_EL2.NV1 is implemented as RAZ" then return FALSE; end;
    if description == "Implementation option is checked with a cpysize of 0" then return FALSE; end;
    if description == "Implementation option is checked with a stage cpysize of 0" then return FALSE; end;
    if description == "Instruction SyncExternal Aborts prioritized over Breakpoints" then return FALSE; end;
    if description == "LD64B or ST64B faults to cacheable non-iWBoWB memory" then return FALSE; end;
    if description == "MPAMSM_EL1 label precedence" then return FALSE; end;
    if description == "PAN applies when S1UnprivBasePerm is reserved" then return FALSE; end;
    if description == "PAN applies when S1UnprivBasePerms are removed" then return FALSE; end;
    if description == "Permission fault on EL0 IC_IVAU execution" then return FALSE; end;
    if description == "Programming HCR_EL2.<NV,NV2> to '10' behaves as '11'" then return FALSE; end;
    if description == "Realm EL2&0 regime affects EPAN" then return FALSE; end;
    if description == "Report SPE ExtAbort as SError" then return FALSE; end;
    if description == "Report SPE ExtAbort asynchronously" then return FALSE; end;
    if description == "Report SPE ExtAbort on TTW as fault" then return FALSE; end;
    if description == "Report TRBE ExtAbort as SError" then return FALSE; end;
    if description == "Report TRBE ExtAbort asynchronously" then return FALSE; end;
    if description == "SCR_EL3.PIEn forces PIR_ELx or POR_ELx to zero" then return FALSE; end;
    if description == "SCR_EL3.SIF affects EPAN" then return FALSE; end;
    if description == "SPE 16bit counters" then return FALSE; end;
    if description == "SPE PAT for tag unchecked access zero" then return FALSE; end;
    if description == "SPE get prev br if not br" then return FALSE; end;
    if description == "SPE non-tbi tag is zero" then return FALSE; end;
    if description == "SPE prev br on eret" then return FALSE; end;
    if description == "SPE prev br on exception" then return FALSE; end;
    if description == "SPE prev br on isb" then return FALSE; end;
    if description == "Secure-only implementation" then return FALSE; end;
    if description == "Shared SMCU" then return FALSE; end;
    if description == "Translation fault on misprogrammed contiguous bit" then return FALSE; end;
    if description == "Treat Contiguous bit as 0" then return FALSE; end;
    if description == "WPTV field is valid" then return FALSE; end;
    if description == "descriptor[15:12] for 64KB granule are OA[51:48]" then return FALSE; end;
    if description == "nT bit causes Translation Fault" then return FALSE; end;
    assert FALSE;
    return FALSE;
end;

readonly func ImpDefInt(description : string) => integer
begin
    if description == "Aligned quantity for atomic access" then return 4096; end;
    if description == "Maximum Physical Address Size" then return 48; end;
    if description == "Number of BRBE Branch records" then return 8; end;
    if description == "Number of address-linking breakpoints" then return 0; end;
    if description == "Number of breakpoints" then return 2; end;
    if description == "Number of context-aware breakpoints" then return 0; end;
    if description == "Number of watchpoints" then return 2; end;
    if description == "SPE Data Source packet payload size" then return 1; end;
    if description == "SPE Events packet payload size" then return 1; end;
    assert FALSE;
    return 0;
end;

readonly func ImpDefBits{N}(description : string) => bits(N)
begin
    assert N == 2;
    var result : bits(N) = Zeros{N};
    if description == "TG0 encoded granule size" then return result; end;
    if description == "TG1 encoded granule size" then
        result[N - 1] = '1';
        return result;
    end;
    if description == "target_el for SPE ImpDef Fault" ||
       description == "target_el for TRBE ImpDef Fault" then
        result[0] = '1';
        return result;
    end;
    assert FALSE;
    return result;
end;

readonly func ImpDefBit(description : string) => bits(1)
begin
    if description == "WPF value on TRUE Watchpoint match" then return '0'; end;
    assert FALSE;
    return '0';
end;

/* Adapter choice: false for a boolean constrained-unpredictable result. */
func ConstrainUnpredictableBool(which : Unpredictable) => boolean
begin
    return FALSE;
end;

/* Adapter choice: deterministic UNKNOWN data at the permitted lower bound. */
func ConstrainUnpredictableInteger(low : integer,
                                   high : integer,
                                   which : Unpredictable)
    => (Constraint, integer)
begin
    assert low <= high;
    return (Constraint_UNKNOWN, low);
end;

/* Adapter choices: stable, non-architectural transforms for repeatable tests. */
func ComputePACIMPDEF(data : bits(64), modifier : bits(64),
                      key0 : bits(64), key1 : bits(64)) => bits(64)
begin
    return data XOR modifier XOR key0 XOR key1;
end;

func ComputePAC2IMPDEF(data : bits(64),
                       modifier1 : bits(64), modifier2 : bits(64),
                       key0 : bits(64), key1 : bits(64)) => bits(64)
begin
    return data XOR modifier1 XOR modifier2 XOR key0 XOR key1;
end;

/* Adapter choice: system writes are ignored and system results read as zero. */
func AArch64_SysInstr(op0 : bits(2), op1 : bits(3),
                      crn : bits(4), crm : bits(4), op2 : bits(3),
                      t : integer)
begin
    return;
end;

func AArch64_SysInstr128(op0 : bits(2), op1 : bits(3),
                         crn : bits(4), crm : bits(4), op2 : bits(3),
                         t : integer, t2 : integer)
begin
    return;
end;

func AArch64_SysInstrWithResult(op0 : bits(2), op1 : bits(3),
                                crn : bits(4), crm : bits(4),
                                op2 : bits(3), t : integer)
begin
    X{64}(t) = Zeros{64};
end;

/* Adapter choices for implementation-supplied branch observations. */
func BranchMispredict() => boolean
begin
    return FALSE;
end;

func BranchRawCycleCount() => integer
begin
    return 0;
end;

func FirstBranchAfterProhibited() => boolean
begin
    return FALSE;
end;

/* Adapter choice: callers do not classify the current operation as vector. */
func IsASEInstruction() => boolean
begin
    return FALSE;
end;

/* Adapter choice: no physical fault address is reportable. */
func IsPFARValid(fault : FaultRecord) => boolean
begin
    return FALSE;
end;

/* Adapter choice: direct specimens have no modeled sequential fetch address. */
func NextInstrAddr{N}() => bits(N)
begin
    return Zeros{N};
end;

/*
 * Adapter choices for copy/set staging: Option-B copy reserves at most one
 * configured 64-byte block for the epilogue, including every byte when fewer
 * remain.  This is a byte-aligned fixed point, so a restarted epilogue selects
 * its entire remaining size and makes forward progress.  Copy iterations use
 * one byte; set iterations use the caller's supported 1- or 16-byte alignment.
 * These are legal deterministic selections, not mandates.
 */
func CPYPreSizeChoice(memcpy : CPYParams) => integer
begin
    return 0;
end;

func CPYPostSizeChoice(memcpy : CPYParams) => integer
begin
    assert !memcpy.implements_option_a;
    assert memcpy.stage IN {MOPSStage_Main, MOPSStage_Epilogue};
    assert memcpy.cpysize >= 0 && memcpy.cpysize <= ArchMaxMOPSCPYSize;
    return Min(memcpy.cpysize, 64);
end;

func CPYSizeChoice(memcpy : CPYParams) => MOPSBlockSize
begin
    return 1;
end;

func SETPreSizeChoice(memset : SETParams) => integer
begin
    return 0;
end;

func SETPostSizeChoice(memset : SETParams) => integer
begin
    return 0;
end;

func SETSizeChoice(memset : SETParams,
                   alignsize : integer) => MOPSBlockSize
begin
    assert alignsize IN {1, 16};
    return 16 as MOPSBlockSize;
end;

/* Adapter choice: data-source metadata is unavailable and reads as zero. */
func SPEGetDataSource(is_load : boolean,
                      accdesc : AccessDescriptor,
                      addrdesc : AddressDescriptor) => (boolean, bits(16))
begin
    return (FALSE, Zeros{16});
end;

/* Adapter choices: no asynchronous error, synchronization, or trace source. */
func SynchronizeContext()
begin
    return;
end;

func SynchronizeErrors()
begin
    return;
end;

func TakeUnmaskedPhysicalSErrorInterrupts(iesb_req : boolean)
begin
    return;
end;

func TakeUnmaskedSErrorInterrupts()
begin
    return;
end;

func TraceUnitFlush()
begin
    return;
end;

/* The current instruction is explicit deterministic adapter state. */
func ThisInstr() => bits(32)
begin
    return A64Oracle_CurrentInstruction;
end;

/* Reserved encodings stop the specimen through a typed local exception. */
type A64Oracle_ReservedEncodingException of exception {-};

noreturn func ReservedEncoding()
begin
    throw A64Oracle_ReservedEncodingException {-};
end;

/* Adapter choice: cross-reference annotations do not affect execution. */
func See(reference : string)
begin
    return;
end;

/*
 * Real, RoundDown, RoundUp, and SqrtRounded are provided by the pinned
 * ASLRef standard library.  They are queue-classification omissions, not
 * environment interfaces, and must not be redeclared here.
 *
 * case, do, looplimit, of, var, and while are ASL grammar tokens incorrectly
 * captured by lexical call extraction.  They are explicitly unresolved as
 * queue artifacts because no legal ASL declaration can implement a keyword.
 */

/*
 * Typed system-register adapter subset.
 *
 * Most accessors below retain the explicit non-architectural RAZ/WI policy.
 * Accessors whose comments name projected CpuState instead round-trip only
 * those fields.  The copied type layouts preserve official field widths.
 */

/* Projected CpuState.tls and the emulator profile's read-only reset slot. */
type TPIDR_EL0_Type of bits(64) {
  [63:0] ThreadID,
};

accessor TPIDR_EL0() <=> value : TPIDR_EL0_Type
begin
    readonly getter
        return A64Oracle_ThreadPointer;
    end;

    setter
        A64Oracle_ThreadPointer = value;
    end;
end;

type TPIDRRO_EL0_Type of bits(64) {
  [63:0] ThreadID,
};

accessor TPIDRRO_EL0() <=> value : TPIDRRO_EL0_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Projected CpuState.accdata storage. */
type ACCDATA_EL1_Type of bits(64) {
  [31:0] ACCDATA,
};

accessor ACCDATA_EL1() <=> value : ACCDATA_EL1_Type
begin
    readonly getter
        var result : ACCDATA_EL1_Type = Zeros{64};
        result.ACCDATA = A64Oracle_ACCDATA;
        return result;
    end;

    setter
        A64Oracle_ACCDATA = value.ACCDATA;
    end;
end;

/* Projected CpuState signing key 2 (data A), high half. */
type APDAKeyHi_EL1_Type of bits(64) {
  [63:0] APDAKeyHi,
};

accessor APDAKeyHi_EL1() <=> value : APDAKeyHi_EL1_Type
begin
    readonly getter
        return A64Oracle_SigningKeyHigh[[2]];
    end;

    setter
        A64Oracle_SigningKeyHigh[[2]] = value;
    end;
end;

/* Projected CpuState signing key 2 (data A), low half. */
type APDAKeyLo_EL1_Type of bits(64) {
  [63:0] APDAKeyLo,
};

accessor APDAKeyLo_EL1() <=> value : APDAKeyLo_EL1_Type
begin
    readonly getter
        return A64Oracle_SigningKeyLow[[2]];
    end;

    setter
        A64Oracle_SigningKeyLow[[2]] = value;
    end;
end;

/* Projected CpuState signing key 3 (data B), high half. */
type APDBKeyHi_EL1_Type of bits(64) {
  [63:0] APDBKeyHi,
};

accessor APDBKeyHi_EL1() <=> value : APDBKeyHi_EL1_Type
begin
    readonly getter
        return A64Oracle_SigningKeyHigh[[3]];
    end;

    setter
        A64Oracle_SigningKeyHigh[[3]] = value;
    end;
end;

/* Projected CpuState signing key 3 (data B), low half. */
type APDBKeyLo_EL1_Type of bits(64) {
  [63:0] APDBKeyLo,
};

accessor APDBKeyLo_EL1() <=> value : APDBKeyLo_EL1_Type
begin
    readonly getter
        return A64Oracle_SigningKeyLow[[3]];
    end;

    setter
        A64Oracle_SigningKeyLow[[3]] = value;
    end;
end;

/* Projected CpuState signing key 4 (generic), high half. */
type APGAKeyHi_EL1_Type of bits(64) {
  [63:0] APGAKeyHi,
};

accessor APGAKeyHi_EL1() <=> value : APGAKeyHi_EL1_Type
begin
    readonly getter
        return A64Oracle_SigningKeyHigh[[4]];
    end;

    setter
        A64Oracle_SigningKeyHigh[[4]] = value;
    end;
end;

/* Projected CpuState signing key 4 (generic), low half. */
type APGAKeyLo_EL1_Type of bits(64) {
  [63:0] APGAKeyLo,
};

accessor APGAKeyLo_EL1() <=> value : APGAKeyLo_EL1_Type
begin
    readonly getter
        return A64Oracle_SigningKeyLow[[4]];
    end;

    setter
        A64Oracle_SigningKeyLow[[4]] = value;
    end;
end;

/* Projected CpuState signing key 0 (instruction A), high half. */
type APIAKeyHi_EL1_Type of bits(64) {
  [63:0] APIAKeyHi,
};

accessor APIAKeyHi_EL1() <=> value : APIAKeyHi_EL1_Type
begin
    readonly getter
        return A64Oracle_SigningKeyHigh[[0]];
    end;

    setter
        A64Oracle_SigningKeyHigh[[0]] = value;
    end;
end;

/* Projected CpuState signing key 0 (instruction A), low half. */
type APIAKeyLo_EL1_Type of bits(64) {
  [63:0] APIAKeyLo,
};

accessor APIAKeyLo_EL1() <=> value : APIAKeyLo_EL1_Type
begin
    readonly getter
        return A64Oracle_SigningKeyLow[[0]];
    end;

    setter
        A64Oracle_SigningKeyLow[[0]] = value;
    end;
end;

/* Projected CpuState signing key 1 (instruction B), high half. */
type APIBKeyHi_EL1_Type of bits(64) {
  [63:0] APIBKeyHi,
};

accessor APIBKeyHi_EL1() <=> value : APIBKeyHi_EL1_Type
begin
    readonly getter
        return A64Oracle_SigningKeyHigh[[1]];
    end;

    setter
        A64Oracle_SigningKeyHigh[[1]] = value;
    end;
end;

/* Projected CpuState signing key 1 (instruction B), low half. */
type APIBKeyLo_EL1_Type of bits(64) {
  [63:0] APIBKeyLo,
};

accessor APIBKeyLo_EL1() <=> value : APIBKeyLo_EL1_Type
begin
    readonly getter
        return A64Oracle_SigningKeyLow[[1]];
    end;

    setter
        A64Oracle_SigningKeyLow[[1]] = value;
    end;
end;

/* Adapter policy for BRBCR_EL1: read as zero, writes ignored. */
type BRBCR_EL1_Type of bits(64) {
  [23] EXCEPTION,
  [22] ERTN,
  [9] FZPSS  /* When FEAT_PMUv3_SS is implemented */,
  [8] FZP  /* When FEAT_PMUv3 is implemented */,
  [6:5] TS,
  [4] MPRED,
  [3] CC,
  [1] E1BRE,
  [0] E0BRE,
};

accessor BRBCR_EL1() <=> value : BRBCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for BRBCR_EL2: read as zero, writes ignored. */
type BRBCR_EL2_Type of bits(64) {
  [23] EXCEPTION,
  [22] ERTN,
  [9] FZPSS  /* When FEAT_PMUv3_SS is implemented */,
  [8] FZP  /* When FEAT_PMUv3 is implemented */,
  [6:5] TS,
  [4] MPRED,
  [3] CC,
  [1] E2BRE,
  [0] E0HBRE,
};

accessor BRBCR_EL2() <=> value : BRBCR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for BRBFCR_EL1: read as zero, writes ignored. */
type BRBFCR_EL1_Type of bits(64) {
  [29:28] BANK,
  [22] CONDDIR,
  [21] DIRCALL,
  [20] INDCALL,
  [19] RTN,
  [18] INDIRECT,
  [17] DIRECT,
  [16] EnI,
  [7] PAUSED,
};

accessor BRBFCR_EL1() <=> value : BRBFCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for BRBIDR0_EL1: read as zero, writes ignored. */
type BRBIDR0_EL1_Type of bits(64) {
  [15:12] CC,
  [11:8] FORMAT,
  [7:0] NUMREC,
};

accessor BRBIDR0_EL1() <=> value : BRBIDR0_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for CONTEXTIDR_EL1: read as zero, writes ignored. */
type CONTEXTIDR_EL1_Type of bits(64) {
  [31:0] PROCID,
};

accessor CONTEXTIDR_EL1() <=> value : CONTEXTIDR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for CONTEXTIDR_EL2: read as zero, writes ignored. */
type CONTEXTIDR_EL2_Type of bits(64) {
  [31:0] PROCID,
};

accessor CONTEXTIDR_EL2() <=> value : CONTEXTIDR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for CPACR: read as zero, writes ignored. */
type CPACR_Type of bits(32) {
  [31] ASEDIS,
  [28] TRCDIS,
  [23:22] cp11,
  [21:20] cp10,
};

accessor CPACR() <=> value : CPACR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/*
 * Adapter policy for CPACR_EL1: explicitly enabled execution features are
 * accessible at EL0; all unrelated fields read as zero and writes are
 * ignored.  This keeps a checked feature partition executable without
 * silently treating a disabled coprocessor as an instruction result.
 */
type CPACR_EL1_Type of bits(64) {
  [33] E0TP1E  /* When FEAT_TPS is implemented */,
  [32] E0TP0E  /* When FEAT_TPS is implemented */,
  [31] TCPAC  /* When FEAT_NV2p1 is implemented */,
  [30] TAM  /* When FEAT_AMUv1 is implemented and FEAT_NV2p1 is implemented */,
  [29] E0POE  /* When FEAT_S1POE is implemented */,
  [28] TTA  /* When System register access to the trace unit registers is implemented */,
  [25:24] SMEN  /* When FEAT_SME is implemented */,
  [21:20] FPEN,
  [17:16] ZEN  /* When FEAT_SVE is implemented */,
};

accessor CPACR_EL1() <=> value : CPACR_EL1_Type
begin
    readonly getter
        var result : CPACR_EL1_Type = Zeros{64};
        if IsFeatureImplemented(FEAT_FP) ||
           IsFeatureImplemented(FEAT_AdvSIMD) then
            result.FPEN = '11';
        end;
        if IsFeatureImplemented(FEAT_SVE) then
            result.ZEN = '11';
        end;
        if IsFeatureImplemented(FEAT_SME) then
            result.SMEN = '11';
        end;
        return result;
    end;

    setter
        return;
    end;
end;

/* Adapter policy for CPTR_EL2: read as zero, writes ignored. */
type CPTR_EL2_Type of bits(64) {
    /* When EffectiveHCR_EL2_E2H() == '1' i.e. EffectiveHCR_EL2_E2H() == '1' or EffectiveHCR_EL2_E2H() == '0' */
  [33] E0TP1E  /* When FEAT_TPS is implemented */,
  [32] E0TP0E  /* When FEAT_TPS is implemented */,
  [31] TCPAC,
  [30] TAM  /* When FEAT_AMUv1 is implemented */,
  [29] E0POE  /* When FEAT_S1POE is implemented */,
  [20] TTA  /* When System register access to the trace unit registers is implemented */,
  [25:24] SMEN  /* When FEAT_SME is implemented */,
  [21:20] FPEN,
  [17:16] ZEN  /* When FEAT_SVE is implemented */,
  [12] TSM  /* When FEAT_SME is implemented */,
  [10] TFP,
  [8] TZ  /* When FEAT_SVE is implemented */,
};

accessor CPTR_EL2() <=> value : CPTR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for CPTR_EL3: read as zero, writes ignored. */
type CPTR_EL3_Type of bits(64) {
  [31] TCPAC,
  [30] TAM  /* When FEAT_AMUv1 is implemented */,
  [20] TTA  /* When System register access to the trace unit registers is implemented */,
  [12] ESM  /* When FEAT_SME is implemented */,
  [10] TFP,
  [8] EZ  /* When FEAT_SVE is implemented */,
};

accessor CPTR_EL3() <=> value : CPTR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DBGBCR_EL1: read as zero, writes ignored. */
type DBGBCR_EL1_Type of bits(64) {
  [31:30] LBNX  /* When FEAT_Debugv8p9 is implemented */,
  [29] SSCE  /* When FEAT_RME is implemented */,
  [28:24] MASK  /* When FEAT_BWE is implemented */,
  [23:20] BT,
  [19:16] LBN,
  [15:14] SSC,
  [13] HMC,
  [8:5] BAS  /* When FEAT_AA32 is implemented */,
  [3] BT2  /* When FEAT_ABLE is implemented and breakpoint n supports address breakpoint linking */,
  [2:1] PMC,
  [0] E,
};

accessor DBGBCR_EL1(i: integer) <=> value : DBGBCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DBGBVR_EL1: read as zero, writes ignored. */
type DBGBVR_EL1_Type of bits(64)  /* When DBGBCR<n>_EL1.BT IN {'000x'} i.e. DBGBCR<n>_EL1.BT==0b000x or When DBGBCR<n>_EL1.BT IN {'001x'} i.e. DBGBCR<n>_EL1.BT==0b001x or When DBGBCR<n>_EL1.BT IN {'011x'}, EL2 is implemented, and FEAT_Debugv8p1 is implemented i.e. DBGBCR<n>_EL1.BT==0b011x, EL2 is implemented, and FEAT_Debugv8p2 is implemented or When DBGBCR<n>_EL1.BT IN {'100x'} and EL2 is implemented i.e. DBGBCR<n>_EL1.BT==0b100x and EL2 is implemented or When DBGBCR<n>_EL1.BT IN {'101x'} and EL2 is implemented i.e. DBGBCR<n>_EL1.BT==0b101x and EL2 is implemented or When DBGBCR<n>_EL1.BT IN {'110x'}, EL2 is implemented, and FEAT_Debugv8p1 is implemented i.e. DBGBCR<n>_EL1.BT==0b110x, EL2 is implemented, and FEAT_Debugv8p1 is implemented or When DBGBCR<n>_EL1.BT IN {'111x'}, EL2 is implemented, and FEAT_Debugv8p1 is implemented i.e. DBGBCR<n>_EL1.BT==0b111x, EL2 is implemented, and FEAT_Debugv8p1 is implemented */;

accessor DBGBVR_EL1(i: integer) <=> value : DBGBVR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DBGOSDLR: read as zero, writes ignored. */
type DBGOSDLR_Type of bits(32) {
  [0] DLK  /* When FEAT_DoubleLock is implemented */,
};

accessor DBGOSDLR() <=> value : DBGOSDLR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DBGOSLSR: read as zero, writes ignored. */
type DBGOSLSR_Type of bits(32) {
  [3, 0] OSLM,
  [2] nTT,
  [1] OSLK,
};

accessor DBGOSLSR() <=> value : DBGOSLSR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DBGPRCR: read as zero, writes ignored. */
type DBGPRCR_Type of bits(32) {
  [0] CORENPDRQ  /* Otherwise or When FEAT_DoPD is implemented */,
};

accessor DBGPRCR() <=> value : DBGPRCR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DBGPRCR_EL1: read as zero, writes ignored. */
type DBGPRCR_EL1_Type of bits(64) {
  [0] CORENPDRQ  /* Otherwise or When FEAT_DoPD is implemented */,
};

accessor DBGPRCR_EL1() <=> value : DBGPRCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DBGWCR_EL1: read as zero, writes ignored. */
type DBGWCR_EL1_Type of bits(64) {
  [31:30] LBNX  /* When FEAT_Debugv8p9 is implemented */,
  [29] SSCE  /* When FEAT_RME is implemented */,
  [28:24] MASK,
  [22] WT2  /* When FEAT_BWE2 is implemented */,
  [20] WT,
  [19:16] LBN,
  [15:14] SSC,
  [13] HMC,
  [12:5] BAS,
  [4:3] LSC,
  [2:1] PAC,
  [0] E,
};

accessor DBGWCR_EL1(i: integer) <=> value : DBGWCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DBGWVR_EL1: read as zero, writes ignored. */
type DBGWVR_EL1_Type of bits(64) {
  [63:57, 56:53, 52:49] RESS  /* Otherwise */,
  [56:53, 52:49, 48:2] VA  /* When FEAT_LVA3 is implemented or When FEAT_LVA is implemented */,
};

accessor DBGWVR_EL1(i: integer) <=> value : DBGWVR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DCZID_EL0: read as zero, writes ignored. */
type DCZID_EL0_Type of bits(64) {
  [8:5] TBS  /* When FEAT_MTETC is implemented */,
  [4] DZP,
  [3:0] BS,
};

accessor DCZID_EL0() <=> value : DCZID_EL0_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DISR: read as zero, writes ignored. */
type DISR_Type of bits(32) {
    /* When the ESB instruction is executed at EL2 or When the ESB instruction is executed at EL0 or EL1 and TTBCR.EAE == '0' or When the ESB instruction is executed at EL0 or EL1 and TTBCR.EAE == '1' */
  [31] A,
};

accessor DISR() <=> value : DISR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DISR_EL1: read as zero, writes ignored. */
type DISR_EL1_Type of bits(64) {
    /* When DISR_EL1.IDS == '0' or When DISR_EL1.IDS == '1' */
  [31] A,
  [24] IDS,
};

accessor DISR_EL1() <=> value : DISR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DLR: read as zero, writes ignored. */
type DLR_Type of bits(32) {
  [31:0] ADDR,
};

accessor DLR() <=> value : DLR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DLR_EL0: read as zero, writes ignored. */
type DLR_EL0_Type of bits(64) {
  [63:0] ADDR,
};

accessor DLR_EL0() <=> value : DLR_EL0_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DSPSR: read as zero, writes ignored. */
type DSPSR_Type of bits(32) {
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [27] Q,
  [26:25, 15:10] IT,
  [24] DIT  /* When FEAT_DIT is implemented */,
  [23] SSBS  /* When FEAT_SSBS is implemented */,
  [22] PAN  /* When FEAT_PAN is implemented */,
  [21] SS,
  [20] IL,
  [19:16] GE,
  [9] E,
  [8] A,
  [7] I,
  [6] F,
  [5] T,
  [4:0] M,
};

accessor DSPSR() <=> value : DSPSR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DSPSR2: read as zero, writes ignored. */
type DSPSR2_Type of bits(32) {
  [4] UINJ  /* When FEAT_UINJ is implemented */,
};

accessor DSPSR2() <=> value : DSPSR2_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for DSPSR_EL0: read as zero, writes ignored. */
type DSPSR_EL0_Type of bits(64) {
    /* When FEAT_AA32 is implemented and exiting Debug state to AArch32 state i.e. exiting Debug state to AArch32 state or When FEAT_AA64 is implemented and entering or exiting Debug state from or to AArch64 state i.e. entering or exiting Debug state from or to AArch64 state */
  [36] UINJ  /* When FEAT_UINJ is implemented */,
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [24] DIT  /* When FEAT_DIT is implemented */,
  [22] PAN  /* When FEAT_PAN is implemented */,
  [21] SS,
  [20] IL,
  [8] A,
  [7] I,
  [6] F,
  [4, 3:0] M,
};

accessor DSPSR_EL0() <=> value : DSPSR_EL0_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for EDECCR: read as zero, writes ignored. */
type EDECCR_Type of bits(32) {
  [22] RLR2  /* When FEAT_RME is implemented */,
  [21] RLR1  /* When FEAT_RME is implemented */,
  [20] RLR0  /* When FEAT_RME is implemented */,
  [18] RLE2  /* When FEAT_RME is implemented */,
  [17] RLE1  /* When FEAT_RME is implemented */,
  [16] RLE0,
  [15] NSR3,
  [14] NSR2  /* When FEAT_Debugv8p2 is implemented and Non-secure EL2 is implemented */,
  [13] NSR1  /* When FEAT_Debugv8p2 is implemented and Non-secure EL1 is implemented */,
  [12] NSR0  /* When FEAT_Debugv8p2 is implemented and Non-secure EL0 is implemented */,
  [11] SR3  /* When FEAT_Debugv8p2 is implemented and EL3 is implemented */,
  [10] SR2  /* When FEAT_Debugv8p2 is implemented and FEAT_SEL2 is implemented */,
  [9] SR1  /* When FEAT_Debugv8p2 is implemented and Secure EL1 is implemented */,
  [8] SR0  /* When FEAT_Debugv8p2 is implemented and Secure EL0 is implemented */,
  [7] NSE3,
  [6] NSE2  /* When Non-secure EL2 is implemented or When FEAT_Debugv8p2 is implemented and Non-secure EL2 is implemented */,
  [5] NSE1  /* When Non-secure EL1 is implemented or When FEAT_Debugv8p2 is implemented and Non-secure EL1 is implemented */,
  [4] NSE0,
  [3] SE3  /* When FEAT_Debugv8p2 is not implemented and EL3 is implemented or When FEAT_Debugv8p2 is implemented and EL3 is implemented */,
  [2] SE2  /* When FEAT_Debugv8p2 is implemented and FEAT_SEL2 is implemented */,
  [1] SE1  /* When Secure EL1 is implemented or When FEAT_Debugv8p2 is implemented and Secure EL1 is implemented */,
  [0] SE0,
};

accessor EDECCR() <=> value : EDECCR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for EDECR: read as zero, writes ignored. */
type EDECR_Type of bits(32) {
  [6] TRBE  /* When FEAT_Debugv8p9 is implemented and FEAT_TRBE_EXT is implemented */,
  [5] TRCE  /* When FEAT_ETEv1p3 is implemented and FEAT_Debugv8p9 is implemented */,
  [4] PME  /* When FEAT_Debugv8p9 is implemented and FEAT_PMUv3p9 is implemented */,
  [2] SS,
  [1] RCE  /* When FEAT_DoPD is not implemented */,
  [0] OSUCE  /* When FEAT_DoPD is not implemented */,
};

accessor EDECR() <=> value : EDECR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for EDESR: read as zero, writes ignored. */
type EDESR_Type of bits(32) {
  [3] EC  /* When FEAT_Debugv8p8 is implemented */,
  [2] SS  /* Otherwise or When FEAT_DoPD is implemented */,
  [1] RC,
  [0] OSUC,
};

accessor EDESR() <=> value : EDESR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for EDHSR: read as zero, writes ignored. */
type EDHSR_Type of bits(64) {
  [40] GCS  /* When FEAT_GCS is implemented and FEAT_Debugv8p9 is implemented */,
  [23:18] WPT,
  [17] WPTV,
  [16] WPF,
  [15] FnP,
  [13] VNCR  /* When FEAT_Debugv8p9 is implemented */,
  [10] FnV,
  [8] CM  /* When FEAT_Debugv8p9 is implemented */,
  [6] WnR  /* When FEAT_Debugv8p9 is implemented */,
};

accessor EDHSR() <=> value : EDHSR_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for EDPRSR: read as zero, writes ignored. */
type EDPRSR_Type of bits(32) {
  [16] EPMADE  /* When FEAT_RME is implemented and FEAT_PMUv3_EXT is implemented */,
  [15] ETADE  /* When FEAT_RME is implemented, FEAT_TRC_EXT is implemented, and FEAT_TRBE is implemented */,
  [14] EDADE  /* When FEAT_RME is implemented */,
  [13] STAD  /* When FEAT_TRC_EXT is implemented and FEAT_TRBE is implemented */,
  [12] ETAD  /* When FEAT_TRC_EXT is implemented and FEAT_TRBE is implemented or When FEAT_RME is implemented, FEAT_TRC_EXT is implemented, and FEAT_TRBE is implemented */,
  [11] SDR,
  [10] SPMAD  /* When FEAT_PMUv3_EXT is implemented or When FEAT_Debugv8p4 is implemented and FEAT_PMUv3_EXT is implemented */,
  [9] EPMAD  /* When FEAT_PMUv3_EXT is implemented or When FEAT_Debugv8p4 is implemented and FEAT_PMUv3_EXT is implemented or When FEAT_RME is implemented and FEAT_PMUv3_EXT is implemented */,
  [8] SDAD  /* Otherwise or When FEAT_Debugv8p4 is implemented */,
  [7] EDAD  /* Otherwise or When FEAT_Debugv8p2 is implemented or When FEAT_Debugv8p4 is implemented or When FEAT_RME is implemented */,
  [6] DLK  /* When FEAT_DoubleLock is implemented or When FEAT_Debugv8p2 is implemented and FEAT_DoubleLock is implemented or When FEAT_Debugv8p4 is implemented */,
  [5] OSLK,
  [4] HALTED,
  [3] SR,
  [2] R,
  [1] SPD,
  [0] PU  /* Otherwise or When FEAT_Debugv8p2 is implemented or When FEAT_DoPD is implemented */,
};

accessor EDPRSR() <=> value : EDPRSR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for EDSCR: read as zero, writes ignored. */
type EDSCR_Type of bits(32) {
  [31] TFO  /* When FEAT_TRF is implemented */,
  [30] RXfull,
  [29] TXfull,
  [28] ITO,
  [27] RXO,
  [26] TXU,
  [25] PipeAdv,
  [24] ITE,
  [23:22] INTdis  /* Otherwise or When FEAT_Debugv8p4 is implemented or When FEAT_RME is implemented */,
  [21] TDA,
  [20] MA,
  [19] SC2  /* When FEAT_PCSRv8 is implemented, EL2 is implemented, FEAT_Debugv8p1 is implemented, and FEAT_PCSRv8p2 is not implemented */,
  [18] NS  /* Otherwise or When FEAT_RME is implemented */,
  [16] SDD  /* When FEAT_Secure is implemented or When FEAT_RME is implemented */,
  [15] NSE  /* When FEAT_RME is implemented */,
  [14] HDE,
  [13:10] RW,
  [9:8] EL,
  [7] A,
  [6] ERR,
  [5:0] STATUS,
};

accessor EDSCR() <=> value : EDSCR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for EDSCR2: read as zero, writes ignored. */
type EDSCR2_Type of bits(32) {
  [3] EHBWE  /* When FEAT_Debugv8p9 is implemented */,
  [1] TTA  /* When FEAT_TRBE_EXT is implemented or FEAT_ETEv1p3 is implemented */,
};

accessor EDSCR2() <=> value : EDSCR2_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for EDWAR: read as zero, writes ignored. */
type EDWAR_Type of bits(64) {
  [63:0] ADDR,
};

accessor EDWAR() <=> value : EDWAR_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for ELR_EL1: read as zero, writes ignored. */
type ELR_EL1_Type of bits(64) {
  [63:0] ADDR,
};

accessor ELR_EL1() <=> value : ELR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for ELR_EL2: read as zero, writes ignored. */
type ELR_EL2_Type of bits(64) {
  [63:0] ADDR,
};

accessor ELR_EL2() <=> value : ELR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for ELR_EL3: read as zero, writes ignored. */
type ELR_EL3_Type of bits(64) {
  [63:0] ADDR,
};

accessor ELR_EL3() <=> value : ELR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for ELR_hyp: read as zero, writes ignored. */
type ELR_hyp_Type of bits(32) {
  [31:0] ADDR,
};

accessor ELR_hyp() <=> value : ELR_hyp_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for ESR_EL1: read as zero, writes ignored. */
type ESR_EL1_Type of bits(64) {
  [55:32] ISS2 {
    [23:17] DPOIndex  /* When FEAT_S1POE2 is implemented */,
    [16] OverlayFetch  /* When FEAT_S1POE2 is implemented */,
    [15:13] LST2  /* When FEAT_EAESR is implemented */,
    [12] Unpriv  /* When FEAT_EAESR is implemented */,
    [11] TPLIM  /* When FEAT_TPS is implemented, !IsSecondStage(Fault), and GetESR_ELx_DFSC(EL1) IN {'0011xx'} */,
    [11] HDBSSF  /* When FEAT_HDBSS is implemented, FEAT_NV is implemented, IsSecondStage(Fault), and GetESR_ELx_DFSC(EL1) IN {'0011xx', '01001x', '0101xx', '10001x', '1001xx'} */,
    [10] TnD  /* When FEAT_MTE_CANONICAL_TAGS is implemented */,
    [9] TagAccess  /* When (FEAT_MTE_PERM is implemented and FEAT_NV is implemented) or FEAT_VMTE is implemented */,
    [8] GCS  /* When FEAT_GCS is implemented */,
    [7] AssuredOnly  /* When FEAT_THE is implemented and FEAT_NV is implemented */,
    [6] Overlay  /* When FEAT_S1POE is implemented */,
    [5] DirtyBit  /* When FEAT_S1PIE is implemented */,
    [4:0] Xs  /* When FEAT_LS64 is implemented */,
  },
  [31:26] EC,
  [25] IL,
  [24:0] ISS,
};

accessor ESR_EL1() <=> value : ESR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for ESR_EL2: read as zero, writes ignored. */
type ESR_EL2_Type of bits(64) {
  [55:32] ISS2 {
    [23:17] DPOIndex  /* When FEAT_S1POE2 is implemented */,
    [16] OverlayFetch  /* When FEAT_S1POE2 is implemented */,
    [15:13] LST2  /* When FEAT_EAESR is implemented */,
    [12] Unpriv  /* When FEAT_EAESR is implemented */,
    [11] TPLIM  /* When FEAT_TPS is implemented, !IsSecondStage(Fault), and GetESR_ELx_DFSC(EL2) IN {'0011xx'} */,
    [11] HDBSSF  /* When FEAT_HDBSS is implemented, IsSecondStage(Fault), and GetESR_ELx_DFSC(EL2) IN {'0011xx', '01001x', '0101xx', '10001x', '1001xx'} */,
    [10] TnD  /* When FEAT_MTE_CANONICAL_TAGS is implemented */,
    [9] TagAccess  /* When FEAT_MTE_PERM is implemented or FEAT_VMTE is implemented */,
    [8] GCS  /* When FEAT_GCS is implemented */,
    [7] AssuredOnly  /* When FEAT_THE is implemented */,
    [6] Overlay  /* When FEAT_S1POE is implemented or FEAT_S2POE is implemented */,
    [5] DirtyBit  /* When FEAT_S1PIE is implemented or FEAT_S2PIE is implemented */,
    [4:0] Xs  /* When FEAT_LS64 is implemented */,
  },
  [31:26] EC,
  [25] IL,
  [24:0] ISS,
};

accessor ESR_EL2() <=> value : ESR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for ESR_EL3: read as zero, writes ignored. */
type ESR_EL3_Type of bits(64) {
  [55:32] ISS2 {
    [23:17] DPOIndex  /* When FEAT_S1POE2 is implemented */,
    [16] OverlayFetch  /* When FEAT_S1POE2 is implemented */,
    [15:13] LST2  /* When FEAT_EAESR is implemented */,
    [12] Unpriv  /* When FEAT_EAESR is implemented */,
    [11] TPLIM  /* When FEAT_TPS is implemented, !IsSecondStage(Fault), and GetESR_ELx_DFSC(EL3) IN {'0011xx'} */,
    [11] HDBSSF  /* When FEAT_HDBSS is implemented, IsSecondStage(Fault), and GetESR_ELx_DFSC(EL3) IN {'0011xx', '01001x', '0101xx', '10001x', '1001xx'} */,
    [10] TnD  /* When FEAT_MTE_CANONICAL_TAGS is implemented */,
    [9] TagAccess  /* When FEAT_VMTE is implemented */,
    [8] GCS  /* When FEAT_GCS is implemented */,
    [6] Overlay  /* When FEAT_S1POE is implemented */,
    [5] DirtyBit  /* When FEAT_S1PIE is implemented */,
    [4:0] Xs  /* When FEAT_LS64 is implemented */,
  },
  [31:26] EC,
  [25] IL,
  [24:0] ISS,
};

accessor ESR_EL3() <=> value : ESR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for FPSCR: read as zero, writes ignored. */
type FPSCR_Type of bits(32) {
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [27] QC,
  [26] AHP,
  [25] DN,
  [24] FZ,
  [23:22] RMode,
  [21:20] Stride,
  [19] FZ16  /* When FEAT_FP16 is implemented */,
  [18:16] Len,
  [15] IDE,
  [12] IXE,
  [11] UFE,
  [10] OFE,
  [9] DZE,
  [8] IOE,
  [7] IDC,
  [4] IXC,
  [3] UFC,
  [2] OFC,
  [1] DZC,
  [0] IOC,
};

accessor FPSCR() <=> value : FPSCR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Projected MTE exclusion mask and random-selection control. */
type GCR_EL1_Type of bits(64) {
  [16] RRND,
  [15:0] Exclude,
};

accessor GCR_EL1() <=> value : GCR_EL1_Type
begin
    readonly getter
        var result : GCR_EL1_Type = Zeros{64};
        result.RRND = if A64Oracle_MTERandomTags then '1' else '0';
        result.Exclude = A64Oracle_MTEExclude;
        return result;
    end;

    setter
        A64Oracle_MTERandomTags = value.RRND == '1';
        A64Oracle_MTEExclude = value.Exclude;
    end;
end;

/* Projected EL0 guarded-stack push/store controls. */
type GCSCRE0_EL1_Type of bits(64) {
  [10] nTR,
  [9] STREn,
  [8] PUSHMEn,
  [5] RVCHKEN,
  [0] PCRSEL,
};

accessor GCSCRE0_EL1() <=> value : GCSCRE0_EL1_Type
begin
    readonly getter
        var result : GCSCRE0_EL1_Type = Zeros{64};
        result.STREn = if A64Oracle_GCSStoreEnabled then '1' else '0';
        result.PUSHMEn = if A64Oracle_GCSPushEnabled then '1' else '0';
        result.PCRSEL = if A64Oracle_GCSImplemented then '1' else '0';
        return result;
    end;

    setter
        A64Oracle_GCSStoreEnabled = value.STREn == '1';
        A64Oracle_GCSPushEnabled = value.PUSHMEn == '1';
    end;
end;

/* Adapter policy for GCSCR_EL1: read as zero, writes ignored. */
type GCSCR_EL1_Type of bits(64) {
  [9] STREn,
  [8] PUSHMEn,
  [6] EXLOCKEN,
  [5] RVCHKEN,
  [0] PCRSEL,
};

accessor GCSCR_EL1() <=> value : GCSCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for GCSCR_EL2: read as zero, writes ignored. */
type GCSCR_EL2_Type of bits(64) {
  [9] STREn,
  [8] PUSHMEn,
  [6] EXLOCKEN,
  [5] RVCHKEN,
  [0] PCRSEL,
};

accessor GCSCR_EL2() <=> value : GCSCR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for GCSCR_EL3: read as zero, writes ignored. */
type GCSCR_EL3_Type of bits(64) {
  [9] STREn,
  [8] PUSHMEn,
  [6] EXLOCKEN,
  [5] RVCHKEN,
  [0] PCRSEL,
};

accessor GCSCR_EL3() <=> value : GCSCR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Projected EL0 guarded-stack pointer; unnamed low bits are retained. */
type GCSPR_EL0_Type of bits(64) {
  [63:3] PTR,
};

accessor GCSPR_EL0() <=> value : GCSPR_EL0_Type
begin
    readonly getter
        return A64Oracle_GCSPointer;
    end;

    setter
        A64Oracle_GCSPointer = value;
    end;
end;

/* Adapter policy for GCSPR_EL1: read as zero, writes ignored. */
type GCSPR_EL1_Type of bits(64) {
  [63:3] PTR,
};

accessor GCSPR_EL1() <=> value : GCSPR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for GCSPR_EL2: read as zero, writes ignored. */
type GCSPR_EL2_Type of bits(64) {
  [63:3] PTR,
};

accessor GCSPR_EL2() <=> value : GCSPR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for GCSPR_EL3: read as zero, writes ignored. */
type GCSPR_EL3_Type of bits(64) {
  [63:3] PTR,
};

accessor GCSPR_EL3() <=> value : GCSPR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for GMID_EL1: read as zero, writes ignored. */
type GMID_EL1_Type of bits(64) {
  [3:0] BS,
};

accessor GMID_EL1() <=> value : GMID_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for GPCBW_EL3: read as zero, writes ignored. */
type GPCBW_EL3_Type of bits(64) {
  [39:37] BWSIZE,
  [36:32] BWSTRIDE,
  [25:0] BWADDR,
};

accessor GPCBW_EL3() <=> value : GPCBW_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for GPCCR_EL3: read as zero, writes ignored. */
type GPCCR_EL3_Type of bits(64) {
  [29] GPCBW  /* When FEAT_RME_GPC3 is implemented */,
  [28] NA7  /* When FEAT_RME_GDI is implemented */,
  [27] NA6  /* When FEAT_RME_GDI is implemented */,
  [26] NSP  /* When FEAT_RME_GDI is implemented */,
  [25] SA  /* When FEAT_RME_GDI is implemented */,
  [24] APPSAA  /* When FEAT_RME_GPC2 is implemented */,
  [23:20] L0GPTSZ,
  [19] NSO  /* When FEAT_RME_GPC2 is implemented */,
  [18] TBGPCD  /* When FEAT_TRBE_EXT is implemented */,
  [17] GPCP,
  [16] GPC,
  [15:14] PGS,
  [13:12] SH,
  [11:10] ORGN,
  [9:8] IRGN,
  [7] SPAD  /* When FEAT_RME_GPC2 is implemented */,
  [6] NSPAD  /* When FEAT_RME_GPC2 is implemented */,
  [5] RLPAD  /* When FEAT_RME_GPC2 is implemented */,
  [3] PPS3  /* When FEAT_RME_GPC3 is implemented */,
  [2:0] PPS  /* Otherwise or When FEAT_RME_GPC3 is implemented */,
};

accessor GPCCR_EL3() <=> value : GPCCR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for GPTBR_EL3: read as zero, writes ignored. */
type GPTBR_EL3_Type of bits(64) {
  [43:40, 39:0] BADDR  /* When FEAT_RME_GPC3 is implemented */,
};

accessor GPTBR_EL3() <=> value : GPTBR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HCPTR: read as zero, writes ignored. */
type HCPTR_Type of bits(32) {
  [31] TCPAC,
  [30] TAM  /* When FEAT_AMUv1 is implemented */,
  [20] TTA,
  [15] TASE,
  [11] TCP11  /* When FEAT_FP is implemented and FEAT_AdvSIMD is implemented */,
  [10] TCP10  /* When FEAT_FP is implemented and FEAT_AdvSIMD is implemented */,
};

accessor HCPTR() <=> value : HCPTR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HCR: read as zero, writes ignored. */
type HCR_Type of bits(32) {
  [30] TRVM,
  [29] HCD  /* When EL3 is not implemented */,
  [27] TGE,
  [26] TVM,
  [25] TTLB,
  [24] TPU,
  [23] TPC,
  [22] TSW,
  [21] TAC,
  [20] TIDCP,
  [19] TSC,
  [18] TID3,
  [17] TID2,
  [16] TID1,
  [15] TID0,
  [14] TWE,
  [13] TWI,
  [12] DC,
  [11:10] BSU,
  [9] FB,
  [8] VA,
  [7] VI,
  [6] VF,
  [5] AMO,
  [4] IMO,
  [3] FMO,
  [2] PTW,
  [1] SWIO,
  [0] VM,
};

accessor HCR() <=> value : HCR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HCR2: read as zero, writes ignored. */
type HCR2_Type of bits(32) {
  [22] TTLBIS  /* When FEAT_EVT is implemented */,
  [20] TOCU  /* When FEAT_EVT is implemented */,
  [18] TICAB  /* When FEAT_EVT is implemented */,
  [17] TID4  /* When FEAT_EVT is implemented */,
  [5] TEA  /* When FEAT_RAS is implemented */,
  [4] TERR  /* When FEAT_RAS is implemented */,
  [1] ID,
  [0] CD,
};

accessor HCR2() <=> value : HCR2_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HCRX_EL2: read as zero, writes ignored. */
type HCRX_EL2_Type of bits(64) {
  [39] VTCO  /* When FEAT_VMTETC is implemented */,
  [38] VTAO  /* When FEAT_VMTE is implemented */,
  [37] VTE  /* When FEAT_VMTE is implemented */,
  [36] FNB  /* Otherwise or When FEAT_TLBID is implemented */,
  [35] VTLBIDOSEn  /* When FEAT_TLBID is implemented */,
  [34] NVnTTLBOS  /* When FEAT_NV3 is implemented */,
  [33] NVnTTLBIS  /* When FEAT_NV3 is implemented */,
  [32] NVnTTLB  /* When FEAT_NV3 is implemented */,
  [31] FDIT  /* When FEAT_FDIT is implemented */,
  [30] TPLIMEn  /* When FEAT_TPS is implemented */,
  [29] POE2En  /* When FEAT_S1POE2 is implemented */,
  [27] NVTGE  /* When FEAT_NV3 is implemented */,
  [26] SRMASKEn  /* When FEAT_SRMASK is implemented */,
  [25] VTLBIDEn  /* When FEAT_TLBID is implemented */,
  [24] PACMEn  /* When FEAT_PAuth_LR is implemented */,
  [23] EnFPM  /* When FEAT_FPMR is implemented */,
  [22] GCSEn  /* When FEAT_GCS is implemented */,
  [21] EnIDCP128  /* When FEAT_SYSREG128 is implemented */,
  [20] EnSDERR  /* When FEAT_ADERR is implemented */,
  [19] TMEA  /* When FEAT_DoubleFault2 is implemented */,
  [18] EnSNERR  /* When FEAT_ANERR is implemented */,
  [17] D128En  /* When FEAT_D128 is implemented */,
  [16] PTTWI  /* When FEAT_THE is implemented */,
  [15] SCTLR2En  /* When FEAT_SCTLR2 is implemented */,
  [14] TCR2En  /* When FEAT_TCR2 is implemented */,
  [11] MSCEn  /* When FEAT_MOPS is implemented */,
  [10] MCE2  /* When FEAT_MOPS is implemented */,
  [9] CMOW  /* When FEAT_CMOW is implemented */,
  [8] VFNMI  /* When FEAT_NMI is implemented */,
  [7] VINMI  /* When FEAT_NMI is implemented */,
  [6] TALLINT  /* When FEAT_NMI is implemented */,
  [5] SMPME  /* When FEAT_SME is implemented */,
  [4] FGTnXS  /* When FEAT_XS is implemented */,
  [3] FnXS  /* When FEAT_XS is implemented */,
  [2] EnASR  /* When FEAT_LS64_V is implemented */,
  [1] EnALS  /* When FEAT_LS64 is implemented */,
  [0] EnAS0  /* When FEAT_LS64_ACCDATA is implemented */,
};

accessor HCRX_EL2() <=> value : HCRX_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HCR_EL2: read as zero, writes ignored. */
type HCR_EL2_Type of bits(64) {
  [63:60] TWEDEL  /* When FEAT_TWED is implemented */,
  [59] TWEDEn  /* When FEAT_TWED is implemented */,
  [58] TID5  /* When FEAT_MTE2 is implemented */,
  [57] DCT  /* When FEAT_MTE2 is implemented */,
  [56] ATA  /* When FEAT_MTE2 is implemented */,
  [55] TTLBOS  /* When FEAT_EVT2 is implemented */,
  [54] TTLBIS  /* When FEAT_EVT2 is implemented */,
  [53] EnSCXT  /* When FEAT_CSV2_2 is implemented or FEAT_CSV2_1p2 is implemented */,
  [52] TOCU  /* When FEAT_EVT is implemented */,
  [51] AMVOFFEN  /* When FEAT_AMUv1p1 is implemented */,
  [50] TICAB  /* When FEAT_EVT is implemented */,
  [49] TID4  /* When FEAT_EVT is implemented */,
  [48] GPF  /* When FEAT_RME is implemented */,
  [47] FIEN  /* When FEAT_RASv1p1 is implemented */,
  [46] FWB  /* When FEAT_S2FWB is implemented */,
  [45] NV2  /* When FEAT_NV2 is implemented */,
  [44] AT  /* When FEAT_NV is implemented */,
  [43] NV1  /* When FEAT_NV is implemented or When FEAT_NV2 is implemented */,
  [42] NV  /* When FEAT_NV is implemented or When FEAT_NV2 is implemented */,
  [41] API  /* When FEAT_PAuth is implemented */,
  [40] APK  /* When FEAT_PAuth is implemented */,
  [37] TEA  /* When FEAT_RAS is implemented */,
  [36] TERR  /* When FEAT_RAS is implemented */,
  [35] TLOR  /* When FEAT_LOR is implemented */,
  [34] E2H  /* When FEAT_VHE is implemented or When FEAT_VHE is implemented and FEAT_E2H0 is implemented */,
  [33] ID,
  [32] CD,
  [31] RW  /* When FEAT_AA32EL1 is implemented */,
  [30] TRVM,
  [29] HCD  /* When EL3 is not implemented */,
  [28] TDZ,
  [27] TGE,
  [26] TVM,
  [25] TTLB,
  [24] TPU,
  [23] TPCP,
  [22] TSW,
  [21] TACR,
  [20] TIDCP,
  [19] TSC,
  [18] TID3,
  [17] TID2,
  [16] TID1,
  [15] TID0  /* When FEAT_AA32 is implemented */,
  [14] TWE,
  [13] TWI,
  [12] DC,
  [11:10] BSU,
  [9] FB  /* Otherwise or When FEAT_TLBID is implemented */,
  [8] VSE,
  [7] VI,
  [6] VF,
  [5] AMO,
  [4] IMO,
  [3] FMO,
  [2] PTW,
  [1] SWIO,
  [0] VM,
};

accessor HCR_EL2() <=> value : HCR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HDBSSBR_EL2: read as zero, writes ignored. */
type HDBSSBR_EL2_Type of bits(64) {
  [55:12] BADDR,
  [3:0] SZ,
};

accessor HDBSSBR_EL2() <=> value : HDBSSBR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HDBSSPROD_EL2: read as zero, writes ignored. */
type HDBSSPROD_EL2_Type of bits(64) {
  [31:26] FSC,
  [18:0] INDEX,
};

accessor HDBSSPROD_EL2() <=> value : HDBSSPROD_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HDCR: read as zero, writes ignored. */
type HDCR_Type of bits(32) {
  [29] HPMFZO  /* When FEAT_PMUv3p7 is implemented */,
  [28] MTPME  /* When FEAT_MTPMU is implemented and EL3 is not implemented */,
  [27] TDCC  /* When FEAT_FGT is implemented */,
  [26] HLP  /* When FEAT_PMUv3p5 is implemented */,
  [23] HCCD  /* When FEAT_PMUv3p5 is implemented */,
  [19] TTRF  /* When FEAT_TRF is implemented */,
  [17] HPMD  /* When FEAT_PMUv3p1 is implemented or When FEAT_PMUv3p1 is implemented and FEAT_Debugv8p2 is implemented */,
  [11] TDRA,
  [10] TDOSA  /* Otherwise or When FEAT_DoubleLock is implemented */,
  [9] TDA,
  [8] TDE,
  [7] HPME  /* When FEAT_PMUv3 is implemented */,
  [6] TPM  /* When FEAT_PMUv3 is implemented */,
  [5] TPMCR  /* When FEAT_PMUv3 is implemented */,
  [4:0] HPMN  /* When FEAT_PMUv3 is implemented */,
};

accessor HDCR() <=> value : HDCR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HFGITR2_EL2: read as zero, writes ignored. */
type HFGITR2_EL2_Type of bits(64) {
  [14] DCGBVA  /* When FEAT_MTETC is implemented */,
  [13] PLBIPERMAE1  /* When FEAT_S1POE2 is implemented */,
  [12] PLBIPERMAE1IS  /* When FEAT_S1POE2 is implemented */,
  [11] PLBIPERMAE1OS  /* When FEAT_S1POE2 is implemented */,
  [10] PLBIVMALLE1  /* When FEAT_S1POE2 is implemented */,
  [9] PLBIASIDE1  /* When FEAT_S1POE2 is implemented */,
  [8] PLBIPERME1  /* When FEAT_S1POE2 is implemented */,
  [7] PLBIVMALLE1IS  /* When FEAT_S1POE2 is implemented */,
  [6] PLBIASIDE1IS  /* When FEAT_S1POE2 is implemented */,
  [5] PLBIPERME1IS  /* When FEAT_S1POE2 is implemented */,
  [4] PLBIVMALLE1OS  /* When FEAT_S1POE2 is implemented */,
  [3] PLBIASIDE1OS  /* When FEAT_S1POE2 is implemented */,
  [2] PLBIPERME1OS  /* When FEAT_S1POE2 is implemented */,
  [1] nDCCIVAPS  /* When FEAT_PoPS is implemented */,
  [0] TSBCSYNC  /* When FEAT_TRBEv1p1 is implemented */,
};

accessor HFGITR2_EL2() <=> value : HFGITR2_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HFGITR_EL2: read as zero, writes ignored. */
type HFGITR_EL2_Type of bits(64) {
  [63] PSBCSYNC  /* When FEAT_SPEv1p5 is implemented */,
  [62] ATS1E1A  /* When FEAT_ATS1A is implemented */,
  [60] COSPRCTX  /* When FEAT_SPECRES2 is implemented */,
  [59] nGCSEPP  /* When FEAT_GCS is implemented */,
  [58] nGCSSTR_EL1  /* When FEAT_GCS is implemented */,
  [57] nGCSPUSHM_EL1  /* When FEAT_GCS is implemented */,
  [56] nBRBIALL  /* When FEAT_BRBE is implemented */,
  [55] nBRBINJ  /* When FEAT_BRBE is implemented */,
  [54] DCCVAC,
  [53] SVC_EL1,
  [52] SVC_EL0,
  [51] ERET,
  [50] CPPRCTX  /* When FEAT_SPECRES is implemented */,
  [49] DVPRCTX  /* When FEAT_SPECRES is implemented */,
  [48] CFPRCTX  /* When FEAT_SPECRES is implemented */,
  [47] TLBIVAALE1,
  [46] TLBIVALE1,
  [45] TLBIVAAE1,
  [44] TLBIASIDE1,
  [43] TLBIVAE1,
  [42] TLBIVMALLE1,
  [41] TLBIRVAALE1  /* When FEAT_TLBIRANGE is implemented */,
  [40] TLBIRVALE1  /* When FEAT_TLBIRANGE is implemented */,
  [39] TLBIRVAAE1  /* When FEAT_TLBIRANGE is implemented */,
  [38] TLBIRVAE1  /* When FEAT_TLBIRANGE is implemented */,
  [37] TLBIRVAALE1IS  /* When FEAT_TLBIRANGE is implemented */,
  [36] TLBIRVALE1IS  /* When FEAT_TLBIRANGE is implemented */,
  [35] TLBIRVAAE1IS  /* When FEAT_TLBIRANGE is implemented */,
  [34] TLBIRVAE1IS  /* When FEAT_TLBIRANGE is implemented */,
  [33] TLBIVAALE1IS,
  [32] TLBIVALE1IS,
  [31] TLBIVAAE1IS,
  [30] TLBIASIDE1IS,
  [29] TLBIVAE1IS,
  [28] TLBIVMALLE1IS,
  [27] TLBIRVAALE1OS  /* When FEAT_TLBIRANGE is implemented and FEAT_TLBIOS is implemented */,
  [26] TLBIRVALE1OS  /* When FEAT_TLBIRANGE is implemented and FEAT_TLBIOS is implemented */,
  [25] TLBIRVAAE1OS  /* When FEAT_TLBIRANGE is implemented and FEAT_TLBIOS is implemented */,
  [24] TLBIRVAE1OS  /* When FEAT_TLBIRANGE is implemented and FEAT_TLBIOS is implemented */,
  [23] TLBIVAALE1OS  /* When FEAT_TLBIOS is implemented */,
  [22] TLBIVALE1OS  /* When FEAT_TLBIOS is implemented */,
  [21] TLBIVAAE1OS  /* When FEAT_TLBIOS is implemented */,
  [20] TLBIASIDE1OS  /* When FEAT_TLBIOS is implemented */,
  [19] TLBIVAE1OS  /* When FEAT_TLBIOS is implemented */,
  [18] TLBIVMALLE1OS  /* When FEAT_TLBIOS is implemented */,
  [17] ATS1E1WP  /* When FEAT_PAN2 is implemented */,
  [16] ATS1E1RP  /* When FEAT_PAN2 is implemented */,
  [15] ATS1E0W,
  [14] ATS1E0R,
  [13] ATS1E1W,
  [12] ATS1E1R,
  [11] DCZVA,
  [10] DCCIVAC,
  [9] DCCVADP  /* When FEAT_DPB2 is implemented */,
  [8] DCCVAP,
  [7] DCCVAU,
  [6] DCCISW,
  [5] DCCSW,
  [4] DCISW,
  [3] DCIVAC,
  [2] ICIVAU,
  [1] ICIALLU,
  [0] ICIALLUIS,
};

accessor HFGITR_EL2() <=> value : HFGITR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HSCTLR: read as zero, writes ignored. */
type HSCTLR_Type of bits(32) {
  [31] DSSBS  /* When FEAT_SSBS is implemented */,
  [30] TE,
  [25] EE  /* When FEAT_MixedEnd is implemented */,
  [19] WXN,
  [12] I,
  [8] SED,
  [7] ITD,
  [5] CP15BEN,
  [4] LSMAOE  /* When FEAT_LSMAOC is implemented */,
  [3] nTLSMD  /* When FEAT_LSMAOC is implemented */,
  [2] C,
  [1] A,
  [0] M,
};

accessor HSCTLR() <=> value : HSCTLR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HSR: read as zero, writes ignored. */
type HSR_Type of bits(32) {
  [31:26] EC,
  [25] IL,
  [24:0] ISS,
};

accessor HSR() <=> value : HSR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for HTRFCR: read as zero, writes ignored. */
type HTRFCR_Type of bits(32) {
  [6:5] TS,
  [3] CX,
  [1] E2TRE,
  [0] E0HTRE,
};

accessor HTRFCR() <=> value : HTRFCR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MAIR2_EL1: read as zero, writes ignored. */
type MAIR2_EL1_Type of bits(64) {
  [63:56] Attr7,
  [55:48] Attr6,
  [47:40] Attr5,
  [39:32] Attr4,
  [31:24] Attr3,
  [23:16] Attr2,
  [15:8] Attr1,
  [7:0] Attr0,
};

accessor MAIR2_EL1() <=> value : MAIR2_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MAIR2_EL2: read as zero, writes ignored. */
type MAIR2_EL2_Type of bits(64) {
  [63:56] Attr7,
  [55:48] Attr6,
  [47:40] Attr5,
  [39:32] Attr4,
  [31:24] Attr3,
  [23:16] Attr2,
  [15:8] Attr1,
  [7:0] Attr0,
};

accessor MAIR2_EL2() <=> value : MAIR2_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MAIR2_EL3: read as zero, writes ignored. */
type MAIR2_EL3_Type of bits(64) {
  [63:56] Attr7,
  [55:48] Attr6,
  [47:40] Attr5,
  [39:32] Attr4,
  [31:24] Attr3,
  [23:16] Attr2,
  [15:8] Attr1,
  [7:0] Attr0,
};

accessor MAIR2_EL3() <=> value : MAIR2_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MAIR_EL1: read as zero, writes ignored. */
type MAIR_EL1_Type of bits(64) {
  [63:56] Attr7,
  [55:48] Attr6,
  [47:40] Attr5,
  [39:32] Attr4,
  [31:24] Attr3,
  [23:16] Attr2,
  [15:8] Attr1,
  [7:0] Attr0,
};

accessor MAIR_EL1() <=> value : MAIR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MAIR_EL2: read as zero, writes ignored. */
type MAIR_EL2_Type of bits(64) {
  [63:56] Attr7,
  [55:48] Attr6,
  [47:40] Attr5,
  [39:32] Attr4,
  [31:24] Attr3,
  [23:16] Attr2,
  [15:8] Attr1,
  [7:0] Attr0,
};

accessor MAIR_EL2() <=> value : MAIR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MAIR_EL3: read as zero, writes ignored. */
type MAIR_EL3_Type of bits(64) {
  [63:56] Attr7,
  [55:48] Attr6,
  [47:40] Attr5,
  [39:32] Attr4,
  [31:24] Attr3,
  [23:16] Attr2,
  [15:8] Attr1,
  [7:0] Attr0,
};

accessor MAIR_EL3() <=> value : MAIR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MDCR_EL2: read as zero, writes ignored. */
type MDCR_EL2_Type of bits(64) {
  [50] EnSTEPOP  /* When FEAT_STEP2 is implemented */,
  [43] EBWE  /* When FEAT_Debugv8p9 is implemented */,
  [41:40] PMEE  /* When FEAT_EBEP is implemented */,
  [36] HPMFZS  /* When FEAT_SPEv1p2 is implemented */,
  [31:30] PMSSE  /* When FEAT_PMUv3_SS is implemented */,
  [29] HPMFZO  /* When FEAT_PMUv3p7 is implemented */,
  [28] MTPME  /* When FEAT_MTPMU is implemented and EL3 is not implemented */,
  [27] TDCC  /* When FEAT_FGT is implemented */,
  [26] HLP  /* When FEAT_PMUv3p5 is implemented */,
  [25:24] E2TB  /* When FEAT_TRBE is implemented */,
  [23] HCCD  /* When FEAT_PMUv3p5 is implemented */,
  [19] TTRF  /* When FEAT_TRF is implemented */,
  [17] HPMD  /* When FEAT_PMUv3p1 is implemented or When FEAT_PMUv3p1 is implemented and FEAT_Debugv8p2 is implemented */,
  [15] EnSPM  /* When FEAT_SPMU is implemented */,
  [14] TPMS  /* When FEAT_SPE is implemented */,
  [13:12] E2PB  /* When FEAT_SPE is implemented */,
  [11] TDRA,
  [10] TDOSA  /* Otherwise or When FEAT_DoubleLock is implemented */,
  [9] TDA,
  [8] TDE,
  [7] HPME  /* When FEAT_PMUv3 is implemented */,
  [6] TPM  /* When FEAT_PMUv3 is implemented */,
  [5] TPMCR  /* When FEAT_PMUv3 is implemented */,
  [4:0] HPMN  /* When FEAT_PMUv3 is implemented */,
};

accessor MDCR_EL2() <=> value : MDCR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MDCR_EL3: read as zero, writes ignored. */
type MDCR_EL3_Type of bits(64) {
  [55] EnPMS4  /* When FEAT_SPE_nVM is implemented or (FEAT_S1POE2 is implemented and FEAT_SPE is implemented) */,
  [54:53] TRBEE  /* When FEAT_TRBE_EXC is implemented */,
  [52:51] PMSEE  /* When FEAT_SPE_EXC is implemented */,
  [50] EnSTEPOP  /* When FEAT_STEP2 is implemented */,
  [49:48] ETBAD  /* When FEAT_TRBE_EXT is implemented */,
  [47] EnITE  /* When FEAT_ITE is implemented */,
  [46:45] EPMSSAD  /* When FEAT_PMUv3_SS is implemented */,
  [44] EnPMSS  /* When FEAT_PMUv3_SS is implemented */,
  [43] EBWE  /* When FEAT_Debugv8p9 is implemented */,
  [42] EnPMS3  /* When FEAT_SPE_FDS is implemented */,
  [41:40] PMEE  /* When FEAT_EBEP is implemented */,
  [39] EnTB2  /* When FEAT_TRBE_MPAM is implemented */,
  [38] E3BREC  /* When FEAT_BRBEv1p1 is implemented */,
  [37] E3BREW  /* When FEAT_BRBEv1p1 is implemented */,
  [36] EnPMSN  /* When FEAT_SPE_FnE is implemented */,
  [35] MPMX  /* When FEAT_PMUv3p7 is implemented */,
  [34] MCCD  /* When FEAT_PMUv3p7 is implemented */,
  [33:32] SBRBE  /* When FEAT_BRBE is implemented */,
  [31:30] PMSSE  /* When FEAT_PMUv3_SS is implemented */,
  [28] MTPME  /* When FEAT_MTPMU is implemented */,
  [27] TDCC  /* When FEAT_FGT is implemented */,
  [26] NSTBE  /* When FEAT_TRBE is implemented and FEAT_RME is implemented */,
  [25:24] NSTB  /* When FEAT_TRBE is implemented or When FEAT_TRBE is implemented and FEAT_RME is implemented */,
  [23] SCCD  /* When FEAT_PMUv3p5 is implemented */,
  [22] ETAD  /* When FEAT_TRC_EXT is implemented and FEAT_TRBE is implemented or When FEAT_RME is implemented, FEAT_TRC_EXT is implemented, and FEAT_TRBE is implemented */,
  [21] EPMAD  /* When FEAT_PMUv3_EXT is implemented or When FEAT_Debugv8p4 is implemented and FEAT_PMUv3_EXT is implemented or When FEAT_RME is implemented and FEAT_PMUv3_EXT is implemented */,
  [20] EDAD  /* Otherwise or When FEAT_Debugv8p2 is implemented or When FEAT_Debugv8p4 is implemented or When FEAT_RME is implemented */,
  [19] TTRF  /* When FEAT_TRF is implemented */,
  [18] STE  /* When FEAT_TRF is implemented and HaveSecureState() */,
  [17] SPME  /* When FEAT_PMUv3 is implemented or When FEAT_PMUv3 is implemented and FEAT_Debugv8p2 is implemented or When FEAT_PMUv3 is implemented and FEAT_PMUv3p7 is implemented */,
  [16] SDD  /* When HaveSecureState() */,
  [15:14] SPD32  /* When FEAT_AA32EL1 is implemented */,
  [13:12] NSPB  /* When FEAT_SPE is implemented or When FEAT_SPE is implemented and FEAT_RME is implemented */,
  [11] NSPBE  /* When FEAT_SPE is implemented and FEAT_RME is implemented */,
  [10] TDOSA  /* Otherwise or When FEAT_DoubleLock is implemented */,
  [9] TDA,
  [7] EnPM2  /* When FEAT_PMUv3p9 is implemented, or FEAT_SPMU is implemented, or FEAT_EBEP is implemented, or FEAT_PMUv3_SS is implemented, or FEAT_SPMU2 is implemented */,
  [6] TPM  /* When FEAT_PMUv3 is implemented */,
  [4] EDADE  /* When FEAT_RME is implemented */,
  [3] ETADE  /* When FEAT_RME is implemented, FEAT_TRC_EXT is implemented, and FEAT_TRBE is implemented */,
  [2] EPMADE  /* When FEAT_RME is implemented and FEAT_PMUv3_EXT is implemented */,
  [0] RLTE  /* When FEAT_RME is implemented and FEAT_TRF is implemented */,
};

accessor MDCR_EL3() <=> value : MDCR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MDSCR_EL1: read as zero, writes ignored. */
type MDSCR_EL1_Type of bits(64) {
  [50] EnSTEPOP  /* When FEAT_STEP2 is implemented */,
  [35] EHBWE  /* When FEAT_Debugv8p9 is implemented */,
  [34] EnSPM  /* When FEAT_SPMU is implemented */,
  [33] TTA  /* When FEAT_TRBE_EXT is implemented or FEAT_ETEv1p3 is implemented */,
  [32] EMBWE  /* When FEAT_Debugv8p9 is implemented */,
  [31] TFO  /* When FEAT_TRF is implemented */,
  [30] RXfull,
  [29] TXfull,
  [27] RXO,
  [26] TXU,
  [23:22] INTdis,
  [21] TDA,
  [19] SC2  /* When FEAT_PCSRv8 is implemented, FEAT_VHE is implemented, and FEAT_PCSRv8p2 is not implemented */,
  [15] MDE,
  [14] HDE,
  [13] KDE,
  [12] TDCC,
  [6] ERR,
  [0] SS,
};

accessor MDSCR_EL1() <=> value : MDSCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MECID_A0_EL2: read as zero, writes ignored. */
type MECID_A0_EL2_Type of bits(64) {
  [15:0] MECID,
};

accessor MECID_A0_EL2() <=> value : MECID_A0_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MECID_A1_EL2: read as zero, writes ignored. */
type MECID_A1_EL2_Type of bits(64) {
  [15:0] MECID,
};

accessor MECID_A1_EL2() <=> value : MECID_A1_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MECID_P0_EL2: read as zero, writes ignored. */
type MECID_P0_EL2_Type of bits(64) {
  [15:0] MECID,
};

accessor MECID_P0_EL2() <=> value : MECID_P0_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MECID_P1_EL2: read as zero, writes ignored. */
type MECID_P1_EL2_Type of bits(64) {
  [15:0] MECID,
};

accessor MECID_P1_EL2() <=> value : MECID_P1_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MECID_RL_A_EL3: read as zero, writes ignored. */
type MECID_RL_A_EL3_Type of bits(64) {
  [15:0] MECID,
};

accessor MECID_RL_A_EL3() <=> value : MECID_RL_A_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAM0_EL1: read as zero, writes ignored. */
type MPAM0_EL1_Type of bits(64) {
    /* When FEAT_MPAMv2 is implemented */
  [63:48] altPMG  /* When MPAMIDR_EL1.HAS_ALT_ID == '1' */,
  [47:32] PMG,
  [31:16] altPARTID  /* When MPAMIDR_EL1.HAS_ALT_ID == '1' */,
  [15:0] PARTID,
  [47:40] PMG_D,
  [39:32] PMG_I,
  [31:16] PARTID_D,
  [15:0] PARTID_I,
};

accessor MPAM0_EL1() <=> value : MPAM0_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAM1_EL1: read as zero, writes ignored. */
type MPAM1_EL1_Type of bits(64) {
    /* When FEAT_MPAMv2 is implemented */
  [63:48] altPMG  /* When MPAMIDR_EL1.HAS_ALT_ID == '1' */,
  [47:32] PMG,
  [31:16] altPARTID  /* When MPAMIDR_EL1.HAS_ALT_ID == '1' */,
  [15:0] PARTID,
  [63] MPAMEN,
  [60] FORCED_NS  /* When FEAT_MPAMv0p1 is implemented */,
  [54] ALTSP_FRCD  /* When FEAT_RME is implemented and MPAMIDR_EL1.HAS_ALTSP == '1' */,
  [47:40] PMG_D,
  [39:32] PMG_I,
  [31:16] PARTID_D,
  [15:0] PARTID_I,
};

accessor MPAM1_EL1() <=> value : MPAM1_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAM2_EL2: read as zero, writes ignored. */
type MPAM2_EL2_Type of bits(64) {
    /* When FEAT_MPAMv2 is implemented */
  [63:48] altPMG  /* When MPAMIDR_EL1.HAS_ALT_ID == '1' */,
  [47:32] PMG,
  [31:16] altPARTID  /* When MPAMIDR_EL1.HAS_ALT_ID == '1' */,
  [15:0] PARTID,
  [63] MPAMEN,
  [58] TIDR  /* When (FEAT_MPAMv0p1 is implemented or FEAT_MPAMv1p1 is implemented) and MPAMIDR_EL1.HAS_TIDR == '1' */,
  [56] ALTSP_HFC  /* When FEAT_RME is implemented and MPAMIDR_EL1.HAS_ALTSP == '1' */,
  [55] ALTSP_EL2  /* When FEAT_RME is implemented and MPAMIDR_EL1.HAS_ALTSP == '1' */,
  [54] ALTSP_FRCD  /* When FEAT_RME is implemented and MPAMIDR_EL1.HAS_ALTSP == '1' */,
  [50] EnMPAMSM  /* When FEAT_SME is implemented */,
  [49] TRAPMPAM0EL1,
  [48] TRAPMPAM1EL1,
  [47:40] PMG_D,
  [39:32] PMG_I,
  [31:16] PARTID_D,
  [15:0] PARTID_I,
};

accessor MPAM2_EL2() <=> value : MPAM2_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAM3_EL3: read as zero, writes ignored. */
type MPAM3_EL3_Type of bits(64) {
    /* When FEAT_MPAMv2 is implemented */
  [63:48] altPMG  /* When MPAMIDR_EL1.HAS_ALT_ID == '1' */,
  [47:32] PMG,
  [31:16] altPARTID  /* When MPAMIDR_EL1.HAS_ALT_ID == '1' */,
  [15:0] PARTID,
  [63] MPAMEN,
  [62] TRAPLOWER,
  [61] SDEFLT  /* When (FEAT_MPAMv0p1 is implemented or FEAT_MPAMv1p1 is implemented) and MPAMIDR_EL1.HAS_SDEFLT == '1' */,
  [60] FORCE_NS  /* When FEAT_MPAMv0p1 is implemented and MPAMIDR_EL1.HAS_FORCE_NS == '1' */,
  [57] ALTSP_HEN  /* When FEAT_RME is implemented and MPAMIDR_EL1.HAS_ALTSP == '1' */,
  [56] ALTSP_HFC  /* When FEAT_RME is implemented and MPAMIDR_EL1.HAS_ALTSP == '1' */,
  [55] ALTSP_EL3  /* When FEAT_RME is implemented and MPAMIDR_EL1.HAS_ALTSP == '1' */,
  [52] RT_ALTSP_NS  /* When FEAT_RME is implemented and MPAMIDR_EL1.HAS_ALTSP == '1' */,
  [47:40] PMG_D,
  [39:32] PMG_I,
  [31:16] PARTID_D,
  [15:0] PARTID_I,
};

accessor MPAM3_EL3() <=> value : MPAM3_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMCTL_EL1: read as zero, writes ignored. */
type MPAMCTL_EL1_Type of bits(64) {
  [63] MPAMEN,
  [52] EN_ALT_IPMG  /* When MPAMIDR_EL1.HAS_INSTR_ALT_ID == '1' */,
  [51] EN_ALT_IPARTID  /* When MPAMIDR_EL1.HAS_INSTR_ALT_ID == '1' */,
  [50] EN_ALT_IPMG_EL0  /* When MPAMIDR_EL1.HAS_INSTR_ALT_ID == '1' */,
  [49] EN_ALT_IPARTID_EL0  /* When MPAMIDR_EL1.HAS_INSTR_ALT_ID == '1' */,
};

accessor MPAMCTL_EL1() <=> value : MPAMCTL_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMCTL_EL2: read as zero, writes ignored. */
type MPAMCTL_EL2_Type of bits(64) {
  [63] MPAMEN,
  [52] EN_ALT_IPMG  /* When MPAMIDR_EL1.HAS_INSTR_ALT_ID == '1' */,
  [51] EN_ALT_IPARTID  /* When MPAMIDR_EL1.HAS_INSTR_ALT_ID == '1' */,
  [50] EN_ALT_IPMG_EL0  /* When MPAMIDR_EL1.HAS_INSTR_ALT_ID == '1' and ELIsInHost(EL2) */,
  [49] EN_ALT_IPARTID_EL0  /* When MPAMIDR_EL1.HAS_INSTR_ALT_ID == '1' and ELIsInHost(EL2) */,
};

accessor MPAMCTL_EL2() <=> value : MPAMCTL_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMCTL_EL3: read as zero, writes ignored. */
type MPAMCTL_EL3_Type of bits(64) {
  [63] MPAMEN,
  [62] nTRAPLOWER,
  [52] EN_ALT_IPMG  /* When MPAMIDR_EL1.HAS_INSTR_ALT_ID == '1' */,
  [51] EN_ALT_IPARTID  /* When MPAMIDR_EL1.HAS_INSTR_ALT_ID == '1' */,
};

accessor MPAMCTL_EL3() <=> value : MPAMCTL_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMHCR_EL2: read as zero, writes ignored. */
type MPAMHCR_EL2_Type of bits(64) {
    /* When FEAT_MPAMv2 is implemented or When FEAT_MPAMv0p1 is implemented or FEAT_MPAMv1p0 is implemented */
  [58] nTIDR,
  [50] nTRAPMPAMSM  /* When FEAT_SME is implemented */,
  [49] nTRAPMPAM0EL1,
  [48] nTRAPMPAM1EL1,
  [31] TRAP_MPAMIDR_EL1,
  [8] GSTAPP_PLK,
  [1] EL1_VPMEN,
  [0] EL0_VPMEN,
};

accessor MPAMHCR_EL2() <=> value : MPAMHCR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMIDR_EL1: read as zero, writes ignored. */
type MPAMIDR_EL1_Type of bits(64) {
    /* When FEAT_MPAMv2 is implemented */
  [56] HAS_BW_CTRL,
  [47:32] PMG_MAX,
  [22] HAS_INSTR_ALT_ID  /* When MPAMIDR_EL1.HAS_ALT_ID == '1' */,
  [21] HAS_ALT_ID,
  [17] HAS_HCR,
  [15:0] PARTID_MAX,
  [61] HAS_SDEFLT,
  [60] HAS_FORCE_NS,
  [59] SP4,
  [58] HAS_TIDR,
  [57] HAS_ALTSP,
  [20:18] VPMR_MAX  /* When MPAMIDR_EL1.HAS_HCR == '1' */,
};

accessor MPAMIDR_EL1() <=> value : MPAMIDR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMSM_EL1: read as zero, writes ignored. */
type MPAMSM_EL1_Type of bits(64) {
    /* When FEAT_MPAMv2 is implemented */
  [47:32] PMG,
  [31:16] PARTID,
  [47:40] PMG_D,
  [31:16] PARTID_D,
};

accessor MPAMSM_EL1() <=> value : MPAMSM_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMVPM0_EL2: read as zero, writes ignored. */
type MPAMVPM0_EL2_Type of bits(64) {
  [63:48] PhyPARTID3,
  [47:32] PhyPARTID2,
  [31:16] PhyPARTID1,
  [15:0] PhyPARTID0,
};

accessor MPAMVPM0_EL2() <=> value : MPAMVPM0_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMVPM1_EL2: read as zero, writes ignored. */
type MPAMVPM1_EL2_Type of bits(64) {
  [63:48] PhyPARTID7,
  [47:32] PhyPARTID6,
  [31:16] PhyPARTID5,
  [15:0] PhyPARTID4,
};

accessor MPAMVPM1_EL2() <=> value : MPAMVPM1_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMVPM2_EL2: read as zero, writes ignored. */
type MPAMVPM2_EL2_Type of bits(64) {
  [63:48] PhyPARTID11,
  [47:32] PhyPARTID10,
  [31:16] PhyPARTID9,
  [15:0] PhyPARTID8,
};

accessor MPAMVPM2_EL2() <=> value : MPAMVPM2_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMVPM3_EL2: read as zero, writes ignored. */
type MPAMVPM3_EL2_Type of bits(64) {
  [63:48] PhyPARTID15,
  [47:32] PhyPARTID14,
  [31:16] PhyPARTID13,
  [15:0] PhyPARTID12,
};

accessor MPAMVPM3_EL2() <=> value : MPAMVPM3_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMVPM4_EL2: read as zero, writes ignored. */
type MPAMVPM4_EL2_Type of bits(64) {
  [63:48] PhyPARTID19,
  [47:32] PhyPARTID18,
  [31:16] PhyPARTID17,
  [15:0] PhyPARTID16,
};

accessor MPAMVPM4_EL2() <=> value : MPAMVPM4_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMVPM5_EL2: read as zero, writes ignored. */
type MPAMVPM5_EL2_Type of bits(64) {
  [63:48] PhyPARTID23,
  [47:32] PhyPARTID22,
  [31:16] PhyPARTID21,
  [15:0] PhyPARTID20,
};

accessor MPAMVPM5_EL2() <=> value : MPAMVPM5_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMVPM6_EL2: read as zero, writes ignored. */
type MPAMVPM6_EL2_Type of bits(64) {
  [63:48] PhyPARTID27,
  [47:32] PhyPARTID26,
  [31:16] PhyPARTID25,
  [15:0] PhyPARTID24,
};

accessor MPAMVPM6_EL2() <=> value : MPAMVPM6_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMVPM7_EL2: read as zero, writes ignored. */
type MPAMVPM7_EL2_Type of bits(64) {
  [63:48] PhyPARTID31,
  [47:32] PhyPARTID30,
  [31:16] PhyPARTID29,
  [15:0] PhyPARTID28,
};

accessor MPAMVPM7_EL2() <=> value : MPAMVPM7_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for MPAMVPMV_EL2: read as zero, writes ignored. */
type MPAMVPMV_EL2_Type of bits(64) {
  [31] VPM_V31,
  [30] VPM_V30,
  [29] VPM_V29,
  [28] VPM_V28,
  [27] VPM_V27,
  [26] VPM_V26,
  [25] VPM_V25,
  [24] VPM_V24,
  [23] VPM_V23,
  [22] VPM_V22,
  [21] VPM_V21,
  [20] VPM_V20,
  [19] VPM_V19,
  [18] VPM_V18,
  [17] VPM_V17,
  [16] VPM_V16,
  [15] VPM_V15,
  [14] VPM_V14,
  [13] VPM_V13,
  [12] VPM_V12,
  [11] VPM_V11,
  [10] VPM_V10,
  [9] VPM_V9,
  [8] VPM_V8,
  [7] VPM_V7,
  [6] VPM_V6,
  [5] VPM_V5,
  [4] VPM_V4,
  [3] VPM_V3,
  [2] VPM_V2,
  [1] VPM_V1,
  [0] VPM_V0,
};

accessor MPAMVPMV_EL2() <=> value : MPAMVPMV_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for NSACR: read as zero, writes ignored. */
type NSACR_Type of bits(32) {
  [20] NSTRCDIS,
  [15] NSASEDIS,
  [11] cp11,
  [10] cp10,
};

accessor NSACR() <=> value : NSACR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for OSDLR_EL1: read as zero, writes ignored. */
type OSDLR_EL1_Type of bits(64) {
  [0] DLK  /* When FEAT_DoubleLock is implemented */,
};

accessor OSDLR_EL1() <=> value : OSDLR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for OSLSR_EL1: read as zero, writes ignored. */
type OSLSR_EL1_Type of bits(64) {
  [3, 0] OSLM,
  [2] nTT,
  [1] OSLK,
};

accessor OSLSR_EL1() <=> value : OSLSR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PIRE0_EL1: read as zero, writes ignored. */
type PIRE0_EL1_Type of bits(64) {
  [63:60] Perm15,
  [59:56] Perm14,
  [55:52] Perm13,
  [51:48] Perm12,
  [47:44] Perm11,
  [43:40] Perm10,
  [39:36] Perm9,
  [35:32] Perm8,
  [31:28] Perm7,
  [27:24] Perm6,
  [23:20] Perm5,
  [19:16] Perm4,
  [15:12] Perm3,
  [11:8] Perm2,
  [7:4] Perm1,
  [3:0] Perm0,
};

accessor PIRE0_EL1() <=> value : PIRE0_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PIRE0_EL2: read as zero, writes ignored. */
type PIRE0_EL2_Type of bits(64) {
  [63:60] Perm15,
  [59:56] Perm14,
  [55:52] Perm13,
  [51:48] Perm12,
  [47:44] Perm11,
  [43:40] Perm10,
  [39:36] Perm9,
  [35:32] Perm8,
  [31:28] Perm7,
  [27:24] Perm6,
  [23:20] Perm5,
  [19:16] Perm4,
  [15:12] Perm3,
  [11:8] Perm2,
  [7:4] Perm1,
  [3:0] Perm0,
};

accessor PIRE0_EL2() <=> value : PIRE0_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PIR_EL1: read as zero, writes ignored. */
type PIR_EL1_Type of bits(64) {
  [63:60] Perm15,
  [59:56] Perm14,
  [55:52] Perm13,
  [51:48] Perm12,
  [47:44] Perm11,
  [43:40] Perm10,
  [39:36] Perm9,
  [35:32] Perm8,
  [31:28] Perm7,
  [27:24] Perm6,
  [23:20] Perm5,
  [19:16] Perm4,
  [15:12] Perm3,
  [11:8] Perm2,
  [7:4] Perm1,
  [3:0] Perm0,
};

accessor PIR_EL1() <=> value : PIR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PIR_EL2: read as zero, writes ignored. */
type PIR_EL2_Type of bits(64) {
  [63:60] Perm15,
  [59:56] Perm14,
  [55:52] Perm13,
  [51:48] Perm12,
  [47:44] Perm11,
  [43:40] Perm10,
  [39:36] Perm9,
  [35:32] Perm8,
  [31:28] Perm7,
  [27:24] Perm6,
  [23:20] Perm5,
  [19:16] Perm4,
  [15:12] Perm3,
  [11:8] Perm2,
  [7:4] Perm1,
  [3:0] Perm0,
};

accessor PIR_EL2() <=> value : PIR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PIR_EL3: read as zero, writes ignored. */
type PIR_EL3_Type of bits(64) {
  [63:60] Perm15,
  [59:56] Perm14,
  [55:52] Perm13,
  [51:48] Perm12,
  [47:44] Perm11,
  [43:40] Perm10,
  [39:36] Perm9,
  [35:32] Perm8,
  [31:28] Perm7,
  [27:24] Perm6,
  [23:20] Perm5,
  [19:16] Perm4,
  [15:12] Perm3,
  [11:8] Perm2,
  [7:4] Perm1,
  [3:0] Perm0,
};

accessor PIR_EL3() <=> value : PIR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMBIDR_EL1: read as zero, writes ignored. */
type PMBIDR_EL1_Type of bits(64) {
  [47:32] MaxBuffSize,
  [11:8] EA,
  [7:6] AddrMode  /* When FEAT_SPE_nVM is implemented */,
  [5] F,
  [4] P,
  [3:0] Align,
};

accessor PMBIDR_EL1() <=> value : PMBIDR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMBLIMITR_EL1: read as zero, writes ignored. */
type PMBLIMITR_EL1_Type of bits(64) {
  [63:12] LIMIT,
  [7] nVM  /* When FEAT_SPE_nVM is implemented */,
  [5] PMFZ  /* When FEAT_SPEv1p2 is implemented */,
  [2:1] FM,
  [0] E,
};

accessor PMBLIMITR_EL1() <=> value : PMBLIMITR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMBMAR_EL1: read as zero, writes ignored. */
type PMBMAR_EL1_Type of bits(64) {
  [46:40] FPOIndex  /* When FEAT_S1POE2 is implemented */,
  [39:33] TIndex  /* When FEAT_S1POE2 is implemented */,
  [32] TTBA  /* When FEAT_S1POE2 is implemented */,
  [9:8] SH,
  [7:0] Attr,
};

accessor PMBMAR_EL1() <=> value : PMBMAR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMBPTR_EL1: read as zero, writes ignored. */
type PMBPTR_EL1_Type of bits(64) {
  [63:0] PTR,
};

accessor PMBPTR_EL1() <=> value : PMBPTR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMBSR_EL1: read as zero, writes ignored. */
type PMBSR_EL1_Type of bits(64) {
  [55:32] MSS2,
  [31:26] EC,
  [19] DL,
  [18] EA  /* When the PE sets this bit as the result of an External abort */,
  [17] S,
  [16] COLL,
  [15:0] MSS {
    [5:0] BSC,
  },
};

accessor PMBSR_EL1() <=> value : PMBSR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMBSR_EL2: read as zero, writes ignored. */
type PMBSR_EL2_Type of bits(64) {
  [55:32] MSS2,
  [31:26] EC,
  [19] DL,
  [18] EA  /* When the PE sets this bit as the result of an External abort */,
  [17] S,
  [16] COLL,
  [15:0] MSS {
    [5:0] BSC,
  },
};

accessor PMBSR_EL2() <=> value : PMBSR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMBSR_EL3: read as zero, writes ignored. */
type PMBSR_EL3_Type of bits(64) {
  [55:32] MSS2,
  [31:26] EC,
  [19] DL,
  [18] EA  /* When the PE sets this bit as the result of an External abort */,
  [17] S,
  [16] COLL,
  [15:0] MSS {
    [5:0] BSC,
  },
};

accessor PMBSR_EL3() <=> value : PMBSR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMCCFILTR: read as zero, writes ignored. */
type PMCCFILTR_Type of bits(32) {
  [31] P,
  [30] U,
  [29] NSK  /* When EL3 is implemented */,
  [28] NSU  /* When EL3 is implemented */,
  [27] NSH  /* When EL2 is implemented */,
  [21] RLU  /* When FEAT_RME is implemented */,
};

accessor PMCCFILTR() <=> value : PMCCFILTR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMCCFILTR_EL0: read as zero, writes ignored. */
type PMCCFILTR_EL0_Type of bits(64) {
  [57:56] VS  /* When FEAT_PMUv3_SME is implemented */,
  [31] P,
  [30] U,
  [29] NSK  /* When EL3 is implemented */,
  [28] NSU  /* When EL3 is implemented */,
  [27] NSH  /* When EL2 is implemented */,
  [26] M  /* When EL3 is implemented */,
  [24] SH  /* When EL3 is implemented and FEAT_SEL2 is implemented */,
  [22] RLK  /* When FEAT_RME is implemented */,
  [21] RLU  /* When FEAT_RME is implemented */,
  [20] RLH  /* When FEAT_RME is implemented */,
};

accessor PMCCFILTR_EL0() <=> value : PMCCFILTR_EL0_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMCCR: read as zero, writes ignored. */
type PMCCR_Type of bits(64) {
  [8] OSLO  /* When FEAT_PMUv3_EXTPMN is implemented */,
  [7] EPME  /* When FEAT_PMUv3_EXTPMN is implemented */,
  [4:0] EPMN  /* When FEAT_PMUv3_EXTPMN is implemented */,
};

accessor PMCCR() <=> value : PMCCR_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMCNTENSET: read as zero, writes ignored. */
type PMCNTENSET_Type of bits(32) {
  [31] C,
  [30] P30,
  [29] P29,
  [28] P28,
  [27] P27,
  [26] P26,
  [25] P25,
  [24] P24,
  [23] P23,
  [22] P22,
  [21] P21,
  [20] P20,
  [19] P19,
  [18] P18,
  [17] P17,
  [16] P16,
  [15] P15,
  [14] P14,
  [13] P13,
  [12] P12,
  [11] P11,
  [10] P10,
  [9] P9,
  [8] P8,
  [7] P7,
  [6] P6,
  [5] P5,
  [4] P4,
  [3] P3,
  [2] P2,
  [1] P1,
  [0] P0,
};

accessor PMCNTENSET() <=> value : PMCNTENSET_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMCNTENSET_EL0: read as zero, writes ignored. */
type PMCNTENSET_EL0_Type of bits(64) {
  [32] F0  /* When FEAT_PMUv3_ICNTR is implemented */,
  [31] C,
  [30] P30,
  [29] P29,
  [28] P28,
  [27] P27,
  [26] P26,
  [25] P25,
  [24] P24,
  [23] P23,
  [22] P22,
  [21] P21,
  [20] P20,
  [19] P19,
  [18] P18,
  [17] P17,
  [16] P16,
  [15] P15,
  [14] P14,
  [13] P13,
  [12] P12,
  [11] P11,
  [10] P10,
  [9] P9,
  [8] P8,
  [7] P7,
  [6] P6,
  [5] P5,
  [4] P4,
  [3] P3,
  [2] P2,
  [1] P1,
  [0] P0,
};

accessor PMCNTENSET_EL0() <=> value : PMCNTENSET_EL0_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMCR: read as zero, writes ignored. */
type PMCR_Type of bits(32) {
  [31:24] IMP  /* When FEAT_PMUv3p7 is not implemented */,
  [23:16] IDCODE  /* When PMCR.IMP != '00000000' */,
  [15:11] N,
  [9] FZO  /* When FEAT_PMUv3p7 is implemented */,
  [7] LP  /* When FEAT_PMUv3p5 is implemented */,
  [6] LC,
  [5] DP  /* When EL3 is implemented, or (FEAT_PMUv3p1 is implemented and EL2 is implemented), or FEAT_PMUv3p7 is implemented, or FEAT_SPE_DPFZS is implemented */,
  [4] X  /* When the implementation includes a PMU event export bus */,
  [3] D,
  [2] C,
  [1] P,
  [0] E,
};

accessor PMCR() <=> value : PMCR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMCR_EL0: read as zero, writes ignored. */
type PMCR_EL0_Type of bits(64) {
  [32] FZS  /* When FEAT_SPEv1p2 is implemented */,
  [31:24] IMP  /* When FEAT_PMUv3p7 is not implemented */,
  [23:16] IDCODE  /* When PMCR_EL0.IMP != '00000000' */,
  [15:11] N,
  [9] FZO  /* When FEAT_PMUv3p7 is implemented */,
  [7] LP  /* When FEAT_PMUv3p5 is implemented */,
  [6] LC  /* When FEAT_AA32 is implemented */,
  [5] DP  /* When EL3 is implemented, or (FEAT_PMUv3p1 is implemented and EL2 is implemented), or FEAT_PMUv3p7 is implemented, or FEAT_SPE_DPFZS is implemented */,
  [4] X  /* When the implementation includes a PMU event export bus */,
  [3] D  /* When FEAT_AA32 is implemented */,
  [2] C,
  [1] P,
  [0] E,
};

accessor PMCR_EL0() <=> value : PMCR_EL0_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMEVTYPER: read as zero, writes ignored. */
type PMEVTYPER_Type of bits(32) {
  [31] P,
  [30] U,
  [29] NSK  /* When EL3 is implemented */,
  [28] NSU  /* When EL3 is implemented */,
  [27] NSH  /* When EL2 is implemented */,
  [25] MT  /* When FEAT_MTPMU is implemented or an IMPLEMENTATION DEFINED multi-threaded PMU extension is implemented */,
  [21] RLU  /* When FEAT_RME is implemented */,
  [15:10, 9:0] evtCount  /* When FEAT_PMUv3p1 is implemented */,
};

accessor PMEVTYPER(i: integer) <=> value : PMEVTYPER_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMEVTYPER_EL0: read as zero, writes ignored. */
type PMEVTYPER_EL0_Type of bits(64) {
  [63:61] TC  /* When FEAT_PMUv3_EDGE is implemented and PMEVTYPER<n>_EL0.TE == '1' or When FEAT_PMUv3_TH2 is implemented, PMEVTYPER<n>_EL0.TE == '0', n is odd, and PMEVTYPER<n>_EL0.TLC == '10' or When FEAT_PMUv3_TH is implemented, (FEAT_PMUv3_EDGE is not implemented or PMEVTYPER<n>_EL0.TE == '0'), and (FEAT_PMUv3_TH2 is not implemented, or n is even, or PMEVTYPER<n>_EL0.TLC IN {'0x'}) */,
  [60] TE  /* When FEAT_PMUv3_EDGE is implemented */,
  [57:56] VS  /* When FEAT_PMUv3_SME is implemented */,
  [55:54] TLC  /* When FEAT_PMUv3_TH2 is implemented and n is odd */,
  [43:32] TH  /* When FEAT_PMUv3_TH is implemented */,
  [31] P,
  [30] U,
  [29] NSK  /* When EL3 is implemented */,
  [28] NSU  /* When EL3 is implemented */,
  [27] NSH  /* When EL2 is implemented */,
  [26] M  /* When EL3 is implemented */,
  [25] MT  /* When FEAT_MTPMU is implemented or an IMPLEMENTATION DEFINED multi-threaded PMU extension is implemented */,
  [24] SH  /* When EL3 is implemented and FEAT_SEL2 is implemented */,
  [22] RLK  /* When FEAT_RME is implemented */,
  [21] RLU  /* When FEAT_RME is implemented */,
  [20] RLH  /* When FEAT_RME is implemented */,
  [15:10, 9:0] evtCount  /* When FEAT_PMUv3p1 is implemented */,
};

accessor PMEVTYPER_EL0(i: integer) <=> value : PMEVTYPER_EL0_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMICFILTR_EL0: read as zero, writes ignored. */
type PMICFILTR_EL0_Type of bits(64) {
  [57:56] VS  /* When FEAT_PMUv3_SME is implemented */,
  [31] P,
  [30] U,
  [29] NSK  /* When EL3 is implemented */,
  [28] NSU  /* When EL3 is implemented */,
  [27] NSH  /* When EL2 is implemented */,
  [26] M  /* When EL3 is implemented */,
  [24] SH  /* When EL3 is implemented and FEAT_SEL2 is implemented */,
  [22] RLK  /* When FEAT_RME is implemented */,
  [21] RLU  /* When FEAT_RME is implemented */,
  [20] RLH  /* When FEAT_RME is implemented */,
  [15:0] evtCount,
};

accessor PMICFILTR_EL0() <=> value : PMICFILTR_EL0_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMICNTR_EL0: read as zero, writes ignored. */
type PMICNTR_EL0_Type of bits(64) {
  [63:0] ICNT,
};

accessor PMICNTR_EL0() <=> value : PMICNTR_EL0_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMINTENCLR: read as zero, writes ignored. */
type PMINTENCLR_Type of bits(32) {
  [31] C,
  [30] P30,
  [29] P29,
  [28] P28,
  [27] P27,
  [26] P26,
  [25] P25,
  [24] P24,
  [23] P23,
  [22] P22,
  [21] P21,
  [20] P20,
  [19] P19,
  [18] P18,
  [17] P17,
  [16] P16,
  [15] P15,
  [14] P14,
  [13] P13,
  [12] P12,
  [11] P11,
  [10] P10,
  [9] P9,
  [8] P8,
  [7] P7,
  [6] P6,
  [5] P5,
  [4] P4,
  [3] P3,
  [2] P2,
  [1] P1,
  [0] P0,
};

accessor PMINTENCLR() <=> value : PMINTENCLR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMINTENCLR_EL1: read as zero, writes ignored. */
type PMINTENCLR_EL1_Type of bits(64) {
  [32] F0  /* When FEAT_PMUv3_ICNTR is implemented */,
  [31] C,
  [30] P30,
  [29] P29,
  [28] P28,
  [27] P27,
  [26] P26,
  [25] P25,
  [24] P24,
  [23] P23,
  [22] P22,
  [21] P21,
  [20] P20,
  [19] P19,
  [18] P18,
  [17] P17,
  [16] P16,
  [15] P15,
  [14] P14,
  [13] P13,
  [12] P12,
  [11] P11,
  [10] P10,
  [9] P9,
  [8] P8,
  [7] P7,
  [6] P6,
  [5] P5,
  [4] P4,
  [3] P3,
  [2] P2,
  [1] P1,
  [0] P0,
};

accessor PMINTENCLR_EL1() <=> value : PMINTENCLR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMOVSSET: read as zero, writes ignored. */
type PMOVSSET_Type of bits(32) {
  [31] C,
  [30] P30,
  [29] P29,
  [28] P28,
  [27] P27,
  [26] P26,
  [25] P25,
  [24] P24,
  [23] P23,
  [22] P22,
  [21] P21,
  [20] P20,
  [19] P19,
  [18] P18,
  [17] P17,
  [16] P16,
  [15] P15,
  [14] P14,
  [13] P13,
  [12] P12,
  [11] P11,
  [10] P10,
  [9] P9,
  [8] P8,
  [7] P7,
  [6] P6,
  [5] P5,
  [4] P4,
  [3] P3,
  [2] P2,
  [1] P1,
  [0] P0,
};

accessor PMOVSSET() <=> value : PMOVSSET_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMOVSSET_EL0: read as zero, writes ignored. */
type PMOVSSET_EL0_Type of bits(64) {
  [32] F0  /* When FEAT_PMUv3_ICNTR is implemented */,
  [31] C,
  [30] P30,
  [29] P29,
  [28] P28,
  [27] P27,
  [26] P26,
  [25] P25,
  [24] P24,
  [23] P23,
  [22] P22,
  [21] P21,
  [20] P20,
  [19] P19,
  [18] P18,
  [17] P17,
  [16] P16,
  [15] P15,
  [14] P14,
  [13] P13,
  [12] P12,
  [11] P11,
  [10] P10,
  [9] P9,
  [8] P8,
  [7] P7,
  [6] P6,
  [5] P5,
  [4] P4,
  [3] P3,
  [2] P2,
  [1] P1,
  [0] P0,
};

accessor PMOVSSET_EL0() <=> value : PMOVSSET_EL0_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMSCR_EL1: read as zero, writes ignored. */
type PMSCR_EL1_Type of bits(64) {
  [11] EnVM  /* When FEAT_SPE_nVM is implemented and FEAT_NV is implemented */,
  [10] KE  /* When FEAT_SPE_EXC is implemented */,
  [9:8] EE  /* When FEAT_SPE_EXC is implemented */,
  [7:6] PCT  /* Otherwise or When EL2 is implemented */,
  [5] TS,
  [4] PA,
  [3] CX,
  [1] E1SPE,
  [0] E0SPE,
};

accessor PMSCR_EL1() <=> value : PMSCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMSCR_EL2: read as zero, writes ignored. */
type PMSCR_EL2_Type of bits(64) {
  [11] EnVM  /* When FEAT_SPE_nVM is implemented */,
  [10] KE  /* When FEAT_SPE_EXC is implemented */,
  [9:8] EE  /* When FEAT_SPE_EXC is implemented */,
  [7:6] PCT,
  [5] TS,
  [4] PA,
  [3] CX,
  [1] E2SPE,
  [0] E0HSPE,
};

accessor PMSCR_EL2() <=> value : PMSCR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMSDSFR_EL1: read as zero, writes ignored. */
type PMSDSFR_EL1_Type of bits(64) {
  [63] S63,
  [62] S62,
  [61] S61,
  [60] S60,
  [59] S59,
  [58] S58,
  [57] S57,
  [56] S56,
  [55] S55,
  [54] S54,
  [53] S53,
  [52] S52,
  [51] S51,
  [50] S50,
  [49] S49,
  [48] S48,
  [47] S47,
  [46] S46,
  [45] S45,
  [44] S44,
  [43] S43,
  [42] S42,
  [41] S41,
  [40] S40,
  [39] S39,
  [38] S38,
  [37] S37,
  [36] S36,
  [35] S35,
  [34] S34,
  [33] S33,
  [32] S32,
  [31] S31,
  [30] S30,
  [29] S29,
  [28] S28,
  [27] S27,
  [26] S26,
  [25] S25,
  [24] S24,
  [23] S23,
  [22] S22,
  [21] S21,
  [20] S20,
  [19] S19,
  [18] S18,
  [17] S17,
  [16] S16,
  [15] S15,
  [14] S14,
  [13] S13,
  [12] S12,
  [11] S11,
  [10] S10,
  [9] S9,
  [8] S8,
  [7] S7,
  [6] S6,
  [5] S5,
  [4] S4,
  [3] S3,
  [2] S2,
  [1] S1,
  [0] S0,
};

accessor PMSDSFR_EL1() <=> value : PMSDSFR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMSEVFR_EL1: read as zero, writes ignored. */
type PMSEVFR_EL1_Type of bits(64) {
  [63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1] E  /* When event 63 is implemented and filtering on event 63 is supported or When event 62 is implemented and filtering on event 62 is supported or When event 61 is implemented and filtering on event 61 is supported or When event 60 is implemented and filtering on event 60 is supported or When event 59 is implemented and filtering on event 59 is supported or When event 58 is implemented and filtering on event 58 is supported or When event 57 is implemented and filtering on event 57 is supported or When event 56 is implemented and filtering on event 56 is supported or When event 55 is implemented and filtering on event 55 is supported or When event 54 is implemented and filtering on event 54 is supported or When event 53 is implemented and filtering on event 53 is supported or When event 52 is implemented and filtering on event 52 is supported or When event 51 is implemented and filtering on event 51 is supported or When event 50 is implemented and filtering on event 50 is supported or When event 49 is implemented and filtering on event 49 is supported or When event 48 is implemented and filtering on event 48 is supported or When FEAT_SPEv1p4 is not implemented, event 31 is implemented, and filtering on event 31 is supported or When FEAT_SPEv1p4 is not implemented, event 30 is implemented, and filtering on event 30 is supported or When FEAT_SPEv1p4 is not implemented, event 29 is implemented, and filtering on event 29 is supported or When FEAT_SPEv1p4 is not implemented, event 28 is implemented, and filtering on event 28 is supported or When FEAT_SPEv1p4 is not implemented, event 27 is implemented, and filtering on event 27 is supported or When FEAT_SPEv1p4 is not implemented, event 26 is implemented, and filtering on event 26 is supported or When FEAT_SPEv1p4 is not implemented, event 25 is implemented, and filtering on event 25 is supported or When (FEAT_SPE_SME is implemented or FEAT_SPEv1p5 is implemented) and event 25 is implemented or When FEAT_SPEv1p4 is not implemented, event 24 is implemented, and filtering on event 24 is supported or When FEAT_SPE_SME is implemented or When FEAT_SPEv1p4 is implemented and event 23 is implemented or When FEAT_SPEv1p4 is implemented and event 22 is implemented or When FEAT_SPEv1p4 is implemented and event 21 is implemented or When FEAT_SPEv1p4 is implemented and event 20 is implemented or When FEAT_SPEv1p4 is implemented and event 19 is implemented or When FEAT_SPEv1p1 is implemented and (FEAT_SVE is implemented or FEAT_SME is implemented) or When FEAT_SPEv1p1 is implemented and (FEAT_SVE is implemented or FEAT_SME is implemented) or When event 15 is implemented and filtering on event 15 is supported or When event 14 is implemented and filtering on event 14 is supported or When event 13 is implemented and filtering on event 13 is supported or When event 12 is implemented and filtering on event 12 is supported or When FEAT_SPEv1p1 is implemented or When (FEAT_SPEv1p4 is implemented or filtering on event 10 is optionally supported) and event 10 is implemented or When (FEAT_SPEv1p4 is implemented or filtering on event 9 is optionally supported) and event 9 is implemented or When (FEAT_SPEv1p4 is implemented or filtering on event 8 is optionally supported) and event 8 is implemented or When FEAT_SPE_FnE is implemented or When FEAT_SPEv1p4 is implemented or filtering on event 4 is optionally supported or When FEAT_SPEv1p4 is implemented or filtering on event 2 is optionally supported or When the PE supports sampling of speculative instructions */,
};

accessor PMSEVFR_EL1() <=> value : PMSEVFR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMSFCR_EL1: read as zero, writes ignored. */
type PMSFCR_EL1_Type of bits(64) {
  [52:48] TYPEm {
    [4] SIMDm,
    [3] FPm,
    [2] STm,
    [1] LDm,
    [0] Bm,
  },
  [20:16] TYPE {
    [4] SIMD  /* When FEAT_SPE_EFT is implemented */,
    [3] FP  /* When FEAT_SPE_EFT is implemented */,
    [2] ST,
    [1] LD,
    [0] B,
  },
  [4] FDS  /* When FEAT_SPE_FDS is implemented */,
  [3] FnE  /* When FEAT_SPE_FnE is implemented */,
  [2] FL,
  [1] FT,
  [0] FE,
};

accessor PMSFCR_EL1() <=> value : PMSFCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMSIDR_EL1: read as zero, writes ignored. */
type PMSIDR_EL1_Type of bits(64) {
  [32] SME,
  [31:28] ALTCLK,
  [27] FPF,
  [26] EFT,
  [25] CRR,
  [24] PBT,
  [23:20] Format,
  [19:16] CountSize,
  [15:12] MaxSize,
  [11:8] Interval,
  [7] FDS  /* When FEAT_SPEv1p4 is implemented */,
  [6] FnE  /* When FEAT_SPE_FnE is implemented */,
  [5] ERnd,
  [4] LDS,
  [3] ArchInst,
  [2] FL,
  [1] FT,
  [0] FE,
};

accessor PMSIDR_EL1() <=> value : PMSIDR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMSLATFR_EL1: read as zero, writes ignored. */
type PMSLATFR_EL1_Type of bits(64) {
  [15:0] MINLAT,
};

accessor PMSLATFR_EL1() <=> value : PMSLATFR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for PMSNEVFR_EL1: read as zero, writes ignored. */
type PMSNEVFR_EL1_Type of bits(64) {
  [63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1] E  /* When event 63 is implemented and filtering on event 63 is supported or When event 62 is implemented and filtering on event 62 is supported or When event 61 is implemented and filtering on event 61 is supported or When event 60 is implemented and filtering on event 60 is supported or When event 59 is implemented and filtering on event 59 is supported or When event 58 is implemented and filtering on event 58 is supported or When event 57 is implemented and filtering on event 57 is supported or When event 56 is implemented and filtering on event 56 is supported or When event 55 is implemented and filtering on event 55 is supported or When event 54 is implemented and filtering on event 54 is supported or When event 53 is implemented and filtering on event 53 is supported or When event 52 is implemented and filtering on event 52 is supported or When event 51 is implemented and filtering on event 51 is supported or When event 50 is implemented and filtering on event 50 is supported or When event 49 is implemented and filtering on event 49 is supported or When event 48 is implemented and filtering on event 48 is supported or When FEAT_SPEv1p4 is not implemented, event 31 is implemented, and filtering on event 31 is supported or When FEAT_SPEv1p4 is not implemented, event 30 is implemented, and filtering on event 30 is supported or When FEAT_SPEv1p4 is not implemented, event 29 is implemented, and filtering on event 29 is supported or When FEAT_SPEv1p4 is not implemented, event 28 is implemented, and filtering on event 28 is supported or When FEAT_SPEv1p4 is not implemented, event 27 is implemented, and filtering on event 27 is supported or When FEAT_SPEv1p4 is not implemented, event 26 is implemented, and filtering on event 26 is supported or When FEAT_SPEv1p4 is not implemented, event 25 is implemented, and filtering on event 25 is supported or When (FEAT_SPE_SME is implemented or FEAT_SPEv1p5 is implemented) and event 25 is implemented or When FEAT_SPEv1p4 is not implemented, event 24 is implemented, and filtering on event 24 is supported or When FEAT_SPE_SME is implemented or When FEAT_SPEv1p4 is implemented and event 23 is implemented or When FEAT_SPEv1p4 is implemented and event 22 is implemented or When FEAT_SPEv1p4 is implemented and event 21 is implemented or When FEAT_SPEv1p4 is implemented and event 20 is implemented or When FEAT_SPEv1p4 is implemented and event 19 is implemented or When FEAT_SPEv1p1 is implemented and (FEAT_SVE is implemented or FEAT_SME is implemented) or When FEAT_SPEv1p1 is implemented and (FEAT_SVE is implemented or FEAT_SME is implemented) or When event 15 is implemented and filtering on event 15 is supported or When event 14 is implemented and filtering on event 14 is supported or When event 13 is implemented and filtering on event 13 is supported or When event 12 is implemented and filtering on event 12 is supported or When FEAT_SPEv1p1 is implemented or When (FEAT_SPEv1p4 is implemented or filtering on event 10 is optionally supported) and event 10 is implemented or When (FEAT_SPEv1p4 is implemented or filtering on event 9 is optionally supported) and event 9 is implemented or When (FEAT_SPEv1p4 is implemented or filtering on event 8 is optionally supported) and event 8 is implemented or When FEAT_SPE_FnE is implemented or When FEAT_SPEv1p4 is implemented or filtering on event 4 is optionally supported or When FEAT_SPEv1p4 is implemented or filtering on event 2 is optionally supported or When the PE supports sampling of speculative instructions */,
};

accessor PMSNEVFR_EL1() <=> value : PMSNEVFR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for POR_EL0: read as zero, writes ignored. */
type POR_EL0_Type of bits(64) {
  [63:60] Perm15,
  [59:56] Perm14,
  [55:52] Perm13,
  [51:48] Perm12,
  [47:44] Perm11,
  [43:40] Perm10,
  [39:36] Perm9,
  [35:32] Perm8,
  [31:28] Perm7,
  [27:24] Perm6,
  [23:20] Perm5,
  [19:16] Perm4,
  [15:12] Perm3,
  [11:8] Perm2,
  [7:4] Perm1,
  [3:0] Perm0,
};

accessor POR_EL0() <=> value : POR_EL0_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for POR_EL1: read as zero, writes ignored. */
type POR_EL1_Type of bits(64) {
  [63:60] Perm15,
  [59:56] Perm14,
  [55:52] Perm13,
  [51:48] Perm12,
  [47:44] Perm11,
  [43:40] Perm10,
  [39:36] Perm9,
  [35:32] Perm8,
  [31:28] Perm7,
  [27:24] Perm6,
  [23:20] Perm5,
  [19:16] Perm4,
  [15:12] Perm3,
  [11:8] Perm2,
  [7:4] Perm1,
  [3:0] Perm0,
};

accessor POR_EL1() <=> value : POR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for POR_EL2: read as zero, writes ignored. */
type POR_EL2_Type of bits(64) {
  [63:60] Perm15,
  [59:56] Perm14,
  [55:52] Perm13,
  [51:48] Perm12,
  [47:44] Perm11,
  [43:40] Perm10,
  [39:36] Perm9,
  [35:32] Perm8,
  [31:28] Perm7,
  [27:24] Perm6,
  [23:20] Perm5,
  [19:16] Perm4,
  [15:12] Perm3,
  [11:8] Perm2,
  [7:4] Perm1,
  [3:0] Perm0,
};

accessor POR_EL2() <=> value : POR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for POR_EL3: read as zero, writes ignored. */
type POR_EL3_Type of bits(64) {
  [63:60] Perm15,
  [59:56] Perm14,
  [55:52] Perm13,
  [51:48] Perm12,
  [47:44] Perm11,
  [43:40] Perm10,
  [39:36] Perm9,
  [35:32] Perm8,
  [31:28] Perm7,
  [27:24] Perm6,
  [23:20] Perm5,
  [19:16] Perm4,
  [15:12] Perm3,
  [11:8] Perm2,
  [7:4] Perm1,
  [3:0] Perm0,
};

accessor POR_EL3() <=> value : POR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for RCWMASK_EL1: read as zero, writes ignored. */
type RCWMASK_EL1_Type of bits(128)  /* When FEAT_D128 is implemented */;

accessor RCWMASK_EL1() <=> value : RCWMASK_EL1_Type
begin
    readonly getter
        return Zeros{128};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for RCWSMASK_EL1: read as zero, writes ignored. */
type RCWSMASK_EL1_Type of bits(128)  /* When FEAT_D128 is implemented */;

accessor RCWSMASK_EL1() <=> value : RCWSMASK_EL1_Type
begin
    readonly getter
        return Zeros{128};
    end;

    setter
        return;
    end;
end;

/* Projected deterministic MTE generator seed and tag inputs. */
type RGSR_EL1_Type of bits(64) {
    /* When GCR_EL1.RRND == '0' */
  [55:8] SEED,
  [3:0] TAG,
};

accessor RGSR_EL1() <=> value : RGSR_EL1_Type
begin
    readonly getter
        var result : RGSR_EL1_Type = Zeros{64};
        result.SEED[15:0] = A64Oracle_MTESeed;
        result.TAG = A64Oracle_MTETag[3:0];
        return result;
    end;

    setter
        A64Oracle_MTESeed = value.SEED[15:0];
        A64Oracle_MTETag[3:0] = value.TAG;
    end;
end;

/* Adapter policy for S2PIR_EL2: read as zero, writes ignored. */
type S2PIR_EL2_Type of bits(64) {
  [63:60] Perm15,
  [59:56] Perm14,
  [55:52] Perm13,
  [51:48] Perm12,
  [47:44] Perm11,
  [43:40] Perm10,
  [39:36] Perm9,
  [35:32] Perm8,
  [31:28] Perm7,
  [27:24] Perm6,
  [23:20] Perm5,
  [19:16] Perm4,
  [15:12] Perm3,
  [11:8] Perm2,
  [7:4] Perm1,
  [3:0] Perm0,
};

accessor S2PIR_EL2() <=> value : S2PIR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for S2POR_EL1: read as zero, writes ignored. */
type S2POR_EL1_Type of bits(64) {
  [63:60] Perm15,
  [59:56] Perm14,
  [55:52] Perm13,
  [51:48] Perm12,
  [47:44] Perm11,
  [43:40] Perm10,
  [39:36] Perm9,
  [35:32] Perm8,
  [31:28] Perm7,
  [27:24] Perm6,
  [23:20] Perm5,
  [19:16] Perm4,
  [15:12] Perm3,
  [11:8] Perm2,
  [7:4] Perm1,
  [3:0] Perm0,
};

accessor S2POR_EL1() <=> value : S2POR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SCR: read as zero, writes ignored. */
type SCR_Type of bits(32) {
  [15] TERR  /* When FEAT_RAS is implemented */,
  [13] TWE,
  [12] TWI,
  [9] SIF,
  [8] HCE,
  [7] SCD,
  [6] nET,
  [5] AW,
  [4] FW,
  [3] EA,
  [2] FIQ,
  [1] IRQ,
  [0] NS,
};

accessor SCR() <=> value : SCR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SCR2_EL3: read as zero, writes ignored. */
type SCR2_EL3_Type of bits(64) {
  [3] VTE  /* When FEAT_VMTE is implemented */,
  [2] SRMASK2En  /* When FEAT_SRMASK2 is implemented */,
  [1] NV3En  /* When FEAT_NV3 is implemented */,
  [0] FDIT  /* When FEAT_FDIT is implemented */,
};

accessor SCR2_EL3() <=> value : SCR2_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SCR_EL3: read as zero, writes ignored. */
type SCR_EL3_Type of bits(64) {
  [63] TPLIMEn  /* When FEAT_TPS is implemented */,
  [62] NSE  /* Otherwise or When FEAT_RME is implemented */,
  [61] HACDBSEn  /* When FEAT_HACDBS is implemented */,
  [60] HDBSSEn  /* When FEAT_HDBSS is implemented */,
  [59] FGTEn2  /* When FEAT_FGT2 is implemented */,
  [58] EnDSE  /* When FEAT_E3DSE is implemented */,
  [57] DSE  /* When FEAT_E3DSE is implemented */,
  [56] VTLBIDEn  /* When FEAT_TLBID is implemented */,
  [55] EnIDCP128  /* When FEAT_SYSREG128 is implemented */,
  [54] SRMASKEn  /* When FEAT_SRMASK is implemented */,
  [53] PFAREn  /* When FEAT_PFAR is implemented */,
  [52] TWERR  /* When FEAT_RASv2 is implemented */,
  [51] TMEA  /* When FEAT_DoubleFault2 is implemented */,
  [50] EnFPM  /* When FEAT_FPMR is implemented */,
  [49] MECEn  /* When FEAT_MEC is implemented */,
  [48] GPF  /* When FEAT_RME is implemented */,
  [47] D128En  /* When FEAT_D128 is implemented */,
  [46] AIEn  /* When FEAT_AIE is implemented */,
  [45] PIEn  /* When FEAT_S1PIE is implemented, or FEAT_S2PIE is implemented, or FEAT_S1POE is implemented, or FEAT_S2POE is implemented */,
  [44] SCTLR2En  /* When FEAT_SCTLR2 is implemented */,
  [43] TCR2En  /* When FEAT_TCR2 is implemented */,
  [42] RCWMASKEn  /* When FEAT_THE is implemented */,
  [41] EnTP2  /* When FEAT_SME is implemented */,
  [40] TRNDR  /* When FEAT_RNG_TRAP is implemented */,
  [39] GCSEn  /* When FEAT_GCS is implemented */,
  [38] HXEn  /* When FEAT_HCX is implemented */,
  [37] ADEn  /* When FEAT_LS64_ACCDATA is implemented */,
  [36] EnAS0  /* When FEAT_LS64_ACCDATA is implemented */,
  [35] AMVOFFEN  /* When FEAT_AMUv1p1 is implemented */,
  [33:30] TWEDEL  /* When FEAT_TWED is implemented */,
  [29] TWEDEn  /* When FEAT_TWED is implemented */,
  [28] ECVEn  /* When FEAT_ECV_POFF is implemented */,
  [27] FGTEn  /* When FEAT_FGT is implemented */,
  [26] ATA  /* When FEAT_MTE2 is implemented */,
  [25] EnSCXT  /* When FEAT_CSV2_2 is implemented or FEAT_CSV2_1p2 is implemented */,
  [24] POE2En  /* When FEAT_S1POE2 is implemented */,
  [23] TID5  /* When FEAT_IDTE3 is implemented and FEAT_MTE2 is implemented */,
  [22] TID3  /* When FEAT_IDTE3 is implemented */,
  [21] FIEN  /* When FEAT_RASv1p1 is implemented */,
  [20] NMEA  /* When FEAT_DoubleFault is implemented */,
  [19] EASE  /* When FEAT_DoubleFault is implemented */,
  [18] EEL2  /* When FEAT_SEL2 is implemented */,
  [17] API  /* When FEAT_PAuth is implemented */,
  [16] APK  /* When FEAT_PAuth is implemented */,
  [15] TERR  /* When FEAT_RAS is implemented */,
  [14] TLOR  /* When FEAT_LOR is implemented */,
  [13] TWE,
  [12] TWI,
  [11] ST,
  [10] RW  /* When FEAT_AA32EL1 is implemented */,
  [9] SIF,
  [8] HCE,
  [7] SMD,
  [3] EA,
  [2] FIQ,
  [1] IRQ,
  [0] NS  /* Otherwise or When FEAT_RME is implemented */,
};

accessor SCR_EL3() <=> value : SCR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SCTLR: read as zero, writes ignored. */
type SCTLR_Type of bits(32) {
  [31] DSSBS  /* When FEAT_SSBS is implemented */,
  [30] TE,
  [29] AFE,
  [28] TRE,
  [25] EE,
  [23] SPAN  /* When FEAT_PAN is implemented */,
  [20] UWXN,
  [19] WXN,
  [18] nTWE,
  [16] nTWI,
  [13] V,
  [12] I,
  [10] EnRCTX  /* When FEAT_SPECRES is implemented */,
  [8] SED,
  [7] ITD,
  [6] UNK,
  [5] CP15BEN,
  [4] LSMAOE  /* When FEAT_LSMAOC is implemented */,
  [3] nTLSMD  /* When FEAT_LSMAOC is implemented */,
  [2] C,
  [1] A,
  [0] M,
};

accessor SCTLR() <=> value : SCTLR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Projected effective EL0 checked-arithmetic controls. */
type SCTLR2_EL1_Type of bits(64) {
  [28] EnTP3  /* When FEAT_S1POE2 is implemented */,
  [26] TLBOSNIS  /* When FEAT_TLBID is implemented */,
  [25] FDIT  /* When FEAT_FDIT is implemented */,
  [24] BTD  /* When FEAT_PAuth_EnhCtl is implemented */,
  [23] BTD0  /* When FEAT_PAuth_EnhCtl is implemented */,
  [22] EnIA2  /* When FEAT_PAuth_EnhCtl is implemented */,
  [21] EnIB2  /* When FEAT_PAuth_EnhCtl is implemented */,
  [20] EnDA2  /* When FEAT_PAuth_EnhCtl is implemented */,
  [19] EnDB2  /* When FEAT_PAuth_EnhCtl is implemented */,
  [17] VT  /* When FEAT_VMTE is implemented */,
  [16] TEOS  /* When FEAT_TEV is implemented */,
  [15] TEIS  /* When FEAT_TEV is implemented */,
  [14] DTZ  /* When FEAT_S1POE2 is implemented */,
  [12] CPTM0  /* When FEAT_CPA2 is implemented */,
  [11] CPTM  /* When FEAT_CPA2 is implemented */,
  [10] CPTA0  /* When FEAT_CPA2 is implemented */,
  [9] CPTA  /* When FEAT_CPA2 is implemented */,
  [8] EnPACM0  /* When FEAT_PAuth_LR is implemented */,
  [7] EnPACM  /* When FEAT_PAuth_LR is implemented */,
  [6] EnIDCP128  /* When FEAT_SYSREG128 is implemented */,
  [5] EASE  /* When FEAT_DoubleFault2 is implemented */,
  [4] EnANERR  /* When FEAT_ANERR is implemented */,
  [3] EnADERR  /* When FEAT_ADERR is implemented */,
  [2] NMEA  /* When FEAT_DoubleFault2 is implemented */,
};

accessor SCTLR2_EL1() <=> value : SCTLR2_EL1_Type
begin
    readonly getter
        var result : SCTLR2_EL1_Type = Zeros{64};
        result.CPTA0 = if A64Oracle_CPAAddEnabled then '1' else '0';
        result.CPTM0 = if A64Oracle_CPAMultiplyEnabled then '1' else '0';
        return result;
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SCTLR2_EL2: read as zero, writes ignored. */
type SCTLR2_EL2_Type of bits(64) {
  [28] EnTP3  /* When FEAT_S1POE2 is implemented */,
  [26] TLBOSNIS  /* When FEAT_TLBID is implemented */,
  [25] FDIT  /* When FEAT_FDIT is implemented */,
  [24] BTD  /* When FEAT_PAuth_EnhCtl is implemented */,
  [23] BTD0  /* When FEAT_PAuth_EnhCtl is implemented */,
  [22] EnIA2  /* When FEAT_PAuth_EnhCtl is implemented */,
  [21] EnIB2  /* When FEAT_PAuth_EnhCtl is implemented */,
  [20] EnDA2  /* When FEAT_PAuth_EnhCtl is implemented */,
  [19] EnDB2  /* When FEAT_PAuth_EnhCtl is implemented */,
  [17] VT  /* When FEAT_VMTE is implemented */,
  [16] TEOS  /* When FEAT_TEV is implemented */,
  [15] TEIS  /* When FEAT_TEV is implemented */,
  [14] DTZ  /* When FEAT_S1POE2 is implemented */,
  [12] CPTM0  /* When FEAT_CPA2 is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [11] CPTM  /* When FEAT_CPA2 is implemented */,
  [10] CPTA0  /* When FEAT_CPA2 is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [9] CPTA  /* When FEAT_CPA2 is implemented */,
  [8] EnPACM0  /* When FEAT_PAuth_LR is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [7] EnPACM  /* When FEAT_PAuth_LR is implemented */,
  [6] EnIDCP128  /* When FEAT_SYSREG128 is implemented */,
  [5] EASE  /* When FEAT_DoubleFault2 is implemented */,
  [4] EnANERR  /* When FEAT_ANERR is implemented */,
  [3] EnADERR  /* When FEAT_ADERR is implemented */,
  [2] NMEA  /* When FEAT_DoubleFault2 is implemented */,
  [1] EMEC  /* When FEAT_MEC is implemented */,
};

accessor SCTLR2_EL2() <=> value : SCTLR2_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SCTLR2_EL3: read as zero, writes ignored. */
type SCTLR2_EL3_Type of bits(64) {
  [24] BTD  /* When FEAT_PAuth_EnhCtl is implemented */,
  [17] VT  /* When FEAT_VMTE is implemented */,
  [16] TEOS  /* When FEAT_TEV is implemented */,
  [15] TEIS  /* When FEAT_TEV is implemented */,
  [14] DTZ  /* When FEAT_S1POE2 is implemented */,
  [11] CPTM  /* When FEAT_CPA2 is implemented */,
  [9] CPTA  /* When FEAT_CPA2 is implemented */,
  [7] EnPACM  /* When FEAT_PAuth_LR is implemented */,
  [4] EnANERR  /* When FEAT_ANERR is implemented */,
  [3] EnADERR  /* When FEAT_ADERR is implemented */,
  [1] EMEC  /* When FEAT_MEC is implemented */,
};

accessor SCTLR2_EL3() <=> value : SCTLR2_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Projected EL0 access, MTE, and signing-profile controls. */
type SCTLR_EL1_Type of bits(64) {
  [63] TIDCP  /* When FEAT_TIDCP1 is implemented */,
  [62] SPINTMASK  /* When FEAT_NMI is implemented */,
  [61] NMI  /* When FEAT_NMI is implemented */,
  [60] EnTP2  /* When FEAT_SME is implemented */,
  [59] TCSO  /* When FEAT_MTE_STORE_ONLY is implemented */,
  [58] TCSO0  /* When FEAT_MTE_STORE_ONLY is implemented */,
  [57] EPAN  /* When FEAT_PAN3 is implemented */,
  [56] EnALS  /* When FEAT_LS64 is implemented */,
  [55] EnAS0  /* When FEAT_LS64_ACCDATA is implemented */,
  [54] EnASR  /* When FEAT_LS64_V is implemented */,
  [49:46] TWEDEL  /* When FEAT_TWED is implemented */,
  [45] TWEDEn  /* When FEAT_TWED is implemented */,
  [44] DSSBS  /* When FEAT_SSBS is implemented */,
  [43] ATA  /* When FEAT_MTE2 is implemented or FEAT_VMTE is implemented */,
  [42] ATA0  /* When FEAT_MTE2 is implemented or FEAT_VMTE is implemented */,
  [41:40] TCF  /* When FEAT_MTE2 is implemented or FEAT_VMTETC is implemented */,
  [39:38] TCF0  /* When FEAT_MTE2 is implemented or FEAT_VMTETC is implemented */,
  [37] ITFSB  /* When FEAT_MTE_ASYNC is implemented */,
  [36] BT1  /* When FEAT_BTI is implemented */,
  [35] BT0  /* When FEAT_BTI is implemented */,
  [34] EnFPM  /* When FEAT_FPMR is implemented */,
  [33] MSCEn  /* When FEAT_MOPS is implemented */,
  [32] CMOW  /* When FEAT_CMOW is implemented */,
  [31] EnIA  /* When FEAT_PAuth is implemented or When FEAT_PAuth_EnhCtl is implemented */,
  [30] EnIB  /* When FEAT_PAuth is implemented or When FEAT_PAuth_EnhCtl is implemented */,
  [29] LSMAOE  /* When FEAT_LSMAOC is implemented */,
  [28] nTLSMD  /* When FEAT_LSMAOC is implemented */,
  [27] EnDA  /* When FEAT_PAuth is implemented or When FEAT_PAuth_EnhCtl is implemented */,
  [26] UCI,
  [25] EE  /* When FEAT_MixedEnd is implemented */,
  [24] E0E  /* When FEAT_MixedEndEL0 is implemented */,
  [23] SPAN  /* When FEAT_PAN is implemented */,
  [22] EIS  /* When FEAT_ExS is implemented */,
  [21] IESB  /* When FEAT_IESB is implemented */,
  [20] TSCXT  /* When FEAT_CSV2_2 is implemented or FEAT_CSV2_1p2 is implemented */,
  [19] WXN,
  [18] nTWE,
  [16] nTWI,
  [15] UCT,
  [14] DZE,
  [13] EnDB  /* When FEAT_PAuth is implemented or When FEAT_PAuth_EnhCtl is implemented */,
  [12] I,
  [11] EOS  /* When FEAT_ExS is implemented */,
  [10] EnRCTX  /* When FEAT_SPECRES is implemented */,
  [9] UMA,
  [8] SED  /* When FEAT_AA32EL0 is implemented */,
  [7] ITD  /* When FEAT_AA32EL0 is implemented */,
  [6] nAA  /* When FEAT_LSE2 is implemented */,
  [5] CP15BEN  /* When FEAT_AA32EL0 is implemented */,
  [4] SA0,
  [3] SA,
  [2] C,
  [1] A,
  [0] M,
};

accessor SCTLR_EL1() <=> value : SCTLR_EL1_Type
begin
    readonly getter
        var result : SCTLR_EL1_Type = Zeros{64};
        // User-mode execution policy: an input contract that explicitly
        // requires FEAT_MOPS also grants EL0 access to the official MOPS
        // procedures.  CheckMOPSEnabled() independently consumes MSCEn.
        result.MSCEn =
            if A64Oracle_IsAllocationFeatureEnabled(FEAT_MOPS) then '1'
            else '0';
        result.ATA0 = if A64Oracle_MTEEnabled then '1' else '0';
        result.EnIA = if A64Oracle_SigningEnabled then '1' else '0';
        result.EnIB = if A64Oracle_SigningEnabled then '1' else '0';
        result.EnDA = if A64Oracle_SigningEnabled then '1' else '0';
        result.EnDB = if A64Oracle_SigningEnabled then '1' else '0';
        result.UMA = if A64Oracle_DAIFAccess then '1' else '0';
        return result;
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SCTLR_EL2: read as zero, writes ignored. */
type SCTLR_EL2_Type of bits(64) {
  [63] TIDCP  /* When FEAT_TIDCP1 is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [62] SPINTMASK  /* When FEAT_NMI is implemented */,
  [61] NMI  /* When FEAT_NMI is implemented */,
  [60] EnTP2  /* When FEAT_SME is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [59] TCSO  /* When FEAT_MTE_STORE_ONLY is implemented */,
  [58] TCSO0  /* When FEAT_MTE_STORE_ONLY is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [57] EPAN  /* When FEAT_PAN3 is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [56] EnALS  /* When FEAT_LS64 is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [55] EnAS0  /* When FEAT_LS64_ACCDATA is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [54] EnASR  /* When FEAT_LS64_V is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [49:46] TWEDEL  /* When FEAT_TWED is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [45] TWEDEn  /* When FEAT_TWED is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [44] DSSBS  /* When FEAT_SSBS is implemented */,
  [43] ATA  /* When FEAT_MTE2 is implemented or FEAT_VMTE is implemented */,
  [42] ATA0  /* When (FEAT_MTE2 is implemented or FEAT_VMTE is implemented) and EffectiveHCR_EL2_E2H() == '1' */,
  [41:40] TCF  /* When FEAT_MTE2 is implemented or FEAT_VMTETC is implemented */,
  [39:38] TCF0  /* When (FEAT_MTE2 is implemented or FEAT_VMTETC is implemented) and EffectiveHCR_EL2_E2H() == '1' */,
  [37] ITFSB  /* When FEAT_MTE_ASYNC is implemented */,
  [36] BT  /* When FEAT_BTI is implemented */,
  [35] BT0  /* When FEAT_BTI is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [34] EnFPM  /* When FEAT_FPMR is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [33] MSCEn  /* When FEAT_MOPS is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [32] CMOW  /* When FEAT_CMOW is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [31] EnIA  /* When FEAT_PAuth is implemented or When FEAT_PAuth_EnhCtl is implemented */,
  [30] EnIB  /* When FEAT_PAuth is implemented or When FEAT_PAuth_EnhCtl is implemented */,
  [29] LSMAOE  /* When FEAT_LSMAOC is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [28] nTLSMD  /* When FEAT_LSMAOC is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [27] EnDA  /* When FEAT_PAuth is implemented or When FEAT_PAuth_EnhCtl is implemented */,
  [26] UCI  /* When EffectiveHCR_EL2_E2H() == '1' */,
  [25] EE  /* When FEAT_MixedEnd is implemented */,
  [24] E0E  /* When FEAT_MixedEndEL0 is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [23] SPAN  /* When EffectiveHCR_EL2_E2H() == '1' */,
  [22] EIS  /* When FEAT_ExS is implemented */,
  [21] IESB  /* When FEAT_IESB is implemented */,
  [20] TSCXT  /* When (FEAT_CSV2_2 is implemented or FEAT_CSV2_1p2 is implemented) and EffectiveHCR_EL2_E2H() == '1' */,
  [19] WXN,
  [18] nTWE  /* When EffectiveHCR_EL2_E2H() == '1' */,
  [16] nTWI  /* When EffectiveHCR_EL2_E2H() == '1' */,
  [15] UCT  /* When EffectiveHCR_EL2_E2H() == '1' */,
  [14] DZE  /* When EffectiveHCR_EL2_E2H() == '1' */,
  [13] EnDB  /* When FEAT_PAuth is implemented or When FEAT_PAuth_EnhCtl is implemented */,
  [12] I,
  [11] EOS  /* When FEAT_ExS is implemented */,
  [10] EnRCTX  /* When FEAT_SPECRES is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [9] UMA  /* When FEAT_S1POE2 is implemented */,
  [8] SED  /* When FEAT_AA32EL0 is not implemented and EffectiveHCR_EL2_E2H() == '1' or When FEAT_AA32EL0 is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [7] ITD  /* When FEAT_AA32EL0 is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [6] nAA  /* When FEAT_LSE2 is implemented */,
  [5] CP15BEN  /* When FEAT_AA32EL0 is not implemented and EffectiveHCR_EL2_E2H() == '1' or When FEAT_AA32EL0 is implemented and EffectiveHCR_EL2_E2H() == '1' */,
  [4] SA0  /* When EffectiveHCR_EL2_E2H() == '1' */,
  [3] SA,
  [2] C,
  [1] A,
  [0] M,
};

accessor SCTLR_EL2() <=> value : SCTLR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SCTLR_EL3: read as zero, writes ignored. */
type SCTLR_EL3_Type of bits(64) {
  [62] SPINTMASK  /* When FEAT_NMI is implemented */,
  [61] NMI  /* When FEAT_NMI is implemented */,
  [59] TCSO  /* When FEAT_MTE_STORE_ONLY is implemented */,
  [44] DSSBS  /* When FEAT_SSBS is implemented */,
  [43] ATA  /* When FEAT_MTE2 is implemented or FEAT_VMTE is implemented */,
  [41:40] TCF  /* When FEAT_MTE2 is implemented or FEAT_VMTETC is implemented */,
  [37] ITFSB  /* When FEAT_MTE_ASYNC is implemented */,
  [36] BT  /* When FEAT_BTI is implemented */,
  [31] EnIA  /* When FEAT_PAuth is implemented */,
  [30] EnIB  /* When FEAT_PAuth is implemented */,
  [27] EnDA  /* When FEAT_PAuth is implemented */,
  [25] EE  /* When FEAT_MixedEnd is implemented */,
  [22] EIS  /* When FEAT_ExS is implemented */,
  [21] IESB  /* When FEAT_IESB is implemented */,
  [19] WXN,
  [13] EnDB  /* When FEAT_PAuth is implemented */,
  [12] I,
  [11] EOS  /* When FEAT_ExS is implemented */,
  [6] nAA  /* When FEAT_LSE2 is implemented */,
  [3] SA,
  [2] C,
  [1] A,
  [0] M,
};

accessor SCTLR_EL3() <=> value : SCTLR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SDCR: read as zero, writes ignored. */
type SDCR_Type of bits(32) {
  [28] MTPME  /* When FEAT_MTPMU is implemented */,
  [27] TDCC  /* When FEAT_FGT is implemented */,
  [23] SCCD  /* When FEAT_PMUv3p5 is implemented */,
  [21] EPMAD  /* When FEAT_PMUv3_EXT is implemented or When FEAT_Debugv8p4 is implemented and FEAT_PMUv3_EXT is implemented */,
  [20] EDAD  /* Otherwise or When FEAT_Debugv8p2 is implemented or When FEAT_Debugv8p4 is implemented */,
  [19] TTRF  /* When FEAT_TRF is implemented */,
  [18] STE  /* When FEAT_TRF is implemented */,
  [17] SPME  /* When FEAT_PMUv3 is implemented or When FEAT_PMUv3 is implemented and FEAT_Debugv8p2 is implemented */,
  [15:14] SPD,
};

accessor SDCR() <=> value : SDCR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SDER: read as zero, writes ignored. */
type SDER_Type of bits(32) {
  [1] SUNIDEN,
  [0] SUIDEN  /* When EL3 is implemented */,
};

accessor SDER() <=> value : SDER_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SDER32_EL3: read as zero, writes ignored. */
type SDER32_EL3_Type of bits(64) {
  [1] SUNIDEN,
  [0] SUIDEN,
};

accessor SDER32_EL3() <=> value : SDER32_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SMCR_EL1: read as zero, writes ignored. */
type SMCR_EL1_Type of bits(64) {
  [31] FA64  /* When FEAT_SME_FA64 is implemented */,
  [30] EZT0  /* When FEAT_SME2 is implemented */,
  [3:0] LEN,
};

accessor SMCR_EL1() <=> value : SMCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SMCR_EL2: read as zero, writes ignored. */
type SMCR_EL2_Type of bits(64) {
  [31] FA64  /* When FEAT_SME_FA64 is implemented */,
  [30] EZT0  /* When FEAT_SME2 is implemented */,
  [3:0] LEN,
};

accessor SMCR_EL2() <=> value : SMCR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SMCR_EL3: read as zero, writes ignored. */
type SMCR_EL3_Type of bits(64) {
  [31] FA64  /* When FEAT_SME_FA64 is implemented */,
  [30] EZT0  /* When FEAT_SME2 is implemented */,
  [3:0] LEN,
};

accessor SMCR_EL3() <=> value : SMCR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SPSR_EL1: read as zero, writes ignored. */
type SPSR_EL1_Type of bits(64) {
    /* When FEAT_AA32 is implemented and exception taken from AArch32 state i.e. exception taken from AArch32 or When exception taken from AArch64 state i.e. exception taken from AArch64 */
  [36] UINJ  /* When FEAT_UINJ is implemented */,
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [24] DIT  /* When FEAT_DIT is implemented */,
  [22] PAN  /* When FEAT_PAN is implemented */,
  [21] SS,
  [20] IL,
  [8] A,
  [7] I,
  [6] F,
  [4, 3:0] M,
};

accessor SPSR_EL1() <=> value : SPSR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SPSR_EL2: read as zero, writes ignored. */
type SPSR_EL2_Type of bits(64) {
    /* When FEAT_AA32 is implemented and exception taken from AArch32 state i.e. exception taken from AArch32 or When exception taken from AArch64 state i.e. exception taken from AArch64 */
  [36] UINJ  /* When FEAT_UINJ is implemented */,
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [24] DIT  /* When FEAT_DIT is implemented */,
  [22] PAN  /* When FEAT_PAN is implemented */,
  [21] SS,
  [20] IL,
  [8] A,
  [7] I,
  [6] F,
  [4, 3:0] M,
};

accessor SPSR_EL2() <=> value : SPSR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SPSR_EL3: read as zero, writes ignored. */
type SPSR_EL3_Type of bits(64) {
    /* When FEAT_AA32 is implemented and exception taken from AArch32 state i.e. exception taken from AArch32 or When exception taken from AArch64 state i.e. exception taken from AArch64 */
  [36] UINJ  /* When FEAT_UINJ is implemented */,
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [24] DIT  /* When FEAT_DIT is implemented */,
  [22] PAN  /* When FEAT_PAN is implemented */,
  [21] SS,
  [20] IL,
  [8] A,
  [7] I,
  [6] F,
  [4, 3:0] M,
};

accessor SPSR_EL3() <=> value : SPSR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SPSR_abt: read as zero, writes ignored. */
type SPSR_abt_Type of bits(32) {
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [27] Q,
  [26:25, 15:10] IT,
  [24] J,
  [23] SSBS  /* When FEAT_SSBS is implemented */,
  [22] PAN  /* When FEAT_PAN is implemented */,
  [21] DIT  /* When FEAT_DIT is implemented */,
  [20] IL,
  [19:16] GE,
  [9] E,
  [8] A,
  [7] I,
  [6] F,
  [5] T,
  [4:0] M,
};

accessor SPSR_abt() <=> value : SPSR_abt_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SPSR_fiq: read as zero, writes ignored. */
type SPSR_fiq_Type of bits(32) {
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [27] Q,
  [26:25, 15:10] IT,
  [24] J,
  [23] SSBS  /* When FEAT_SSBS is implemented */,
  [22] PAN  /* When FEAT_PAN is implemented */,
  [21] DIT  /* When FEAT_DIT is implemented */,
  [20] IL,
  [19:16] GE,
  [9] E,
  [8] A,
  [7] I,
  [6] F,
  [5] T,
  [4:0] M,
};

accessor SPSR_fiq() <=> value : SPSR_fiq_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SPSR_hyp: read as zero, writes ignored. */
type SPSR_hyp_Type of bits(32) {
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [27] Q,
  [26:25, 15:10] IT,
  [24] J,
  [23] SSBS  /* When FEAT_SSBS is implemented */,
  [22] PAN  /* When FEAT_PAN is implemented */,
  [21] DIT  /* When FEAT_DIT is implemented */,
  [20] IL,
  [19:16] GE,
  [9] E,
  [8] A,
  [7] I,
  [6] F,
  [5] T,
  [4:0] M,
};

accessor SPSR_hyp() <=> value : SPSR_hyp_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SPSR_irq: read as zero, writes ignored. */
type SPSR_irq_Type of bits(32) {
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [27] Q,
  [26:25, 15:10] IT,
  [24] J,
  [23] SSBS  /* When FEAT_SSBS is implemented */,
  [22] PAN  /* When FEAT_PAN is implemented */,
  [21] DIT  /* When FEAT_DIT is implemented */,
  [20] IL,
  [19:16] GE,
  [9] E,
  [8] A,
  [7] I,
  [6] F,
  [5] T,
  [4:0] M,
};

accessor SPSR_irq() <=> value : SPSR_irq_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SPSR_mon: read as zero, writes ignored. */
type SPSR_mon_Type of bits(32) {
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [27] Q,
  [26:25, 15:10] IT,
  [24] J,
  [23] SSBS  /* When FEAT_SSBS is implemented */,
  [22] PAN  /* When FEAT_PAN is implemented */,
  [21] DIT  /* When FEAT_DIT is implemented */,
  [20] IL,
  [19:16] GE,
  [9] E,
  [8] A,
  [7] I,
  [6] F,
  [5] T,
  [4:0] M,
};

accessor SPSR_mon() <=> value : SPSR_mon_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SPSR_svc: read as zero, writes ignored. */
type SPSR_svc_Type of bits(32) {
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [27] Q,
  [26:25, 15:10] IT,
  [24] J,
  [23] SSBS  /* When FEAT_SSBS is implemented */,
  [22] PAN  /* When FEAT_PAN is implemented */,
  [21] DIT  /* When FEAT_DIT is implemented */,
  [20] IL,
  [19:16] GE,
  [9] E,
  [8] A,
  [7] I,
  [6] F,
  [5] T,
  [4:0] M,
};

accessor SPSR_svc() <=> value : SPSR_svc_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for SPSR_und: read as zero, writes ignored. */
type SPSR_und_Type of bits(32) {
  [31] N,
  [30] Z,
  [29] C,
  [28] V,
  [27] Q,
  [26:25, 15:10] IT,
  [24] J,
  [23] SSBS  /* When FEAT_SSBS is implemented */,
  [22] PAN  /* When FEAT_PAN is implemented */,
  [21] DIT  /* When FEAT_DIT is implemented */,
  [20] IL,
  [19:16] GE,
  [9] E,
  [8] A,
  [7] I,
  [6] F,
  [5] T,
  [4:0] M,
};

accessor SPSR_und() <=> value : SPSR_und_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TCR2_EL1: read as zero, writes ignored. */
type TCR2_EL1_Type of bits(64) {
  [36] TVAD1  /* When FEAT_VMTE is implemented */,
  [35] TVAD0  /* When FEAT_VMTE is implemented */,
  [34:30] VTB1  /* When FEAT_VMTE is implemented */,
  [29:25] VTB0  /* When FEAT_VMTE is implemented */,
  [24:22] POIW  /* When FEAT_S1POE2 is implemented */,
  [21] FNGNA1  /* When FEAT_THE is implemented */,
  [20] FNGNA0  /* When FEAT_THE is implemented */,
  [19] POE2F  /* When FEAT_S1POE2 is implemented */,
  [18] FNG1  /* When FEAT_ASID2 is implemented */,
  [17] FNG0  /* When FEAT_ASID2 is implemented */,
  [16] A2  /* When FEAT_ASID2 is implemented */,
  [15] DisCH1  /* When FEAT_D128 is implemented and TCR2_EL1.D128 == '1' */,
  [14] DisCH0  /* When FEAT_D128 is implemented and TCR2_EL1.D128 == '1' */,
  [11] HAFT  /* When FEAT_HAFT is implemented */,
  [10] PTTWI  /* When FEAT_THE is implemented */,
  [5] D128  /* When FEAT_D128 is implemented */,
  [4] AIE  /* When FEAT_AIE is implemented */,
  [3] POE  /* When FEAT_S1POE is implemented */,
  [2] E0POE  /* When FEAT_S1POE is implemented */,
  [1] PIE  /* When FEAT_S1PIE is implemented */,
  [0] PnCH  /* When FEAT_THE is implemented */,
};

accessor TCR2_EL1() <=> value : TCR2_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TCR2_EL2: read as zero, writes ignored. */
type TCR2_EL2_Type of bits(64) {
    /* When EffectiveHCR_EL2_E2H() == '0' or When EffectiveHCR_EL2_E2H() == '1' */
  [12] AMEC0  /* When FEAT_MEC is implemented */,
  [11] HAFT  /* When FEAT_HAFT is implemented */,
  [10] PTTWI  /* When FEAT_THE is implemented */,
  [4] AIE  /* When FEAT_AIE is implemented */,
  [3] POE  /* When FEAT_S1POE is implemented */,
  [1] PIE  /* When FEAT_S1PIE is implemented */,
  [0] PnCH  /* When FEAT_THE is implemented */,
  [36] TVAD1  /* When FEAT_VMTE is implemented */,
  [35] TVAD0  /* When FEAT_VMTE is implemented */,
  [34:30] VTB1  /* When FEAT_VMTE is implemented */,
  [29:25] VTB0  /* When FEAT_VMTE is implemented */,
  [24:22] POIW  /* When FEAT_S1POE2 is implemented */,
  [19] POE2F  /* When FEAT_S1POE2 is implemented */,
  [18] FNG1  /* When FEAT_ASID2 is implemented */,
  [17] FNG0  /* When FEAT_ASID2 is implemented */,
  [16] A2  /* When FEAT_ASID2 is implemented */,
  [15] DisCH1  /* When FEAT_D128 is implemented and TCR2_EL2.D128 == '1' */,
  [14] DisCH0  /* When FEAT_D128 is implemented and TCR2_EL2.D128 == '1' */,
  [13] AMEC1  /* When FEAT_MEC is implemented */,
  [5] D128  /* When FEAT_D128 is implemented */,
  [2] E0POE  /* When FEAT_S1POE is implemented */,
};

accessor TCR2_EL2() <=> value : TCR2_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TCR_EL1: read as zero, writes ignored. */
type TCR_EL1_Type of bits(64) {
  [61] MTX1  /* When FEAT_MTE_NO_ADDRESS_TAGS is implemented or FEAT_MTE_CANONICAL_TAGS is implemented */,
  [60] MTX0  /* When FEAT_MTE_NO_ADDRESS_TAGS is implemented or FEAT_MTE_CANONICAL_TAGS is implemented */,
  [59] DS  /* Otherwise or When FEAT_LPA2 is implemented and (FEAT_D128 is not implemented or TCR2_EL1.D128 == '0') */,
  [58] TCMA1  /* When FEAT_MTE2 is implemented or FEAT_VMTETC is implemented */,
  [57] TCMA0  /* When FEAT_MTE2 is implemented or FEAT_VMTETC is implemented */,
  [56] E0PD1  /* When FEAT_E0PD is implemented */,
  [55] E0PD0  /* When FEAT_E0PD is implemented */,
  [54] NFD1  /* When FEAT_SVE is implemented */,
  [53] NFD0  /* When FEAT_SVE is implemented */,
  [52] TBID1  /* When FEAT_PAuth is implemented */,
  [51] TBID0  /* When FEAT_PAuth is implemented */,
  [50] HWU162  /* When FEAT_HPDS2 is implemented */,
  [49] HWU161  /* When FEAT_HPDS2 is implemented */,
  [48] HWU160  /* When FEAT_HPDS2 is implemented */,
  [47] HWU159  /* When FEAT_HPDS2 is implemented */,
  [46] HWU062  /* When FEAT_HPDS2 is implemented */,
  [45] HWU061  /* When FEAT_HPDS2 is implemented */,
  [44] HWU060  /* When FEAT_HPDS2 is implemented */,
  [43] HWU059  /* When FEAT_HPDS2 is implemented */,
  [42] HPD1  /* When FEAT_HPDS is implemented */,
  [41] HPD0  /* When FEAT_HPDS is implemented */,
  [40] HD  /* When FEAT_HAFDBS is implemented */,
  [39] HA  /* When FEAT_HAF is implemented */,
  [38] TBI1,
  [37] TBI0,
  [36] AS,
  [34:32] IPS,
  [31:30] TG1,
  [29:28] SH1,
  [27:26] ORGN1,
  [25:24] IRGN1,
  [23] EPD1,
  [22] A1,
  [21:16] T1SZ,
  [15:14] TG0,
  [13:12] SH0,
  [11:10] ORGN0,
  [9:8] IRGN0,
  [7] EPD0,
  [5:0] T0SZ,
};

accessor TCR_EL1() <=> value : TCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TCR_EL2: read as zero, writes ignored. */
type TCR_EL2_Type of bits(64) {
    /* When EffectiveHCR_EL2_E2H() == '0' i.e. EffectiveHCR_EL2_E2H() == '0' or When EffectiveHCR_EL2_E2H() == '1' i.e. EffectiveHCR_EL2_E2H() == '1' */
  [33] MTX  /* When FEAT_MTE_NO_ADDRESS_TAGS is implemented or FEAT_MTE_CANONICAL_TAGS is implemented */,
  [59] DS  /* When FEAT_LPA2 is implemented and (FEAT_D128 is not implemented or TCR2_EL2.D128 == '0') */,
  [30] TCMA  /* When FEAT_MTE2 is implemented or FEAT_VMTETC is implemented */,
  [29] TBID  /* When FEAT_PAuth is implemented */,
  [28] HWU62  /* When FEAT_HPDS2 is implemented */,
  [27] HWU61  /* When FEAT_HPDS2 is implemented */,
  [26] HWU60  /* When FEAT_HPDS2 is implemented */,
  [25] HWU59  /* When FEAT_HPDS2 is implemented */,
  [24] HPD  /* When FEAT_HPDS is implemented */,
  [40] HD  /* When FEAT_HAFDBS is implemented */,
  [39] HA  /* When FEAT_HAF is implemented */,
  [20] TBI,
  [18:16] PS,
  [15:14] TG0,
  [13:12] SH0,
  [11:10] ORGN0,
  [9:8] IRGN0,
  [5:0] T0SZ,
  [61] MTX1  /* When FEAT_MTE_NO_ADDRESS_TAGS is implemented or FEAT_MTE_CANONICAL_TAGS is implemented */,
  [60] MTX0  /* When FEAT_MTE_NO_ADDRESS_TAGS is implemented or FEAT_MTE_CANONICAL_TAGS is implemented */,
  [58] TCMA1  /* When FEAT_MTE2 is implemented or FEAT_VMTETC is implemented */,
  [57] TCMA0  /* When FEAT_MTE2 is implemented or FEAT_VMTETC is implemented */,
  [56] E0PD1  /* When FEAT_E0PD is implemented */,
  [55] E0PD0  /* When FEAT_E0PD is implemented */,
  [54] NFD1  /* When FEAT_SVE is implemented */,
  [53] NFD0  /* When FEAT_SVE is implemented */,
  [52] TBID1  /* When FEAT_PAuth is implemented */,
  [51] TBID0  /* When FEAT_PAuth is implemented */,
  [50] HWU162  /* When FEAT_HPDS2 is implemented */,
  [49] HWU161  /* When FEAT_HPDS2 is implemented */,
  [48] HWU160  /* When FEAT_HPDS2 is implemented */,
  [47] HWU159  /* When FEAT_HPDS2 is implemented */,
  [46] HWU062  /* When FEAT_HPDS2 is implemented */,
  [45] HWU061  /* When FEAT_HPDS2 is implemented */,
  [44] HWU060  /* When FEAT_HPDS2 is implemented */,
  [43] HWU059  /* When FEAT_HPDS2 is implemented */,
  [42] HPD1  /* When FEAT_HPDS is implemented */,
  [41] HPD0  /* When FEAT_HPDS is implemented */,
  [38] TBI1,
  [37] TBI0,
  [36] AS,
  [34:32] IPS,
  [31:30] TG1,
  [29:28] SH1,
  [27:26] ORGN1,
  [25:24] IRGN1,
  [23] EPD1,
  [22] A1,
  [21:16] T1SZ,
  [7] EPD0,
};

accessor TCR_EL2() <=> value : TCR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TCR_EL3: read as zero, writes ignored. */
type TCR_EL3_Type of bits(64) {
  [53] TVAD  /* When FEAT_VMTE is implemented */,
  [52:48] VTB  /* When FEAT_VMTE is implemented */,
  [47:45] POIW  /* When FEAT_S1POE2 is implemented */,
  [44] POE2F  /* When FEAT_S1POE2 is implemented */,
  [43] DisCH0  /* When FEAT_D128 is implemented and TCR_EL3.D128 == '1' */,
  [42] HAFT  /* When FEAT_HAFT is implemented */,
  [41] PTTWI  /* When FEAT_THE is implemented */,
  [38] D128  /* When FEAT_D128 is implemented */,
  [37] AIE  /* When FEAT_AIE is implemented */,
  [36] POE  /* When FEAT_S1POE is implemented */,
  [35] PIE  /* When FEAT_S1PIE is implemented */,
  [34] PnCH  /* When FEAT_THE is implemented */,
  [33] MTX  /* When FEAT_MTE_NO_ADDRESS_TAGS is implemented or FEAT_MTE_CANONICAL_TAGS is implemented */,
  [32] DS  /* Otherwise or When FEAT_LPA2 is implemented and (FEAT_D128 is not implemented or TCR_EL3.D128 == '0') */,
  [30] TCMA  /* When FEAT_MTE2 is implemented or FEAT_VMTETC is implemented */,
  [29] TBID  /* When FEAT_PAuth is implemented */,
  [28] HWU62  /* When FEAT_HPDS2 is implemented */,
  [27] HWU61  /* When FEAT_HPDS2 is implemented */,
  [26] HWU60  /* When FEAT_HPDS2 is implemented */,
  [25] HWU59  /* When FEAT_HPDS2 is implemented */,
  [24] HPD  /* When FEAT_HPDS is implemented */,
  [22] HD  /* When FEAT_HAFDBS is implemented */,
  [21] HA  /* When FEAT_HAF is implemented */,
  [20] TBI,
  [18:16] PS,
  [15:14] TG0,
  [13:12] SH0,
  [11:10] ORGN0,
  [9:8] IRGN0,
  [5:0] T0SZ,
};

accessor TCR_EL3() <=> value : TCR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TFSRE0_EL1: read as zero, writes ignored. */
type TFSRE0_EL1_Type of bits(64) {
    /* When FEAT_MTE_ASYNC is not implemented */
  [1] TF1,
  [0] TF0,
};

accessor TFSRE0_EL1() <=> value : TFSRE0_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TFSR_EL1: read as zero, writes ignored. */
type TFSR_EL1_Type of bits(64) {
    /* When FEAT_MTE_ASYNC is not implemented */
  [1] TF1,
  [0] TF0,
};

accessor TFSR_EL1() <=> value : TFSR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TFSR_EL2: read as zero, writes ignored. */
type TFSR_EL2_Type of bits(64) {
    /* When FEAT_MTE_ASYNC is not implemented */
  [1] TF1,
  [0] TF0,
};

accessor TFSR_EL2() <=> value : TFSR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TFSR_EL3: read as zero, writes ignored. */
type TFSR_EL3_Type of bits(64) {
    /* When FEAT_MTE_ASYNC is not implemented */
  [0] TF0,
};

accessor TFSR_EL3() <=> value : TFSR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TRBLIMITR_EL1: read as zero, writes ignored. */
type TRBLIMITR_EL1_Type of bits(64) {
  [63:12] LIMIT,
  [6] XE  /* When FEAT_TRBE_EXT is implemented */,
  [5] nVM,
  [4:3] TM,
  [2:1] FM,
  [0] E,
};

accessor TRBLIMITR_EL1() <=> value : TRBLIMITR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TRBMAR_EL1: read as zero, writes ignored. */
type TRBMAR_EL1_Type of bits(64) {
  [46:40] FPOIndex  /* When FEAT_S1POE2 is implemented */,
  [39:33] TIndex  /* When FEAT_S1POE2 is implemented */,
  [32] TTBA  /* When FEAT_S1POE2 is implemented */,
  [11:10] PAS  /* When FEAT_TRBE_EXT is implemented */,
  [9:8] SH,
  [7:0] Attr,
};

accessor TRBMAR_EL1() <=> value : TRBMAR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TRBSR_EL1: read as zero, writes ignored. */
type TRBSR_EL1_Type of bits(64) {
  [55:32] MSS2,
  [31:26] EC,
  [22] IRQ,
  [21] TRG,
  [20] WRAP,
  [18] EA  /* When the PE sets this bit as the result of an External abort */,
  [17] S,
  [15:0] MSS {
    [5:0] BSC,
  },
};

accessor TRBSR_EL1() <=> value : TRBSR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TRBSR_EL2: read as zero, writes ignored. */
type TRBSR_EL2_Type of bits(64) {
  [55:32] MSS2,
  [31:26] EC,
  [22] IRQ,
  [21] TRG,
  [20] WRAP,
  [18] EA  /* When the PE sets this bit as the result of an External abort */,
  [17] S,
  [15:0] MSS {
    [5:0] BSC,
  },
};

accessor TRBSR_EL2() <=> value : TRBSR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TRBSR_EL3: read as zero, writes ignored. */
type TRBSR_EL3_Type of bits(64) {
  [55:32] MSS2,
  [31:26] EC,
  [22] IRQ,
  [21] TRG,
  [20] WRAP,
  [18] EA  /* When the PE sets this bit as the result of an External abort */,
  [17] S,
  [15:0] MSS {
    [5:0] BSC,
  },
};

accessor TRBSR_EL3() <=> value : TRBSR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TRFCR: read as zero, writes ignored. */
type TRFCR_Type of bits(32) {
  [6:5] TS,
  [1] E1TRE,
  [0] E0TRE,
};

accessor TRFCR() <=> value : TRFCR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TRFCR_EL1: read as zero, writes ignored. */
type TRFCR_EL1_Type of bits(64) {
  [11] DnVM  /* When FEAT_TRBEv1p1 is implemented and FEAT_NV is implemented */,
  [10] KE  /* When FEAT_TRBE_EXC is implemented */,
  [9:8] EE  /* When FEAT_TRBE_EXC is implemented */,
  [6:5] TS,
  [3] CX  /* When FEAT_NV2p1 is implemented */,
  [1] E1TRE,
  [0] E0TRE,
};

accessor TRFCR_EL1() <=> value : TRFCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TRFCR_EL2: read as zero, writes ignored. */
type TRFCR_EL2_Type of bits(64) {
  [11] DnVM  /* When FEAT_TRBEv1p1 is implemented */,
  [10] KE  /* When FEAT_TRBE_EXC is implemented */,
  [9:8] EE  /* When FEAT_TRBE_EXC is implemented */,
  [6:5] TS,
  [3] CX,
  [1] E2TRE,
  [0] E0HTRE,
};

accessor TRFCR_EL2() <=> value : TRFCR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TTBCR: read as zero, writes ignored. */
type TTBCR_Type of bits(32) {
    /* When TTBCR.EAE == '0' i.e. AArch32-TTBCR.EAE == 0 or When TTBCR.EAE == '1' i.e. AArch32-TTBCR.EAE==1 */
  [31] EAE,
  [5] PD1,
  [4] PD0,
  [2:0] N,
  [29:28] SH1,
  [27:26] ORGN1,
  [25:24] IRGN1,
  [23] EPD1,
  [22] A1,
  [18:16] T1SZ,
  [13:12] SH0,
  [11:10] ORGN0,
  [9:8] IRGN0,
  [7] EPD0,
  [6] T2E  /* When FEAT_AA32HPD is implemented */,
  [2:0] T0SZ,
};

accessor TTBCR() <=> value : TTBCR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TTBR0_EL1: read as zero, writes ignored. */
type TTBR0_EL1_Type of bits(128) {
    /* When FEAT_D128 is implemented and TCR2_EL1.D128 == '1' or When FEAT_D128 is not implemented or TCR2_EL1.D128 == '0' */
  [63:48] ASID,
  [2:1] SKL,
  [0] CnP  /* When FEAT_TTCNP is implemented */,
};

accessor TTBR0_EL1() <=> value : TTBR0_EL1_Type
begin
    readonly getter
        return Zeros{128};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TTBR0_EL2: read as zero, writes ignored. */
type TTBR0_EL2_Type of bits(128) {
    /* When FEAT_D128 is implemented, TCR2_EL2.D128 == '1', and EffectiveHCR_EL2_E2H() == '1' or When FEAT_D128 is not implemented or TCR2_EL2.D128 == '0' */
  [63:48] ASID  /* When FEAT_VHE is implemented */,
  [2:1] SKL,
  [0] CnP  /* When FEAT_TTCNP is implemented */,
};

accessor TTBR0_EL2() <=> value : TTBR0_EL2_Type
begin
    readonly getter
        return Zeros{128};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TTBR0_EL3: read as zero, writes ignored. */
type TTBR0_EL3_Type of bits(64) {
    /* When FEAT_D128 is implemented and TCR_EL3.D128 == '1' or When FEAT_D128 is not implemented or TCR_EL3.D128 == '0' */
  [2:1] SKL,
  [0] CnP  /* When FEAT_TTCNP is implemented */,
};

accessor TTBR0_EL3() <=> value : TTBR0_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TTBR1_EL1: read as zero, writes ignored. */
type TTBR1_EL1_Type of bits(128) {
    /* When FEAT_D128 is implemented and TCR2_EL1.D128 == '1' or When FEAT_D128 is not implemented or TCR2_EL1.D128 == '0' */
  [63:48] ASID,
  [2:1] SKL,
  [0] CnP  /* When FEAT_TTCNP is implemented */,
};

accessor TTBR1_EL1() <=> value : TTBR1_EL1_Type
begin
    readonly getter
        return Zeros{128};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for TTBR1_EL2: read as zero, writes ignored. */
type TTBR1_EL2_Type of bits(128) {
    /* When FEAT_D128 is implemented, TCR2_EL2.D128 == '1', and EffectiveHCR_EL2_E2H() == '1' or When FEAT_D128 is not implemented or TCR2_EL2.D128 == '0' */
  [63:48] ASID,
  [2:1] SKL,
  [0] CnP  /* When FEAT_TTCNP is implemented */,
};

accessor TTBR1_EL2() <=> value : TTBR1_EL2_Type
begin
    readonly getter
        return Zeros{128};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VDFSR: read as zero, writes ignored. */
type VDFSR_Type of bits(32) {
  [15:14] AET,
  [12] ExT,
};

accessor VDFSR() <=> value : VDFSR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VDISR: read as zero, writes ignored. */
type VDISR_Type of bits(32) {
    /* When TTBCR.EAE == '0' or When TTBCR.EAE == '1' */
  [31] A,
  [15:14] AET,
  [12] ExT,
  [9] LPAE,
};

accessor VDISR() <=> value : VDISR_Type
begin
    readonly getter
        return Zeros{32};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VDISR_EL2: read as zero, writes ignored. */
type VDISR_EL2_Type of bits(64) {
    /* When EL1 is using AArch64 or When EL1 is using AArch32 and VDISR_EL2.LPAE == '0' or When EL1 is using AArch32 and VDISR_EL2.LPAE == '1' */
  [31] A,
};

accessor VDISR_EL2() <=> value : VDISR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VDISR_EL3: read as zero, writes ignored. */
type VDISR_EL3_Type of bits(64) {
  [31] A,
  [24] IDS,
  [23:0] ISS,
};

accessor VDISR_EL3() <=> value : VDISR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VMECID_A_EL2: read as zero, writes ignored. */
type VMECID_A_EL2_Type of bits(64) {
  [15:0] MECID,
};

accessor VMECID_A_EL2() <=> value : VMECID_A_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VMECID_P_EL2: read as zero, writes ignored. */
type VMECID_P_EL2_Type of bits(64) {
  [15:0] MECID,
};

accessor VMECID_P_EL2() <=> value : VMECID_P_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VSESR_EL2: read as zero, writes ignored. */
type VSESR_EL2_Type of bits(64) {
    /* When EL1 is using AArch32 or When EL1 is using AArch64 */
  [15:14] AET,
  [12] ExT,
  [24] IDS,
  [23:0] ISS,
};

accessor VSESR_EL2() <=> value : VSESR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VSESR_EL3: read as zero, writes ignored. */
type VSESR_EL3_Type of bits(64) {
  [24] IDS,
  [23:0] ISS,
};

accessor VSESR_EL3() <=> value : VSESR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VSTCR_EL2: read as zero, writes ignored. */
type VSTCR_EL2_Type of bits(64) {
  [33] SL2  /* When FEAT_LPA2 is implemented and (FEAT_D128 is not implemented or VTCR_EL2.D128 == '0') */,
  [30] SA,
  [29] SW,
  [15:14] TG0,
  [7:6] SL0  /* When FEAT_TTST is not implemented and (FEAT_D128 is not implemented or VTCR_EL2.D128 == '0') or When FEAT_TTST is implemented and (FEAT_D128 is not implemented or VTCR_EL2.D128 == '0') */,
  [5:0] T0SZ,
};

accessor VSTCR_EL2() <=> value : VSTCR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VSTTBR_EL2: read as zero, writes ignored. */
type VSTTBR_EL2_Type of bits(64) {
    /* When FEAT_D128 is implemented and VTCR_EL2.D128 == '1' or When FEAT_D128 is not implemented or VTCR_EL2.D128 == '0' */
  [2:1] SKL,
  [0] CnP,
};

accessor VSTTBR_EL2() <=> value : VSTTBR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VTCR_EL2: read as zero, writes ignored. */
type VTCR_EL2_Type of bits(64) {
  [45] HDBSS  /* When FEAT_HDBSS is implemented */,
  [44] HAFT  /* When FEAT_HAFT is implemented */,
  [41] TL0  /* When FEAT_THE is implemented */,
  [40] GCSH  /* When FEAT_THE is implemented and FEAT_GCS is implemented */,
  [38] D128  /* When FEAT_D128 is implemented */,
  [37] S2POE  /* When FEAT_S2POE is implemented */,
  [36] S2PIE  /* When FEAT_S2PIE is implemented */,
  [35] TL1  /* When FEAT_THE is implemented */,
  [34] AssuredOnly  /* When FEAT_THE is implemented */,
  [33] SL2  /* When FEAT_LPA2 is implemented and (FEAT_D128 is not implemented or VTCR_EL2.D128 == '0') */,
  [32] DS  /* When FEAT_LPA2 is implemented and (FEAT_D128 is not implemented or VTCR_EL2.D128 == '0') */,
  [30] NSA  /* When FEAT_SEL2 is implemented */,
  [29] NSW  /* When FEAT_SEL2 is implemented */,
  [28] HWU62  /* When FEAT_HPDS2 is implemented */,
  [27] HWU61  /* When FEAT_HPDS2 is implemented */,
  [26] HWU60  /* When FEAT_HPDS2 is implemented */,
  [25] HWU59  /* When FEAT_HPDS2 is implemented */,
  [22] HD  /* When FEAT_HAFDBS is implemented */,
  [21] HA  /* When FEAT_HAF is implemented */,
  [19] VS  /* When FEAT_VMID16 is implemented */,
  [18:16] PS,
  [15:14] TG0,
  [13:12] SH0,
  [11:10] ORGN0,
  [9:8] IRGN0,
  [7:6] SL0  /* When FEAT_TTST is not implemented and (FEAT_D128 is not implemented or VTCR_EL2.D128 == '0') or When FEAT_TTST is implemented and (FEAT_D128 is not implemented or VTCR_EL2.D128 == '0') */,
  [5:0] T0SZ,
};

accessor VTCR_EL2() <=> value : VTCR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VTTBR: read as zero, writes ignored. */
type VTTBR_Type of bits(64) {
  [55:48] VMID,
  [47:1] BADDR,
  [0] CnP  /* When FEAT_TTCNP is implemented */,
};

accessor VTTBR() <=> value : VTTBR_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for VTTBR_EL2: read as zero, writes ignored. */
type VTTBR_EL2_Type of bits(128) {
    /* When FEAT_D128 is implemented and VTCR_EL2.D128 == '1' or When FEAT_D128 is not implemented or VTCR_EL2.D128 == '0' */
  [63:48] VMID {
    [15:0] VMID,
  },
  [2:1] SKL,
  [0] CnP  /* When FEAT_TTCNP is implemented */,
};

accessor VTTBR_EL2() <=> value : VTTBR_EL2_Type
begin
    readonly getter
        return Zeros{128};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for ZCR_EL1: read as zero, writes ignored. */
type ZCR_EL1_Type of bits(64) {
  [3:0] LEN,
};

accessor ZCR_EL1() <=> value : ZCR_EL1_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for ZCR_EL2: read as zero, writes ignored. */
type ZCR_EL2_Type of bits(64) {
  [3:0] LEN,
};

accessor ZCR_EL2() <=> value : ZCR_EL2_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/* Adapter policy for ZCR_EL3: read as zero, writes ignored. */
type ZCR_EL3_Type of bits(64) {
  [3:0] LEN,
};

accessor ZCR_EL3() <=> value : ZCR_EL3_Type
begin
    readonly getter
        return Zeros{64};
    end;

    setter
        return;
    end;
end;

/*
 * Public exact-state bridge used by the concrete materialization executor.
 * The key array is installed separately to keep this scalar call finite and
 * index-checkable.  FPCR/FPSR/FPMR are installed by the system-register
 * module because their typed storage is owned by the FP state module.
 */
func A64Oracle_InitializeProjectedSystemState(
    thread_pointer : bits(64),
    pstate_ssbs : boolean,
    pstate_dit : boolean,
    pstate_tco : boolean,
    pstate_sm : boolean,
    pstate_za : boolean,
    pstate_daif_access : boolean,
    pstate_daif : bits(8),
    mte_enabled : boolean,
    mte_exclude : bits(16),
    mte_random_tags : boolean,
    mte_seed : bits(16),
    mte_tag : bits(8),
    cpa_add_enabled : boolean,
    cpa_multiply_enabled : boolean,
    accdata : bits(32),
    gcs_pointer : bits(64),
    gcs_implemented : boolean,
    gcs_push_enabled : boolean,
    gcs_store_enabled : boolean,
    signing_enabled : boolean)
begin
    A64Oracle_ProjectedSystemStateInstalled = TRUE;
    A64Oracle_ThreadPointer = thread_pointer;
    PSTATE.SSBS = if pstate_ssbs then '1' else '0';
    PSTATE.DIT = if pstate_dit then '1' else '0';
    PSTATE.TCO = if pstate_tco then '1' else '0';
    PSTATE.SM = if pstate_sm then '1' else '0';
    PSTATE.ZA = if pstate_za then '1' else '0';
    A64Oracle_DAIFAccess = pstate_daif_access;
    A64Oracle_ProjectedPSTATEDAIFHigh = pstate_daif[7:4];
    PSTATE.D = pstate_daif[3];
    PSTATE.A = pstate_daif[2];
    PSTATE.I = pstate_daif[1];
    PSTATE.F = pstate_daif[0];
    A64Oracle_MTEEnabled = mte_enabled;
    A64Oracle_MTEExclude = mte_exclude;
    A64Oracle_MTERandomTags = mte_random_tags;
    A64Oracle_MTESeed = mte_seed;
    A64Oracle_MTETag = mte_tag;
    A64Oracle_CPAAddEnabled = cpa_add_enabled;
    A64Oracle_CPAMultiplyEnabled = cpa_multiply_enabled;
    A64Oracle_ACCDATA = accdata;
    A64Oracle_GCSPointer = gcs_pointer;
    A64Oracle_GCSImplemented = gcs_implemented;
    A64Oracle_GCSPushEnabled = gcs_push_enabled;
    A64Oracle_GCSStoreEnabled = gcs_store_enabled;
    A64Oracle_SigningEnabled = signing_enabled;
end;

func A64Oracle_SetProjectedSigningKey(
    index : integer, low : bits(64), high : bits(64))
begin
    assert index >= 0 && index < 5;
    A64Oracle_SigningKeyLow[[index]] = low;
    A64Oracle_SigningKeyHigh[[index]] = high;
end;

readonly func A64Oracle_IsProjectedSystemStateInstalled() => boolean
begin return A64Oracle_ProjectedSystemStateInstalled; end;

readonly func A64Oracle_GetProjectedThreadPointer() => bits(64)
begin return A64Oracle_ThreadPointer; end;

readonly func A64Oracle_GetProjectedPSTATESSBS() => boolean
begin return PSTATE.SSBS == '1'; end;

readonly func A64Oracle_GetProjectedPSTATEDIT() => boolean
begin return PSTATE.DIT == '1'; end;

readonly func A64Oracle_GetProjectedPSTATETCO() => boolean
begin return PSTATE.TCO == '1'; end;

readonly func A64Oracle_GetProjectedPSTATESM() => boolean
begin return PSTATE.SM == '1'; end;

readonly func A64Oracle_GetProjectedPSTATEZA() => boolean
begin return PSTATE.ZA == '1'; end;

readonly func A64Oracle_GetProjectedPSTATEDAIFAccess() => boolean
begin return A64Oracle_DAIFAccess; end;

readonly func A64Oracle_GetProjectedPSTATEDAIF() => bits(8)
begin
    return A64Oracle_ProjectedPSTATEDAIFHigh ::
        PSTATE.D :: PSTATE.A :: PSTATE.I :: PSTATE.F;
end;

readonly func A64Oracle_GetProjectedMTEEnabled() => boolean
begin return A64Oracle_MTEEnabled; end;

readonly func A64Oracle_GetProjectedMTEExclude() => bits(16)
begin return A64Oracle_MTEExclude; end;

readonly func A64Oracle_GetProjectedMTERandomTags() => boolean
begin return A64Oracle_MTERandomTags; end;

readonly func A64Oracle_GetProjectedMTESeed() => bits(16)
begin return A64Oracle_MTESeed; end;

readonly func A64Oracle_GetProjectedMTETag() => bits(8)
begin return A64Oracle_MTETag; end;

readonly func A64Oracle_GetProjectedCPAAddEnabled() => boolean
begin return A64Oracle_CPAAddEnabled; end;

readonly func A64Oracle_GetProjectedCPAMultiplyEnabled() => boolean
begin return A64Oracle_CPAMultiplyEnabled; end;

readonly func A64Oracle_GetProjectedACCDATA() => bits(32)
begin return A64Oracle_ACCDATA; end;

readonly func A64Oracle_GetProjectedGCSPointer() => bits(64)
begin return A64Oracle_GCSPointer; end;

readonly func A64Oracle_GetProjectedGCSImplemented() => boolean
begin return A64Oracle_GCSImplemented; end;

readonly func A64Oracle_GetProjectedGCSPushEnabled() => boolean
begin return A64Oracle_GCSPushEnabled; end;

readonly func A64Oracle_GetProjectedGCSStoreEnabled() => boolean
begin return A64Oracle_GCSStoreEnabled; end;

readonly func A64Oracle_GetProjectedSigningKeyLow(index : integer) => bits(64)
begin
    assert index >= 0 && index < 5;
    return A64Oracle_SigningKeyLow[[index]];
end;

readonly func A64Oracle_GetProjectedSigningKeyHigh(index : integer) => bits(64)
begin
    assert index >= 0 && index < 5;
    return A64Oracle_SigningKeyHigh[[index]];
end;

readonly func A64Oracle_GetProjectedSigningEnabled() => boolean
begin return A64Oracle_SigningEnabled; end;

func A64Oracle_SetCounterPolicy(
    frequency : bits(64), initial : bits(64), step : bits(64))
begin
    A64Oracle_CounterFrequency = frequency;
    A64Oracle_CounterNext = initial;
    A64Oracle_CounterStep = step;
    A64Oracle_CounterReadCount = 0;
end;

func A64Oracle_ReadProjectedVirtualCounter() => bits(64)
begin
    let result : bits(64) = A64Oracle_CounterNext;
    let maximum : bits(64) = Ones{64};
    if !IsZero(A64Oracle_CounterStep) then
        if UInt(A64Oracle_CounterNext) >
           UInt(maximum - A64Oracle_CounterStep) then
            A64Oracle_CounterNext = maximum;
        else
            A64Oracle_CounterNext =
                A64Oracle_CounterNext + A64Oracle_CounterStep;
        end;
    end;
    A64Oracle_CounterReadCount = A64Oracle_CounterReadCount + 1;
    return result;
end;

readonly func A64Oracle_GetCounterFrequency() => bits(64)
begin return A64Oracle_CounterFrequency; end;

readonly func A64Oracle_GetCounterNext() => bits(64)
begin return A64Oracle_CounterNext; end;

readonly func A64Oracle_GetCounterStep() => bits(64)
begin return A64Oracle_CounterStep; end;

readonly func A64Oracle_GetCounterReadCount() => integer
begin return A64Oracle_CounterReadCount; end;

/*
 * These projected bytes cannot be initialized as official architectural
 * state.  The executor must normalize/relate them explicitly rather than
 * silently comparing an invented approximation.
 *   bit 0: derived 256-byte tag-choice table
 *   bit 1: emulator-private random algorithm and 64-bit stream state
 *   bit 2: single CpuState reservation versus architectural monitor state
 *   bit 3: deferred/host execution state and engine scratch
 */
readonly func A64Oracle_GetProjectedNormalizationObligations() => bits(4)
begin return '1111'; end;

/*
 * Abstract register families named by shared pseudocode but omitted from the
 * instruction XML's declaration set.  Each alias uses the concrete 2026-06
 * register layout supplied above; MAIR is consumed only as a 64-bit vector.
 */
type TRBSRType of TRBSR_EL1_Type;
type PMBSRType of PMBSR_EL1_Type;
type S1PIRType of PIR_EL1_Type;
type S2PIRType of S2PIR_EL2_Type;
type MAIRType of bits(64);
type S1PORType of POR_EL1_Type;
type SCTLRType of SCTLR_EL1_Type;
type ESRType of ESR_EL1_Type;

/*
 * Indexed BRB layouts extracted from the pinned 2026-06 System Register XML.
 * ASLRef deliberately omits their variables/accessors, while shared
 * pseudocode still requires the field-bearing types.
 */
type BRBINF_EL1_Type of bits(64) {
    [46] CCU,
    [45:32] CC,
    [13:8] TYPE,
    [7:6] EL,
    [5] MPRED,
    [1:0] VALID
};

type BRBTGT_EL1_Type of bits(64) {
    [63:0] ADDRESS
};

type BRBSRC_EL1_Type of bits(64) {
    [63:0] ADDRESS
};
