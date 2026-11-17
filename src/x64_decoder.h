#pragma once

#include <cstdint>

namespace coui {

// Minimal x86-64 instruction decoder: only what the signature scanner and the
// inline hooker need — instruction length, operand shape, and the location of
// relative / RIP-relative operands so a copied prologue can be checked (and,
// when it is safe, relocated).
struct InsnInfo {
    uint8_t len = 0;
    bool valid = false;

    uint8_t opcode = 0;   // primary opcode byte
    uint8_t opcode2 = 0;  // second byte when the primary was 0x0F
    bool hasModrm = false;
    uint8_t modrm = 0;
    uint8_t modrmMod = 0;
    uint8_t modrmReg = 0;  // reg field (or opcode extension)
    uint8_t modrmRm = 0;
    bool hasSib = false;
    bool rexW = false;

    // Instruction has a RIP-relative memory operand (modrm.mod == 0, rm == 5).
    bool ripRelative = false;
    uint8_t ripDispOffset = 0;  // offset of the disp32 inside the instruction

    // Relative branch / call with a displacement relative to the next insn.
    uint8_t relSize = 0;  // 0 = none, 1 = rel8, 4 = rel32
    uint8_t relOffset = 0;

    bool isReturn = false;
    bool isBranch = false;  // any jmp/jcc/call (direct or indirect)
    bool isCall = false;
    bool isUnconditionalJump = false;
    bool isConditionalJump = false;

    // Position independent and safe to memcpy into a trampoline unchanged.
    bool relocatable() const { return valid && !ripRelative && relSize == 0; }

    // RIP-relative LEA: the canonical "address of a string constant" idiom.
    bool isRipLea() const {
        return valid && ripRelative && hasModrm && modrmMod == 0 && modrmRm == 5 &&
               opcode2 == 0 && opcode == 0x8D;
    }
};

// Decode one instruction at `code`. Reads at most 15 bytes.
InsnInfo decode(const uint8_t* code);

}  // namespace coui
