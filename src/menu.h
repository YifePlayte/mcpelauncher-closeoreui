#pragma once

namespace coui {

// Registers CloseOreUI entries in mcpelauncher's Mods menu.
//
// The menu is an optional extra: it only exists when the launcher was built with
// USE_IMGUI, and it needs libmcpelauncher_menu.so to be loaded. When either is
// missing this degrades to a single log line and the config file remains the way
// to configure the mod.
//
// Call from mod_preinit: the menu has to be populated before the game's main loop
// builds it.
bool installMenu();

}  // namespace coui
