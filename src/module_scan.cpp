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
        region.executable = (ph.p_flags & PF_X) != 0;
        region.readable = (ph.p_flags & PF_R) != 0;
        region.writable = (ph.p_flags & PF_W) != 0;
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

    while (std::getline(maps, line)) {
        unsigned long start = 0;
        unsigned long end = 0;
        char perms[8] = {0};
        if (std::sscanf(line.c_str(), "%lx-%lx %7s", &start, &end, perms) != 3) {
            continue;
        }
        if (line.find(kGameLibrary) == std::string::npos) {
            continue;
        }

        Region region;
        region.start = static_cast<uintptr_t>(start);
        region.size = static_cast<size_t>(end - start);
        region.readable = perms[0] == 'r';
        region.writable = perms[1] == 'w';
        region.executable = perms[2] == 'x';
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
    out.path = kGameLibrary;
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

bool collectExecutableRange(const ModuleInfo& module, uintptr_t& start, size_t& size) {
    start = 0;
    size = 0;

    // The first executable PT_LOAD is the .text segment. Returning just that one
    // keeps the scanner from walking into .plt / .init stubs of unrelated pages
    // and is what every pattern search in the launcher does too.
    for (const Region& region : module.regions) {
        if (region.executable) {
            start = region.start;
            size = region.size;
            return size != 0;
        }
    }
    return false;
}

}  // namespace coui
