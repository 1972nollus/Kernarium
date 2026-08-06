/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Deterministic user-state event, exception, trap, hint, and barrier adapter
 * for selected Arm-generated A64 ASL procedures.
 *
 * This file does not claim that its reset choices are architectural reset
 * values.  Every implementation-defined decision is stored in an
 * A64Oracle_* input variable.  Terminal architectural outcomes are recorded
 * in typed state and then raised as A64Oracle_TerminalOutcome, so an exception
 * or trap can never silently become successful instruction execution.
 *
 * The official shared definitions of the 53 queue symbols implemented here
 * must be omitted when this adapter is composed.  Referenced architectural
 * data types such as FaultRecord, IssType, FullAddress, PASpace, BranchType,
 * CrossTriggerIn, PhysMemRetStatus, AddressDescriptor, and AccessDescriptor
 * remain official dependency inputs and are not redefined here.
 */

type Exception of enumeration {
    Exception_Uncategorized,
    Exception_WFxTrap,
    Exception_CP15RTTrap,
    Exception_CP15RRTTrap,
    Exception_CP14RTTrap,
    Exception_CP14DTTrap,
    Exception_CP14RRTTrap,
    Exception_AdvSIMDFPAccessTrap,
    Exception_FPIDTrap,
    Exception_OtherTrap,
    Exception_PACTrap,
    Exception_IllegalState,
    Exception_SupervisorCall,
    Exception_HypervisorCall,
    Exception_MonitorCall,
    Exception_SystemRegisterTrap,
    Exception_ERetTrap,
    Exception_InstructionAbort,
    Exception_PCAlignment,
    Exception_DataAbort,
    Exception_NV2DataAbort,
    Exception_PACFail,
    Exception_SPAlignment,
    Exception_FPTrappedException,
    Exception_SError,
    Exception_Breakpoint,
    Exception_SoftwareStep,
    Exception_Watchpoint,
    Exception_NV2Watchpoint,
    Exception_SoftwareBreakpoint,
    Exception_VectorCatch,
    Exception_IRQ,
    Exception_SVEAccessTrap,
    Exception_SMEAccessTrap,
    Exception_GPC,
    Exception_BranchTarget,
    Exception_MemCpyMemSet,
    Exception_GCSFail,
    Exception_Profiling,
    Exception_SystemRegister128Trap,
    Exception_FIQ
};

type ExceptionRecord of record {
    exceptype : Exception,
    syndrome : IssType,
    paddress : FullAddress,
    vaddress : bits(64),
    ipavalid : boolean,
    pavalid : boolean,
    NS : bit,
    ipaddress : bits(NUM_PABITS),
    trappedsyscallinst : boolean
};

type ExceptionalOccurrenceTargetState of enumeration {
    AArch32_NonDebugState,
    AArch64_NonDebugState,
    DebugState
};

type A64Oracle_EventKind of enumeration {
    A64Oracle_Event_None,
    A64Oracle_Event_ExceptionTaken,
    A64Oracle_Event_AArch32Undefined,
    A64Oracle_Event_AArch32Abort,
    A64Oracle_Event_AArch64Abort,
    A64Oracle_Event_BranchTarget,
    A64Oracle_Event_SystemAccessTrap,
    A64Oracle_Event_Undefined,
    A64Oracle_Event_WaitHint
};

type A64Oracle_HintKind of enumeration {
    A64Oracle_Hint_None,
    A64Oracle_Hint_Branch,
    A64Oracle_Hint_CLRBHB,
    A64Oracle_Hint_DGH,
    A64Oracle_Hint_SharedUpdate,
    A64Oracle_Hint_StoreConcurrentPriority,
    A64Oracle_Hint_StoreShared,
    A64Oracle_Hint_WFE,
    A64Oracle_Hint_WFET,
    A64Oracle_Hint_WFI,
    A64Oracle_Hint_WFIT,
    A64Oracle_Hint_Yield
};

