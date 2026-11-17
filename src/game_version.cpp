#include "game_version.h"

#include "log.h"

#include <cstdio>

#include <dlfcn.h>

namespace coui {
namespace {

// mcpelauncher exposes the running game version through libmcpelauncher_mod.so
// as pointers to ints, not as plain symbols:
//
//   mcpelauncher_package_version_major  -> &MinecraftVersion::major
//   ...
//   mcpelauncher_package_name           -> package name C string
//
// Resolved at runtime so a launcher build that drops one does not break the mod.
int* resolveInt(const char* name) {
    void* handle = dlopen("libmcpelauncher_mod.so", RTLD_NOW);
    if (handle == nullptr) {
        return nullptr;
    }
    // No dlclose: the handle belongs to the launcher and other mods may still
    // resolve symbols through it.
    return static_cast<int*>(dlsym(handle, name));
}

const char* resolveString(const char* name) {
    void* handle = dlopen("libmcpelauncher_mod.so", RTLD_NOW);
    if (handle == nullptr) {
        return nullptr;
    }
    return static_cast<const char*>(dlsym(handle, name));
}

}  // namespace

std::string GameVersion::toString() const {
    if (!known) {
        return "unknown";
    }
    char buf[40];
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d", major, minor, patch, revision);
    return buf;
}

GameVersion queryGameVersion() {
    GameVersion version;

    if (int* major = resolveInt("mcpelauncher_package_version_major")) {
        version.major = *major;
        version.known = true;
    }
    if (int* minor = resolveInt("mcpelauncher_package_version_minor")) {
        version.minor = *minor;
    }
    if (int* patch = resolveInt("mcpelauncher_package_version_patch")) {
        version.patch = *patch;
    }
    if (int* revision = resolveInt("mcpelauncher_package_version_revision")) {
        version.revision = *revision;
    }
    if (int* code = resolveInt("mcpelauncher_package_version_code")) {
        version.code = *code;
    }
    if (const char* package = resolveString("mcpelauncher_package_name")) {
        version.package = package;
    }

    return version;
}

const BuildTarget& buildTarget() {
    static const BuildTarget target{CLOSEOREUI_TARGET_ABI, CLOSEOREUI_MOD_VERSION};
    return target;
}

}  // namespace coui
