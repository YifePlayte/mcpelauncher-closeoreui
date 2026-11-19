#include "module_scan.h"

#include "log.h"
#include "target.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <link.h>

namespace coui {
namespace {

bool pathMatchesGame(const char* path) {
    if (path == nullptr) {
        return false;
    }
    const char* slash = strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    return strcmp(name, kGameLibrary) == 0;
}

struct IterateState {
    ModuleInfo* out = nullptr;
    bool found = false;
};

int phdrCallback(struct dl_phdr_info* info, size_t /*size*/, void* data) {
    auto* state = static_cast<IterateState*>(data);
    if (state->found || !pathMatchesGame(info->dlpi_name)) {
        return 0;
    }

    ModuleInfo& module = *state->out;
    module.path = info->dlpi_name;
    module.base = static_cast<uintptr_t>(info->dlpi_addr);
    module.regions.clear();

    uintptr_t lowest = UINTPTR_MAX;
    uintptr_t highest = 0;

    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) {
            continue;
        }
        Region region;
        region.start = module.base + static_cast<uintptr_t>(ph.p_vaddr);
        region.size = static_cast<size_t>(ph.p_memsz);
        module.regions.push_back(region);

        if (region.start < lowest) {
            lowest = region.start;
        }
        if (region.start + region.size > highest) {
            highest = region.start + region.size;
        }
    }

    if (!module.regions.empty() && lowest != UINTPTR_MAX) {
        module.size = highest - lowest;
        state->found = true;
    }
    return 1;
}

// Fallback: parse /proc/self/maps. Useful on hosts where the launcher's
// dl_iterate_phdr does not know about the game (e.g. some preload setups).
bool findFromProcMaps(ModuleInfo& out) {
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) {
        return false;
    }

    std::string line;
    std::vector<Region> regions;
    uintptr_t lowest = UINTPTR_MAX;
    uintptr_t highest = 0;
    std::string resolvedPath;

    while (std::getline(maps, line)) {
        unsigned long start = 0;
        unsigned long end = 0;
        // Only the address range matters; permissions are not needed to bound the
        // image, and the path is taken from the line itself.
        if (std::sscanf(line.c_str(), "%lx-%lx", &start, &end) != 2) {
            continue;
        }
        if (line.find(kGameLibrary) == std::string::npos) {
            continue;
        }

        if (resolvedPath.empty()) {
            // The path is the last field of the line; every line for this library
            // repeats it, so the first match is enough. Keeping the real path makes
            // the log identify which version was patched.
            const size_t slash = line.rfind('/');
            if (slash != std::string::npos) {
                size_t begin = line.find_last_of(' ', slash);
                begin = (begin == std::string::npos) ? 0 : begin + 1;
                resolvedPath = line.substr(begin);
            }
        }

        Region region;
        region.start = static_cast<uintptr_t>(start);
        region.size = static_cast<size_t>(end - start);
        regions.push_back(region);

        if (region.start < lowest) {
            lowest = region.start;
        }
        if (region.start + region.size > highest) {
            highest = region.start + region.size;
        }
    }

    if (regions.empty()) {
        return false;
    }

    out.base = lowest;
    out.size = highest - lowest;
    out.regions = std::move(regions);
    out.path = resolvedPath.empty() ? kGameLibrary : resolvedPath;
    return true;
}

}  // namespace

bool findMinecraftModule(ModuleInfo& out, std::string& error) {
    IterateState state;
    state.out = &out;
    dl_iterate_phdr(&phdrCallback, &state);

    if (state.found) {
        return true;
    }

    if (findFromProcMaps(out)) {
        logWarn("dl_iterate_phdr did not report %s, using /proc/self/maps", kGameLibrary);
        return true;
    }

    error = std::string("could not locate ") + kGameLibrary;
    return false;
}

}  // namespace coui
