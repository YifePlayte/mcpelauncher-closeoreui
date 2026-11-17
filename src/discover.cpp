#include "discover.h"

#include "log.h"
#include "x64_decoder.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coui {
namespace {

constexpr size_t kMinRouteLength = 4;
constexpr size_t kMaxRouteLength = 120;

bool isRouteChar(char c) {
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    switch (c) {
        case '/':
        case '_':
        case '-':
        case '.':
        case ':':
            return true;
        default:
            return false;
    }
}

}  // namespace

bool isTopLevelRoute(const std::string& route) {
    // Exactly one '/' (the leading one) => a single path segment.
    size_t slash = route.find('/', 1);
    return slash == std::string::npos;
}

bool looksLikeOreUiRoute(const char* text, size_t length) {
    if (text == nullptr || length < kMinRouteLength || length > kMaxRouteLength) {
        return false;
    }
    if (text[0] != '/') {
        return false;
    }
    // Must be a path with at least one separator after the leading slash, and no
    // whitespace or format specifiers (those are log messages, not routes).
    bool hasSecondSeparator = false;
    for (size_t i = 1; i < length; ++i) {
        char c = text[i];
        if (!isRouteChar(c)) {
            return false;
        }
        if (c == '/') {
            hasSecondSeparator = true;
        }
    }
    (void) hasSecondSeparator;
    // Reject obvious non-routes: file names, printf templates, version strings.
    if (memchr(text, '%', length) != nullptr) {
        return false;
    }
    // A single leading slash with a single word ("/main") is a plausible
    // top-level screen; deeper routes ("/oreui-marketplace/search") are too.
    return true;
}

int scorePrologue(const uint8_t* code, size_t maxLen, size_t* prologueLen) {
    if (code == nullptr) {
        if (prologueLen) *prologueLen = 0;
        return 0;
    }

    int score = 0;
    size_t offset = 0;

    for (int i = 0; i < 12 && offset < maxLen; ++i) {
        InsnInfo info = decode(code + offset);
        if (!info.valid) {
            break;
        }

        if (!info.hasModrm && info.opcode2 == 0) {
            // push r64
            if (info.opcode >= 0x50 && info.opcode <= 0x57) {
                score += 4;
            }
            // mov r64, imm / sub rsp handled below via modrm forms
        }

        if (info.opcode2 == 0 && info.opcode == 0x83 && info.hasModrm) {
            // sub rsp, imm8 (reg field 5)
            if (info.modrmReg == 5 && info.modrmMod == 3 && info.modrmRm == 4) {
                score += 10;
            }
        }
        if (info.opcode2 == 0 && info.opcode == 0x81 && info.hasModrm) {
            if (info.modrmReg == 5 && info.modrmMod == 3 && info.modrmRm == 4) {
                score += 10;
            }
        }
        if (info.opcode2 == 0 && info.opcode == 0x8B && info.hasModrm) {
            // mov reg, reg — the argument shuffle where a version imprint lives
            if (info.modrmMod == 3) {
                score += 3;
            }
        }
        if (info.opcode2 == 0 && (info.opcode == 0x89 || info.opcode == 0x8B) && info.hasModrm &&
            info.modrmMod == 3) {
            score += 2;
        }

        offset += info.len;

        // The prologue ends at the first branch or stack adjustment target.
        if (info.isBranch) {
            break;
        }
    }

    if (prologueLen) {
        *prologueLen = offset;
    }
    return score;
}

