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
    size_t predicatePairCount = 0;
    size_t mapCallCount = 0;       // repeated direct calls with the original map as receiver
    bool registrationShape = false;
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
// constants, constructs paired predicates in the game ABI, and repeatedly passes
// the original first argument to registration calls. Insufficient or ambiguous
// evidence is refused instead of falling back to an unrelated route helper.
bool discoverOreUiConfigFunction(const ElfImage& image, const FunctionTable& functions,
                                 DiscoveryResult& out, std::string& error);

// Exposed for the self-test harness.
bool looksLikeOreUiRoute(const char* text, size_t length);

// True for a single-segment screen route such as "/main-menu" or "/settings".
// Nested routes ("/gameplay/death") are in-world UI, not config-map keys.
bool isTopLevelRoute(const std::string& route);
int scorePrologue(const uint8_t* code, size_t maxLen, size_t* prologueLen = nullptr);

// Exposed for the self-test harness: runs the registration-structure check over
// the instruction stream at `start`. `routes` maps absolute string addresses to
// their text, exactly as the scanner collects them. Compiled only for host tests
// (tests/run_tests.sh), never into the released module.
#if defined(CLOSEOREUI_TESTING)
bool verifyRegistrationShape(const ElfImage& image, uintptr_t start, size_t length,
                             const std::vector<std::pair<uintptr_t, std::string>>& routes,
                             CandidateReport& report);
#endif

}  // namespace coui
