/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Sparse deterministic A64 system-register dispatch for ASLRef evidence.
 *
 * Only selectors exposed by the emulator's EL0 virtual profile are executable
 * here. Their encodings are fixed by the checked Arm 2026-06 System Register
 * XML. Dynamic counters consume explicit deterministic policy inputs; they
 * never infer values from expected terminal state. All other selectors, and
 * all 128-bit accesses, produce an explicit unsupported outcome without
 * inventing a register value or mutating architectural state.
 */

type A64OracleSysRegOutcome of enumeration {
    A64OracleSysRegOutcome_None,
    A64OracleSysRegOutcome_Read64,
    A64OracleSysRegOutcome_Write64,
    A64OracleSysRegOutcome_UnsupportedRead64,
    A64OracleSysRegOutcome_UnsupportedWrite64,
    A64OracleSysRegOutcome_UnsupportedRead128,
    A64OracleSysRegOutcome_UnsupportedWrite128
};

var A64Oracle_LastSysRegOutcome : A64OracleSysRegOutcome;
var A64Oracle_LastSysRegSelector : bits(16);
var A64Oracle_LastSysRegTarget : integer;
var A64Oracle_LastSysRegTarget2 : integer;

constant A64Oracle_FPCRMask : bits(64) =
    '0000000000000000000000000000000000000111111101111001111100000111';
constant A64Oracle_FPSRMask : bits(64) =
    '0000000000000000000000000000000011111000000000000000000010011111';
constant A64Oracle_FPMRMask : bits(64) =
    '0000000000000000000000000011111111111111011111111100000111111111';

func A64Oracle_RecordSysRegOperation(outcome : A64OracleSysRegOutcome,
                                    op0 : bits(2),
                                    op1 : bits(3),
                                    crn : bits(4),
                                    crm : bits(4),
                                    op2 : bits(3),
                                    t : integer,
                                    t2 : integer)
begin
    A64Oracle_LastSysRegOutcome = outcome;
    A64Oracle_LastSysRegSelector = op0 :: op1 :: crn :: crm :: op2;
    A64Oracle_LastSysRegTarget = t;
    A64Oracle_LastSysRegTarget2 = t2;
end;

func A64Oracle_ResetSysRegState()
begin
    A64Oracle_LastSysRegOutcome = A64OracleSysRegOutcome_None;
    A64Oracle_LastSysRegSelector = Zeros{16};
    A64Oracle_LastSysRegTarget = 31;
    A64Oracle_LastSysRegTarget2 = 31;
end;

func A64Oracle_IsFPCRSelector(op0 : bits(2), op1 : bits(3),
                             crn : bits(4), crm : bits(4),
                             op2 : bits(3)) => boolean
begin
    return op0 == '11' && op1 == '011' && crn == '0100' &&
           crm == '0100' && op2 == '000';
end;

func A64Oracle_IsFPSRSelector(op0 : bits(2), op1 : bits(3),
                             crn : bits(4), crm : bits(4),
                             op2 : bits(3)) => boolean
begin
    return op0 == '11' && op1 == '011' && crn == '0100' &&
           crm == '0100' && op2 == '001';
end;

func A64Oracle_IsFPMRSelector(op0 : bits(2), op1 : bits(3),
                             crn : bits(4), crm : bits(4),
                             op2 : bits(3)) => boolean
begin
    return op0 == '11' && op1 == '011' && crn == '0100' &&
           crm == '0100' && op2 == '010';
end;

func A64Oracle_IsTPIDREL0Selector(op0 : bits(2), op1 : bits(3),
                                  crn : bits(4), crm : bits(4),
                                  op2 : bits(3)) => boolean
begin
    return op0 == '11' && op1 == '011' && crn == '1101' &&
           crm == '0000' && op2 == '010';
end;

func A64Oracle_IsTPIDRROEL0Selector(op0 : bits(2), op1 : bits(3),
                                    crn : bits(4), crm : bits(4),
                                    op2 : bits(3)) => boolean
begin
    return op0 == '11' && op1 == '011' && crn == '1101' &&
           crm == '0000' && op2 == '011';
end;

func A64Oracle_IsDCZIDEL0Selector(op0 : bits(2), op1 : bits(3),
                                  crn : bits(4), crm : bits(4),
                                  op2 : bits(3)) => boolean
begin
    return op0 == '11' && op1 == '011' && crn == '0000' &&
           crm == '0000' && op2 == '111';
end;

func A64Oracle_IsCTREL0Selector(op0 : bits(2), op1 : bits(3),
                                crn : bits(4), crm : bits(4),
                                op2 : bits(3)) => boolean
begin
    return op0 == '11' && op1 == '011' && crn == '0000' &&
           crm == '0000' && op2 == '001';
end;

func A64Oracle_IsNZCVSelector(op0 : bits(2), op1 : bits(3),
                              crn : bits(4), crm : bits(4),
                              op2 : bits(3)) => boolean
begin
    return op0 == '11' && op1 == '011' && crn == '0100' &&
           crm == '0010' && op2 == '000';
end;

func A64Oracle_IsCNTFRQEL0Selector(op0 : bits(2), op1 : bits(3),
                                   crn : bits(4), crm : bits(4),
                                   op2 : bits(3)) => boolean
