/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Deterministic program-counter state for the per-form A64 ASLRef oracle.
 *
 * Arm's instruction pseudocode names _PC directly, so it must remain a real
 * variable.  The base user-state adapter keeps A64Oracle_PC as its portable
 * observation value; the explicit load/store bridge below prevents either
 * value from silently masquerading as the other.
 */

var _PC : bits(64);

func A64Oracle_LoadPCState()
begin
    _PC = A64Oracle_PC;
end;

func A64Oracle_StorePCState()
begin
    A64Oracle_PC = _PC;
end;

func A64Oracle_ResetPCState()
begin
    _PC = Zeros{64};
    A64Oracle_PC = Zeros{64};
end;

func A64Oracle_SetPC(value : bits(64))
begin
    _PC = value;
    A64Oracle_PC = value;
end;

func A64Oracle_GetPC() => bits(64)
begin
    return _PC;
end;
