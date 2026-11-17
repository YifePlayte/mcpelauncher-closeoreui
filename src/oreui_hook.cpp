#include "oreui_hook.h"

#include "inline_hook.h"
#include "log.h"
#include "oreui_config.h"

#include <atomic>
#include <cstring>

namespace coui {
namespace {

// The registration function takes the OreUIConfig map by reference as its first
// argument — in the x86-64 System V ABI that is %rdi, not AArch64's x0. The other
// five are opaque: the detour forwards them untouched, so their exact types do not
// matter as long as they stay pointer-sized.
using ConfigFunction = void (*)(OreUIConfigMap&, void*, void*, void*, void*, void*);

InlineHook gHook;
ConfigFunction gOrigin = nullptr;
std::atomic<uint32_t> gApplyCount{0};
std::atomic<bool> gWarnedBadMap{false};

void detour(OreUIConfigMap& configs, void* a2, void* a3, void* a4, void* a5, void* a6) {
    // Run the game's own registration first. Everything the map is supposed to
    // contain is in place afterwards, including entries this mod has no opinion
    // about.
    //
    // The map is not validated before this call on purpose: forwarding an argument
    // the game handed us is always safe, whereas probing it first would read
    // arbitrary memory if we had hooked the wrong function, turning a wrong guess
    // into a crash.
    if (gOrigin != nullptr) {
        gOrigin(configs, a2, a3, a4, a5, a6);
    }

    if (!looksLikeConfigMap(&configs)) {
        if (!gWarnedBadMap.exchange(true)) {
            logError("config map pointer %p looks invalid; leaving the map untouched",
                     static_cast<void*>(&configs));
        }
        return;
    }

    int changed = applyConfigToMap(configs);
    if (changed < 0) {
        return;
    }

    uint32_t count = gApplyCount.fetch_add(1);
    if (count == 0) {
        logInfo("first application: %d screens patched", changed);
    }
}

}  // namespace

bool installConfigHook(uintptr_t functionAddress, uintptr_t functionEnd, std::string& error) {
    if (gHook.installed()) {
        error = "hook already installed";
        return false;
    }

    void* original = nullptr;
    if (!gHook.install(functionAddress, functionEnd, reinterpret_cast<void*>(&detour), &original,
                      error)) {
        return false;
    }

    gOrigin = reinterpret_cast<ConfigFunction>(original);
    logDebug("trampoline at %p", original);
    return true;
}

}  // namespace coui
