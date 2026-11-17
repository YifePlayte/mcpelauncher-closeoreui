#pragma once

#include <string>
#include <vector>

namespace coui {

// Registers CloseOreUI entries in mcpelauncher's Mods menu.
//
// Must be called from mod_init, once the OreUI config function has been located:
// the per-screen submenu needs the list of routes the game actually registered,
// and mcpelauncher's addmenu() appends a *separate top-level menu* on every call,
// so the whole tree has to be registered in one go.
//
// `screens` is the list of registered routes, e.g. "/main-menu". Passing an empty
// list registers only the two global switches.
//
// The menu is optional: it needs the launcher to be built with USE_IMGUI and to
// expose libmcpelauncher_menu.so. When unavailable this logs once and the config
// file remains the way to configure the mod.
void installMenu(const std::vector<std::string>& screens);

}  // namespace coui
