#pragma once

#include "eh_frame.h"
#include "elf_image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coui {

struct CandidateReport {
    uintptr_t address = 0;
    size_t routeCount = 0;         // distinct OreUI route strings referenced
    size_t topLevelRouteCount = 0; // subset of those that are top-level screens
    size_t referenceCount = 0;     // total LEA references to those strings
    size_t functionSize = 0;
    int prologueScore = 0;
    std::vector<std::string> samples;
};

struct DiscoveryResult {
    uintptr_t configFunction = 0;
    uintptr_t configFunctionEnd = 0;  // exclusive; bounds the inline hook patch
    size_t routeCount = 0;
    size_t topLevelRouteCount = 0;
    int prologueScore = 0;
    size_t candidatesConsidered = 0;
    std::vector<CandidateReport> topCandidates;
};

// Find the OreUI config-registration function.
//
// The function has no exported symbol, so it is located structurally: it is the
// function that takes the address of the largest number of OreUI route string
// constants ("/oreui-marketplace/search", "/main-menu", ...) because those
// strings are exactly the keys it inserts into the config map.
bool discoverOreUiConfigFunction(const ElfImage& image, const FunctionTable& functions,
                                 DiscoveryResult& out, std::string& error);

// Exposed for the self-test harness.
bool looksLikeOreUiRoute(const char* text, size_t length);

// True for a single-segment screen route such as "/main-menu" or "/settings".
// Nested routes ("/gameplay/death") are in-world UI, not config-map keys.
bool isTopLevelRoute(const std::string& route);
int scorePrologue(const uint8_t* code, size_t maxLen, size_t* prologueLen = nullptr);

}  // namespace coui
