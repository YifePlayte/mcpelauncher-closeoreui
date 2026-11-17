#include "x64_decoder.h"

#include <cstddef>
#include <cstdint>

namespace coui {
namespace {

inline bool isLegacyPrefix(uint8_t b) {
    switch (b) {
        case 0xF0:  // lock
        case 0xF2:  // repne
        case 0xF3:  // rep / repne
        case 0x2E:  // cs
        case 0x36:  // ss
        case 0x3E:  // ds
        case 0x26:  // es
        case 0x64:  // fs
        case 0x65:  // gs
        case 0x66:  // opsize
        case 0x67:  // addrsize
            return true;
        default:
            return false;
    }
}

struct Cursor {
    const uint8_t* base;
    size_t pos = 0;

    bool need(size_t n) const { return n <= 15; }
    uint8_t byte() { return base[pos++]; }
    uint8_t peek() const { return base[pos]; }
};

// Immediate size in bytes for a one-byte opcode, after modrm.
int immediateSize1Byte(uint8_t op, bool rexW, bool opsize16, bool hasModrm) {
    if (op >= 0x70 && op <= 0x7F) return 1;         // jcc rel8
    if (op >= 0xB0 && op <= 0xB7) return 1;         // mov r8, imm8
    if (op >= 0xB8 && op <= 0xBF) return rexW ? 8 : (opsize16 ? 2 : 4);
    if (op == 0xE8 || op == 0xE9) return 4;         // call/jmp rel32
    if (op == 0xEB) return 1;                       // jmp rel8
    if (op == 0xA8) return 1;                       // test al, imm8
    if (op == 0xA9) return opsize16 ? 2 : 4;        // test eax, imm
    if (op == 0x68) return opsize16 ? 2 : 4;        // push imm
    if (op == 0x6A) return 1;                       // push imm8
    if (op == 0xC2 || op == 0xCA) return 2;         // ret imm16
    if (op == 0xCD) return 1;                       // int imm8
    if (op == 0xC8) return 3;                       // enter
    if (op == 0xA0 || op == 0xA1 || op == 0xA2 || op == 0xA3) {
        // moffs — addrsize dependent
        return 8;
    }

    // AL/eAX, imm form: low 3 bits 4 (imm8) or 5 (imm32)
    if (op < 0x40) {
        switch (op & 7) {
            case 4: return 1;
            case 5: return opsize16 ? 2 : 4;
            default: break;
        }
    }

    if (!hasModrm) {
        return 0;
    }

    switch (op) {
        case 0x80: case 0x82: case 0x83:
            return 1;
        case 0x81:
            return opsize16 ? 2 : 4;
        case 0xC0: case 0xC1:
            return 1;
        case 0xC6:
            return 1;
        case 0xC7:
            return opsize16 ? 2 : 4;
        case 0xF6:
        case 0xF7:
            // Group 3: only /0 (test) and /1 (test) carry an immediate.
            return 0;  // caller refines using modrm.reg
        default:
            return 0;
    }
}

// Whether `op` is an opcode this decoder knows how to measure. The set is
// deliberately narrow: it covers what appears in compiler-generated code, and
// anything outside it is rejected rather than guessed. VEX/EVEX (0xC4/0xC5/0x62
// with the right prefix) and other rarely-seen encodings are absent on purpose —
// they do not occur in the target prologues, and rejecting them is what keeps the
// hook from splitting an instruction.
bool isKnownOneByteOpcode(uint8_t op, bool hasModrm) {
    // Arithmetic/logic group: ADD..CMP, 0x00-0x3D, every 8th opcode plus the
    // two-register forms.
    if (op <= 0x3D && (op & 7) < 6) {
        return true;
    }
    switch (op) {
        // AL/eAX/rAX immediate forms and the ones the length table handles.
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0x63: case 0x68: case 0x69: case 0x6A: case 0x6B:
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9C: case 0x9D:
        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC6: case 0xC7:
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCF:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        case 0xE8: case 0xE9: case 0xEB:
        case 0xEC: case 0xED: case 0xEE: case 0xEF:
        case 0xF6: case 0xF7: case 0xF8: case 0xF9:
        case 0xFC: case 0xFD: case 0xFE: case 0xFF:
            return true;
        case 0x0F:
            // Handled by the two-byte path.
            return true;
        default:
            break;
    }
    (void) hasModrm;
    return false;
}

bool isKnownTwoByteOpcode(uint8_t op2, bool hasModrm) {
    // 0F 1F (multi-byte nop), the jcc rel32 group, movzx/movsx, setcc, cmov,
    // bit ops and the SSE subsets the length table understands.
    if (op2 >= 0x80 && op2 <= 0x8F) return true;   // jcc rel32
    if (op2 >= 0x40 && op2 <= 0x4F) return true;   // cmovcc
    if (op2 >= 0x90 && op2 <= 0x9F) return true;   // setcc
    if (op2 >= 0xC8 && op2 <= 0xCF) return true;   // bswap
    switch (op2) {
        case 0x05: case 0x06: case 0x07: case 0x08: case 0x09:
        case 0x0B: case 0x0E:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
        case 0x35: case 0x37:
        case 0x38: case 0x3A:
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0x60: case 0x61: case 0x62: case 0x63:
        case 0x64: case 0x65: case 0x66: case 0x67:
        case 0x68: case 0x69: case 0x6A: case 0x6B:
        case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0xA2: case 0xA3: case 0xA4: case 0xA5:
        case 0xA8: case 0xA9: case 0xAA: case 0xAB:
        case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC4: case 0xC5: case 0xC6:
            return true;
        default:
            break;
    }
    (void) hasModrm;
    return false;
}

}  // namespace

InsnInfo decode(const uint8_t* code) {
    InsnInfo info;
    if (code == nullptr) {
        return info;
    }

    Cursor c{code};
    bool opsize16 = false;
    bool rexW = false;
    bool hadF3 = false;

    // Legacy prefixes (at most 15 total instructions bytes; 4 prefixes is plenty
    // in practice, but loop bounded by 15 to stay safe).
    for (int i = 0; i < 14; ++i) {
        uint8_t b = c.peek();
        if (!isLegacyPrefix(b)) {
            break;
        }
        if (b == 0x66) opsize16 = true;
        if (b == 0xF3) hadF3 = true;
        c.byte();
    }

    // REX
    if ((c.peek() & 0xF0) == 0x40) {
        rexW = (c.peek() & 0x08) != 0;
        c.byte();
    }

    uint8_t op = c.byte();
    bool hasModrm = false;
    bool opcode2 = false;
    uint8_t op2 = 0;
    bool opcode3 = false;

    if (op == 0x0F) {
        opcode2 = true;
        op2 = c.byte();
        if (op2 == 0x38 || op2 == 0x3A) {
            opcode3 = true;
            c.byte();  // third opcode byte is only needed to pick the format
        }
    }

    // --- determine modrm presence -------------------------------------------
    if (!opcode2) {
        switch (op) {
            case 0x00: case 0x01: case 0x02: case 0x03:
            case 0x08: case 0x09: case 0x0A: case 0x0B:
            case 0x10: case 0x11: case 0x12: case 0x13:
            case 0x18: case 0x19: case 0x1A: case 0x1B:
            case 0x20: case 0x21: case 0x22: case 0x23:
            case 0x28: case 0x29: case 0x2A: case 0x2B:
            case 0x30: case 0x31: case 0x32: case 0x33:
            case 0x38: case 0x39: case 0x3A: case 0x3B:
            case 0x62: case 0x63:
            case 0x69: case 0x6B:
            case 0x80: case 0x81: case 0x82: case 0x83:
            case 0x84: case 0x85: case 0x86: case 0x87:
            case 0x88: case 0x89: case 0x8A: case 0x8B:
            case 0x8C: case 0x8D: case 0x8E: case 0x8F:
            case 0xC0: case 0xC1:
            case 0xC4: case 0xC5: case 0xC6: case 0xC7:
            case 0xD0: case 0xD1: case 0xD2: case 0xD3:
            case 0xD8: case 0xD9: case 0xDA: case 0xDB:
            case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            case 0xF6: case 0xF7:
            case 0xFE: case 0xFF:
                hasModrm = true;
                break;
            default:
                break;
        }
        // 0x8F is pop r/m (modrm). 0x63 movsxd.
    } else {
        // Two-byte opcodes: almost all 0F xx have modrm except a few.
        if (opcode3) {
            hasModrm = true;
        } else {
            switch (op2) {
                case 0x05: case 0x06: case 0x07: case 0x08: case 0x09:
                case 0x0B: case 0x0E:
                case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
                case 0x35: case 0x37:
                case 0x77: case 0xA2:
                case 0x80: case 0x81: case 0x82: case 0x83:
                case 0x84: case 0x85: case 0x86: case 0x87:
                case 0x88: case 0x89: case 0x8A: case 0x8B:
                case 0x8C: case 0x8D: case 0x8E: case 0x8F:
                case 0xC8: case 0xC9: case 0xCA: case 0xCB:
                case 0xCC: case 0xCD: case 0xCE: case 0xCF:
                    hasModrm = false;
                    break;
                default:
                    hasModrm = true;
                    break;
            }
        }
    }

    // --- modrm / sib / displacement -----------------------------------------
    uint8_t modrm = 0;
    uint8_t modrmReg = 0;
    if (hasModrm) {
        modrm = c.byte();
        modrmReg = (modrm >> 3) & 7;
        uint8_t mod = (modrm >> 6) & 3;
        uint8_t rm = modrm & 7;

        if (mod != 3) {
            if (rm == 4) {
                uint8_t sib = c.byte();
                if (mod == 0 && ((sib >> 0) & 7) == 5) {
                    c.pos += 4;
                }
            } else if (mod == 0 && rm == 5) {
                info.ripRelative = true;
                info.ripDispOffset = static_cast<uint8_t>(c.pos);
                c.pos += 4;
            }
            if (mod == 1) {
                c.pos += 1;
            } else if (mod == 2) {
                c.pos += 4;
            }
        }
    }

    // --- unknown-opcode detection -------------------------------------------
    //
    // An opcode outside the set the length tables cover would otherwise fall
    // through with a guessed length. The prologue walker cannot tolerate that: it
    // needs exact instruction boundaries or the patch lands mid-instruction.
    bool unknownOpcode = false;
    if (!opcode2) {
        unknownOpcode = !isKnownOneByteOpcode(op, hasModrm);
    } else if (!opcode3) {
        unknownOpcode = !isKnownTwoByteOpcode(op2, hasModrm);
    }

    // --- immediate -----------------------------------------------------------
    int imm = 0;
    if (!opcode2) {
        imm = immediateSize1Byte(op, rexW, opsize16, hasModrm);
        if (hasModrm && (op == 0xF6 || op == 0xF7) && (modrmReg == 0 || modrmReg == 1)) {
            imm = (op == 0xF6) ? 1 : (opsize16 ? 2 : 4);
        }
    } else if (opcode3) {
        if (op2 == 0x3A) {
            imm = 1;
        }
    } else {
        if (op2 >= 0x80 && op2 <= 0x8F) {
            imm = 4;  // jcc rel32
        } else {
            switch (op2) {
                case 0x70: case 0x71: case 0x72: case 0x73:
                case 0xA4: case 0xAC: case 0xBA:
                case 0xC2: case 0xC4: case 0xC5: case 0xC6:
                case 0x0F:  // 3DNow
                    imm = 1;
                    break;
                default:
                    break;
            }
        }
    }

    if (imm > 0) {
        if (op == 0xE8 || op == 0xE9) {
            info.relSize = 4;
            info.relOffset = static_cast<uint8_t>(c.pos);
            info.isCall = (op == 0xE8);
            info.isBranch = true;
        } else if (op == 0xEB) {
            info.relSize = 1;
            info.relOffset = static_cast<uint8_t>(c.pos);
            info.isUnconditionalJump = true;
            info.isBranch = true;
        } else if (op >= 0x70 && op <= 0x7F) {
            info.relSize = 1;
            info.relOffset = static_cast<uint8_t>(c.pos);
            info.isConditionalJump = true;
            info.isBranch = true;
        } else if (opcode2 && op2 >= 0x80 && op2 <= 0x8F) {
            info.relSize = 4;
            info.relOffset = static_cast<uint8_t>(c.pos);
            info.isConditionalJump = true;
            info.isBranch = true;
        }
        c.pos += static_cast<size_t>(imm);
    }

    if (op == 0xC3 || op == 0xC2 || op == 0xCB || op == 0xCA || op == 0xCF) {
        info.isReturn = true;
    }
    if (!opcode2 && (op == 0xE9 || op == 0xEB || op == 0xFF)) {
        if (op == 0xFF && hasModrm && (modrmReg == 4 || modrmReg == 5)) {
            info.isUnconditionalJump = true;  // jmp r/m64
        }
        if (op == 0xE9 || op == 0xEB) {
            info.isUnconditionalJump = true;
        }
        info.isBranch = true;
    }
    if (!opcode2 && op == 0xFF && hasModrm && modrmReg == 2) {
        info.isCall = true;
        info.isBranch = true;
    }
    if (!opcode2 && op == 0xFF && hasModrm && (modrmReg == 2 || modrmReg == 3)) {
        info.isBranch = true;
    }

    (void) hadF3;
    info.opcode = op;
    info.opcode2 = opcode2 ? op2 : 0;
    info.hasModrm = hasModrm;
    info.modrm = modrm;
    info.modrmMod = static_cast<uint8_t>((modrm >> 6) & 3);
    info.modrmReg = static_cast<uint8_t>(modrmReg);
    info.modrmRm = static_cast<uint8_t>(modrm & 7);
    info.hasSib = hasModrm && ((modrm >> 6) & 3) != 3 && (modrm & 7) == 4;
    info.rexW = rexW;
    // The decoder is used to walk a function prologue before overwriting it, so an
    // unknown opcode must be a hard failure, not a guess: guessing a short length
    // would make the caller patch into the middle of an instruction.
    info.len = static_cast<uint8_t>(c.pos);
    info.valid = c.pos > 0 && c.pos <= 15 && !unknownOpcode;
    if (!info.valid) {
        info.len = 0;
    }
    return info;
}

}  // namespace coui
