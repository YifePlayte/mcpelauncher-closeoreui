#include "eh_frame.h"
#include "elf_image.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int gFailures = 0;
int gChecks = 0;
void check(bool condition, const char* message) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        printf("FAIL: %s\n", message);
    }
}
template <class T>
void put(uint8_t* destination, T value) { memcpy(destination, &value, sizeof(value)); }

struct Fixture {
    alignas(16) std::array<uint8_t, 4096> bytes{};

    explicit Fixture(size_t headerOffset) {
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
        ph[0] = {PT_LOAD, PF_R, 0, 0, 0, bytes.size(), bytes.size(), 4096};
        ph[1] = {PT_LOAD, PF_R | PF_X, 0x400, 0x400, 0, 0x100, 0x100, 16};
        ph[2] = {PT_GNU_EH_FRAME, PF_R, headerOffset, headerOffset, 0, 12, 12, 4};
        uint8_t* hdr = bytes.data() + headerOffset;
        hdr[0] = 1;
        hdr[1] = 0x1b; // pcrel | sdata4
        hdr[2] = 3;    // udata4
        hdr[3] = 0xff; // no search table; must use FDE fallback
        put<int32_t>(hdr + 4, 0x300 - static_cast<int32_t>(headerOffset + 4));
        put<uint32_t>(hdr + 8, 1);
        const uint8_t cie[] = {
            16, 0, 0, 0, 0, 0, 0, 0,
            1, 'z', 'R', 0, 1, 0x78, 16, 1, 0x1b, 0x0c, 7, 8,
        };
        memcpy(bytes.data() + 0x300, cie, sizeof(cie));
        uint8_t* fde = bytes.data() + 0x314;
        put<uint32_t>(fde, 16);
        put<uint32_t>(fde + 4, 0x318 - 0x300);
        put<int32_t>(fde + 8, 0x400 - 0x31c);
        put<uint32_t>(fde + 12, 16);
        memset(bytes.data() + 0x400, 0x90, 16);
        bytes[0x40f] = 0xc3;
    }

    bool build(coui::FunctionTable& table) {
        coui::ElfImage image;
        std::string error;
        check(image.parse(reinterpret_cast<uintptr_t>(bytes.data()), "fixture", error),
              "parse synthetic loaded image");
        return table.build(image, error);
    }
};

} // namespace

int main() {
    for (size_t offset : {size_t{0x200}, size_t{0x380}}) {
        Fixture fixture(offset);
        coui::FunctionTable table;
        check(fixture.build(table), "FDE fallback works before/after the header");
        check(table.size() == 1, "fallback yields exactly one function");
        check(table.isFunctionStart(reinterpret_cast<uintptr_t>(fixture.bytes.data()) + 0x400),
              "fallback recovers the exact function entry");
    }
    {
        Fixture fixture(0x200);
        put<uint32_t>(fixture.bytes.data() + 0x300, 0x10000);
        coui::FunctionTable table;
        check(!fixture.build(table), "oversized FDE record rejected at containing segment bound");
    }
    {
        Fixture fixture(0x200);
        put<int32_t>(fixture.bytes.data() + 0x204, 0xff8 - 0x204);
        put<uint32_t>(fixture.bytes.data() + 0xff8, 0xffffffff);
        coui::FunctionTable table;
        check(!fixture.build(table), "truncated extended-length record refused");
    }
    {
        // The FDE fallback ignores the CIE and always decodes the initial
        // location as pcrel|sdata4. When the binary search table is usable, its
        // result is authoritative: merging the fallback would add addresses that
        // are not function entries (here 0x41c, inside a 16-byte function at 0x400).
        Fixture fixture(0x200);
        uint8_t* hdr = fixture.bytes.data() + 0x200;
        hdr[3] = 0x3b;  // datarel | sdata4: a working search table
        // The fixture's PT_GNU_EH_FRAME is 12 bytes (header only); grow it so the
        // search table is inside the segment the parser is allowed to read.
        reinterpret_cast<Elf64_Phdr*>(fixture.bytes.data() + sizeof(Elf64_Ehdr))[2].p_memsz = 32;
        put<int32_t>(hdr + 12, 0x400 - 0x200);  // function entry
        put<int32_t>(hdr + 16, 0x314 - 0x200);  // its FDE
        // CIE declares an FDE encoding the fallback does not understand (udata4).
        fixture.bytes[0x310] = 0x03;
        // Encoded as pcrel from 0x31c this is 0x41c, i.e. inside the function at
        // 0x400, not its entry. The fallback ignores the CIE and would add it.
        put<int32_t>(fixture.bytes.data() + 0x31c, 0x41c - 0x31c);

        coui::FunctionTable table;
        check(fixture.build(table), "usable search table builds a function table");
        check(table.size() == 1, "usable search table is not appended to by the FDE fallback");
        check(table.isFunctionStart(reinterpret_cast<uintptr_t>(fixture.bytes.data()) + 0x400),
              "table entry is still accepted");
        check(!table.isFunctionStart(reinterpret_cast<uintptr_t>(fixture.bytes.data()) + 0x41c),
              "fallback-encoded mid-function address is not merged into a usable table");
    }
    printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures != 0;
}
