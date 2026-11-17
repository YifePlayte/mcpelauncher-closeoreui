#include "oreui_hook.h"

#include "inline_hook.h"
#include "log.h"
#include "oreui_config.h"

#include <atomic>
#include <cstring>

namespace coui {
namespace {

// Signature recovered from the upstream mod: the registration function takes the
// OreUIConfig map by reference in x0 and several further opaque arguments.
//
// Declaring them as void* is deliberate — the detour never touches them, and a
// wrong guess only matters if the ABI class of an argument changes, which it
// cannot for pointers.
using ConfigFunction = void (*)(OreUIConfigMap&, void*, void*, void*, void*, void*);

InlineHook gHook;
ConfigFunction gOrigin = nullptr;
std::atomic<uint32_t> gApplyCount{0};
bool gWarnedBadMap = false;

void detour(OreUIConfigMap& configs, void* a2, void* a3, void* a4, void* a5, void* a6) {
    // Run the game's own registration first. Everything the map is supposed to
    // contain is in place afterwards, including entries this mod has no opinion
    // about.
    if (gOrigin != nullptr) {
        gOrigin(configs, a2, a3, a4, a5, a6);
    }

    if (!looksLikeConfigMap(&configs)) {
        if (!gWarnedBadMap) {
            gWarnedBadMap = true;
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