type A64Oracle_EffectKind of enumeration {
    A64Oracle_Effect_None,
    A64Oracle_Effect_ConsumptionSpeculationBarrier,
    A64Oracle_Effect_GCSSynchronizationBarrier,
    A64Oracle_Effect_InstructionSynchronizationBarrier,
    A64Oracle_Effect_ProfilingSynchronizationBarrier,
    A64Oracle_Effect_SpeculationBarrier,
    A64Oracle_Effect_SendEvent,
    A64Oracle_Effect_CTISignal
};

type A64Oracle_CheckMode of enumeration {
    A64Oracle_Check_Continue,
    A64Oracle_Check_Trap,
    A64Oracle_Check_Undefined
};

type A64Oracle_TerminalOutcome of exception {
    kind : A64Oracle_EventKind
};

/* Observable event state. */
var A64Oracle_LastEvent : A64Oracle_EventKind;
var A64Oracle_Terminal : boolean;
var A64Oracle_HasException : boolean;
var A64Oracle_LastException : ExceptionRecord;
var A64Oracle_HasFault : boolean;
var A64Oracle_LastFault : FaultRecord;
var A64Oracle_LastTargetEL : bits(2);
var A64Oracle_PreferredExceptionReturn : bits(64);
var A64Oracle_VectorOffset : integer;
var A64Oracle_LastAddress : bits(64);
var A64Oracle_LastImmediate : bits(16);
var A64Oracle_LastIntegerPayload : integer;
var A64Oracle_LastBoolean0 : boolean;
var A64Oracle_LastBoolean1 : boolean;

/* Observable nonterminal effects. */
var A64Oracle_LastHint : A64Oracle_HintKind;
var A64Oracle_HintCount : integer;
var A64Oracle_LastHintTimeout : integer;
var A64Oracle_LastHintFlag : boolean;
var A64Oracle_HasBranchHint : boolean;
var A64Oracle_LastBranchHint : BranchType;
var A64Oracle_HasBranchOutcome : boolean;
var A64Oracle_BranchTaken : boolean;
var A64Oracle_BranchTarget : bits(64);
var A64Oracle_BranchType : BranchType;
var A64Oracle_BranchConditional : boolean;
var A64Oracle_LastEffect : A64Oracle_EffectKind;
var A64Oracle_EffectCount : integer;
var A64Oracle_HasCTIInput : boolean;
var A64Oracle_LastCTIInput : CrossTriggerIn;

/* Explicit candidate-input choices for implementation-defined behavior. */
var A64Oracle_DefaultPASpace : PASpace;
var A64Oracle_ExceptionTargetEL : bits(2);
var A64Oracle_AArch32ExceptionsUseAArch64 : boolean;
var A64Oracle_FPAccessTrap : boolean;
var A64Oracle_ERetTrap : boolean;
var A64Oracle_SMCCheck : A64Oracle_CheckMode;
var A64Oracle_SVCTrap : boolean;
var A64Oracle_StopOnWaitHint : boolean;
var A64Oracle_IllegalReturn : boolean;
var A64Oracle_ActionRequiredChoice : boolean;
var A64Oracle_ErrorContainedChoice : boolean;
var A64Oracle_ErrorSynchronizedChoice : boolean;
var A64Oracle_ErrorIMPDEFChoice : boolean;
var A64Oracle_ErrorUCChoice : boolean;
var A64Oracle_ErrorUERChoice : boolean;
var A64Oracle_ErrorUEUChoice : boolean;
var A64Oracle_ErrorUncategorizedChoice : boolean;
var A64Oracle_StateRecoverableChoice : boolean;
var A64Oracle_ExternalAbortSynchronousChoice : boolean;
var A64Oracle_SynchronizableSErrorChoice : boolean;
var A64Oracle_WatchpointFARNotPreciseChoice : boolean;
var A64Oracle_SyncExternalAbortTargetEL : bits(2);

