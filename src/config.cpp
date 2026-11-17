#include "config.h"

#include "json.h"
#include "log.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace coui {
namespace {

constexpr const char* kModDirName = "CloseOreUI";
constexpr const char* kConfigFileName = "config.json";

bool isDirectory(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool fileExists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void makeDirs(const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::string current;
    current.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            std::string prefix = current;
            while (prefix.size() > 1 && prefix.back() == '/') {
                prefix.pop_back();
            }
            if (!prefix.empty()) {
                mkdir(prefix.c_str(), 0755);
            }
        }
    }
}

bool directoryWritable(const std::string& dir) {
    makeDirs(dir);
    if (!isDirectory(dir)) {
        return false;
    }
    const std::string probe = dir + "/.write_test";
    FILE* f = fopen(probe.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    fclose(f);
    unlink(probe.c_str());
    return true;
}

// The launcher stores mods in its data directory. On Linux that is
// $XDG_DATA_HOME/mcpelauncher/mods (default ~/.local/share/mcpelauncher/mods).
//
// We deliberately accept several spellings because users run the launcher from
// flatpak, from an AppImage, or with -d pointing at a custom data dir.
std::string dataHome() {
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg != nullptr && *xdg != '\0') {
        return xdg;
    }
    const char* home = getenv("HOME");
    if (home != nullptr && *home != '\0') {
        return std::string(home) + "/.local/share";
    }
    return {};
}

}  // namespace

std::vector<std::string> configDirCandidates() {
    std::vector<std::string> candidates;

    // 1. $MCPELAUNCHER_DATA_DIR /mods/CloseOreUI — explicit override, also set by
    //    the upstream UI when a custom data directory is configured.
    if (const char* dataDir = getenv("MCPELAUNCHER_DATA_DIR")) {
        if (*dataDir != '\0') {
            candidates.push_back(std::string(dataDir) + "/mods/" + kModDirName);
        }
    }

    // 2. The standard launcher data directory.
    const std::string home = dataHome();
    if (!home.empty()) {
        candidates.push_back(home + "/mcpelauncher/mods/" + kModDirName);
    }

    // 3. Flatpak sandbox location. Only offered when actually inside the sandbox:
    //    adding it unconditionally would make a native install pick the flatpak
    //    directory whenever both happen to exist.
    if (getenv("FLATPAK_ID") != nullptr && !home.empty()) {
        candidates.push_back(home + "/.var/app/io.mrarm.mcpelauncher/data/mcpelauncher/mods/" +
                             kModDirName);
    }

    // 4. XDG_DATA_DIRS entries (system-wide installs).
    if (const char* dirs = getenv("XDG_DATA_DIRS")) {
        std::string list(dirs);
        size_t start = 0;
        while (start <= list.size()) {
            size_t sep = list.find(':', start);
            std::string entry =
                    list.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
            if (!entry.empty()) {
                candidates.push_back(entry + "/mcpelauncher/mods/" + kModDirName);
            }
            if (sep == std::string::npos) {
                break;
            }
            start = sep + 1;
        }
    }

    // 5. Last resort: next to the current working directory, so a portable
    //    setup works without touching the user profile.
    candidates.push_back("./mods/" + std::string(kModDirName));

    return candidates;
}

ModConfigLoaded loadModConfig() {
    ModConfigLoaded loaded;

    std::string targetDir;
    for (const std::string& candidate : configDirCandidates()) {
        if (directoryWritable(candidate)) {
            targetDir = candidate;
            break;
        }
    }
    if (targetDir.empty()) {
        auto candidates = configDirCandidates();
        targetDir = candidates.empty() ? std::string("./mods/") + kModDirName : candidates.front();
    }

    const std::string targetPath = targetDir + "/" + kConfigFileName;
    loaded.path = targetPath;

    // Read an existing file from any candidate directory, so that a config left
    // behind by the flatpak build still takes effect in a native build.
    std::string sourcePath;
    if (fileExists(targetPath)) {
        sourcePath = targetPath;
    } else {
        for (const std::string& candidate : configDirCandidates()) {
            const std::string probe = candidate + "/" + kConfigFileName;
            if (fileExists(probe)) {
                sourcePath = probe;
                break;
            }
        }
    }

    ModConfig& config = loaded.config;

    if (!sourcePath.empty()) {
        std::string text;
        std::string ioError;
        if (!readFile(sourcePath, text, ioError)) {
            loaded.error = ioError;
            loaded.parseFailed = true;
            logError("cannot read config: %s", ioError.c_str());
        } else {
            Json root;
            std::string parseError;
            if (!Json::parse(text, root, parseError)) {
                loaded.error = parseError;
                loaded.parseFailed = true;
                logError("config.json is invalid (%s), using defaults", parseError.c_str());
            } else {
                loaded.fileExisted = true;
                const Json* enabled = root.find("enabled");
                if (enabled != nullptr) {
                    bool ok = false;
                    bool value = enabled->toBool(&ok);
                    if (ok) {
                        config.enabled = value;
                    }
                }
                const Json* defaultScreen = root.find("defaultScreenValue");
                if (defaultScreen == nullptr) {
                    defaultScreen = root.find("default");
                }
                if (defaultScreen != nullptr) {
                    bool ok = false;
                    bool value = defaultScreen->toBool(&ok);
                    if (ok) {
                        config.defaultScreenValue = value;
                    }
                }

                // Per-screen settings live under "screens"; a flat layout is also
                // accepted so a user can write {"screens": {...}} or
                // {"screens": [{"name": "/main-menu", "value": false}]}.
                const Json* screens = root.find("screens");
                if (screens == nullptr) {
                    screens = root.find("oreui");
                }
                if (screens != nullptr) {
                    if (screens->isObject()) {
                        for (const auto& member : screens->members()) {
                            bool ok = false;
                            bool value = member.second.toBool(&ok);
                            if (ok) {
                                config.overrides[member.first] = value;
                            } else {
                                logWarn("ignoring screen %s: not a boolean", member.first.c_str());
                            }
                        }
                    } else if (screens->isArray()) {
                        for (const Json& item : screens->items()) {
                            if (!item.isObject()) {
                                continue;
                            }
                            const Json* name = item.find("name");
                            if (name == nullptr) {
                                name = item.find("key");
                            }
                            std::string key = name != nullptr ? name->asString() : std::string();
                            if (key.empty()) {
                                continue;
                            }
                            bool ok = false;
                            bool value = item.toBool(&ok);
                            if (ok) {
                                config.overrides[key] = value;
                            }
                        }
                    }
                }
            }
        }
    } else {
        // First run: write a commented-by-example config so the user has
        // something to edit, and log where it went.
        Json root = Json::makeObject();
        root["enabled"] = Json::makeBool(config.enabled);
        root["defaultScreenValue"] = Json::makeBool(config.defaultScreenValue);
        Json screens = Json::makeObject();
        root["screens"] = screens;

        std::string writeError;
        if (!writeFileAtomically(targetPath, root.dump(4) + "\n", writeError)) {
            logWarn("could not create default config: %s", writeError.c_str());
        } else {
            logInfo("created default config at %s", targetPath.c_str());
        }
        loaded.fileExisted = true;
    }

    return loaded;
}

}  // namespace coui
