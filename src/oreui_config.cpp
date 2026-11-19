#include "oreui_config.h"

#include "config.h"
#include "json.h"
#include "log.h"
#include "menu.h"

#include <algorithm>
#include <cstdio>
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

// libc++ unordered_map ABI v1: bucket array, bucket count, first node, element
// count. The host libstdc++ test map has the same four-word container prefix.
struct MapPeek {
    const void* const* buckets;
    size_t bucketCount;
    const void* firstNode;
    size_t size;
};

// The registration owns its map for the duration of this call. Check readable
// mappings before probing a raw ABI object, rather than dereferencing arbitrary
// pointers when a future build has an incompatible layout.
class ReadableMemory {
public:
    ReadableMemory() {
        FILE* maps = fopen("/proc/self/maps", "r");
        if (maps == nullptr) return;
        char line[512];
        while (fgets(line, sizeof(line), maps) != nullptr) {
            unsigned long begin = 0;
            unsigned long end = 0;
            char permissions[5]{};
            if (sscanf(line, "%lx-%lx %4s", &begin, &end, permissions) == 3 &&
                permissions[0] == 'r' && begin < end) {
                ranges_.emplace_back(begin, end);
            }
        }
        fclose(maps);
    }

    bool contains(const void* pointer, size_t size) const {
        uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
        if (address == 0 || size > UINTPTR_MAX - address) return false;
        const uintptr_t end = address + size;
        for (const auto& range : ranges_) {
            if (range.second <= address) continue;
            if (range.first > address) return false;
            if (range.second >= end) return true;
            address = range.second;
        }
        return false;
    }

private:
    std::vector<std::pair<uintptr_t, uintptr_t>> ranges_;
};

// libc++ puts the cached hash before the value; libstdc++'s host node puts its
// value immediately after the next pointer. This distinction is only for tests.
#if defined(_LIBCPP_VERSION)
constexpr size_t kNodePrefix = 2 * sizeof(void*);
#elif defined(__GLIBCXX__)
constexpr size_t kNodePrefix = sizeof(void*);
#else
#error Unsupported unordered_map ABI
#endif
using MapValue = OreUIConfigMap::value_type;
constexpr size_t kValueOffset =
        (kNodePrefix + alignof(MapValue) - 1) / alignof(MapValue) * alignof(MapValue);

bool mapLooksSane(const void* map, size_t* reportedSize) {
    if (map == nullptr || reinterpret_cast<uintptr_t>(map) % alignof(OreUIConfigMap) != 0) {
        return false;
    }
    ReadableMemory memory;
    if (!memory.contains(map, sizeof(MapPeek))) return false;
    MapPeek peek{};
    memcpy(&peek, map, sizeof(peek));
    if (reportedSize != nullptr) *reportedSize = peek.size;

    // Capacity is not the element count. Empty reserved maps can still own a
    // bucket array, but only populated maps have a first node.
    if (peek.size > 100000) {
        return false;
    }
    if ((peek.bucketCount == 0) != (peek.buckets == nullptr) ||
        (peek.size == 0) != (peek.firstNode == nullptr) ||
        (peek.size > 0 && peek.bucketCount == 0) ||
        peek.bucketCount > SIZE_MAX / sizeof(void*)) {
        return false;
    }
    if (peek.bucketCount > 0 &&
        !memory.contains(peek.buckets, peek.bucketCount * sizeof(void*))) {
        return false;
    }

    const void* node = peek.firstNode;
    size_t visited = 0;
    while (node != nullptr) {
        if (visited >= peek.size || reinterpret_cast<uintptr_t>(node) % alignof(MapValue) != 0 ||
            !memory.contains(node, kValueOffset + sizeof(MapValue))) {
            return false;
        }
        const auto* value = reinterpret_cast<const MapValue*>(
                static_cast<const uint8_t*>(node) + kValueOffset);
        const std::string& key = value->first;
        const size_t length = key.size();
        if (length < 4 || length > 120 || !memory.contains(key.data(), length + 1) ||
            key.data()[0] != '/' || key.data()[length] != '\0') {
            return false;
        }
        const void* next = nullptr;
        memcpy(&next, node, sizeof(next));
        node = next;
        ++visited;
    }
    return visited == peek.size;
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
            routes.push_back(item.first);
            // The master switch only disables mutation, not the menu that lets
            // the user enable the mod again after restarting the game.
            if (!gConfig.enabled) continue;
            const bool value = lookupScreenValueLocked(item.first);
            setEntryValue(item.second, value);
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