/* Pending SError and external-abort observation state. */
var A64Oracle_PhysicalSErrorPending : boolean;
var A64Oracle_PendingPhysicalSError : FaultRecord;
var A64Oracle_HasExternalAbortQuery : boolean;
var A64Oracle_LastPhysMemStatus : PhysMemRetStatus;
var A64Oracle_LastExternalAbortWrite : boolean;
var A64Oracle_LastAddressDescriptor : AddressDescriptor;
var A64Oracle_LastAccessSize : integer;
var A64Oracle_LastAccessDescriptor : AccessDescriptor;
var A64Oracle_HasIllegalReturnSPSR : boolean;
var A64Oracle_LastIllegalReturnSPSR : bits(64);

func A64Oracle_ResetEventState()
begin
    A64Oracle_LastEvent = A64Oracle_Event_None;
    A64Oracle_Terminal = FALSE;
    A64Oracle_HasException = FALSE;
    A64Oracle_HasFault = FALSE;
    A64Oracle_LastTargetEL = '00';
    A64Oracle_PreferredExceptionReturn = Zeros{64};
    A64Oracle_VectorOffset = 0;
    A64Oracle_LastAddress = Zeros{64};
    A64Oracle_LastImmediate = Zeros{16};
    A64Oracle_LastIntegerPayload = 0;
    A64Oracle_LastBoolean0 = FALSE;
    A64Oracle_LastBoolean1 = FALSE;

    A64Oracle_LastHint = A64Oracle_Hint_None;
    A64Oracle_HintCount = 0;
    A64Oracle_LastHintTimeout = 0;
    A64Oracle_LastHintFlag = FALSE;
    A64Oracle_HasBranchHint = FALSE;
    A64Oracle_HasBranchOutcome = FALSE;
    A64Oracle_BranchTaken = FALSE;
    A64Oracle_BranchTarget = Zeros{64};
    A64Oracle_BranchConditional = FALSE;
    A64Oracle_LastEffect = A64Oracle_Effect_None;
    A64Oracle_EffectCount = 0;
    A64Oracle_HasCTIInput = FALSE;

    A64Oracle_DefaultPASpace = PAS_NonSecure;
    A64Oracle_ExceptionTargetEL = '01';
    A64Oracle_AArch32ExceptionsUseAArch64 = FALSE;
    A64Oracle_FPAccessTrap = FALSE;
    A64Oracle_ERetTrap = FALSE;
    A64Oracle_SMCCheck = A64Oracle_Check_Continue;
    A64Oracle_SVCTrap = FALSE;
    A64Oracle_StopOnWaitHint = FALSE;
    A64Oracle_IllegalReturn = FALSE;
    A64Oracle_ActionRequiredChoice = FALSE;
    A64Oracle_ErrorContainedChoice = FALSE;
    A64Oracle_ErrorSynchronizedChoice = FALSE;
    A64Oracle_ErrorIMPDEFChoice = FALSE;
    A64Oracle_ErrorUCChoice = FALSE;
    A64Oracle_ErrorUERChoice = FALSE;
    A64Oracle_ErrorUEUChoice = FALSE;
    A64Oracle_ErrorUncategorizedChoice = FALSE;
    A64Oracle_StateRecoverableChoice = FALSE;
    A64Oracle_ExternalAbortSynchronousChoice = FALSE;
    A64Oracle_SynchronizableSErrorChoice = FALSE;
    A64Oracle_WatchpointFARNotPreciseChoice = FALSE;
    A64Oracle_SyncExternalAbortTargetEL = '01';

    A64Oracle_PhysicalSErrorPending = FALSE;
    A64Oracle_HasExternalAbortQuery = FALSE;
    A64Oracle_HasIllegalReturnSPSR = FALSE;
end;

noreturn func A64Oracle_Stop(kind : A64Oracle_EventKind)
begin
    A64Oracle_LastEvent = kind;
    A64Oracle_Terminal = TRUE;
    throw A64Oracle_TerminalOutcome { kind = kind };
end;

noreturn func A64Oracle_RecordException(
    kind : A64Oracle_EventKind,
    target_el : bits(2),
    except : ExceptionRecord,
    preferred_exception_return : bits(64),
    vect_offset : integer)
