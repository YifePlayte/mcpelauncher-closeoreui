#include "inline_hook.h"

#include "log.h"
#include "x64_decoder.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace coui {
namespace {

// E9 <rel32> — 5 bytes, reaches +-2 GiB. Preferred because it wastes no prologue
// bytes on a function that might be tiny.
constexpr size_t kRelJumpSize = 5;

// FF 25 00 00 00 00 / <8 byte destination> — 14 bytes, position independent and
// reaches anywhere. Used only when rel32 cannot.
constexpr size_t kAbsJumpSize = 14;

constexpr size_t kMaxPrologue = 32;

size_t pageSize() {
    static const size_t value = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return value == 0 ? 4096 : value;
}

uintptr_t pageDown(uintptr_t addr) {
    return addr & ~(static_cast<uintptr_t>(pageSize()) - 1);
}

int64_t readRel(const uint8_t* p, uint8_t size) {
    if (size == 1) {
        return static_cast<int8_t>(*p);
    }
    int32_t v = 0;
    memcpy(&v, p, 4);
    return v;
}

void writeRelJump(uint8_t* at, uintptr_t from, uintptr_t destination) {
    at[0] = 0xE9;
    int32_t rel = static_cast<int32_t>(static_cast<int64_t>(destination) -
                                       static_cast<int64_t>(from + kRelJumpSize));
    memcpy(at + 1, &rel, sizeof(rel));
}

void writeAbsJump(uint8_t* at, uintptr_t destination) {
    at[0] = 0xFF;
    at[1] = 0x25;
    memset(at + 2, 0, 4);
    memcpy(at + 6, &destination, sizeof(destination));
}

// Allocates executable memory within +-2 GiB of `near` so that a rel32 jump can
// reach it and relocated RIP-relative operands stay in range.
void* allocateNear(uintptr_t near) {
    const size_t size = pageSize();
    const uintptr_t granularity = 0x10000;

    for (uintptr_t delta = granularity; delta < 0x70000000ULL; delta += granularity) {
        for (int direction = 0; direction < 2; ++direction) {
            uintptr_t hint = direction == 0 ? near + delta : near - delta;
            if (direction == 1 && hint < delta) {
                continue;
            }
            hint = pageDown(hint);
            void* p = mmap(reinterpret_cast<void*>(hint), size,
                           PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) {
                continue;
            }
            auto addr = reinterpret_cast<uintptr_t>(p);
            uintptr_t diff = addr > near ? addr - near : near - addr;
            if (diff <= 0x7FFFFFF0ULL) {
                return p;
            }
            munmap(p, size);
        }
    }
    return nullptr;
}

bool makeWritable(uintptr_t start, size_t len) {
    uintptr_t begin = pageDown(start);
    uintptr_t end = pageDown(start + len - 1) + pageSize();
    return mprotect(reinterpret_cast<void*>(begin), end - begin,
                    PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

void flushCode(uintptr_t start, size_t len) {
    __builtin___clear_cache(reinterpret_cast<char*>(start), reinterpret_cast<char*>(start + len));
}

}  // namespace

InlineHook::~InlineHook() {
    uninstall();
    if (trampoline_ != nullptr) {
        munmap(trampoline_, pageSize());
        trampoline_ = nullptr;
    }
}

bool InlineHook::install(uintptr_t target, uintptr_t targetEnd, void* detour, void** original,
                         std::string& error) {
    if (installed_) {
        error = "hook already installed";
        return false;
    }
    if (target == 0 || detour == nullptr || original == nullptr) {
        error = "invalid hook arguments";
        return false;
    }

    const auto* code = reinterpret_cast<const uint8_t*>(target);

    // Prefer a 5-byte rel32 jump; fall back to the 14-byte absolute form. Both
    // sizes are tried against the function bound so we never spill into the next
    // function.
    size_t patchSize = kRelJumpSize;
    bool absolute = false;
    {
        int64_t delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(detour)) -
                        static_cast<int64_t>(target + kRelJumpSize);
        if (delta > INT32_MAX || delta < INT32_MIN) {
            patchSize = kAbsJumpSize;
            absolute = true;
        }
    }

    // Collect whole instructions until the patch fits.
    //
    // The loop guard plus the post-decode bound keep `covered` under the size of
    // savedBytes_. A single x86 instruction can be 15 bytes, so checking only
    // before decoding is not enough: `covered` could otherwise jump past the
    // buffer and memcpy() below would overflow.
    std::vector<InsnInfo> insns;
    size_t covered = 0;
    while (covered < patchSize) {
        if (covered >= kMaxPrologue) {
            error = "prologue too long to relocate";
            return false;
        }
        InsnInfo info = decode(code + covered);
        if (!info.valid) {
            // Report the bytes: an unknown opcode at this point means the decoder
            // does not cover something in this build's prologue, which is worth
            // seeing in a bug report rather than only "cannot hook".
            char hex[3 * 8 + 1] = {0};
            for (int b = 0; b < 8 && covered + b < kMaxPrologue; ++b) {
                snprintf(hex + b * 3, 4, "%02x ", code[covered + b]);
            }
            error = std::string("undecodable instruction in prologue at +") +
                    std::to_string(covered) + ": " + hex;
            return false;
        }
        if (!info.relocatable()) {
            error = "prologue contains a position dependent instruction";
            return false;
        }
        if (covered + info.len > kMaxPrologue) {
            error = "instruction at the end of the patch window would exceed it";
            return false;
        }
        insns.push_back(info);
        covered += info.len;
    }

    // The patch must not run past the end of the function.
    if (targetEnd != 0 && target + covered > targetEnd) {
        error = "function is too short to hook without clobbering its successor";
        return false;
    }

    if (trampoline_ == nullptr) {
        void* mem = allocateNear(target);
        if (mem == nullptr) {
            error = "could not allocate trampoline near the target";
            return false;
        }
        trampoline_ = mem;
    }

    auto* tramp = reinterpret_cast<uint8_t*>(trampoline_);
    memset(tramp, 0xCC, pageSize());
    memcpy(tramp, code, covered);

    size_t offset = 0;
    for (const InsnInfo& info : insns) {
        if (info.ripRelative) {
            uintptr_t oldNext = target + offset + info.len;
            uintptr_t newNext = reinterpret_cast<uintptr_t>(tramp) + offset + info.len;
            int32_t disp = 0;
            memcpy(&disp, code + offset + info.ripDispOffset, sizeof(disp));
            int64_t absoluteTarget = static_cast<int64_t>(oldNext) + disp;
            int64_t newDisp = absoluteTarget - static_cast<int64_t>(newNext);
            if (newDisp > INT32_MAX || newDisp < INT32_MIN) {
                error = "RIP-relative operand out of range after relocation";
                return false;
            }
            int32_t narrowed = static_cast<int32_t>(newDisp);
            memcpy(tramp + offset + info.ripDispOffset, &narrowed, sizeof(narrowed));
        } else if (info.relSize != 0) {
            int64_t rel = readRel(code + offset + info.relOffset, info.relSize);
            uintptr_t absoluteTarget = target + offset + info.len + static_cast<uintptr_t>(rel);
            uintptr_t newNext = reinterpret_cast<uintptr_t>(tramp) + offset + info.len;
            int64_t newRel = static_cast<int64_t>(absoluteTarget) - static_cast<int64_t>(newNext);
            if (info.relSize == 1) {
                if (newRel > 127 || newRel < -128) {
                    error = "short branch out of range after relocation";
                    return false;
                }
                int8_t narrowed = static_cast<int8_t>(newRel);
                memcpy(tramp + offset + info.relOffset, &narrowed, 1);
            } else {
                if (newRel > INT32_MAX || newRel < INT32_MIN) {
                    error = "branch out of range after relocation";
                    return false;
                }
                int32_t narrowed = static_cast<int32_t>(newRel);
                memcpy(tramp + offset + info.relOffset, &narrowed, 4);
            }
        }
        offset += info.len;
    }

    // Jump back to the instruction after the patched prologue.
    uintptr_t trampAddr = reinterpret_cast<uintptr_t>(tramp);
    int64_t backDelta = static_cast<int64_t>(target + covered) -
                        static_cast<int64_t>(trampAddr + covered + kRelJumpSize);
    if (backDelta <= INT32_MAX && backDelta >= INT32_MIN) {
        writeRelJump(tramp + covered, trampAddr + covered, target + covered);
    } else {
        writeAbsJump(tramp + covered, target + covered);
    }
    flushCode(trampAddr, covered + kAbsJumpSize);

    // `*original` is deliberately assigned only after the patch is in place: a
    // caller that sees install() fail must not be handed a trampoline for a hook
    // that was never applied.
    if (!makeWritable(target, covered)) {
        error = std::string("mprotect failed: ") + strerror(errno);
        return false;
    }

    memcpy(savedBytes_, code, covered);
    savedLen_ = covered;

    // Pad any bytes between the jump's end and the relocated instructions with
    // NOPs so the leftover bytes never decode as something else.
    uint8_t patch[kAbsJumpSize];
    memset(patch, 0x90, sizeof(patch));
    if (absolute) {
        writeAbsJump(patch, reinterpret_cast<uintptr_t>(detour));
    } else {
        writeRelJump(patch, target, reinterpret_cast<uintptr_t>(detour));
    }
    memcpy(reinterpret_cast<void*>(target), patch, covered);
    flushCode(target, covered);

    *original = trampoline_;
    target_ = target;
    installed_ = true;
    logDebug("hook installed: target=%p detour=%p trampoline=%p prologue=%zu (%s)", 
             reinterpret_cast<void*>(target), detour, trampoline_, covered,
             absolute ? "abs" : "rel32");
    return true;
}

void InlineHook::uninstall() {
    if (!installed_) {
        return;
    }
    if (makeWritable(target_, savedLen_)) {
        memcpy(reinterpret_cast<void*>(target_), savedBytes_, savedLen_);
        flushCode(target_, savedLen_);
    } else {
        logError("failed to restore hook target %p", reinterpret_cast<void*>(target_));
    }
    installed_ = false;
}

}  // namespace coui
