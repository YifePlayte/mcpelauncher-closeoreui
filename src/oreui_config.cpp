#include "oreui_config.h"

#include "config.h"
#include "json.h"
#include "log.h"

#include <cerrno>
#include <cstring>
#include <string>

#include <sys/stat.h>

namespace coui {
namespace {

ModConfig gConfig;
std::string gConfigPath;
bool gConfigReady = false;
struct stat gConfigStat {};

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

bool refreshConfigIfChanged() {
    if (gConfigPath.empty()) {
        return false;
    }
    struct stat st {};
    if (stat(gConfigPath.c_str(), &st) != 0) {
        return false;
    }
    if (gConfigReady && st.st_mtime == gConfigStat.st_mtime &&
        st.st_size == gConfigStat.st_size) {
        return false;
    }

    ModConfigLoaded loaded = loadModConfig();
    gConfig = loaded.config;
    gConfigStat = st;
    gConfigReady = true;
    logInfo("config reloaded from %s (%zu screen overrides)", loaded.path.c_str(),
            gConfig.overrides.size());
    return true;
}

void setEntryValue(OreUIConfigEntry& entry, bool value) {
    // Capture by value: the predicates outlive this call and must not dangle.
    entry.mUnknown3 = [value]() { return value; };
    entry.mUnknown4 = [value]() { return value; };
}

}  // namespace

bool looksLikeConfigMap(const void* candidate) {
    return mapLooksSane(candidate, nullptr);
}

const std::string& configFilePath() {
    return gConfigPath;
}

const ModConfig& currentConfig() {
    return gConfig;
}

void setEnabled(bool enabled) {
    if (gConfig.enabled == enabled) {
        return;
    }
    gConfig.enabled = enabled;

    // Write the whole document back; the file is owned by this mod, and doing it
    // all at once keeps the on-disk shape predictable for hand editing.
    Json root = Json::makeObject();
    root["enabled"] = Json::makeBool(gConfig.enabled);
    root["defaultScreenValue"] = Json::makeBool(gConfig.defaultScreenValue);
    Json screens = Json::makeObject();
    for (const auto& entry : gConfig.overrides) {
        screens[entry.first] = Json::makeBool(entry.second);
    }
    root["screens"] = screens;

    std::string error;
    if (!writeFileAtomically(gConfigPath, root.dump(4) + "\n", error)) {
        logError("cannot save config: %s", error.c_str());
        return;
    }

    // Sync the change-stamp so refreshConfigIfChanged() does not treat our own
    // write as an external edit and reload it straight back.
    struct stat st {};
    if (stat(gConfigPath.c_str(), &st) == 0) {
        gConfigStat = st;
        gConfigReady = true;
    }
}

void reloadConfig() {
    ModConfigLoaded loaded = loadModConfig();
    gConfig = loaded.config;
    gConfigPath = loaded.path;
    gConfigReady = false;

    struct stat st {};
    if (stat(gConfigPath.c_str(), &st) == 0) {
        gConfigStat = st;
        gConfigReady = true;
    }

    if (!gConfig.enabled) {
        logInfo("mod disabled by config (%s)", gConfigPath.c_str());
    }
}

int applyConfigToMap(OreUIConfigMap& configs) {
    if (!gConfig.enabled) {
        return 0;
    }

    size_t size = 0;
    if (!mapLooksSane(&configs, &size)) {
        logError("config map failed validation (%zu entries); refusing to patch",
                 size);
        return -1;
    }

    // Pick up edits without requiring a restart.
    refreshConfigIfChanged();

    int changed = 0;
    for (auto& item : configs) {
        bool value = gConfig.defaultScreenValue;
        auto override = gConfig.overrides.find(item.first);
        if (override != gConfig.overrides.end()) {
            value = override->second;
        }
        setEntryValue(item.second, value);
        ++changed;
        logDebug("screen %s -> %s", item.first.c_str(), value ? "oreui" : "legacy");
    }

    if (changed > 0) {
        logInfo("applied OreUI config to %d screens", changed);
    }
    return changed;
}

}  // namespace coui