begin
    A64Oracle_HasException = TRUE;
    A64Oracle_LastException = except;
    A64Oracle_LastTargetEL = target_el;
    A64Oracle_PreferredExceptionReturn = preferred_exception_return;
    A64Oracle_VectorOffset = vect_offset;
    A64Oracle_Stop(kind);
end;

noreturn func A64Oracle_RecordFault(
    kind : A64Oracle_EventKind,
    fault : FaultRecord)
begin
    A64Oracle_HasFault = TRUE;
    A64Oracle_LastFault = fault;
    A64Oracle_Stop(kind);
end;

func ExceptionSyndrome(exceptype : Exception) => ExceptionRecord
begin
    var result : ExceptionRecord;
    result.exceptype = exceptype;
    result.syndrome.iss = Zeros{25};
    result.syndrome.iss2 = Zeros{24};
    result.paddress.paspace = A64Oracle_DefaultPASpace;
    result.paddress.address = Zeros{NUM_PABITS};
    result.vaddress = Zeros{64};
    result.ipavalid = FALSE;
    result.pavalid = FALSE;
    result.NS = '0';
    result.ipaddress = Zeros{NUM_PABITS};
    result.trappedsyscallinst = FALSE;
    return result;
end;

noreturn func AArch64_TakeException(
    target_el : bits(2),
    exception_in : ExceptionRecord,
    preferred_exception_return : bits(64),
    vect_offset_in : integer)
begin
    A64Oracle_RecordException(
        A64Oracle_Event_ExceptionTaken,
        target_el,
        exception_in,
        preferred_exception_return,
        vect_offset_in
    );
end;

noreturn func AArch32_TakeUndefInstrException(except : ExceptionRecord)
begin
    A64Oracle_RecordException(
        A64Oracle_Event_AArch32Undefined,
        A64Oracle_ExceptionTargetEL,
        except,
        A64Oracle_PreferredExceptionReturn,
        A64Oracle_VectorOffset
    );
end;

noreturn func AArch32_TakeUndefInstrException()
begin
    let except : ExceptionRecord =
        ExceptionSyndrome(Exception_Uncategorized);
    AArch32_TakeUndefInstrException(except);
end;

func AArch32_Abort(fault : FaultRecord)
begin
    A64Oracle_RecordFault(A64Oracle_Event_AArch32Abort, fault);
end;

func AArch64_Abort(fault : FaultRecord)
begin
    A64Oracle_RecordFault(A64Oracle_Event_AArch64Abort, fault);
end;

func AArch64_AdvSIMDFPAccessTrap(target_el : bits(2))
begin
    var except : ExceptionRecord =
        ExceptionSyndrome(Exception_AdvSIMDFPAccessTrap);
    AArch64_TakeException(
        target_el,
        except,
        A64Oracle_PreferredExceptionReturn,
        A64Oracle_VectorOffset
    );
end;

func AArch64_BranchTargetException(vaddress : bits(52))
begin
    var except : ExceptionRecord = ExceptionSyndrome(Exception_BranchTarget);
    except.vaddress = ZeroExtend{64}(vaddress);
    A64Oracle_LastAddress = except.vaddress;
    A64Oracle_RecordException(
        A64Oracle_Event_BranchTarget,
        A64Oracle_ExceptionTargetEL,
        except,
        A64Oracle_PreferredExceptionReturn,
        A64Oracle_VectorOffset
    );
end;

func AArch64_OtherInstrTrap(target_el : bits(2), iss : bits(25))
begin
    var except : ExceptionRecord = ExceptionSyndrome(Exception_OtherTrap);
    except.syndrome.iss = iss;
    AArch64_TakeException(
        target_el,
        except,
        A64Oracle_PreferredExceptionReturn,
        A64Oracle_VectorOffset
    );
end;

