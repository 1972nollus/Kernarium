/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Deterministic initialization and lossless observation helpers for the
 * official-ASL execution oracle.  These routines contain no instruction
 * semantics.  They only compose the checked state adapters and expose fixed
 * chunks that the Python runner independently serializes and digests.
 */

constant A64Execution_GuestPC : bits(64) =
    '0000000000000000000000000000000001001111000000000000000000000000';
constant A64Execution_PatternMemoryBase : bits(64) =
    '0000000000000000000000000000000000000000000100000000000000000000';

func A64Execution_Reset(instruction : bits(32))
begin
    A64Oracle_ResetUserRegisterState();
    A64Oracle_ResetPCState();
    A64Oracle_ResetStateEnvironment();
    A64Oracle_ResetEventState();
    A64Oracle_ResetFPState();
    A64Oracle_ResetMemoryState();
    A64Oracle_ResetSVEState();
    A64Oracle_ResetSMEState();
    A64Oracle_ResetSysRegState();
    A64Oracle_SetCurrentInstruction(instruction);
    A64Oracle_SetPC(A64Execution_GuestPC);
end;

func A64Execution_InitializeZero(instruction : bits(32))
begin
    A64Execution_Reset(instruction);
    A64Oracle_SetArchitecturalNZCV('0000');
    A64Oracle_SetMemoryBase(Zeros{64});
end;

func A64Execution_InitializePattern(instruction : bits(32))
begin
    A64Execution_Reset(instruction);
    A64Oracle_SetArchitecturalNZCV('1010');
    A64Oracle_SetMemoryBase(A64Execution_PatternMemoryBase);
    SP{64}() = A64Execution_PatternMemoryBase + 2048;
    for index = 0 to 30 do
        A64Oracle_SetX(
            index,
            A64Execution_PatternMemoryBase + 256 + index * 16
        );
    end;
    var vector_value : bits(128) =
        '0000000100000001000000010000000100000001000000010000000100000001'
        ::
        '0000000100000001000000010000000100000001000000010000000100000001';
    let vector_step : bits(128) = vector_value;
    for index = 0 to 31 do
        A64Oracle_SetV128(index, vector_value);
        vector_value = vector_value + vector_step;
    end;
    var memory_byte : bits(8) = '00000011';
    for index = 0 to A64Oracle_MemoryByteCount - 1 do
        A64Oracle_SetMemoryByte(index, memory_byte);
        memory_byte = memory_byte + '00010001';
    end;
    var memory_tag : bits(4) = '0000';
    for index = 0 to A64Oracle_MemoryTagCount - 1 do
        A64Oracle_SetMemoryTag(index, memory_tag);
        memory_tag = memory_tag + '0001';
    end;
end;

func A64Execution_MemoryChunk(index : integer) => bits(128)
begin
    assert index >= 0 && index < 256;
    var result : bits(128) = Zeros{128};
    for byte = 0 to 15 do
        result[byte * 8 +: 8] =
            A64Oracle_GetMemoryByte(index * 16 + byte);
    end;
    return result;
end;

func A64Execution_TagChunk(index : integer) => bits(64)
begin
    assert index >= 0 && index < 16;
    var result : bits(64) = Zeros{64};
    for tag = 0 to 15 do
        result[tag * 4 +: 4] =
            A64Oracle_GetMemoryTag(index * 16 + tag);
    end;
    return result;
end;
