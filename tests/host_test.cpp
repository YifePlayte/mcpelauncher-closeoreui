// Host-side self test. Not part of the mod — it validates the pieces that are
// hard to debug inside the game: the instruction decoder, the .eh_frame function
// table, and the inline hooker.
//
// Build with the system compiler (not the NDK toolchain):
//   g++ -std=c++20 -Isrc tests/host_test.cpp src/{x64_decoder,eh_frame,elf_image,inline_hook,log}.cpp -o build/host_test -ldl

#include "discover.h"
#include "eh_frame.h"
#include "elf_image.h"
#include "inline_hook.h"
#include "log.h"
#include "x64_decoder.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <link.h>

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        printf("FAIL: %s\n", what);
    }
}

void expectLen(const char* hex, uint8_t expectedLen, const char* what) {
    std::vector<uint8_t> bytes;
    for (const char* p = hex; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        char pair[3] = {p[0], p[1], 0};
        bytes.push_back(static_cast<uint8_t>(strtoul(pair, nullptr, 16)));
        p += 2;
    }
    bytes.resize(16, 0xCC);
    coui::InsnInfo info = coui::decode(bytes.data());
    ++gChecks;
    if (!info.valid || info.len != expectedLen) {
        ++gFailures;
        printf("FAIL: %s — got len %u (valid=%d), expected %u\n", what,
               static_cast<unsigned>(info.len), info.valid ? 1 : 0,
               static_cast<unsigned>(expectedLen));
    }
}

// ---------------------------------------------------------------------------
// 1. Decoder
// ---------------------------------------------------------------------------
void testDecoder() {
    // Prologues and instructions the hooker has to relocate.
    expectLen("55", 1, "push rbp");
    expectLen("4157", 2, "push r15");
    expectLen("53", 1, "push rbx");
    expectLen("4881EC38010000", 7, "sub rsp, 0x138");
    expectLen("4883EC38", 4, "sub rsp, 0x38");
    expectLen("488B05AABBCCDD", 7, "mov rax, [rip+disp32]");
    expectLen("488D3D01020304", 7, "lea rdi, [rip+disp32]");
    expectLen("4889E5", 3, "mov rbp, rsp");
    expectLen("4C8D0D00000000", 7, "lea r9, [rip+0]");
    expectLen("E8AABBCCDD", 5, "call rel32");
    expectLen("E9AABBCCDD", 5, "jmp rel32");
    expectLen("EB05", 2, "jmp rel8");
    expectLen("7405", 2, "je rel8");
    expectLen("0F8405AABBCC", 6, "je rel32");
    expectLen("C3", 1, "ret");
    expectLen("0F1F440000", 5, "nop dword [rax+rax]");
    expectLen("48 83 3D ?? ?? ?? ?? 00", 8, "cmp qword [rip+disp32], 0");
    expectLen("F30F1EFA", 4, "endbr64");
    expectLen("660F1F440000", 6, "nop word ptr (66 prefix)");
    expectLen("48C7C0FFFFFFFF", 7, "mov rax, -1 (imm32 sign extended)");
    expectLen("B801000000", 5, "mov eax, 1");
    expectLen("41B801000000", 6, "mov r8d, 1");
    expectLen("BF01000000", 5, "mov edi, 1");
    expectLen("31C0", 2, "xor eax, eax");
    expectLen("48833D0000000000", 8, "cmp qword [rip+0], imm8");
    expectLen("488D 3D 00 00 00 00", 7, "lea rdi, [rip+0] with space");

    // Flag classification used by the hooker.
    {
        uint8_t ret[] = {0xC3};
        auto info = coui::decode(ret);
        check(info.isReturn, "ret flagged as return");
        check(info.relocatable(), "ret is relocatable");
    }
    {
        uint8_t call[] = {0xE8, 0, 0, 0, 0};
        auto info = coui::decode(call);
        check(info.isCall && info.relSize == 4, "call rel32 flagged");
        check(!info.relocatable(), "call rel32 is not relocatable");
    }
    {
        uint8_t lea[] = {0x48, 0x8D, 0x3D, 1, 2, 3, 4};
        auto info = coui::decode(lea);
        check(info.ripRelative, "lea rip flagged rip-relative");
        check(info.isRipLea(), "lea rip flagged as rip lea");
        check(!info.relocatable(), "rip-relative is not relocatable");
    }
}

// ---------------------------------------------------------------------------
// 2. Function table against this very binary
// ---------------------------------------------------------------------------
struct FindSelf {
    uintptr_t base = 0;
    const char* path = nullptr;
};

int findSelfCallback(struct dl_phdr_info* info, size_t, void* data) {
    auto* state = static_cast<FindSelf*>(data);
    if (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0') {
        state->base = info->dlpi_addr;
        return 1;
    }
    return 0;
}

