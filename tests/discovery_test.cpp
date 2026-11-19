// End-to-end test for the discovery pipeline, run against a purpose-built binary
// that mimics the shape of the game's OreUI registration function: a large
// function that references many route strings, surrounded by decoys that
// reference only one or two, plus a tiny helper that references many.
//
// Build and run via tests/run_tests.sh.

#include "discover.h"
#include "eh_frame.h"
#include "elf_image.h"
#include "log.h"
#include "target.h"

#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <string>

#include <link.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

// --- fixtures --------------------------------------------------------------

// Decoys are marked noinline and reference a single route each.
//
// Route constants have to survive optimisation, so they are read through a
// volatile sink.

volatile int gSink = 0;

// Host std::function layouts differ from the Android libc++ target. These marker
// objects deliberately model the game's paired 48-byte predicate construction,
// while opaqueEmplace models calls whose first argument is the original map.
extern "C" {
volatile uint64_t gPredicateOne[8] = {1};
volatile uint64_t gPredicateTwo[8] = {2};
}

__attribute__((always_inline)) inline void constructPredicates(void* object) {
    asm volatile("lea gPredicateOne(%%rip), %%rax\n\t"
                 "mov %%rax, (%[object])\n\t"
                 "lea gPredicateTwo(%%rip), %%rax\n\t"
                 "mov %%rax, %c[stride](%[object])"
                 : : [object] "r" (object), [stride] "i" (coui::kGamePredicateSize)
                 : "rax", "memory");
}

__attribute__((noinline)) void opaqueEmplace(void* map, const char* route, void* predicates) {
    gSink = (map != nullptr) + route[0] +
            static_cast<int>(*static_cast<const uint64_t*>(predicates) & 1);
}

#define REGISTER_ROUTE(route) do { \
    constructPredicates(predicates); \
    opaqueEmplace(map, route, predicates); \
} while (false)

// Opaque sink: keeps each string address alive without letting the optimiser
// constant-fold strlen() into an immediate, which would erase the references the
// scanner is supposed to find.
__attribute__((noinline)) void touch(const char* text) {
    gSink += (int) text[0];
    gSink += (int) text[1];
}

__attribute__((noinline)) void decoySearch() {
    touch("/oreui-marketplace/search");
}
__attribute__((noinline)) void decoyWishlist() {
    touch("/oreui-marketplace/wishlist");
}
__attribute__((noinline)) void decoyPlayerPermissions() {
    touch("/player-permissions");
}
__attribute__((noinline)) void decoySendInvites() {
    touch("/send-invites");
}
__attribute__((noinline)) void decoyBedtime() {
    touch("/gameplay/bedtime");
}
__attribute__((noinline)) void decoyPreStart() {
    touch("/pre-start");
}

// A small helper that touches just as many routes as the real function. The
// upstream matrix warns that raw reference count is misleading; this fixture
// checks that we do not simply pick the densest one.
__attribute__((noinline)) void denseHelper() {
    touch("/oreui-marketplace/search");
    touch("/oreui-marketplace/wishlist");
    touch("/player-permissions");
    touch("/send-invites");
    touch("/gameplay/bedtime");
    touch("/pre-start");
    touch("/start-from-template");
}

// The "real" function: a genuine prologue (stack frame, callee-saved registers,
// stack protector) and every route referenced directly, which is the shape the
// game's binary has. An array of pointers would defeat the test: the compiler
// emits one lea to the array instead of one per string.
__attribute__((noinline)) void oreUiRegister(void* map) {
    alignas(16) uint8_t predicates[2 * coui::kGamePredicateSize];
#if defined(DISCOVERY_POINTER_TABLE)
    static const char* volatile routes[] = {
        "/main-menu", "/play", "/settings", "/create-new-world", "/edit-world",
        "/send-invites", "/player-permissions", "/safezone", "/pre-start", "/world-templates"
    };
    for (auto& route : routes) {
        REGISTER_ROUTE(route);
    }
#else
    REGISTER_ROUTE("/oreui-marketplace/search");
    REGISTER_ROUTE("/oreui-marketplace/wishlist");
    REGISTER_ROUTE("/start-from-template");
    REGISTER_ROUTE("/create-new-world");
    REGISTER_ROUTE("/player-permissions");
    REGISTER_ROUTE("/send-invites");
    REGISTER_ROUTE("/gameplay/bedtime");
    REGISTER_ROUTE("/pre-start");
    REGISTER_ROUTE("/main-menu");
    REGISTER_ROUTE("/settings/general");
    REGISTER_ROUTE("/world-templates");
    REGISTER_ROUTE("/realms/join");
#endif
}