func AArch64_SystemAccessTrap(target_el : bits(2), ec : integer)
begin
    A64Oracle_LastTargetEL = target_el;
    A64Oracle_LastIntegerPayload = ec;
    A64Oracle_Stop(A64Oracle_Event_SystemAccessTrap);
end;

func AArch64_CheckFPAdvSIMDTrap()
begin
    if A64Oracle_FPAccessTrap then
        AArch64_AdvSIMDFPAccessTrap(A64Oracle_ExceptionTargetEL);
    end;
end;

func AArch64_CheckForERetTrap(
    eret_with_pac : boolean,
    pac_uses_key_a : boolean)
begin
    A64Oracle_LastBoolean0 = eret_with_pac;
    A64Oracle_LastBoolean1 = pac_uses_key_a;
    if A64Oracle_ERetTrap then
        var except : ExceptionRecord = ExceptionSyndrome(Exception_ERetTrap);
        AArch64_TakeException(
            A64Oracle_ExceptionTargetEL,
            except,
            A64Oracle_PreferredExceptionReturn,
            A64Oracle_VectorOffset
        );
    end;
end;

func AArch64_CheckForSMCUndefOrTrap(imm : bits(16))
begin
    A64Oracle_LastImmediate = imm;
    if A64Oracle_SMCCheck == A64Oracle_Check_Undefined then
        A64Oracle_Stop(A64Oracle_Event_Undefined);
    elsif A64Oracle_SMCCheck == A64Oracle_Check_Trap then
        var except : ExceptionRecord = ExceptionSyndrome(Exception_MonitorCall);
        except.syndrome.iss[15:0] = imm;
        except.trappedsyscallinst = TRUE;
        AArch64_TakeException(
            A64Oracle_ExceptionTargetEL,
            except,
            A64Oracle_PreferredExceptionReturn,
            A64Oracle_VectorOffset
        );
    end;
end;

func AArch64_CheckForSVCTrap(immediate : bits(16))
begin
    A64Oracle_LastImmediate = immediate;
    if A64Oracle_SVCTrap then
        var except : ExceptionRecord =
            ExceptionSyndrome(Exception_SupervisorCall);
        except.syndrome.iss[15:0] = immediate;
        except.trappedsyscallinst = TRUE;
        AArch64_TakeException(
            A64Oracle_ExceptionTargetEL,
            except,
            A64Oracle_PreferredExceptionReturn,
            A64Oracle_VectorOffset
        );
    end;
end;

func AArch32_GeneralExceptionsToAArch64() => boolean
begin
    return A64Oracle_AArch32ExceptionsUseAArch64;
end;

func IllegalExceptionReturn{N}(spsr : bits(N)) => boolean
begin
    A64Oracle_HasIllegalReturnSPSR = TRUE;
    A64Oracle_LastIllegalReturnSPSR = ZeroExtend{64}(spsr);
    return A64Oracle_IllegalReturn;
end;

func A64Oracle_RecordHint(kind : A64Oracle_HintKind, timeout : integer)
begin
    A64Oracle_LastHint = kind;
    A64Oracle_HintCount = A64Oracle_HintCount + 1;
    A64Oracle_LastHintTimeout = timeout;
end;

func Hint_Branch(hint : BranchType)
begin
    A64Oracle_RecordHint(A64Oracle_Hint_Branch, 0);
    A64Oracle_HasBranchHint = TRUE;
    A64Oracle_LastBranchHint = hint;
end;

/*
 * Instrumented official branch boundary.  The architectural state updates
 * and optional BRBE/SPE calls are the released BranchTo behavior; the five
 * A64Oracle_* assignments are lossless observations only.
 */
func BranchTo{N}(target : bits(N), branch_type : BranchType,
                 branch_conditional : boolean)
