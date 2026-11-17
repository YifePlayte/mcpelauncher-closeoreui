#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coui {

struct Region {
    uintptr_t start = 0;
    size_t size = 0;
    bool executable = false;
    bool readable = false;
    bool writable = false;
};

struct ModuleInfo {
    std::string path;
    uintptr_t base = 0;
    size_t size = 0;
    std::vector<Region> regions;
};

// Locate libminecraftpe.so inside the launcher's bionic linker.
//
// The mod itself is loaded by the same linker that loaded the game, so
// dl_iterate_phdr() reports the game. A /proc/self/maps fallback covers the case
// where the launcher's dl_iterate_phdr is unavailable for some reason.
bool findMinecraftModule(ModuleInfo& out, std::string& error);

// All executable bytes of a module, in one contiguous logical view.
// Returns empty when the module has no executable segment.
bool collectExecutableRange(const ModuleInfo& module, uintptr_t& start, size_t& size);

}  // namespace coui