__attribute__((noinline)) void unrelatedRealmsHelper() {
    touch("/realms-create");
    touch("/realms-join");
    touch("/realms-settings");
    touch("/realms-leave");
    touch("/realms-invites");
    touch("/realms-backup");
    touch("/realms-subscription");
}

// Paired callbacks and lots of routes alone are also insufficient: this helper
// passes a route string, not the original map, as each call's receiver.
__attribute__((noinline)) void unrelatedCallbackFactory(void*) {
    alignas(16) uint8_t predicates[2 * coui::kGamePredicateSize];
#define CALLBACK_ROUTE(route) do { \
    constructPredicates(predicates); \
    opaqueEmplace(const_cast<char*>(route), route, predicates); \
} while (false)
    CALLBACK_ROUTE("/callback-one");
    CALLBACK_ROUTE("/callback-two");
    CALLBACK_ROUTE("/callback-three");
    CALLBACK_ROUTE("/callback-four");
    CALLBACK_ROUTE("/callback-five");
    CALLBACK_ROUTE("/callback-six");
    CALLBACK_ROUTE("/callback-seven");
    CALLBACK_ROUTE("/callback-eight");
#undef CALLBACK_ROUTE
}

// A byte write to BH must invalidate the saved map in RBX, not just register 7
// (RDI in the full-width encoding). Rich route/callback evidence is not enough.
__attribute__((noinline)) void clobberedMapHelper(void* map) {
    alignas(16) uint8_t predicates[2 * coui::kGamePredicateSize];
    asm volatile("xor %%bh, %%bh" : "+b" (map) : : "cc");
    REGISTER_ROUTE("/clobbered-one");
    REGISTER_ROUTE("/clobbered-two");
    REGISTER_ROUTE("/clobbered-three");
    REGISTER_ROUTE("/clobbered-four");
    REGISTER_ROUTE("/clobbered-five");
    REGISTER_ROUTE("/clobbered-six");
    REGISTER_ROUTE("/clobbered-seven");
    REGISTER_ROUTE("/clobbered-eight");
}

// Saving the map into a stack slot and then replacing the base register must
// retire that slot. The deterministic form of this regression lives in
// tests/shape_test.cpp, which drives the check with a hand-encoded instruction
// stream; a compiler-generated function cannot be relied on to reproduce the
// exact encoding (frame pointers are optional).

#if defined(DISCOVERY_AMBIGUOUS)
__attribute__((noinline)) void competingRegister(void* map) {
    alignas(16) uint8_t predicates[2 * coui::kGamePredicateSize];
    REGISTER_ROUTE("/start-from-template");
    REGISTER_ROUTE("/create-new-world");
    REGISTER_ROUTE("/player-permissions");
    REGISTER_ROUTE("/send-invites");
    REGISTER_ROUTE("/pre-start");
    REGISTER_ROUTE("/main-menu");
    REGISTER_ROUTE("/world-templates");
}
#endif

// --- harness ---------------------------------------------------------------

// Scan an owned, non-executable ELF snapshot. Directly scanning an ASan-built
// process's global segments would intentionally enter compiler redzones between
// globals. Copying from the file keeps all scanner sanitizer checks enabled.
struct Snapshot {
    void* memory = MAP_FAILED;
    size_t size = 0;

    ~Snapshot() {
        if (memory != MAP_FAILED) munmap(memory, size);
    }

