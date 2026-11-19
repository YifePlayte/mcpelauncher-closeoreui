#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coui {

// x86-64 inline hook: overwrite a function prologue with a jump to a detour and
// keep the displaced instructions in a trampoline that forwards to the original
// body.
//
// Two safety properties matter here:
//
//  * the overwritten range must stay inside the target function. Decoding the
//    prologue without a known end silently corrupts whatever function follows a
//    short one, which is exactly how a mod takes down a game;
//  * the copied bytes must be position independent, because they are executed
//    from a different address; anything that is not is refused instead of being
//    rewritten, so a prologue that cannot be moved simply is not hooked.
class InlineHook {
public:
    InlineHook() = default;
    ~InlineHook();

    InlineHook(const InlineHook&) = delete;
    InlineHook& operator=(const InlineHook&) = delete;

    // `targetEnd` is a known, readable exclusive function bound. Unknown (0) or
    // non-increasing bounds are refused before any instruction bytes are read.
    //
    // `original` receives a callable pointer with the same signature as the
    // target. Returns false and fills `error` on failure; the target is left
    // untouched in that case.
    bool install(uintptr_t target, uintptr_t targetEnd, void* detour, void** original,
                 std::string& error);

    // Restores the original bytes. Safe to call when not installed.
    void uninstall();

    bool installed() const { return installed_; }
    uintptr_t target() const { return target_; }

private:
    bool installed_ = false;
    uintptr_t target_ = 0;
    uint8_t savedBytes_[32] = {0};
    size_t savedLen_ = 0;
    void* trampoline_ = nullptr;
};

}  // namespace coui
