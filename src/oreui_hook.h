#pragma once

#include <cstdint>
#include <string>

namespace coui {

// Installs the inline hook on the OreUI config-registration function and applies
// the config to the map it builds.
//
// The hook runs the original function body first (so the game fills its map
// exactly as it normally would, including any internal bookkeeping), then
// rewrites the predicates of every entry before returning.
bool installConfigHook(uintptr_t functionAddress, uintptr_t functionEnd, std::string& error);

}  // namespace coui