begin
    return op0 == '11' && op1 == '011' && crn == '1110' &&
           crm == '0000' && op2 == '000';
end;

func A64Oracle_IsCNTVCTEL0Selector(op0 : bits(2), op1 : bits(3),
                                   crn : bits(4), crm : bits(4),
                                   op2 : bits(3)) => boolean
begin
    return op0 == '11' && op1 == '011' && crn == '1110' &&
           crm == '0000' && op2 == '010';
end;

func A64Oracle_InitializeProjectedFPSystemState(
    fpcr : bits(32), fpsr : bits(32), fpmr : bits(64))
begin
    FPCR() = ZeroExtend{64}(fpcr);
    FPSR() = ZeroExtend{64}(fpsr);
    FPMR() = fpmr;
end;

func A64Oracle_GetProjectedFPCR() => bits(32)
begin return FPCR()[31:0]; end;

func A64Oracle_GetProjectedFPSR() => bits(32)
begin return FPSR()[31:0]; end;

func A64Oracle_GetProjectedFPMR() => bits(64)
begin return FPMR(); end;

func AArch64_SysRegRead(op0 : bits(2), op1 : bits(3), crn : bits(4),
                        crm : bits(4), op2 : bits(3), t : integer)
begin
    if A64Oracle_IsTPIDREL0Selector(op0, op1, crn, crm, op2) then
        X{64}(t) = TPIDR_EL0();
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Read64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsTPIDRROEL0Selector(op0, op1, crn, crm, op2) then
        X{64}(t) = TPIDRRO_EL0();
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Read64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsDCZIDEL0Selector(op0, op1, crn, crm, op2) then
        X{64}(t) = A64Oracle_DCZIDEL0Value;
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Read64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsCTREL0Selector(op0, op1, crn, crm, op2) then
        X{64}(t) = A64Oracle_CTREl0Value;
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Read64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsFPCRSelector(op0, op1, crn, crm, op2) then
        X{64}(t) = FPCR() AND A64Oracle_FPCRMask;
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Read64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsFPSRSelector(op0, op1, crn, crm, op2) then
        X{64}(t) = FPSR() AND A64Oracle_FPSRMask;
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Read64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsFPMRSelector(op0, op1, crn, crm, op2) then
        X{64}(t) = FPMR() AND A64Oracle_FPMRMask;
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Read64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsNZCVSelector(op0, op1, crn, crm, op2) then
        X{64}(t) = Zeros{32} :: A64Oracle_GetArchitecturalNZCV() ::
            Zeros{28};
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Read64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsCNTFRQEL0Selector(op0, op1, crn, crm, op2) then
        X{64}(t) = A64Oracle_GetCounterFrequency();
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Read64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsCNTVCTEL0Selector(op0, op1, crn, crm, op2) then
        X{64}(t) = A64Oracle_ReadProjectedVirtualCounter();
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Read64, op0, op1, crn, crm, op2, t, 31
        );
    else
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_UnsupportedRead64,
            op0, op1, crn, crm, op2, t, 31
        );
    end;
end;

func AArch64_SysRegRead128(op0 : bits(2), op1 : bits(3),
                           crn : bits(4), crm : bits(4),
                           op2 : bits(3), t : integer, t2 : integer)
begin
    A64Oracle_RecordSysRegOperation(
        A64OracleSysRegOutcome_UnsupportedRead128,
        op0, op1, crn, crm, op2, t, t2
    );
end;

func AArch64_SysRegWrite(op0 : bits(2), op1 : bits(3), crn : bits(4),
                         crm : bits(4), op2 : bits(3), t : integer)
begin
    if A64Oracle_IsTPIDREL0Selector(op0, op1, crn, crm, op2) then
        TPIDR_EL0() = X{64}(t);
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Write64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsFPCRSelector(op0, op1, crn, crm, op2) then
        FPCR() = X{64}(t) AND A64Oracle_FPCRMask;
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Write64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsFPSRSelector(op0, op1, crn, crm, op2) then
        FPSR() = X{64}(t) AND A64Oracle_FPSRMask;
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Write64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsFPMRSelector(op0, op1, crn, crm, op2) then
        FPMR() = X{64}(t) AND A64Oracle_FPMRMask;
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Write64, op0, op1, crn, crm, op2, t, 31
        );
    elsif A64Oracle_IsNZCVSelector(op0, op1, crn, crm, op2) then
        A64Oracle_SetArchitecturalNZCV(X{64}(t)[31:28]);
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_Write64, op0, op1, crn, crm, op2, t, 31
        );
    else
        A64Oracle_RecordSysRegOperation(
            A64OracleSysRegOutcome_UnsupportedWrite64,
            op0, op1, crn, crm, op2, t, 31
        );
    end;
end;

func AArch64_SysRegWrite128(op0 : bits(2), op1 : bits(3),
                            crn : bits(4), crm : bits(4),
                            op2 : bits(3), t : integer, t2 : integer)
begin
    A64Oracle_RecordSysRegOperation(
        A64OracleSysRegOutcome_UnsupportedWrite128,
        op0, op1, crn, crm, op2, t, t2
    );
end;
