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
//  * the relocated bytes must stay position independent, or the trampoline
//    computes wrong addresses.
class InlineHook {
public:
    InlineHook() = default;
    ~InlineHook();

    InlineHook(const InlineHook&) = delete;
    InlineHook& operator=(const InlineHook&) = delete;

    // `targetEnd` is the exclusive end of the target function (any value beyond
    // the last byte is acceptable; it is only used to bound the patch). Pass 0
    // when the end is unknown, in which case a fixed maximum is assumed.
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
