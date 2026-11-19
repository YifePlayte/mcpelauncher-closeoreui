// Regression test for the registration-structure check itself.
//
// The end-to-end discovery test in discovery_test.cpp checks which function is
// chosen. This one feeds the checker a hand-encoded instruction stream, so the
// encodings are deterministic: no compiler version can decide to omit a frame
// pointer and silently turn the regression into a no-op.
//
// Build and run via tests/run_tests.sh.
#include "discover.h"
#include "elf_image.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

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

constexpr size_t kImageSize = 0x8000;
constexpr size_t kCode = 0x1000;
constexpr size_t kData = 0x2000;
constexpr size_t kRodata = 0x3000;

// Emits x86-64 bytes into the synthetic image, resolving RIP-relative
// displacements against absolute addresses in the same image.
struct Emitter {
    uint8_t* code = nullptr;
    uintptr_t base = 0;
    size_t pos = 0;

    void bytes(std::initializer_list<uint8_t> values) {
        for (uint8_t value : values) code[pos++] = value;
    }

    uintptr_t here() const { return base + kCode + pos; }

    // lea reg, [rip+disp32] — the canonical "address of a constant" idiom.
    void leaRip(unsigned regField, uintptr_t target) {
        code[pos++] = 0x48;
        code[pos++] = 0x8D;
        code[pos++] = static_cast<uint8_t>(0x05 | (regField << 3));
        int32_t disp = static_cast<int32_t>(target - (here() + 4));
        memcpy(code + pos, &disp, 4);
        pos += 4;
    }

    void movRegToStackSlot(unsigned regField, int32_t disp) {
        code[pos++] = 0x48;
        code[pos++] = 0x89;
        code[pos++] = static_cast<uint8_t>(0x85 | (regField << 3));
        memcpy(code + pos, &disp, 4);
        pos += 4;
    }

    void movStackSlotToReg(unsigned regField, int32_t disp) {
        code[pos++] = 0x48;
        code[pos++] = 0x8B;
        code[pos++] = static_cast<uint8_t>(0x85 | (regField << 3));
        memcpy(code + pos, &disp, 4);
        pos += 4;
    }

    void call(uintptr_t target) {
        code[pos++] = 0xE8;
        int32_t rel = static_cast<int32_t>(target - (here() + 4));
        memcpy(code + pos, &rel, 4);
        pos += 4;
    }

    void ret() { code[pos++] = 0xC3; }
};

// One test image: header, three PT_LOADs, six top-level routes and two
// writable "predicate vtables".
struct Image {
    std::array<uint8_t, kImageSize> bytes{};
    std::vector<std::pair<uintptr_t, std::string>> routes;
    uintptr_t base = 0;
    coui::ElfImage parsed;

    Image() {
        base = reinterpret_cast<uintptr_t>(bytes.data());
        auto* header = reinterpret_cast<Elf64_Ehdr*>(bytes.data());
        memcpy(header->e_ident, ELFMAG, SELFMAG);
        header->e_ident[EI_CLASS] = ELFCLASS64;
        header->e_ident[EI_DATA] = ELFDATA2LSB;
        header->e_ident[EI_VERSION] = EV_CURRENT;
        header->e_type = ET_DYN;
        header->e_machine = EM_X86_64;
        header->e_version = EV_CURRENT;
        header->e_ehsize = sizeof(Elf64_Ehdr);
        header->e_phoff = sizeof(Elf64_Ehdr);
        header->e_phentsize = sizeof(Elf64_Phdr);
        header->e_phnum = 3;
        auto* ph = reinterpret_cast<Elf64_Phdr*>(bytes.data() + header->e_phoff);
        ph[0] = {PT_LOAD, PF_R | PF_X, kCode, kCode, 0, 0x1000, 0x1000, 4096};
        ph[1] = {PT_LOAD, PF_R | PF_W, kData, kData, 0, 0x1000, 0x1000, 4096};
        ph[2] = {PT_LOAD, PF_R, kRodata, kRodata, 0, 0x1000, 0x1000, 4096};

        const char* names[] = {"/main-menu", "/settings", "/play", "/edit-world",
                               "/send-invites", "/pre-start"};
        size_t cursor = kRodata;
        for (const char* name : names) {
            size_t length = strlen(name) + 1;
            memcpy(bytes.data() + cursor, name, length);
            routes.emplace_back(base + cursor, name);
            cursor += length;
        }

        std::string error;
        if (!parsed.parse(base, "fixture", error)) {
            printf("FAIL: cannot parse synthetic image: %s\n", error.c_str());
            ++gFailures;
        }
    }

    Emitter emitter() { return Emitter{bytes.data() + kCode, base, 0}; }
    uintptr_t predicateA() const { return base + kData + 0x100; }
    uintptr_t predicateB() const { return base + kData + 0x140; }

    bool verify(size_t length, coui::CandidateReport& report) {
        return coui::verifyRegistrationShape(parsed, base + kCode, length, routes, report);
    }
};

