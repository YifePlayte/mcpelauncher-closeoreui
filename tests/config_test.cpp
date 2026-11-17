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
#include <string>

#include <sys/stat.h>
#include <unistd.h>

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
        return {};
    }
    return out;
}

void writeFile(const std::string& path, const std::string& content) {
    std::string error;
    coui::writeFileAtomically(path, content, error);
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Points the config loader at a private directory so the tests never touch the
// user's real configuration.
void setup() {
    char tmpl[] = "/tmp/closeoreui-test-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (dir == nullptr) {
        printf("FAIL: cannot create temp dir\n");
        exit(1);
    }
    gRoot = dir;
    setenv("MCPELAUNCHER_DATA_DIR", gRoot.c_str(), 1);
}

void teardown() {
    if (gRoot.empty()) {
        return;
    }
    // Only the files this test created live here; a recursive remove of a known
    // temp path is safe.
    std::string cmd = "rm -rf '" + gRoot + "'";
    if (system(cmd.c_str()) != 0) {
        printf("warning: could not remove %s\n", gRoot.c_str());
    }
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

    teardown();

    printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
