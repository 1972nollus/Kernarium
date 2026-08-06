/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Deterministic floating-point control/status and trap adapter for selected
 * Arm-generated A64 ASL procedures.
 *
 * FPCR, FPSR, and FPMR retain their official 64-bit register shapes.  The
 * first two access the raw storage declared by
 * a64_aslref_user_state_interface.asl; FPMR adds the corresponding raw slot.
 * Accumulated exceptions are therefore architectural state, not host floating
 * point state.  Trap routing and the implementation-defined vector syndrome
 * fields are explicit A64Oracle_* candidate inputs.  A trap is handed to the
 * typed terminal-event adapter and never treated as successful execution.
 */

type FPSR_Type of bits(64) {
    [31] N,
    [30] Z,
    [29] C,
    [28] V,
    [27] QC,
    [7] IDC,
    [4] IXC,
    [3] UFC,
    [2] OFC,
    [1] DZC,
    [0] IOC
};

type FPCR_Type of bits(64) {
    [26] AHP,
    [25] DN,
    [24] FZ,
    [23:22] RMode,
    [21:20] Stride,
    [19] FZ16,
    [18:16] Len,
    [15] IDE,
    [13] EBF,
    [12] IXE,
    [11] UFE,
    [10] OFE,
    [9] DZE,
    [8] IOE,
    [2] NEP,
    [1] AH,
    [0] FIZ
};

type FPMR_Type of bits(64) {
    [37:32] LSCALE2,
    [31:24] NSCALE,
    [22:16] LSCALE,
    [15] OSC,
    [14] OSM,
    [8:6] F8D,
    [5:3] F8S2,
    [2:0] F8S1
};

var A64Oracle_FPMR : bits(64);

accessor FPCR() <=> value : FPCR_Type
begin
    setter
        A64Oracle_FPCR = value;
    end;

    getter
        return A64Oracle_FPCR;
    end;
end;

accessor FPSR() <=> value : FPSR_Type
begin
    setter
        A64Oracle_FPSR = value;
    end;

    getter
        return A64Oracle_FPSR;
    end;
end;

accessor FPMR() <=> value : FPMR_Type
begin
    setter
        A64Oracle_FPMR = value;
    end;

    getter
        return A64Oracle_FPMR;
    end;
end;

/* Explicit trap-syndrome and routing inputs. */
var A64Oracle_FPTrapTargetEL : bits(2);
var A64Oracle_FPPreferredExceptionReturn : bits(64);
var A64Oracle_FPVectorSyndromeValid : boolean;
var A64Oracle_FPVectorIteration : bits(3);
var A64Oracle_AArch32FPEXC : bits(32);

func A64Oracle_ResetFPState()
begin
    A64Oracle_FPCR = Zeros{64};
    A64Oracle_FPSR = Zeros{64};
    A64Oracle_FPMR = Zeros{64};
    A64Oracle_FPTrapTargetEL = '01';
    A64Oracle_FPPreferredExceptionReturn = Zeros{64};
    A64Oracle_FPVectorSyndromeValid = FALSE;
    A64Oracle_FPVectorIteration = Zeros{3};
    A64Oracle_AArch32FPEXC = Zeros{32};
end;

func GetAccumulatedFPExceptions() => bits(8)
begin
    return FPSR().IDC :: '00' :: FPSR().IXC :: FPSR().UFC ::
        FPSR().OFC :: FPSR().DZC :: FPSR().IOC;
end;

func SetAccumulatedFPExceptions(accumulated_exceptions : bits(8))
begin
    FPSR().IDC = accumulated_exceptions[7];
    FPSR().IXC = accumulated_exceptions[4];
    FPSR().UFC = accumulated_exceptions[3];
    FPSR().OFC = accumulated_exceptions[2];
    FPSR().DZC = accumulated_exceptions[1];
    FPSR().IOC = accumulated_exceptions[0];
end;

func AArch64_FPTrappedException(
    is_ase : boolean,
    accumulated_exceptions : bits(8))
begin
    var except : ExceptionRecord =
        ExceptionSyndrome(Exception_FPTrappedException);

    if is_ase then
        except.syndrome.iss[23] =
            if A64Oracle_FPVectorSyndromeValid then '1' else '0';
    else
        except.syndrome.iss[23] = '1';
    end;
    except.syndrome.iss[10:8] = A64Oracle_FPVectorIteration;
    if except.syndrome.iss[23] == '1' then
        except.syndrome.iss[7,4:0] = accumulated_exceptions[7,4:0];
    end;

    AArch64_TakeException(
        A64Oracle_FPTrapTargetEL,
        except,
        A64Oracle_FPPreferredExceptionReturn,
        0
    );
end;

func AArch32_FPTrappedException(accumulated_exceptions : bits(8))
begin
    if AArch32_GeneralExceptionsToAArch64() then
        AArch64_FPTrappedException(FALSE, accumulated_exceptions);
    end;

    A64Oracle_AArch32FPEXC[29] = '1';
    A64Oracle_AArch32FPEXC[26] = '1';
    A64Oracle_AArch32FPEXC[10:8] = '111';
    A64Oracle_AArch32FPEXC[7,4:0] = accumulated_exceptions[7,4:0];

    var except : ExceptionRecord =
        ExceptionSyndrome(Exception_FPTrappedException);
    except.syndrome.iss[23] = '1';
    except.syndrome.iss[10:8] = '111';
    except.syndrome.iss[7,4:0] = accumulated_exceptions[7,4:0];
    AArch32_TakeUndefInstrException(except);
end;