begin
    Hint_Branch(branch_type);
    A64Oracle_HasBranchOutcome = TRUE;
    A64Oracle_BranchTaken = TRUE;
    A64Oracle_BranchType = branch_type;
    A64Oracle_BranchConditional = branch_conditional;
    if N == 32 then
        assert UsingAArch32();
        A64Oracle_BranchTarget = ZeroExtend{64}(target);
        _PC = A64Oracle_BranchTarget;
    else
        assert N == 64 && !UsingAArch32();
        let target_vaddress : bits(64) =
            AArch64_BranchAddr(target[63:0], PSTATE.EL);
        if IsFeatureImplemented(FEAT_BRBE) &&
           branch_type IN {BranchType_DIR, BranchType_INDIR,
                           BranchType_DIRCALL, BranchType_INDCALL,
                           BranchType_RET} then
            BRBEBranch(branch_type, branch_conditional, target_vaddress);
        end;
        let branch_taken : boolean = TRUE;
        if IsFeatureImplemented(FEAT_SPE) then
            SPEBranch{N}(
                target, branch_type, branch_conditional, branch_taken
            );
        end;
        A64Oracle_BranchTarget = target_vaddress;
        _PC = target_vaddress;
    end;
end;

func BranchNotTaken(branch_type : BranchType, branch_conditional : boolean)
begin
    A64Oracle_HasBranchOutcome = TRUE;
    A64Oracle_BranchTaken = FALSE;
    A64Oracle_BranchTarget = _PC;
    A64Oracle_BranchType = branch_type;
    A64Oracle_BranchConditional = branch_conditional;
    let branch_taken : boolean = FALSE;
    if IsFeatureImplemented(FEAT_SPE) then
        SPEBranch{64}(
            ARBITRARY : bits(64), branch_type, branch_conditional, branch_taken
        );
    end;
end;

func Hint_CLRBHB()
begin
    A64Oracle_RecordHint(A64Oracle_Hint_CLRBHB, 0);
end;

func Hint_DGH()
begin
    A64Oracle_RecordHint(A64Oracle_Hint_DGH, 0);
end;

func Hint_SharedUpdate(priority : boolean)
begin
    A64Oracle_RecordHint(A64Oracle_Hint_SharedUpdate, 0);
    A64Oracle_LastHintFlag = priority;
end;

func Hint_StoreConcurrentPriority()
begin
    A64Oracle_RecordHint(A64Oracle_Hint_StoreConcurrentPriority, 0);
end;

func Hint_StoreShared(stream : boolean)
begin
    A64Oracle_RecordHint(A64Oracle_Hint_StoreShared, 0);
    A64Oracle_LastHintFlag = stream;
end;

func Hint_WFE()
begin
    A64Oracle_RecordHint(A64Oracle_Hint_WFE, 0);
    if A64Oracle_StopOnWaitHint then
        A64Oracle_Stop(A64Oracle_Event_WaitHint);
    end;
end;

func Hint_WFET(localtimeout : integer)
begin
    A64Oracle_RecordHint(A64Oracle_Hint_WFET, localtimeout);
    if A64Oracle_StopOnWaitHint then
        A64Oracle_Stop(A64Oracle_Event_WaitHint);
    end;
end;

func Hint_WFI()
begin
    A64Oracle_RecordHint(A64Oracle_Hint_WFI, 0);
    if A64Oracle_StopOnWaitHint then
        A64Oracle_Stop(A64Oracle_Event_WaitHint);
    end;
end;

func Hint_WFIT(localtimeout : integer)
begin
    A64Oracle_RecordHint(A64Oracle_Hint_WFIT, localtimeout);
    if A64Oracle_StopOnWaitHint then
        A64Oracle_Stop(A64Oracle_Event_WaitHint);
    end;
end;

func Hint_Yield()
begin
    A64Oracle_RecordHint(A64Oracle_Hint_Yield, 0);
end;

func A64Oracle_RecordEffect(kind : A64Oracle_EffectKind)
begin
    A64Oracle_LastEffect = kind;
    A64Oracle_EffectCount = A64Oracle_EffectCount + 1;
end;

func ConsumptionOfSpeculativeDataBarrier()
begin
    A64Oracle_RecordEffect(A64Oracle_Effect_ConsumptionSpeculationBarrier);
end;

