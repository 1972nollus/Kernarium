/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Deterministic raw SVE state for per-form A64 ASLRef tests.
 *
 * The shapes mirror Arm's 2026-06 shared System state.  This adapter makes
 * the otherwise implicit vector and predicate contents explicit and gives a
 * harness one deterministic zero-state initializer.  It does not claim that
 * zero is an architectural reset value.
 */

type VecLen of integer{128, 256, 512, 1024, 2048};
type PredLen of integer{16, 32, 64, 128, 256};

constant MAX_VL : VecLen = 2048;
constant MAX_PL : PredLen = 256;
constant ZT0_LEN : integer{} = 512;

var _Z : array [[32]] of bits(MAX_VL);
var _P : array [[16]] of bits(MAX_PL);
var _FFR : bits(MAX_PL);

func A64Oracle_ResetSVEState()
begin
    for i = 0 to 31 do
        _Z[[i]] = Zeros{MAX_VL};
    end;
    for i = 0 to 15 do
        _P[[i]] = Zeros{MAX_PL};
    end;
    _FFR = Zeros{MAX_PL};
end;
