#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace coui {

struct ModConfig {
    // Master switch. When false the mod loads but never touches the game.
    bool enabled = true;

    // Default value written for every OreUI screen found in the game's config
    // map. false forces the legacy UI, which is the point of this mod.
    bool defaultScreenValue = false;

    // Per-screen overrides, keyed by the exact route name the game uses
    // (e.g. "/main-menu").
    std::unordered_map<std::string, bool> overrides;
};

struct ModConfigLoaded {
    ModConfig config;
    std::string path;
    bool fileExisted = false;
    bool parseFailed = false;
    std::string error;
};

// Locates the config directory, creating it when needed, and loads the config.
ModConfigLoaded loadModConfig();

// Candidate directories, in priority order, for diagnostics.
std::vector<std::string> configDirCandidates();

}  // namespace coui