func GCSSynchronizationBarrier()
begin
    A64Oracle_RecordEffect(A64Oracle_Effect_GCSSynchronizationBarrier);
end;

func InstructionSynchronizationBarrier()
begin
    A64Oracle_RecordEffect(A64Oracle_Effect_InstructionSynchronizationBarrier);
end;

func ProfilingSynchronizationBarrier()
begin
    A64Oracle_RecordEffect(A64Oracle_Effect_ProfilingSynchronizationBarrier);
end;

func SpeculationBarrier()
begin
    A64Oracle_RecordEffect(A64Oracle_Effect_SpeculationBarrier);
end;

func SendEvent()
begin
    A64Oracle_RecordEffect(A64Oracle_Effect_SendEvent);
end;

func CTI_SignalEvent(id : CrossTriggerIn)
begin
    A64Oracle_RecordEffect(A64Oracle_Effect_CTISignal);
    A64Oracle_HasCTIInput = TRUE;
    A64Oracle_LastCTIInput = id;
end;

func ClearPendingPhysicalSError()
begin
    A64Oracle_PhysicalSErrorPending = FALSE;
end;

func GetPendingPhysicalSError() => FaultRecord
begin
    assert A64Oracle_PhysicalSErrorPending;
    return A64Oracle_PendingPhysicalSError;
end;

func IsPhysicalSErrorPending() => boolean
begin
    return A64Oracle_PhysicalSErrorPending;
end;

func IsSynchronizablePhysicalSErrorPending() => boolean
begin
    return A64Oracle_PhysicalSErrorPending &&
        A64Oracle_SynchronizableSErrorChoice;
end;

func PendSErrorInterrupt(fault : FaultRecord)
begin
    A64Oracle_PendingPhysicalSError = fault;
    A64Oracle_PhysicalSErrorPending = TRUE;
end;

func IsExternalAbortTakenSynchronously(
    memstatus : PhysMemRetStatus,
    iswrite : boolean,
    desc : AddressDescriptor,
    size : integer,
    accdesc : AccessDescriptor) => boolean
begin
    A64Oracle_HasExternalAbortQuery = TRUE;
    A64Oracle_LastPhysMemStatus = memstatus;
    A64Oracle_LastExternalAbortWrite = iswrite;
    A64Oracle_LastAddressDescriptor = desc;
    A64Oracle_LastAccessSize = size;
    A64Oracle_LastAccessDescriptor = accdesc;
    return A64Oracle_ExternalAbortSynchronousChoice;
end;

func SyncExternalAbortTarget(fault : FaultRecord) => bits(2)
begin
    A64Oracle_HasFault = TRUE;
    A64Oracle_LastFault = fault;
    return A64Oracle_SyncExternalAbortTargetEL;
end;

func WatchpointFARNotPrecise(fault : FaultRecord) => boolean
begin
    A64Oracle_HasFault = TRUE;
    A64Oracle_LastFault = fault;
    return A64Oracle_WatchpointFARNotPreciseChoice;
end;

func ActionRequired() => boolean
begin
    return A64Oracle_ActionRequiredChoice;
end;

func ErrorIsContained() => boolean
begin
    return A64Oracle_ErrorContainedChoice;
end;

func ErrorIsSynchronized() => boolean
begin
    return A64Oracle_ErrorSynchronizedChoice;
end;

func ReportErrorAsIMPDEF() => boolean
begin
    return A64Oracle_ErrorIMPDEFChoice;
end;

func ReportErrorAsUC() => boolean
begin
    return A64Oracle_ErrorUCChoice;
end;

func ReportErrorAsUER() => boolean
begin
    return A64Oracle_ErrorUERChoice;
end;

func ReportErrorAsUEU() => boolean
begin
    return A64Oracle_ErrorUEUChoice;
end;

func ReportErrorAsUncategorized() => boolean
begin
    return A64Oracle_ErrorUncategorizedChoice;
end;

func StateIsRecoverable() => boolean
begin
    return A64Oracle_StateRecoverableChoice;
end;
