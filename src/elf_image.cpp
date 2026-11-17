#include "elf_image.h"

#include <cstring>

namespace coui {

bool ElfImage::parse(uintptr_t base, const std::string& path, std::string& error) {
    base_ = base;
    path_ = path;
    segments_.clear();
    hasEhFrame_ = false;

    auto* header = reinterpret_cast<const Elf64_Ehdr*>(base);
    if (header->e_ident[EI_MAG0] != ELFMAG0 || header->e_ident[EI_MAG1] != ELFMAG1 ||
        header->e_ident[EI_MAG2] != ELFMAG2 || header->e_ident[EI_MAG3] != ELFMAG3) {
        error = "no ELF header at module base";
        return false;
    }
    if (header->e_ident[EI_CLASS] != ELFCLASS64) {
        error = "module is not ELFCLASS64";
        return false;
    }
    if (header->e_phentsize != sizeof(Elf64_Phdr)) {
        error = "unexpected e_phentsize";
        return false;
    }

    header_ = header;
    auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(base + header->e_phoff);
    for (int i = 0; i < header->e_phnum; ++i) {
        const Elf64_Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD && ph.p_type != PT_GNU_EH_FRAME) {
            continue;
        }
        if (ph.p_type == PT_GNU_EH_FRAME) {
            Segment seg;
            seg.vaddr = base + ph.p_vaddr;
            seg.memSize = ph.p_memsz;
            seg.fileSize = ph.p_filesz;
            seg.flags = ph.p_flags;
            ehFrame_.vaddr = seg.vaddr;
            ehFrame_.memSize = seg.memSize;
            ehFrame_.flags = seg.flags;
            hasEhFrame_ = true;
            continue;
        }
        Segment seg;
        seg.vaddr = base + ph.p_vaddr;
        seg.memSize = ph.p_memsz;
        seg.fileSize = ph.p_filesz;
        seg.flags = ph.p_flags;
        segments_.push_back(seg);
    }

    if (segments_.empty()) {
        error = "module has no PT_LOAD segments";
        return false;
    }
    return true;
}

bool ElfImage::executableSegment(Segment& out) const {
    for (const Segment& seg : segments_) {
        if (seg.flags & PF_X) {
            out = seg;
            return true;
        }
    }
    return false;
}

bool ElfImage::ehFrameHeaderSegment(Segment& out) const {
    if (!hasEhFrame_) {
        return false;
    }
    out.vaddr = ehFrame_.vaddr;
    out.memSize = ehFrame_.memSize;
    out.flags = ehFrame_.flags;
    return true;
}

bool ElfImage::containsAddress(uintptr_t addr, uint32_t requiredFlags) const {
    for (const Segment& seg : segments_) {
        if (addr >= seg.vaddr && addr < seg.vaddr + seg.memSize) {
            if ((seg.flags & requiredFlags) == requiredFlags) {
                return true;
            }
        }
    }
    return false;
}

bool ElfImage::readCString(uintptr_t addr, std::string& out, size_t maxLen) const {
    out.clear();
    if (!containsAddress(addr, PF_R)) {
        return false;
    }

    const char* ptr = reinterpret_cast<const char*>(addr);
    size_t limit = maxLen;
    for (size_t i = 0; i < limit; ++i) {
        // Never run past the containing segment.
        if (!containsAddress(addr + i, PF_R)) {
            return false;
        }
        char c = ptr[i];
        if (c == '\0') {
            return !out.empty();
        }
        out.push_back(c);
    }
    return false;
}

}  // namespace coui
