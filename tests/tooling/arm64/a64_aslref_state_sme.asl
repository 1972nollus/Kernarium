/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Deterministic raw SME state for per-form A64 ASLRef tests.
 *
 * MAX_VL and ZT0_LEN are owned by a64_aslref_state_sve.asl, matching the
 * shared Arm pseudocode on which these storage shapes depend.  The reset
 * helper chooses zero only as repeatable oracle input, not architectural
 * reset state.
 */

var _ZA : array [[256]] of bits(MAX_VL);
var _ZT0 : bits(ZT0_LEN);

func A64Oracle_ResetSMEState()
begin
    for i = 0 to 255 do
        _ZA[[i]] = Zeros{MAX_VL};
    end;
    _ZT0 = Zeros{ZT0_LEN};
end;
