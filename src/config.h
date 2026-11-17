#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace coui {

// Configuration, as stored in CloseOreUI/config.json.
//
// Field semantics are easy to get wrong, so they are spelled out here rather than
// only in the README. The two switches mean different things and are not
// interchangeable:
//
//   enabled            - whether this mod does anything at all. When false the
//                        config map is left completely untouched, so the game
//                        renders whatever it would have rendered on its own.
//                        This is the "uninstall without moving files" switch.
//
//   defaultScreenValue - what the mod forces each screen to. This is the value
//                        handed to the game's per-screen predicates. The game
//                        treats a true predicate as "this screen uses OreUI", so
//                        false means the legacy JSON-UI. This is the switch users
//                        actually want; it only takes effect while enabled is
//                        true.
//
//   overrides          - per-screen exceptions to defaultScreenValue, keyed by
//                        the exact route the game registers (e.g. "/main-menu").
//                        A screen listed here ignores defaultScreenValue.
struct ModConfig {
    bool enabled = true;
    bool defaultScreenValue = false;
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
