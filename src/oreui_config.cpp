#include "oreui_config.h"

#include "config.h"
#include "json.h"
#include "log.h"
#include "menu.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace coui {
namespace {

ModConfig gConfig;

// Guards gConfig. The per-screen predicates installed into the game are called
// from the render/game thread, while the menu writes from the launcher's UI
// thread, so every access goes through this.
//
// A shared_mutex is used rather than a plain mutex because reads happen far more
// often than writes: the predicates may be evaluated every frame for every screen.
std::shared_mutex gConfigMutex;

std::string gConfigPath;

}  // namespace

// Snapshot of a config value. Caller must hold gConfigMutex.
static bool lookupScreenValueLocked(const std::string& screen) {
    if (!gConfig.enabled) {
        // Master switch off means "leave the game alone": OreUI for everything is
        // the neutral state, so the legacy UI is not forced.
        return true;
    }
    auto found = gConfig.overrides.find(screen);
    if (found != gConfig.overrides.end()) {
        return found->second;
    }
    return gConfig.defaultScreenValue;
}

namespace {

// libc++'s unordered_map layout (ABI version 1) as seen through its first three
// words: the bucket pointer array, the element count, and the capacity word.
// Reading this is the only cheap way to sanity check a map we received through a
// raw ABI boundary, and because the mod is compiled with the same libc++ ABI as
// the game it is also the map we are about to mutate.
struct MapPeek {
    const void* const* buckets;
    size_t size;
};

MapPeek peekMap(const void* map) {
    MapPeek peek{};
    memcpy(&peek, map, sizeof(peek));
    return peek;
}

bool mapLooksSane(const void* map, size_t* reportedSize) {
    if (map == nullptr) {
        return false;
    }
    MapPeek peek = peekMap(map);

    // An empty map has a null bucket array but a zero size. A populated map has
    // both. Anything else means we are looking at the wrong object.
    if (peek.size > 100000) {
        return false;
    }
    if (peek.size > 0 && peek.buckets == nullptr) {
        return false;
    }
    if (reportedSize != nullptr) {
        *reportedSize = peek.size;
    }
    return true;
}

// The game evaluates each predicate exactly once, during startup, and caches the
// result — measured with a call counter: 1 call over a 45-second run. Returning a
// live value therefore cannot change anything at runtime, so the value is captured
// by copy and the cheap form is preferred.
void setEntryValue(OreUIConfigEntry& entry, bool value) {
    entry.mUnknown3 = [value]() { return value; };
    entry.mUnknown4 = [value]() { return value; };
}

}  // namespace

bool looksLikeConfigMap(const void* candidate) {
    return mapLooksSane(candidate, nullptr);
}

bool screenValue(const std::string& screen) {
    std::shared_lock<std::shared_mutex> lock(gConfigMutex);
    return lookupScreenValueLocked(screen);
}

const std::string& configFilePath() {
    return gConfigPath;
}

const ModConfig& currentConfig() {
    // Callers that only read a single field are better off using lookupScreenValue()
    // or the dedicated getters; this returns a reference to mutable state and is
    // kept for the menu's read-only inspection. The lock is not held here, so a
    // concurrent menu write can be observed mid-change — acceptable because every
    // consumer re-reads on the next frame anyway.
    return gConfig;
}

// Serialises `config` and writes it. Called without gConfigMutex held: the caller
// takes a snapshot, releases the lock, then persists, so the game thread is never
// blocked behind file I/O.
static bool persistSnapshot(const ModConfig& config) {
    if (gConfigPath.empty()) {
        return false;
    }

    Json root = Json::makeObject();
    root["enabled"] = Json::makeBool(config.enabled);
    root["defaultScreenValue"] = Json::makeBool(config.defaultScreenValue);
    Json screens = Json::makeObject();
    for (const auto& entry : config.overrides) {
        screens[entry.first] = Json::makeBool(entry.second);
    }
    root["screens"] = screens;

    std::string error;
    if (!writeFileAtomically(gConfigPath, root.dump(4) + "\n", error)) {
        logError("cannot save config: %s", error.c_str());
        return false;
    }

    return true;
}

