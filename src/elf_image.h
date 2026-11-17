#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <elf.h>

namespace coui {

// The game is aarch64 Android, but the launcher runs it natively on x86_64 via
// its own linker, so from this process' point of view everything is a 64-bit ELF.
// Explicit Elf64_* types avoid depending on ElfW, which bionic does not export
// from <elf.h>.
using ElfHeader = Elf64_Ehdr;
using ElfProgramHeader = Elf64_Phdr;

struct Segment {
    uintptr_t vaddr = 0;  // absolute address in this process
    size_t memSize = 0;
    size_t fileSize = 0;
    uint32_t flags = 0;   // PF_*
};

// A loaded ELF image, parsed straight out of this process' memory.
class ElfImage {
public:
    // `base` is the load bias: every p_vaddr already accounts for it.
    bool parse(uintptr_t base, const std::string& path, std::string& error);

    uintptr_t base() const { return base_; }
    const std::string& path() const { return path_; }
    const std::vector<Segment>& segments() const { return segments_; }
    const ElfHeader* header() const { return header_; }

    // First PT_LOAD with PF_X.
    bool executableSegment(Segment& out) const;

    // PT_GNU_EH_FRAME, i.e. .eh_frame_hdr. Returns false on stripped builds.
    bool ehFrameHeaderSegment(Segment& out) const;

    // Reads a NUL-terminated string at an absolute address, bounded by the
    // readable PT_LOAD segments.
    bool readCString(uintptr_t addr, std::string& out, size_t maxLen = 512) const;

    bool containsAddress(uintptr_t addr, uint32_t requiredFlags) const;

private:
    uintptr_t base_ = 0;
    std::string path_;
    const ElfHeader* header_ = nullptr;
    std::vector<Segment> segments_;

    Segment ehFrame_{};
    bool hasEhFrame_ = false;
};

}  // namespace coui
