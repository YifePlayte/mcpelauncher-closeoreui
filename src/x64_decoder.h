#pragma once

#include <cstddef>
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
    uint8_t sib = 0;
    uint8_t rex = 0;
    bool rexW = false;
    bool address32 = false;

    // Displacement of any memory operand (including SIB-based stack operands).
    uint8_t dispOffset = 0;
    uint8_t dispSize = 0;

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
        return valid && ripRelative && !address32 && hasModrm && modrmMod == 0 && modrmRm == 5 &&
               opcode2 == 0 && opcode == 0x8D;
    }
};

// Decode one instruction, reading no more than min(available, 15) bytes.
// Truncated and unsupported instructions are invalid. Pointer callers must supply
// the actual remaining range; an instruction-sized array supplies its own bound.
InsnInfo decode(const uint8_t* code, size_t available);

template <size_t N>
InsnInfo decode(const uint8_t (&code)[N]) {
    return decode(code, N);
}

}  // namespace coui
