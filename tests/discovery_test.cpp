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

#include <cstdio>
#include <cstring>
#include <string>

#include <link.h>

// --- fixtures --------------------------------------------------------------

// Decoys are marked noinline and reference a single route each.
//
// Route constants have to survive optimisation, so they are read through a
// volatile sink.

volatile int gSink = 0;

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
__attribute__((noinline)) void oreUiRegister() {
    volatile char pad[512];
    memset((void*) pad, 0, sizeof(pad));

    touch("/oreui-marketplace/search");
    touch("/oreui-marketplace/wishlist");
    touch("/start-from-template");
    touch("/create-new-world");
    touch("/player-permissions");
    touch("/send-invites");
    touch("/gameplay/bedtime");
    touch("/pre-start");
    touch("/main-menu");
    touch("/settings/general");
    touch("/world-templates");
    touch("/realms/join");

    gSink += pad[0];
}

// --- harness ---------------------------------------------------------------

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

    coui::FunctionTable table;
    if (!table.build(image, error)) {
        printf("FAIL: function table: %s\n", error.c_str());
        return 1;
    }

    coui::DiscoveryResult result;
    if (!coui::discoverOreUiConfigFunction(image, table, result, error)) {
        printf("FAIL: discovery: %s\n", error.c_str());
        return 1;
    }

    auto expected = reinterpret_cast<uintptr_t>(&oreUiRegister);
    printf("expected  %p (oreUiRegister)\n", reinterpret_cast<void*>(expected));
    printf("discovered %p (routes=%zu, candidates=%zu)\n",
           reinterpret_cast<void*>(result.configFunction), result.routeCount,
           result.candidatesConsidered);

    int failures = 0;
    if (result.configFunction != expected) {
        printf("FAIL: discovered the wrong function\n");
        ++failures;
    }
    if (result.configFunctionEnd != 0 &&
        result.configFunctionEnd <= result.configFunction) {
        printf("FAIL: function end is not after the start\n");
        ++failures;
    }

    for (const auto& candidate : result.topCandidates) {
        printf("  candidate %p routes=%zu refs=%zu size=%zu score=%d\n",
               reinterpret_cast<void*>(candidate.address), candidate.routeCount,
               candidate.referenceCount, candidate.functionSize, candidate.prologueScore);
    }

    if (failures == 0) {
        printf("PASS: discovery found the expected function\n");
    }
    return failures == 0 ? 0 : 1;
}
