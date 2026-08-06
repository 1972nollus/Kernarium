/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Minimal deterministic user-mode state adapter for selected Arm-generated
 * A64 ASL instruction procedures.
 *
 * This adapter is deliberately small enough to type-check independently of
 * the full-machine ASL implementation.  It supplies the GPR, SP, PC, NZCV,
 * and raw 128-bit SIMD/FP register state needed by pure register specimens.
 * Raw FPCR/FPSR storage is reserved for future typed register adapters.
 * Memory, exceptions/traps, architectural feature selection, typed FPCR/FPSR
 * accessors, implementation-defined choices, and system-register behavior are
 * absent.  An instruction depending on any absent interface is not executable
 * evidence and cannot be marked behaviorally complete.
 */

var A64Oracle_R : array [[31]] of bits(64);
var A64Oracle_SP : bits(64);
var A64Oracle_PC : bits(64);
var A64Oracle_NZCV : bits(4);
var A64Oracle_V : array [[32]] of bits(128);
var A64Oracle_FPCR : bits(64);
var A64Oracle_FPSR : bits(64);

accessor X{width}(n : integer) <=> value : bits(width)
begin
    setter
        assert n >= 0 && n <= 31;
        assert width IN {32, 64};
        if n != 31 then
            A64Oracle_R[[n]] = ZeroExtend{64}(value);
        end;
    end;

    // Pinned Arm shared_pseudocode.xml accessor_X_1 permits narrow reads so
    // byte/halfword stores can select the low lane of a GPR.  Writes remain
    // architectural W/X writes and are therefore restricted to 32/64 bits.
    getter
        assert n >= 0 && n <= 31;
        let rw : integer{} = width as integer{8, 16, 32, 64};
        if n == 31 then
            return Zeros{rw};
        else
            return A64Oracle_R[[n]][rw-1:0];
        end;
    end;
end;

accessor SP{width}() <=> value : bits(width)
begin
    setter
        assert width IN {32, 64};
        A64Oracle_SP = ZeroExtend{64}(value);
    end;

    getter
        let rw : integer{} = width as integer{32, 64};
        return A64Oracle_SP[rw-1:0];
    end;
end;

accessor V{width}(n : integer) <=> value : bits(width)
begin
    setter
        assert n >= 0 && n <= 31;
        assert width IN {8, 16, 32, 64, 128};
        A64Oracle_V[[n]] = ZeroExtend{128}(value);
    end;

    getter
        assert n >= 0 && n <= 31;
        let rw : integer{} = width as integer{8, 16, 32, 64, 128};
        return A64Oracle_V[[n]][rw-1:0];
    end;
end;

func A64Oracle_ResetUserRegisterState()
begin
    for i = 0 to 30 do
        A64Oracle_R[[i]] = Zeros{64};
    end;
    for i = 0 to 31 do
        A64Oracle_V[[i]] = Zeros{128};
    end;
    A64Oracle_SP = Zeros{64};
    A64Oracle_PC = Zeros{64};
    A64Oracle_NZCV = Zeros{4};
    A64Oracle_FPCR = Zeros{64};
    A64Oracle_FPSR = Zeros{64};
end;

func A64Oracle_SetX(n : integer, value : bits(64))
begin
    assert n >= 0 && n <= 30;
    A64Oracle_R[[n]] = value;
end;

func A64Oracle_GetX(n : integer) => bits(64)
begin
    assert n >= 0 && n <= 30;
    return A64Oracle_R[[n]];
end;

func A64Oracle_SetV128(n : integer, value : bits(128))
begin
    assert n >= 0 && n <= 31;
    A64Oracle_V[[n]] = value;
end;

func A64Oracle_GetV128(n : integer) => bits(128)
begin
    assert n >= 0 && n <= 31;
    return A64Oracle_V[[n]];
end;

func A64Oracle_SetNZCV(value : bits(4))
begin
    A64Oracle_NZCV = value;
end;

func A64Oracle_GetNZCV() => bits(4)
begin
    return A64Oracle_NZCV;
end;
