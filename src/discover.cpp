#include "discover.h"

#include "log.h"
#include "target.h"
#include "x64_decoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coui {
namespace {

constexpr size_t kMinRouteLength = 4;
constexpr size_t kMaxRouteLength = 120;
constexpr size_t kMinRegistrationRoutes = 6;
constexpr size_t kMinRouteLead = 2;
constexpr size_t kMaxRegistrationSize = 1024 * 1024;

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

unsigned regOperand(const InsnInfo& info) {
    return info.modrmReg | ((info.rex & 4) ? 8 : 0);
}

unsigned rmOperand(const InsnInfo& info) {
    return info.modrmRm | ((info.rex & 1) ? 8 : 0);
}

// Only MOV propagates map provenance. For other instructions, discard encoded
// register fields even if they are read-only operands or opcode extensions. This
// intentionally loses some evidence instead of emulating x86 write effects.
uint16_t referencedRegisters(const InsnInfo& info) {
    auto bit = [](unsigned reg) { return static_cast<uint16_t>(1u << reg); };
    const uint8_t op = info.opcode;
    const uint8_t op2 = info.opcode2;
    uint16_t mask = 0;
    // 0F 1E /r (ENDBR64/ENDBR32 with the F3 prefix) and 0F 1F /r are NOP hints:
    // their modrm is an encoding detail, not a register operand. Treating it as
    // one would discard %rdi at a CET-enabled function entry, which is exactly
    // where the map arrives.
    if (info.opcode == 0x0F && (info.opcode2 == 0x1E || info.opcode2 == 0x1F)) {
        return 0;
    }
    if (info.hasModrm) {
        mask = bit(regOperand(info));
        if (info.modrmMod == 3) mask |= bit(rmOperand(info));
        if ((op == 0xF6 || op == 0xF7) && info.modrmReg >= 4) mask |= bit(0) | bit(2);
        if (op == 0x0F && (op2 == 0xB0 || op2 == 0xB1)) mask |= bit(0);
    } else if ((op != 0x0F && ((op >= 0xB0 && op <= 0xBF) || (op >= 0x58 && op <= 0x5F))) ||
               (op == 0x0F && op2 >= 0xC8 && op2 <= 0xCF)) {
        mask = bit(((op == 0x0F ? op2 : op) & 7) | ((info.rex & 1) ? 8 : 0));
    } else if (op != 0x0F && op >= 0x90 && op <= 0x97) {
        mask = bit(0) | bit((op & 7) | ((info.rex & 1) ? 8 : 0));
    } else if (info.isBranch || info.isReturn || (op >= 0x50 && op <= 0x57) || op == 0x68 ||
               op == 0x6A || op == 0x9C || op == 0x9D || op == 0xF8 || op == 0xF9 ||
               op == 0xFC || op == 0xFD || (op == 0x0F && op2 == 0x77)) {
        return 0;
    } else {
        mask = 0xFC7; // implicit effects: discard System V caller-saved registers
        if (op == 0x0F && op2 == 0xA2) mask |= bit(3); // CPUID also writes %rbx
    }
    // No REX: fields 4..7 may mean AH/CH/DH/BH. Discard both interpretations.
    return info.rex == 0 ? mask | ((mask & 0xF0) >> 4) : mask;
}

int memoryBase(const InsnInfo& info) {
    if (!info.hasModrm || info.modrmMod == 3 || info.ripRelative || info.address32) return -1;
    if (!info.hasSib) return static_cast<int>(rmOperand(info));
    if (((info.sib >> 3) & 7) != 4 || (info.rex & 2) ||
        (info.modrmMod == 0 && (info.sib & 7) == 5)) return -1;
    return (info.sib & 7) | ((info.rex & 1) ? 8 : 0);
}

int64_t displacement(const uint8_t* code, const InsnInfo& info) {
    if (info.dispSize == 1) return static_cast<int8_t>(code[info.dispOffset]);
    int32_t value = 0;
    if (info.dispSize == 4) memcpy(&value, code + info.dispOffset, sizeof(value));
    return value;
}

// A second, independent check after route ranking. Decode from the actual entry
// to verify instruction boundaries, paired predicate construction, and repeated
// map-receiver calls. A helper that merely names routes cannot satisfy this.
bool checkRegistrationShape(const ElfImage& image, uintptr_t start, size_t length,
                            const std::unordered_map<uintptr_t, uint32_t>& routeAt,
                            const std::vector<std::pair<uintptr_t, std::string>>& routes,
                            CandidateReport& report) {
    if (length == 0 || length > kMaxRegistrationSize) return false;
    const auto* code = reinterpret_cast<const uint8_t*>(start);
    std::array<bool, 16> mapRegister{};
    mapRegister[7] = true; // original %rdi
    std::array<std::unordered_set<int64_t>, 2> mapSlots; // %rsp and %rbp saves
    std::unordered_map<uintptr_t, size_t> mapCalls;
    std::unordered_set<std::string> routeNames;
    struct PredicateStore {
        size_t offset;
        int base;
        int64_t displacement;
        uintptr_t table;
    };
    PredicateStore previous{};
    bool havePrevious = false;
    size_t pairs = 0;

    auto clobberCall = [&] {
        for (unsigned reg : {0u, 1u, 2u, 6u, 7u, 8u, 9u, 10u, 11u}) mapRegister[reg] = false;
    };
    auto forgetMemory = [&](int base, int64_t disp, size_t width = 8) {
        if (base != 4 && base != 5) return;
        auto& slots = mapSlots[base - 4];
        for (auto it = slots.begin(); it != slots.end();) {
            if (*it >= disp - 7 && *it < disp + static_cast<int64_t>(width)) it = slots.erase(it);
            else ++it;
        }
    };
    // Writing %rsp or %rbp retires the slots recorded against it: the same
    // displacement no longer names the same word. Without this, `mov %rsp,%rbp`
    // or `pop %rbp` leaves stale slots that make an unrelated helper look like a
    // registration function.
    auto retireStackBase = [&](unsigned writtenReg) {
        if (writtenReg == 4 || writtenReg == 5) mapSlots[writtenReg - 4].clear();
    };

    for (size_t offset = 0; offset < length;) {
        const InsnInfo info = decode(code + offset, length - offset);
        if (!info.valid) return false;
        const unsigned reg = regOperand(info);
        const unsigned rm = rmOperand(info);
        const int base = memoryBase(info);
        const int64_t disp = displacement(code + offset, info);

        if (info.isRipLea() && info.rexW) {
            int32_t relative = 0;
            memcpy(&relative, code + offset + info.ripDispOffset, sizeof(relative));
            const uintptr_t target = start + offset + info.len + static_cast<intptr_t>(relative);
            auto route = routeAt.find(target);
            if (route != routeAt.end()) routeNames.insert(routes[route->second].second);

            // A predicate vtable LEA is immediately stored into its object; the
            // second predicate occupies the next 48-byte std::function slot.
            if (image.containsAddress(target, PF_R | PF_W) && offset + info.len < length) {
                const size_t nextOffset = offset + info.len;
                const InsnInfo next = decode(code + nextOffset, length - nextOffset);
                const int nextBase = memoryBase(next);
                if (next.valid && next.opcode == 0x89 && next.opcode2 == 0 && next.rexW &&
                    regOperand(next) == reg && nextBase >= 0) {
                    const int64_t nextDisp = displacement(code + nextOffset, next);
                    if (havePrevious && nextOffset - previous.offset <= 96 &&
                        previous.base == nextBase && previous.table != target &&
                        nextDisp == previous.displacement + static_cast<int64_t>(kGamePredicateSize)) {
                        ++pairs;
                    }
                    previous = {nextOffset, nextBase, nextDisp, target};
                    havePrevious = true;
                }
            }
        }

        if (info.isCall) {
            if (mapRegister[7] && info.relSize == 4) {
                int32_t relative = 0;
                memcpy(&relative, code + offset + info.relOffset, sizeof(relative));
                uintptr_t target = start + offset + info.len + static_cast<intptr_t>(relative);
                if (image.containsAddress(target, PF_X)) ++mapCalls[target];
            }
            clobberCall();
        } else if (info.opcode2 == 0 && info.opcode == 0x89 && info.rexW) {
            if (info.modrmMod == 3) {
                mapRegister[rm] = mapRegister[reg];
                retireStackBase(rm);
            } else {
                forgetMemory(base, disp);
                if (mapRegister[reg] && (base == 4 || base == 5)) {
                    mapSlots[base - 4].insert(disp);
                }
            }
        } else if (info.opcode2 == 0 && info.opcode == 0x8B && info.rexW) {
            mapRegister[reg] = info.modrmMod == 3 ? mapRegister[rm] :
                    ((base == 4 || base == 5) && mapSlots[base - 4].count(disp) != 0);
            retireStackBase(reg);
        } else if (info.opcode2 == 0 && info.opcode == 0x8D) {
            mapRegister[reg] = false;
            retireStackBase(reg);
        } else {
            const uint16_t affected = referencedRegisters(info);
            for (unsigned index = 0; index < mapRegister.size(); ++index) {
                if (affected & (1u << index)) mapRegister[index] = false;
            }
            // push/pop/enter/leave move %rsp, so an rsp-relative displacement
            // recorded earlier points somewhere else afterwards. A `pop` into
            // %rbp (or any register) additionally replaces that base.
            if (info.opcode2 == 0 &&
                ((info.opcode >= 0x50 && info.opcode <= 0x5F) || info.opcode == 0x68 ||
                 info.opcode == 0x6A || info.opcode == 0x9C || info.opcode == 0x9D ||
                 info.opcode == 0xC8 || info.opcode == 0xC9)) {
                mapSlots[0].clear();
                mapSlots[1].clear();
            }
            if (info.hasModrm && info.modrmMod != 3) {
                if ((info.opcode == 0x0F && info.opcode2 == 0xAE) ||
                    (info.opcode >= 0xD8 && info.opcode <= 0xDF) ||
                    (base < 0 && !info.ripRelative)) {
                    mapSlots[0].clear();
                    mapSlots[1].clear();
                } else {
                    // Scalar operands cover at most one word; only packed-SSE
                    // stores can cover two. A blanket 16-byte invalidation would
                    // erase the map save next to scalar locals even on a read.
                    const uint8_t op2 = info.opcode2;
                    const bool wideStore = info.opcode == 0x0F &&
                            (op2 == 0x11 || op2 == 0x13 || op2 == 0x17 || op2 == 0x29 ||
                             op2 == 0x2B || op2 == 0x7F || op2 == 0xE7 ||
                             op2 == 0x38 || op2 == 0x3A);
                    forgetMemory(base, disp, wideStore ? 16 : 8);
                }
            }
        }

        offset += info.len;
        if (info.isReturn) break; // do not count padding or exception-only tails
    }

    report.routeCount = routeNames.size();
    report.topLevelRouteCount = 0;
    for (const auto& route : routeNames) {
        if (isTopLevelRoute(route)) ++report.topLevelRouteCount;
    }
    report.predicatePairCount = pairs;
    report.mapCallCount = 0;
    for (const auto& call : mapCalls) report.mapCallCount = std::max(report.mapCallCount, call.second);
    return report.topLevelRouteCount >= kMinRegistrationRoutes &&
           pairs >= kMinRegistrationRoutes && report.mapCallCount >= kMinRegistrationRoutes;
}

}  // namespace

