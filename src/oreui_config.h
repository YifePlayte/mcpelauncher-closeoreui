#pragma once

#include "config.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace coui {

// Mirrors the game's OreUIConfig entry.
//
// Layout is taken from the upstream ForceCloseOreUI mod, which reverse engineered
// it for the same binary: two opaque pointers followed by two predicates. The
// predicates are what decides whether a screen is served by OreUI or by the
// legacy JSON-UI, so overriding them is the whole mechanism.
struct OreUIConfigEntry {
    void* mUnknown1;
    void* mUnknown2;
    std::function<bool()> mUnknown3;
    std::function<bool()> mUnknown4;
};

using OreUIConfigMap = std::unordered_map<std::string, OreUIConfigEntry>;

// Plausibility check on a pointer we are about to treat as the config map.
// Guards against a misidentified hook target taking the game down.
bool looksLikeConfigMap(const void* candidate);

// Applies the current config file to a live map. Returns the number of entries
// changed, or -1 when the map failed validation.
int applyConfigToMap(OreUIConfigMap& configs);

// Reloads the config file from disk. Called once at startup and again whenever
// applyConfigToMap notices the file changed on disk.
void reloadConfig();

// Resolves and returns the config file path in use (for logging).
const std::string& configFilePath();

// Live view of the loaded config, for the menu.
const ModConfig& currentConfig();

// Flips the master switch and writes the config back to disk.
void setEnabled(bool enabled);

}  // namespace coui