// Applies `mutate` under the write lock, then persists a snapshot outside it.
static void mutateConfig(const std::function<void(ModConfig&)>& mutate) {
    ModConfig snapshot;
    {
        std::unique_lock<std::shared_mutex> lock(gConfigMutex);
        mutate(gConfig);
        snapshot = gConfig;
    }
    persistSnapshot(snapshot);
}

void setEnabled(bool enabled) {
    mutateConfig([enabled](ModConfig& config) {
        config.enabled = enabled;
    });
}

void setDefaultScreenValue(bool legacyUi) {
    mutateConfig([legacyUi](ModConfig& config) {
        config.defaultScreenValue = legacyUi;

        // Overrides that now match the new default are redundant; drop them so the
        // file keeps holding only the differences.
        for (auto it = config.overrides.begin(); it != config.overrides.end();) {
            if (it->second == legacyUi) {
                it = config.overrides.erase(it);
            } else {
                ++it;
            }
        }
    });
}

void setScreenOverride(const std::string& screen, bool value) {
    mutateConfig([&screen, value](ModConfig& config) {
        // An override equal to the global default is redundant, so store nothing:
        // the file stays "only what differs", which is also how a human would
        // write it. Pairing this with setScreenOverride's inverse from the menu
        // means toggling a screen back to the default cleans up its line.
        if (value == config.defaultScreenValue) {
            config.overrides.erase(screen);
            return;
        }
        config.overrides[screen] = value;
    });
}


void reloadConfig() {
    ModConfigLoaded loaded = loadModConfig();
    gConfigPath = loaded.path;

    size_t redundant = 0;
    bool enabled = true;
    std::string message;
    {
        std::unique_lock<std::shared_mutex> lock(gConfigMutex);
        gConfig = loaded.config;

        // A hand-written file may list entries equal to the default. Drop them in
        // memory so the menu and the effective behaviour agree with the "only what
        // differs" rule, but do not rewrite the file behind the user's back — that
        // would look like the mod vandalising their config. The next menu click
        // persists the cleaned-up form.
        for (auto it = gConfig.overrides.begin(); it != gConfig.overrides.end();) {
            if (it->second == gConfig.defaultScreenValue) {
                it = gConfig.overrides.erase(it);
                ++redundant;
            } else {
                ++it;
            }
        }
        enabled = gConfig.enabled;
        message = gConfigPath;
    }

    if (redundant > 0) {
        logInfo("ignoring %zu screen override(s) equal to defaultScreenValue=%s",
                redundant, loaded.config.defaultScreenValue ? "true" : "false");
    }
    if (!enabled) {
        logInfo("mod disabled by config (%s)", message.c_str());
    }
}

int applyConfigToMap(OreUIConfigMap& configs) {
    {
        std::shared_lock<std::shared_mutex> lock(gConfigMutex);
        if (!gConfig.enabled) {
            return 0;
        }
    }

    size_t size = 0;
    if (!mapLooksSane(&configs, &size)) {
        logError("config map failed validation (%zu entries); refusing to patch",
                 size);
        return -1;
    }

    int changed = 0;
    std::vector<std::string> routes;
    routes.reserve(configs.size());

    {
        std::unique_lock<std::shared_mutex> lock(gConfigMutex);

        for (auto& item : configs) {
            const bool value = lookupScreenValueLocked(item.first);
            setEntryValue(item.second, value);
            routes.push_back(item.first);
            ++changed;
            logDebug("screen %s -> %s", item.first.c_str(), value ? "oreui" : "legacy");
        }
    }

    // Register the menu here rather than in mod_init: this is the first and only
    // place the real screen list is available, and the launcher appends a separate
    // top-level menu per addmenu() call, so the whole tree must go in at once.
    // Sorting keeps the list stable and readable; the map's own order is by hash.
    std::sort(routes.begin(), routes.end());
    installMenu(routes);

    if (changed > 0) {
        logInfo("applied OreUI config to %d screens", changed);
    }
    return changed;
}

}  // namespace coui
