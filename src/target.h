#pragma once

#include <cstddef>

// Build-time target declaration.
//
// This project has exactly one build target: the mcpelauncher mod ABI. Every
// constant here describes that ABI, so that adapting to a different launcher is
// a deliberate edit rather than an accidental assumption.

namespace coui {

// Mod entry points. mcpelauncher's ModLoader dlsym()s these on every .so in the
// mods directory: mod_preinit before the game library is loaded, mod_init after.
constexpr const char* kModPreinitSymbol = "mod_preinit";
constexpr const char* kModInitSymbol = "mod_init";

// The game library this project knows how to locate.
constexpr const char* kGameLibrary = "libminecraftpe.so";

// ABI directory name mcpelauncher uses inside a version directory, i.e.
// <version>/lib/<abi>/libminecraftpe.so. Kept in sync with PathHelper::getAbiDir.
constexpr const char* kSupportedAbi = "x86_64";

// libc++ ABI v1 stores each bool predicate in a 48-byte std::function. This is
// part of the game entry ABI, not the host test compiler's standard-library ABI.
constexpr size_t kGamePredicateSize = 48;

// The launcher loads liblog/libdl/libc/libc++ from its own bionic linker, so the
// mod is an Android shared library even when the host is a desktop Linux.
constexpr const char* kPlatform = "linux-x86_64";

}  // namespace coui
