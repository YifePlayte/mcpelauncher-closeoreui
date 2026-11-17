#include "menu.h"

#include "log.h"
#include "oreui_config.h"

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include <dlfcn.h>

namespace coui {
namespace {

// ABI declared by mcpelauncher-client's imgui_ui.h.
//
// This is the only place the layout is assumed, so a mismatch shows up here rather
// than as corrupted menu state.
struct MenuEntryABI {
    const char* name;
    void* user;
    bool (*selected)(void* user);
    void (*click)(void* user);
    size_t length;
    MenuEntryABI* subentries;
};

using AddMenuFn = void (*)(size_t length, MenuEntryABI* entries);

AddMenuFn gAddMenu = nullptr;

// ---------------------------------------------------------------------------
// Lifetime
//
// mcpelauncher's convertEntries() copies each entry into its own MenuEntry but
// keeps two raw pointers from ours:
//
//   * `name`      — stored as std::string in the launcher, so the C string is only
//                   read during registration. Still kept static below to avoid
//                   reasoning about that.
//   * callbacks   — std::bind(selected, user), so `user` must stay valid for the
//                   life of the process.
//
// Everything the callbacks point at is therefore allocated once and never freed.
// ---------------------------------------------------------------------------

struct ScreenState {
    std::string route;
};

// Leaked on purpose: the launcher keeps pointers into it until the process exits.
//
// A deque, not a vector: push_back must not relocate existing elements, because
// mcpelauncher stores the route strings' c_str() pointers. Inside libc++ a
// short std::string lives in its own SSO buffer, so moving it would change the
// address and leave the menu pointing at freed memory.
std::deque<ScreenState>* gScreens = nullptr;

bool enabledSelected(void* /*user*/) {
    return currentConfig().enabled;
}

void enabledClicked(void* /*user*/) {
    setEnabled(!currentConfig().enabled);
    logInfo("menu: mod %s", currentConfig().enabled ? "enabled" : "disabled");
}

bool legacySelected(void* /*user*/) {
    // The tick means "this mod is doing its job", i.e. the legacy UI is forced.
    // defaultScreenValue is "use OreUI", so the tick is its negation.
    return !currentConfig().defaultScreenValue;
}

void legacyClicked(void* /*user*/) {
    setDefaultScreenValue(!currentConfig().defaultScreenValue);
    logInfo("menu: %s",
            currentConfig().defaultScreenValue ? "OreUI restored" : "legacy UI forced");
}

bool screenSelected(void* user) {
    auto* state = static_cast<ScreenState*>(user);
    // screenValue() resolves master switch, override and default in one place;
    // the tick shows the opposite because it means "legacy UI is active".
    return !screenValue(state->route);
}

void screenClicked(void* user) {
    auto* state = static_cast<ScreenState*>(user);

    // Flip the effective value. setScreenOverride() drops the entry instead of
    // storing it when the result equals the global default, so toggling a screen
    // back to the default cleans up config.json rather than leaving a redundant
    // line behind.
    const bool nextOreUi = !screenValue(state->route);
    setScreenOverride(state->route, nextOreUi);
    logInfo("menu: %s -> %s", state->route.c_str(), nextOreUi ? "OreUI" : "legacy UI");
}

// Submenu parents need real callbacks: the launcher std::bind()s them
// unconditionally, and binding a null pointer yields a truthy std::function that
// segfaults when invoked.
bool parentSelected(void* /*user*/) {
    return false;
}

void parentClicked(void* /*user*/) {}

// Resolves mcpelauncher_addmenu once. RTLD_DEFAULT works because the launcher
// exposes the symbol from its own executable; the fallback covers builds that load
// it as a separate library.
bool menuAvailable() {
    if (gAddMenu != nullptr) {
        return true;
    }
    gAddMenu = reinterpret_cast<AddMenuFn>(dlsym(RTLD_DEFAULT, "mcpelauncher_addmenu"));
    if (gAddMenu == nullptr) {
        void* handle = dlopen("libmcpelauncher_menu.so", RTLD_NOW);
        if (handle != nullptr) {
            gAddMenu = reinterpret_cast<AddMenuFn>(dlsym(handle, "mcpelauncher_addmenu"));
        }
    }
    return gAddMenu != nullptr;
}

}  // namespace

void installMenu(const std::vector<std::string>& screens) {
    if (!menuAvailable()) {
        logInfo("menu: not available (launcher built without USE_IMGUI); "
                "configure via %s",
                configFilePath().c_str());
        return;
    }

    // Registering twice would append a second copy of the whole tree, because
    // mcpelauncher's addmenu() appends rather than replaces. The module calls this
    // once per process, but guard anyway: a duplicate menu is confusing and a
    // second set of screen entries is pure waste.
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    // The launcher renders its menu with the game's Mojangles.ttf, which has no CJK
    // glyphs and is loaded without a custom glyph range, so every label here must
    // be ASCII or it comes out blank.
    static const char* kRoot = "CloseOreUI";
    static const char* kEnabled = "Mod enabled";
    static const char* kLegacy = "Force legacy UI (disable OreUI)";
    static const char* kScreens = "Per-screen overrides";

    // Owns the per-screen state the launcher will point at.
    //
    // Never cleared or reallocated: mcpelauncher keeps pointers to both the route
    // strings and the user objects for the life of the process, so shrinking this
    // would leave the menu holding dangling pointers. A repeated registration is
    // not expected (applyConfigToMap runs once, when the game builds its config
    // map), but appending is safe whereas clearing is not.
    if (gScreens == nullptr) {
        gScreens = new std::deque<ScreenState>();
    }

    // Leaf entries for individual screens. Built fresh each call, but the pointed-to
    // strings live in gScreens, so only this array is temporary — which is fine
    // because convertEntries() consumes it immediately.
    std::vector<MenuEntryABI> screenLeaves;
    screenLeaves.reserve(screens.size());
    for (const std::string& route : screens) {
        gScreens->push_back(ScreenState{route});
        ScreenState* state = &gScreens->back();
        screenLeaves.push_back(MenuEntryABI{
                state->route.c_str(), state, &screenSelected, &screenClicked, 0, nullptr});
    }

    // Fixed leaf items first, then the per-screen submenu.
    std::vector<MenuEntryABI> items;
    items.reserve(3);
    items.push_back(MenuEntryABI{kEnabled, nullptr, &enabledSelected, &enabledClicked, 0, nullptr});
    items.push_back(MenuEntryABI{kLegacy, nullptr, &legacySelected, &legacyClicked, 0, nullptr});
    if (!screenLeaves.empty()) {
        items.push_back(MenuEntryABI{kScreens, nullptr, &parentSelected, &parentClicked,
                                     screenLeaves.size(), screenLeaves.data()});
    }

    MenuEntryABI root[1];
    root[0] = MenuEntryABI{kRoot, nullptr, &parentSelected, &parentClicked, items.size(),
                           items.data()};

    gAddMenu(1, root);
    logInfo("menu: registered 'Mods > %s' (%zu items, %zu screens)", kRoot, items.size(),
            screenLeaves.size());
}

}  // namespace coui