#if defined(CLOSEOREUI_TESTING)
bool verifyRegistrationShape(const ElfImage& image, uintptr_t start, size_t length,
                             const std::vector<std::pair<uintptr_t, std::string>>& routes,
                             CandidateReport& report) {
    std::unordered_map<uintptr_t, uint32_t> routeAt;
    for (uint32_t i = 0; i < routes.size(); ++i) routeAt.emplace(routes[i].first, i);
    return checkRegistrationShape(image, start, length, routeAt, routes, report);
}
#endif

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
    // No whitespace or format specifiers: those are log messages, not routes.
    for (size_t i = 1; i < length; ++i) {
        if (!isRouteChar(text[i])) {
            return false;
        }
    }
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
        InsnInfo info = decode(code + offset, maxLen - offset);
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
        size_t readable = stats.functionSize;
        if (addr < textStart || addr >= textStart + textSize) {
            continue;
        }
        readable = std::min(readable, textSize - static_cast<size_t>(addr - textStart));
        report.prologueScore = scorePrologue(reinterpret_cast<const uint8_t*>(addr),
                                             std::min<size_t>(readable, 64),
                                             &prologueLen);
        if (report.topLevelRouteCount >= kMinRegistrationRoutes) {
            report.registrationShape = checkRegistrationShape(image, addr, readable,
                                                               routeAt, routes, report);
        }

        candidates.push_back(std::move(report));
    }

    if (candidates.empty()) {
        error = "no candidate function is a real function entry";
        return false;
    }

    // Rank by number of distinct TOP-LEVEL screens. That is what the config map
    // holds, and it separates the registration function from helpers that happen
    // to touch many nested routes. Only independently validated registrations are
    // eligible; route count alone must never authorize an inline hook.
    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateReport& a, const CandidateReport& b) {
                  if (a.registrationShape != b.registrationShape) return a.registrationShape;
                  if (a.topLevelRouteCount != b.topLevelRouteCount) {
                      return a.topLevelRouteCount > b.topLevelRouteCount;
                  }
                  if (a.routeCount != b.routeCount) return a.routeCount > b.routeCount;
                  if (a.prologueScore != b.prologueScore) return a.prologueScore > b.prologueScore;
                  return a.functionSize > b.functionSize;
               });

    out.candidatesConsidered = candidates.size();
    out.topCandidates.assign(candidates.begin(),
                              candidates.begin() + std::min<size_t>(candidates.size(), 8));
    if (!candidates.front().registrationShape) {
        error = "no candidate has sufficient routes, paired predicates and map registration calls";
        return false;
    }
    if (candidates.size() > 1 && candidates[1].registrationShape &&
        candidates.front().topLevelRouteCount - candidates[1].topLevelRouteCount < kMinRouteLead) {
        error = "ambiguous OreUI registrations: top-level route lead is less than two";
        return false;
    }

    out.configFunction = candidates.front().address;
    out.configFunctionEnd =
            candidates.front().functionSize != 0
                    ? candidates.front().address + candidates.front().functionSize
                    : 0;
    out.routeCount = candidates.front().routeCount;
    out.topLevelRouteCount = candidates.front().topLevelRouteCount;
    out.prologueScore = candidates.front().prologueScore;
    return true;
}

}  // namespace coui
