// Run the actual runtime discovery code on an ELF without loading/executing it.
// Build via tests/run_tests.sh; no game files are needed to build this tool.
#include "discover.h"
#include "eh_frame.h"
#include "elf_image.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool readAt(int fd, void* destination, size_t size, uint64_t offset) {
    auto* bytes = static_cast<uint8_t*>(destination);
    while (size > 0) {
        ssize_t got = pread(fd, bytes, size, static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        bytes += got;
        offset += static_cast<size_t>(got);
        size -= static_cast<size_t>(got);
    }
    return true;
}

struct ImageStorage {
    int fd = -1;
    void* memory = MAP_FAILED;
    size_t size = 0;
    ~ImageStorage() {
        if (memory != MAP_FAILED) munmap(memory, size);
        if (fd >= 0) close(fd);
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: scan_so <ELF> [expected RVA]\n");
        return 2;
    }
    ImageStorage storage;
    storage.fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    struct stat st{};
    Elf64_Ehdr header{};
    if (storage.fd < 0 || fstat(storage.fd, &st) != 0 ||
        st.st_size < static_cast<off_t>(sizeof(header)) ||
        !readAt(storage.fd, &header, sizeof(header), 0) ||
        memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_machine != EM_X86_64 || header.e_phentsize != sizeof(Elf64_Phdr)) {
        fprintf(stderr, "invalid or unreadable x86-64 ELF\n");
        return 2;
    }
    const uint64_t fileSize = static_cast<uint64_t>(st.st_size);
    if (header.e_phoff > fileSize || header.e_phnum == 0 ||
        header.e_phnum > (fileSize - header.e_phoff) / sizeof(Elf64_Phdr)) return 2;
    std::vector<Elf64_Phdr> phdrs(header.e_phnum);
    if (!readAt(storage.fd, phdrs.data(), phdrs.size() * sizeof(Elf64_Phdr), header.e_phoff)) return 2;
    constexpr uint64_t kMaxImage = 2ULL * 1024 * 1024 * 1024;
    bool headersMapped = false;
    for (const auto& ph : phdrs) {
        if (ph.p_type != PT_LOAD) continue;
        if (ph.p_vaddr > kMaxImage || ph.p_memsz > kMaxImage - ph.p_vaddr ||
            ph.p_filesz > ph.p_memsz || ph.p_offset > fileSize ||
            ph.p_filesz > fileSize - ph.p_offset) return 2;
        storage.size = std::max(storage.size, static_cast<size_t>(ph.p_vaddr + ph.p_memsz));
        if (ph.p_vaddr == 0 && ph.p_offset == 0 &&
            ph.p_filesz >= header.e_phoff + phdrs.size() * sizeof(Elf64_Phdr)) headersMapped = true;
    }
    if (!headersMapped || storage.size < sizeof(header)) return 2;
    for (const auto& ph : phdrs) {
        if (ph.p_type != PT_GNU_EH_FRAME) continue;
        bool readable = false;
        for (const auto& load : phdrs) {
            if (load.p_type == PT_LOAD && (load.p_flags & PF_R) && ph.p_vaddr >= load.p_vaddr &&
                ph.p_vaddr - load.p_vaddr <= load.p_memsz &&
                ph.p_memsz <= load.p_memsz - (ph.p_vaddr - load.p_vaddr)) readable = true;
        }
        if (!readable) return 2;
    }
    // Deliberately not executable. This image is only read by the scanners; no
    // linker, constructors, imports or game code are run.
    storage.memory = mmap(nullptr, storage.size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (storage.memory == MAP_FAILED) return 2;
    for (const auto& ph : phdrs) {
        if (ph.p_type == PT_LOAD && !readAt(storage.fd,
                static_cast<uint8_t*>(storage.memory) + ph.p_vaddr, ph.p_filesz, ph.p_offset)) return 2;
    }
    coui::ElfImage image;
    coui::FunctionTable functions;
    coui::DiscoveryResult result;
    std::string error;
    const uintptr_t base = reinterpret_cast<uintptr_t>(storage.memory);
    if (!image.parse(base, argv[1], error) || !functions.build(image, error)) {
        fprintf(stderr, "parse failed: %s\n", error.c_str());
        return 2;
    }
    const bool found = coui::discoverOreUiConfigFunction(image, functions, result, error);
    printf("%s: functions=%zu found=%d RVA=%#zx top=%zu routes=%zu error=%s\n", argv[1],
           functions.size(), found, found ? result.configFunction - base : 0,
           result.topLevelRouteCount, result.routeCount, error.c_str());
    for (const auto& candidate : result.topCandidates) {
        printf("  %#zx top=%zu routes=%zu pairs=%zu mapcalls=%zu verified=%d size=%zu\n",
               candidate.address - base, candidate.topLevelRouteCount, candidate.routeCount,
               candidate.predicatePairCount, candidate.mapCallCount, candidate.registrationShape,
               candidate.functionSize);
    }
    if (!found) return 1;
    if (argc == 3) {
        char* end = nullptr;
        const auto expected = static_cast<uintptr_t>(strtoull(argv[2], &end, 0));
        if (end == argv[2] || *end != '\0' || result.configFunction - base != expected) {
            fprintf(stderr, "unexpected discovery RVA\n");
            return 1;
        }
    }
    return 0;
}