// Emits six complete registrations: paired predicate construction followed by a
// call that passes whatever register the stream last loaded the map into.
// `reloadMap` decides how the map receiver is restored before each call.
template <class Reload>
void emitRegistrations(Image& image, Emitter& e, Reload reloadMap) {
    e.bytes({0x55});              // push %rbp
    e.bytes({0x48, 0x89, 0xE5});  // mov %rsp,%rbp
    e.movRegToStackSlot(7, -0x180);  // mov %rdi,-0x180(%rbp): save the map

    for (unsigned i = 0; i < image.routes.size(); ++i) {
        int32_t slot = -0x90 - static_cast<int32_t>(i) * 0x100;
        e.leaRip(0, i % 2 ? image.predicateA() : image.predicateB());
        e.movRegToStackSlot(0, slot);
        e.leaRip(0, i % 2 ? image.predicateB() : image.predicateA());
        e.movRegToStackSlot(0, slot + 0x30);
        e.leaRip(0, image.routes[i].first);
        reloadMap(e);
        e.call(image.base + kCode);  // repeated "registration" call with %rdi
    }
    e.ret();
}

void testAcceptsRegistrationShape() {
    Image image;
    Emitter e = image.emitter();
    emitRegistrations(image, e, [](Emitter& emitter) {
        emitter.movStackSlotToReg(7, -0x180);  // frame intact: the slot still holds the map
    });

    coui::CandidateReport report;
    const bool verified = image.verify(e.pos, report);
    check(verified, "registration-shaped stream is accepted");
    check(report.topLevelRouteCount == 6, "six top-level routes counted");
    check(report.predicatePairCount == 6, "six predicate pairs counted");
    check(report.mapCallCount == 6, "six map-receiver calls counted");
}

// endbr64 is F3 0F 1E FA: modrm=0xFA means mod=3, reg=7, rm=2. Those fields are
// an encoding detail, not register operands. Read as operands they would clear
// %rdi (the map) at the entry of every CET-enabled build, and a real GCC 13
// build does emit it, which is how this was found.
void testAcceptsCetEntry() {
    Image image;
    Emitter e = image.emitter();
    e.bytes({0xF3, 0x0F, 0x1E, 0xFA});  // endbr64
    emitRegistrations(image, e, [](Emitter& emitter) {
        emitter.movStackSlotToReg(7, -0x180);
    });

    coui::CandidateReport report;
    const bool verified = image.verify(e.pos, report);
    check(verified, "endbr64 prologue does not hide the map argument");
    check(report.mapCallCount == 6, "endbr64 does not discard the original %rdi");
}

// The same instruction stream without the hint must behave identically, which
// shows the endbr64 case is about the hint and not about the rest of the body.
void testAcceptsWithoutCetEntry() {
    Image image;
    Emitter e = image.emitter();
    e.bytes({0x90});  // nop
    emitRegistrations(image, e, [](Emitter& emitter) {
        emitter.movStackSlotToReg(7, -0x180);
    });

    coui::CandidateReport report;
    check(image.verify(e.pos, report), "nop-prefixed stream is equivalent to endbr64");
    check(report.mapCallCount == 6, "nop-prefixed stream keeps the map argument");
}

void testRejectsReassignedFramePointer() {
    Image image;
    Emitter e = image.emitter();
    emitRegistrations(image, e, [](Emitter& emitter) {
        emitter.bytes({0x48, 0x89, 0xE5});     // mov %rsp,%rbp: the base register now
        emitter.movStackSlotToReg(7, -0x180);  // names a different word entirely
    });

    coui::CandidateReport report;
    const bool verified = image.verify(e.pos, report);
    check(!verified, "reassigned frame pointer does not authorize a hook");
    check(report.mapCallCount == 0, "stale stack slots are retired, so no map call is counted");
    check(report.predicatePairCount == 6, "predicate evidence alone is not enough either");
}

void testRejectsPoppedFramePointer() {
    Image image;
    Emitter e = image.emitter();
    e.bytes({0x55});              // push %rbp
    e.bytes({0x48, 0x89, 0xE5});  // mov %rsp,%rbp
    e.movRegToStackSlot(7, -0x180);
    e.bytes({0x5D});              // pop %rbp: the frame base is replaced
    for (unsigned i = 0; i < image.routes.size(); ++i) {
        int32_t slot = -0x90 - static_cast<int32_t>(i) * 0x100;
        e.leaRip(0, i % 2 ? image.predicateA() : image.predicateB());
        e.movRegToStackSlot(0, slot);
        e.leaRip(0, i % 2 ? image.predicateB() : image.predicateA());
        e.movRegToStackSlot(0, slot + 0x30);
        e.leaRip(0, image.routes[i].first);
        e.movStackSlotToReg(7, -0x180);
        e.call(image.base + kCode);
    }
    e.ret();

    coui::CandidateReport report;
    const bool verified = image.verify(e.pos, report);
    check(!verified, "popped frame pointer does not authorize a hook");
    check(report.mapCallCount == 0, "popping the frame retires its slots");
}

}  // namespace

int main() {
    printf("== registration shape ==\n");
    testAcceptsRegistrationShape();
    testAcceptsCetEntry();
    testAcceptsWithoutCetEntry();
    testRejectsReassignedFramePointer();
    testRejectsPoppedFramePointer();

    printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
