// Host-side tests for the configuration rules.
//
// These cover the parts that are easy to get subtly wrong and hard to notice at
// runtime: which value wins for a screen, and the invariant that config.json only
// stores overrides that differ from the global default.
//
// The config module reads and writes a real file, so these tests point it at a
// temporary directory through MCPELAUNCHER_DATA_DIR and clean up afterwards.

#include "config.h"
#include "json.h"
#include "oreui_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct MenuEntryABI {
    const char* name;
    void* user;
    bool (*selected)(void*);
    void (*click)(void*);
    size_t length;
    MenuEntryABI* subentries;
};
struct MenuSnapshot {
    std::string name;
    void* user;
    bool (*selected)(void*);
    void (*click)(void*);
    std::vector<MenuSnapshot> children;
};
int gMenuCalls = 0;
std::vector<MenuSnapshot> gMenu;

MenuSnapshot copyEntry(const MenuEntryABI& entry) {
    MenuSnapshot copy{entry.name, entry.user, entry.selected, entry.click, {}};
    for (size_t i = 0; i < entry.length; ++i) copy.children.push_back(copyEntry(entry.subentries[i]));
    return copy;
}
extern "C" void mcpelauncher_addmenu(size_t length, MenuEntryABI* entries) {
    ++gMenuCalls;
    for (size_t i = 0; i < length; ++i) gMenu.push_back(copyEntry(entries[i]));
}

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        printf("FAIL: %s\n", what);
    }
}

std::string gRoot;

std::string configPath() {
    return gRoot + "/mods/CloseOreUI/config.json";
}

std::string readFile(const std::string& path) {
    std::string out;
    std::string error;
    if (!coui::readFile(path, out, error)) {
        check(false, "fixture read succeeds");
        return {};
    }
    return out;
}