    bool load(coui::ElfImage& image, std::string& error) {
        const uintptr_t base = image.base();
        for (const auto& segment : image.segments()) {
            size = std::max(size, static_cast<size_t>(segment.vaddr - base + segment.memSize));
        }
        memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) return false;
        int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        const auto* header = image.header();
        const auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(base + header->e_phoff);
        bool ok = true;
        for (size_t i = 0; i < header->e_phnum && ok; ++i) {
            const auto& ph = phdrs[i];
            if (ph.p_type != PT_LOAD) continue;
            size_t copied = 0;
            while (copied < ph.p_filesz) {
                ssize_t got = pread(fd, static_cast<uint8_t*>(memory) + ph.p_vaddr + copied,
                                    ph.p_filesz - copied, static_cast<off_t>(ph.p_offset + copied));
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) { ok = false; break; }
                copied += static_cast<size_t>(got);
            }
        }
        close(fd);
        return ok && image.parse(reinterpret_cast<uintptr_t>(memory), "/proc/self/exe", error);
    }
};

int main() {
    struct Self {
        uintptr_t base = 0;
    } self;

    dl_iterate_phdr(
            [](struct dl_phdr_info* info, size_t, void* data) -> int {
                if (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0') {
                    static_cast<Self*>(data)->base = info->dlpi_addr;
                    return 1;
                }
                return 0;
            },
            &self);

    if (self.base == 0) {
        printf("SKIP: cannot resolve own load bias\n");
        return 0;
    }

    coui::ElfImage image;
    std::string error;
    if (!image.parse(self.base, "/proc/self/exe", error)) {
        printf("FAIL: parse: %s\n", error.c_str());
        return 1;
    }
    Snapshot snapshot;
    if (!snapshot.load(image, error)) {
        printf("FAIL: cannot load owned ELF snapshot: %s\n", error.c_str());
        return 1;
    }

    coui::FunctionTable table;
    if (!table.build(image, error)) {
        printf("FAIL: function table: %s\n", error.c_str());
        return 1;
    }

    coui::DiscoveryResult result;
    const bool found = coui::discoverOreUiConfigFunction(image, table, result, error);
#if defined(DISCOVERY_POINTER_TABLE) || defined(DISCOVERY_AMBIGUOUS)
    if (found || result.configFunction != 0 || error.empty()) {
        printf("FAIL: unsafe or ambiguous discovery was not refused\n");
        return 1;
    }
    printf("PASS: discovery refused unsafe/ambiguous fixture: %s\n", error.c_str());
    return 0;
#else
    if (!found) {
        printf("FAIL: discovery: %s\n", error.c_str());
        return 1;
    }

    auto expected = image.base() + reinterpret_cast<uintptr_t>(&oreUiRegister) - self.base;
    printf("expected  %p (oreUiRegister)\n", reinterpret_cast<void*>(expected));
    printf("discovered %p (routes=%zu, candidates=%zu)\n",
           reinterpret_cast<void*>(result.configFunction), result.routeCount,
           result.candidatesConsidered);

    int failures = 0;
    if (!result.topCandidates.front().registrationShape ||
        result.topCandidates.front().predicatePairCount < 6 ||
        result.topCandidates.front().mapCallCount < 6) {
        printf("FAIL: registration was not independently validated\n");
        ++failures;
    }
    if (result.configFunction != expected) {
        printf("FAIL: discovered the wrong function\n");
        ++failures;
    }
    const auto clobbered = image.base() + reinterpret_cast<uintptr_t>(&clobberedMapHelper) - self.base;
    auto decoy = std::find_if(result.topCandidates.begin(), result.topCandidates.end(),
                             [clobbered](const auto& candidate) { return candidate.address == clobbered; });
    if (decoy == result.topCandidates.end() || decoy->registrationShape || decoy->mapCallCount != 0) {
        printf("FAIL: clobbered map receiver was not rejected\n");
        ++failures;
    }
    if (result.configFunctionEnd != 0 &&
        result.configFunctionEnd <= result.configFunction) {
        printf("FAIL: function end is not after the start\n");
        ++failures;
    }

    for (const auto& candidate : result.topCandidates) {
        printf("  candidate %p routes=%zu refs=%zu size=%zu score=%d pairs=%zu mapcalls=%zu verified=%d\n",
               reinterpret_cast<void*>(candidate.address), candidate.routeCount,
               candidate.referenceCount, candidate.functionSize, candidate.prologueScore,
               candidate.predicatePairCount, candidate.mapCallCount, candidate.registrationShape);
    }

    if (failures == 0) {
        printf("PASS: discovery found the expected function\n");
    }
    return failures == 0 ? 0 : 1;
#endif
}
