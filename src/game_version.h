#pragma once

#include <string>

namespace coui {

// The game version currently running, read from the launcher. Reported for
// diagnostics only — nothing in this mod gates on it.
struct GameVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    int revision = 0;
    int code = 0;
    std::string package;
    bool known = false;

    std::string toString() const;
};

GameVersion queryGameVersion();

// Build-time metadata injected by CMake.
//
// Deliberately holds no Minecraft version: this mod is not built *for* a game
// release. The target function is located structurally at load time, so pinning
// the binary to a version would be misleading rather than informative. Which
// versions have actually been verified is recorded in the release notes instead.
struct BuildTarget {
    std::string abi;
    std::string modVersion;
};

const BuildTarget& buildTarget();

}  // namespace coui
