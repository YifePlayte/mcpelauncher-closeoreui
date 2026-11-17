#include "menu.h"

#include "log.h"
#include "oreui_config.h"

#include <cstddef>
#include <vector>

#include <dlfcn.h>

namespace coui {
namespace {

// ABI declared by mcpelauncher-client's imgui_ui.h.
//
// This is the only place the layout is assumed, so a mismatch shows up here
// rather than as corrupted menu state.
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

// The launcher copies the entry array immediately (it std::bind()s the callbacks
// and keeps only the name pointer), so the array may live on the stack. The name
// strings, however, are retained by pointer and must outlive this function.
//
// gUser is held by reference through std::bind, so it must be a stable object.
struct MenuState {
    int screenCount = 0;
};

MenuState gMenuState;

// Master switch: `enabled` turns the whole mod on and off. When it is off,
// screenValue() reports OreUI for every screen, i.e. the game behaves as if the
// mod were not installed.
bool masterSelected(void* /*user*/) {
    return currentConfig().enabled;
}

void masterClicked(void* /*user*/) {
    setEnabled(!currentConfig().enabled);
    logInfo("menu: mod %s", currentConfig().enabled ? "enabled" : "disabled");
}

bool reloadSelected(void* /*user*/) {
    return false;  // never shows a tick
}

void reloadClicked(void* /*user*/) {
    reloadConfig();
    logInfo("menu: config reloaded from %s", configFilePath().c_str());
}

// Submenu parents still need real callbacks: the launcher std::bind()s them
// unconditionally, and binding a null pointer yields a truthy std::function that
// segfaults if it is ever invoked.
bool parentSelected(void* /*user*/) {
    return false;
}

void parentClicked(void* /*user*/) {
    // A submenu parent is never invoked as a leaf, but the callback must be real.
}

}  // namespace

bool installMenu() {
    // RTLD_DEFAULT works because the launcher exposes the symbol from its own
    // executable; the fallback covers builds that load it as a library.
    gAddMenu = reinterpret_cast<AddMenuFn>(dlsym(RTLD_DEFAULT, "mcpelauncher_addmenu"));
    if (gAddMenu == nullptr) {
        void* handle = dlopen("libmcpelauncher_menu.so", RTLD_NOW);
        if (handle != nullptr) {
            gAddMenu = reinterpret_cast<AddMenuFn>(dlsym(handle, "mcpelauncher_addmenu"));
        }
    }
    if (gAddMenu == nullptr) {
        logInfo("menu: not available (launcher built without USE_IMGUI); "
                "configure via %s",
                configFilePath().c_str());
        return false;
    }

    // Names must be static: the launcher keeps the pointers.
    //
    // These are ASCII on purpose. The launcher renders its menu with the game's
    // Mojangles.ttf, which has no CJK glyphs and is loaded without a custom glyph
    // range, so non-ASCII text comes out blank.
    static const char* kRoot = "CloseOreUI";
    static const char* kMaster = "Use legacy UI (disable OreUI)";
    static const char* kReload = "Reload config.json";

    MenuEntryABI entries[2];
    entries[0] = MenuEntryABI{kMaster, &gMenuState, &masterSelected, &masterClicked, 0, nullptr};
    entries[1] = MenuEntryABI{kReload, &gMenuState, &reloadSelected, &reloadClicked, 0, nullptr};

    // The launcher's convertEntries() does std::bind(entry.selected, entry.user)
    // unconditionally. Binding a null function pointer succeeds and yields a
    // truthy std::function that segfaults when called, so a submenu parent must
    // not leave these null — even though appendMenu() currently only calls them
    // for leaf entries. Give the parent real callbacks that do nothing.
    MenuEntryABI root[1];
    root[0] = MenuEntryABI{kRoot, &gMenuState, &parentSelected, &parentClicked, 2, entries};

    gAddMenu(1, root);
    logInfo("menu: registered 'Mods > %s'", kRoot);
    return true;
}

}  // namespace coui
