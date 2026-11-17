#include "discover.h"
#include "eh_frame.h"
#include "elf_image.h"
#include "game_version.h"
#include "log.h"
#include "menu.h"
#include "module_scan.h"
#include "oreui_config.h"
#include "oreui_hook.h"
#include "target.h"

// mcpelauncher mod ABI: the launcher dlopen()s this shared object and calls
// mod_preinit() before the game library is loaded, and mod_init() after.
//
//   mod_preinit -> the game is not loaded yet; this is where the launcher menu is
//                  registered and the config is read.
//   mod_init    -> libminecraftpe.so is mapped; this is where we patch.

extern "C" {

__attribute__((visibility("default"))) void mod_preinit() {
    coui::logInfo("CloseOreUI preinit (mod %s, target %s/%s)", CLOSEOREUI_MOD_VERSION,
                  coui::kPlatform, coui::kSupportedAbi);

    // Nothing to do yet: the config is read in mod_init, and the menu can only be
    // registered once the game's screen list is known, which happens later still.
}

__attribute__((visibility("default"))) void mod_init() {
    coui::logInfo("CloseOreUI init (mod %s)", CLOSEOREUI_MOD_VERSION);

    // The running game version is reported for diagnostics only. There is no
    // build-time version to compare against by design: the locator is structural,
    // so the same binary is expected to work across patch releases.
    coui::GameVersion game = coui::queryGameVersion();
    coui::logInfo("running game: %s (versionCode=%d)", game.toString().c_str(), game.code);

    coui::reloadConfig();
    coui::logInfo("config: %s", coui::configFilePath().c_str());

    coui::ModuleInfo module;
    std::string error;
    if (!coui::findMinecraftModule(module, error)) {
        coui::logError("cannot find %s: %s", coui::kGameLibrary, error.c_str());
        return;
    }
    coui::logInfo("game mapped at %p (%zu bytes, %zu segments)",
                  reinterpret_cast<void*>(module.base), module.size, module.regions.size());

    coui::ElfImage image;
    if (!image.parse(module.base, module.path, error)) {
        coui::logError("cannot parse the game ELF: %s", error.c_str());
        return;
    }

    coui::FunctionTable functions;
    std::string tableError;
    if (!functions.build(image, tableError)) {
        coui::logError("cannot build the function table: %s", tableError.c_str());
        return;
    }
    coui::logInfo("function table: %zu entries", functions.size());

    coui::DiscoveryResult discovery;
    if (!coui::discoverOreUiConfigFunction(image, functions, discovery, error)) {
        coui::logError("cannot locate the OreUI config function: %s", error.c_str());
        return;
    }

    coui::logInfo("OreUI config function at %p (top-level screens=%zu, routes=%zu, candidates=%zu)",
                  reinterpret_cast<void*>(discovery.configFunction),
                  discovery.topLevelRouteCount, discovery.routeCount,
                  discovery.candidatesConsidered);

    for (const auto& candidate : discovery.topCandidates) {
        coui::logDebug("candidate %p top=%zu routes=%zu refs=%zu size=%zu prologue=%d",
                       reinterpret_cast<void*>(candidate.address), candidate.topLevelRouteCount,
                       candidate.routeCount, candidate.referenceCount, candidate.functionSize,
                       candidate.prologueScore);
    }

    if (!coui::installConfigHook(discovery.configFunction, discovery.configFunctionEnd, error)) {
        coui::logError("cannot hook the OreUI config function: %s", error.c_str());
        return;
    }
    coui::logInfo("hook installed");
}

}  // extern "C"