void testFunctionTable() {
    FindSelf self;
    dl_iterate_phdr(findSelfCallback, &self);
    if (self.base == 0) {
        printf("SKIP: could not resolve own load bias\n");
        return;
    }

    coui::ElfImage image;
    std::string error;
    if (!image.parse(self.base, "/proc/self/exe", error)) {
        printf("FAIL: cannot parse own ELF: %s\n", error.c_str());
        ++gFailures;
        return;
    }

    coui::FunctionTable table;
    if (!table.build(image, error)) {
        printf("FAIL: cannot build function table: %s\n", error.c_str());
        ++gFailures;
        return;
    }

    printf("function table: %zu entries\n", table.size());
    check(table.size() > 50, "function table has a plausible number of entries");

    // Every entry must sit inside the executable segment.
    coui::Segment text;
    if (!image.executableSegment(text)) {
        printf("FAIL: no executable segment\n");
        ++gFailures;
        return;
    }

    size_t inside = 0;
    for (uintptr_t start : table.starts()) {
        if (start >= text.vaddr && start < text.vaddr + text.memSize) {
            ++inside;
        }
    }
    printf("entries inside .text: %zu / %zu\n", inside, table.size());
    check(inside * 100 >= table.size() * 90, "most function starts land inside .text");

    // Sequential decode from a function start must not walk over the next start.
    // A handful of functions legitimately contain data or alignment padding, so
    // require a strong majority rather than perfection.
    size_t checked = 0;
    size_t consistent = 0;
    for (size_t i = 0; i + 1 < table.starts().size(); ++i) {
        uintptr_t start = table.starts()[i];
        uintptr_t next = table.starts()[i + 1];
        if (start < text.vaddr || next > text.vaddr + text.memSize) {
            continue;
        }
        size_t len = static_cast<size_t>(next - start);
        if (len == 0 || len > 200000) {
            continue;
        }
        ++checked;

        // Decode forward; we accept landing exactly on `next` or overrunning it
        // by at most 15 bytes (the last instruction straddling the boundary).
        uintptr_t cursor = start;
        bool ok = false;
        while (cursor < next) {
            auto info = coui::decode(reinterpret_cast<const uint8_t*>(cursor));
            if (!info.valid) {
                break;
            }
            cursor += info.len;
        }
        if (cursor == next) {
            ok = true;
        } else if (cursor > next && cursor - next <= 15) {
            ok = true;
        }
        if (ok) {
            ++consistent;
        }
    }
    printf("sequential decode consistent: %zu / %zu\n", consistent, checked);
    if (checked > 100) {
        check(consistent * 100 >= checked * 95,
              "sequential decode from function starts agrees with the function table");
    }
}

// ---------------------------------------------------------------------------
// 3. Inline hook against a real function
// ---------------------------------------------------------------------------
int gHookCalls = 0;
int (*gOriginal)(int, int) = nullptr;

int __attribute__((noinline)) addNumbers(int a, int b) {
    // Volatile prevents the compiler from turning this into a leaf that gets
    // inlined into the test, which would make the hook meaningless.
    volatile int pad = a + b;
    return pad;
}

int hookAddNumbers(int a, int b) {
    ++gHookCalls;
    return gOriginal(a, b) * 10;
}

void testInlineHook() {
    int before = addNumbers(2, 3);
    check(before == 5, "baseline call works");

    // A tiny function with unknown length must be refused rather than clobbering
    // its successor — this is the regression that took the game down.
    {
        coui::InlineHook guarded;
        std::string guardError;
        void* guardOriginal = nullptr;
        bool installed = guarded.install(reinterpret_cast<uintptr_t>(&addNumbers),
                                         reinterpret_cast<uintptr_t>(&addNumbers) + 4,
                                         reinterpret_cast<void*>(&hookAddNumbers), &guardOriginal,
                                         guardError);
        check(!installed, "hook refuses to patch past the function end");
    }

    coui::InlineHook hook;
    std::string error;
    void* original = nullptr;
    if (!hook.install(reinterpret_cast<uintptr_t>(&addNumbers),
                      reinterpret_cast<uintptr_t>(&addNumbers) + 64,
                      reinterpret_cast<void*>(&hookAddNumbers), &original, error)) {
        printf("FAIL: cannot install inline hook: %s\n", error.c_str());
        ++gFailures;
        return;
    }
    gOriginal = reinterpret_cast<int (*)(int, int)>(original);

    int hooked = addNumbers(2, 3);
    printf("hooked result: %d (calls=%d)\n", hooked, gHookCalls);
    check(gHookCalls == 1, "detour was called");
    check(hooked == 50, "detour forwarded to the original and transformed the result");

    hook.uninstall();
    int restored = addNumbers(2, 3);
    check(restored == 5, "original behaviour restored after uninstall");
    check(gHookCalls == 1, "detour not called after uninstall");
}

// ---------------------------------------------------------------------------
// 4. Route recognition (the heuristic the discovery relies on)
// ---------------------------------------------------------------------------
void testRouteRecognition() {
    check(coui::looksLikeOreUiRoute("/oreui-marketplace/search", 23), "oreui route accepted");
    check(coui::looksLikeOreUiRoute("/main-menu", 10), "main-menu accepted");
    check(coui::looksLikeOreUiRoute("/create-new-world", 17), "create-new-world accepted");
    check(!coui::looksLikeOreUiRoute("main-menu", 9), "missing leading slash rejected");
    check(!coui::looksLikeOreUiRoute("/bad route", 10), "space rejected");
    check(!coui::looksLikeOreUiRoute("/fmt%s", 6), "printf format rejected");
    check(!coui::looksLikeOreUiRoute("ab", 2), "too short rejected");
}

}  // namespace

int main() {
    printf("== decoder ==\n");
    testDecoder();
    printf("== routes ==\n");
    testRouteRecognition();
    printf("== function table ==\n");
    testFunctionTable();
    printf("== inline hook ==\n");
    testInlineHook();

    printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