void writeFile(const std::string& path, const std::string& content) {
    std::string error;
    check(coui::writeFileAtomically(path, content, error), "fixture write succeeds");
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Points the config loader at a private directory so the tests never touch the
// user's real configuration.
void setup() {
    const char* base = getenv("TMPDIR");
    std::string tmpl = std::string(base && *base ? base : "/tmp") + "/closeoreui-test-XXXXXX";
    const char* dir = mkdtemp(tmpl.data());
    if (dir == nullptr) {
        printf("FAIL: cannot create temp dir\n");
        exit(1);
    }
    gRoot = dir;
    setenv("MCPELAUNCHER_DATA_DIR", gRoot.c_str(), 1);
    setenv("HOME", (gRoot + "/home").c_str(), 1);
    setenv("XDG_DATA_HOME", (gRoot + "/xdg").c_str(), 1);
    setenv("XDG_DATA_DIRS", (gRoot + "/system").c_str(), 1);
    unsetenv("FLATPAK_ID");
    std::filesystem::create_directories(gRoot + "/mods/CloseOreUI");
}

void teardown() {
    if (gRoot.empty()) {
        return;
    }
    std::filesystem::remove_all(gRoot); // only this test's mkdtemp directory
}

// --- tests -----------------------------------------------------------------

void testDefaultValueWins() {
    writeFile(configPath(),
              "{\"enabled\":true,\"defaultScreenValue\":false,\"screens\":{}}");
    coui::reloadConfig();

    check(!coui::screenValue("/main-menu"), "default false makes a screen legacy");
    check(coui::currentConfig().enabled, "enabled read from file");
}

void testOverrideBeatsDefault() {
    writeFile(configPath(),
              "{\"enabled\":true,\"defaultScreenValue\":false,"
              "\"screens\":{\"/settings\":true}}");
    coui::reloadConfig();

    check(!coui::screenValue("/main-menu"), "unlisted screen follows default");
    check(coui::screenValue("/settings"), "override beats default");
}

void testDisabledIgnoresEverything() {
    writeFile(configPath(),
              "{\"enabled\":false,\"defaultScreenValue\":false,"
              "\"screens\":{\"/main-menu\":false}}");
    coui::reloadConfig();

    // With the mod disabled every screen must report OreUI, i.e. "game untouched",
    // regardless of what the rest of the file says.
    check(coui::screenValue("/main-menu"), "disabled master switch forces OreUI");
    check(coui::screenValue("/settings"), "disabled master switch forces OreUI (2)");
}

void testRedundantOverridesAreIgnored() {
    // Overrides equal to the default are dropped in memory on load, so the effective
    // answer still matches the default and the menu does not show a pointless entry.
    writeFile(configPath(),
              "{\"enabled\":true,\"defaultScreenValue\":false,"
              "\"screens\":{\"/main-menu\":false,\"/settings\":true}}");
    coui::reloadConfig();

    check(coui::currentConfig().overrides.count("/main-menu") == 0,
          "override equal to default is dropped on load");
    check(coui::currentConfig().overrides.count("/settings") == 1,
          "differing override is kept on load");
    // The file itself is not rewritten behind the user's back.
    check(contains(readFile(configPath()), "/main-menu"),
          "load does not rewrite the user's file");
}

void testWritingDefaultRemovesEntry() {
    writeFile(configPath(),
              "{\"enabled\":true,\"defaultScreenValue\":false,\"screens\":{}}");
    coui::reloadConfig();

    coui::setScreenOverride("/main-menu", true);
    check(coui::currentConfig().overrides.count("/main-menu") == 1, "override stored");
    check(coui::screenValue("/main-menu"), "override takes effect");
    check(contains(readFile(configPath()), "/main-menu"), "override persisted");

    // Writing the default value must clear the entry rather than store a redundant
    // line — this is what makes toggling a screen back leave config.json clean.
    coui::setScreenOverride("/main-menu", false);
    check(coui::currentConfig().overrides.count("/main-menu") == 0,
          "writing the default removes the override");
    check(!contains(readFile(configPath()), "/main-menu"),
          "removed override is gone from the file");
    check(!coui::screenValue("/main-menu"), "screen falls back to the default");
}

void testChangingDefaultPrunesOverrides() {
    writeFile(configPath(),
              "{\"enabled\":true,\"defaultScreenValue\":false,"
              "\"screens\":{\"/settings\":true}}");
    coui::reloadConfig();
    check(coui::currentConfig().overrides.count("/settings") == 1, "override present");

    // Flipping the global default to true makes the /settings override redundant.
    coui::setDefaultScreenValue(true);
    check(coui::currentConfig().overrides.count("/settings") == 0,
          "overrides matching the new default are pruned");
    check(coui::screenValue("/settings"), "screen follows the new default");
}

void testMasterSwitchPersists() {
    writeFile(configPath(),
              "{\"enabled\":true,\"defaultScreenValue\":false,\"screens\":{}}");
    coui::reloadConfig();

    coui::setEnabled(false);
    check(!coui::currentConfig().enabled, "enabled flag updated");
    check(contains(readFile(configPath()), "\"enabled\": false"), "enabled persisted");

    coui::reloadConfig();
    check(!coui::currentConfig().enabled, "disabled state survives a reload");
}

void testMalformedConfigFallsBack() {
    writeFile(configPath(), "{not json");
    coui::reloadConfig();

    // A broken file must not crash or leave nonsense behind: defaults apply.
    check(coui::currentConfig().enabled, "malformed file falls back to enabled");
    check(!coui::screenValue("/main-menu"), "malformed file falls back to legacy default");
}

void testMapValidation() {
    coui::OreUIConfigMap configs;
    check(coui::looksLikeConfigMap(&configs), "empty map accepted");
    configs.reserve(128);
    check(coui::looksLikeConfigMap(&configs), "empty reserved map accepted");
    configs["/main-menu"] = {nullptr, nullptr, [] { return true; }, [] { return false; }};
    check(coui::looksLikeConfigMap(&configs), "populated map with readable route accepted");
    check(!coui::looksLikeConfigMap(nullptr), "null map rejected");
    check(!coui::looksLikeConfigMap(reinterpret_cast<void*>(1)), "invalid map address rejected");
    struct Prefix {
        const void* buckets;
        size_t bucketCount;
        const void* firstNode;
        size_t size;
    } prefix{};
    memcpy(&prefix, &configs, sizeof(prefix));
    Prefix bad = prefix;
    bad.size = 200000;
    check(!coui::looksLikeConfigMap(&bad), "element count limit is not a bucket count limit");
    bad = prefix;
    bad.firstNode = reinterpret_cast<void*>(1);
    check(!coui::looksLikeConfigMap(&bad), "unreadable node rejected");
    bad = prefix;
    bad.bucketCount = SIZE_MAX;
    check(!coui::looksLikeConfigMap(&bad), "overflowing bucket range rejected");
    const void* next = nullptr;
    memcpy(&next, prefix.firstNode, sizeof(next));
    memcpy(const_cast<void*>(prefix.firstNode), &prefix.firstNode, sizeof(prefix.firstNode));
    check(!coui::looksLikeConfigMap(&configs), "cyclic node list refused before iteration");
    memcpy(const_cast<void*>(prefix.firstNode), &next, sizeof(next));
    check(coui::looksLikeConfigMap(&configs), "restored node list remains valid");
    coui::OreUIConfigMap unrelated;
    unrelated["not-a-route"] = {nullptr, nullptr, [] { return true; }, [] { return false; }};
    check(!coui::looksLikeConfigMap(&unrelated), "non-route keys rejected");
    check(coui::applyConfigToMap(unrelated) == -1, "application validates before reading or modifying entries");
    check(gMenuCalls == 0, "invalid application registers no menu");
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* guard = mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(guard != MAP_FAILED, "allocate unreadable map fixture");
    if (guard != MAP_FAILED) {
        check(!coui::looksLikeConfigMap(guard), "unreadable map header refused without memcpy");
        munmap(guard, page);
    }
}

void testDisabledStartupStillHasMenu() {
    writeFile(configPath(), "{\"enabled\":false,\"defaultScreenValue\":false,\"screens\":{}}");
    coui::reloadConfig();
    coui::OreUIConfigMap configs;
    for (const char* route : {"/main-menu", "/settings"}) {
        configs[route] = {nullptr, nullptr, [] { return true; }, [] { return false; }};
    }
    check(coui::applyConfigToMap(configs) == 0, "disabled application changes no entries");
    for (const auto& entry : configs) {
        check(entry.second.mUnknown3() && !entry.second.mUnknown4(), "disabled predicates untouched");
    }
    check(gMenuCalls == 1 && gMenu.size() == 1, "disabled startup registers the menu once");
    if (gMenu.empty() || gMenu[0].children.size() != 3) {
        check(false, "disabled menu has global switches and per-screen entries");
        return;
    }
    MenuSnapshot& enabled = gMenu[0].children[0];
    check(!enabled.selected(enabled.user), "master switch displays disabled state");
    check(gMenu[0].children[2].children.size() == 2, "disabled startup retains the screen list");
    enabled.click(enabled.user);
    check(coui::currentConfig().enabled, "menu can re-enable the mod");
    check(contains(readFile(configPath()), "\"enabled\": true"), "re-enabled state persisted");
    for (const auto& entry : configs) {
        check(entry.second.mUnknown3() && !entry.second.mUnknown4(), "menu does not pretend to hot-reload predicates");
    }
    check(coui::applyConfigToMap(configs) == 2, "next registration applies enabled config");
    check(gMenuCalls == 1, "another registration does not append a duplicate menu");
    for (const auto& entry : configs) {
        check(!entry.second.mUnknown3() && !entry.second.mUnknown4(), "both enabled predicates follow legacy default");
    }
}

}  // namespace

int main() {
    setup();

    printf("== config rules ==\n");
    testDefaultValueWins();
    testOverrideBeatsDefault();
    testDisabledIgnoresEverything();
    testRedundantOverridesAreIgnored();
    testWritingDefaultRemovesEntry();
    testChangingDefaultPrunesOverrides();
    testMasterSwitchPersists();
    testMalformedConfigFallsBack();
    testMapValidation();
    testDisabledStartupStillHasMenu();

    teardown();

    printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