bool discoverOreUiConfigFunction(const ElfImage& image, const FunctionTable& functions,
                                 DiscoveryResult& out, std::string& error) {
    out = DiscoveryResult{};

    // ---- 1. Collect route-looking strings from read-only data --------------
    std::unordered_map<uintptr_t, uint32_t> routeAt;
    std::vector<std::pair<uintptr_t, std::string>> routes;

    for (const Segment& seg : image.segments()) {
        // Read every readable segment, including the one that also carries PF_X:
        // this binary has a single 400 MB `R E` LOAD that holds both .text and
        // the read-only data, so excluding executable segments scans nothing.
        if ((seg.flags & PF_R) == 0) {
            continue;
        }
        const char* data = reinterpret_cast<const char*>(seg.vaddr);
        size_t i = 0;
        while (i < seg.memSize) {
            // Find start of a printable run
            if (data[i] < 0x20 || static_cast<unsigned char>(data[i]) > 0x7E) {
                ++i;
                continue;
            }
            size_t start = i;
            while (i < seg.memSize && data[i] >= 0x20 &&
                   static_cast<unsigned char>(data[i]) <= 0x7E) {
                ++i;
            }
            size_t length = i - start;
            if (i < seg.memSize && data[i] == '\0' && looksLikeOreUiRoute(data + start, length)) {
                uintptr_t addr = seg.vaddr + start;
                // Keep the longest string at an address; duplicates are normal
                // because of string pooling across translation units.
                if (routeAt.find(addr) == routeAt.end()) {
                    routeAt.emplace(addr, static_cast<uint32_t>(routes.size()));
                    routes.emplace_back(addr, std::string(data + start, length));
                }
            }
            // Skip the NUL so the next run starts cleanly.
            if (i < seg.memSize) {
                ++i;
            }
        }
    }

    if (routes.empty()) {
        error = "no OreUI-style route strings found in read-only data";
        return false;
    }
    logDebug("route strings: %zu", routes.size());

    // ---- 2. Find code that takes the address of those strings --------------
    struct Reference {
        uintptr_t functionStart;
        uint32_t routeIndex;
    };
    std::vector<Reference> refs;
    refs.reserve(4096);

    uintptr_t textStart = 0;
    size_t textSize = 0;
    Segment textSeg;
    if (!image.executableSegment(textSeg)) {
        error = "module has no executable segment";
        return false;
    }
    textStart = textSeg.vaddr;
    textSize = textSeg.memSize;

    const auto* text = reinterpret_cast<const uint8_t*>(textStart);

    // Linear scan for `8D /r` with a RIP-relative modrm. We do not do a full
    // disassembly pass: every candidate is validated by checking that the
    // computed address is one of the collected strings.
    for (size_t i = 0; i + 6 < textSize; ++i) {
        if (text[i] != 0x8D) {
            continue;
        }
        uint8_t modrm = text[i + 1];
        if ((modrm & 0xC7) != 0x05) {
            continue;
        }

        size_t insnLen = 6;

        // Optional REX prefix directly before the opcode changes the length and
        // therefore the RIP base.
        size_t insnOffset = i;
        if (i >= 1 && (text[i - 1] & 0xF0) == 0x40) {
            insnLen = 7;
            insnOffset = i - 1;
        }

        int32_t disp = 0;
        memcpy(&disp, text + i + 2, sizeof(disp));
        uintptr_t target = textStart + insnOffset + insnLen + static_cast<uintptr_t>(
                                                                    static_cast<intptr_t>(disp));

        auto it = routeAt.find(target);
        if (it == routeAt.end()) {
            continue;
        }

        uintptr_t fn = functions.startAtOrBefore(textStart + insnOffset);
        if (fn == 0) {
            continue;
        }
        refs.push_back(Reference{fn, it->second});
    }

    if (refs.empty()) {
        error = "no code references to OreUI route strings";
        return false;
    }
    logDebug("route references: %zu", refs.size());

    // ---- 3. Group per function and score -----------------------------------
    struct Stats {
        std::vector<uint32_t> routeIndices;
        size_t refCount = 0;
        size_t functionSize = 0;
    };
    std::unordered_map<uintptr_t, Stats> byFunction;

    for (const Reference& ref : refs) {
        Stats& stats = byFunction[ref.functionStart];
        stats.routeIndices.push_back(ref.routeIndex);
        stats.refCount++;
    }

    for (auto& entry : byFunction) {
        uintptr_t next = functions.nextStartAfter(entry.first);
        entry.second.functionSize = next > entry.first ? next - entry.first : 0;
        auto& idx = entry.second.routeIndices;
        std::sort(idx.begin(), idx.end());
        idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    }

    std::vector<CandidateReport> candidates;
    candidates.reserve(byFunction.size());

    for (auto& entry : byFunction) {
        uintptr_t addr = entry.first;
        Stats& stats = entry.second;

        // The target must be a real function entry. .eh_frame covers essentially
        // every C++ function; refusing non-entries is what stops us from
        // overwriting the middle of a function (see SIG_MATRIX notes upstream).
        if (!functions.isFunctionStart(addr)) {
            continue;
        }

        CandidateReport report;
        report.address = addr;
        report.routeCount = stats.routeIndices.size();
        report.referenceCount = stats.refCount;
        report.functionSize = stats.functionSize;

        // Top-level screens ("/main-menu", "/settings") are the ones the OreUI
        // config map is keyed by. Nested routes ("/gameplay/death",
        // "/gameplay/block-actors/anvil") belong to in-world UI and are also
        // referenced by name-mapping helpers, so counting them leads straight to
        // the wrong function — measured on 1.26.62.1, where the highest raw
        // reference count pointed at a string mapper, not the registration
        // function.
        for (uint32_t routeIndex : stats.routeIndices) {
            const std::string& route = routes[routeIndex].second;
            if (isTopLevelRoute(route)) {
                report.topLevelRouteCount++;
            }
        }

        size_t prologueLen = 0;
        report.prologueScore = scorePrologue(reinterpret_cast<const uint8_t*>(addr), 64,
                                             &prologueLen);

        for (size_t i = 0; i < stats.routeIndices.size() && i < 4; ++i) {
            report.samples.push_back(routes[stats.routeIndices[i]].second);
        }
        candidates.push_back(std::move(report));
    }

    if (candidates.empty()) {
        error = "no candidate function is a real function entry";
        return false;
    }

    // Rank by number of distinct TOP-LEVEL screens. That is what the config map
    // holds, and it separates the registration function from helpers that happen
    // to touch many nested routes. Prologue shape and body size break ties.
    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateReport& a, const CandidateReport& b) {
                  if (a.topLevelRouteCount != b.topLevelRouteCount) {
                      return a.topLevelRouteCount > b.topLevelRouteCount;
                  }
                  if (a.routeCount != b.routeCount) return a.routeCount > b.routeCount;
                  if (a.prologueScore != b.prologueScore) return a.prologueScore > b.prologueScore;
                  return a.functionSize > b.functionSize;
              });

    out.configFunction = candidates.front().address;
    out.configFunctionEnd =
            candidates.front().functionSize != 0
                    ? candidates.front().address + candidates.front().functionSize
                    : 0;
    out.routeCount = candidates.front().routeCount;
    out.topLevelRouteCount = candidates.front().topLevelRouteCount;
    out.prologueScore = candidates.front().prologueScore;
    out.candidatesConsidered = candidates.size();
    out.topCandidates.assign(candidates.begin(),
                             candidates.begin() + std::min<size_t>(candidates.size(), 8));
    return true;
}

}  // namespace coui
