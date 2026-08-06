/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exact official-semantics specimen corresponding to the existing isolated
 * harness case "ADD immediate width=64 sh=0 imm=5 rn=5 rd=0".
 * Encoding: 0x910014a0 (ADD X0, X5, #5).
 */

func main() => integer
begin
    A64Oracle_ResetUserRegisterState();
    A64Oracle_SetX(
        5,
        '0000000000000000000000000000000000000000000000000000000000100101'
    );
    A64Oracle_SetNZCV('1010');

    instr_add_addsub_imm('10010001000000000001010010100000');

    let result_x0 : bits(64) = A64Oracle_GetX(0);
    let preserved_x5 : bits(64) = A64Oracle_GetX(5);
    let result_nzcv : bits(4) = A64Oracle_GetNZCV();
    assert result_x0 ==
        '0000000000000000000000000000000000000000000000000000000000101010';
    assert preserved_x5 ==
        '0000000000000000000000000000000000000000000000000000000000100101';
    assert result_nzcv == '1010';
    print "A64_ASLREF_ADD_X0=", result_x0;
    print "A64_ASLREF_ADD_X5=", preserved_x5;
    print "A64_ASLREF_ADD_NZCV=", result_nzcv;
    return 0;
end;
