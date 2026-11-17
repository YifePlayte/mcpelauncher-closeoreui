#pragma once

#include "elf_image.h"

#include <cstdint>
#include <vector>

namespace coui {

// Function start addresses recovered from .eh_frame_hdr (the `PT_GNU_EH_FRAME`
// segment) plus `.eh_frame` FDEs that carry no search-table entry.
//
// Every C++ function that can throw, and every function with a frame pointer or
// callee-saved registers, has an FDE. That makes this a far more reliable source
// of "is this address really a function entry?" than scanning backwards for a
// plausible prologue.
class FunctionTable {
public:
    bool build(const ElfImage& image, std::string& error);

    bool empty() const { return starts_.empty(); }

    // Exact membership test.
    bool isFunctionStart(uintptr_t addr) const;

    // Sorted ascending.
    const std::vector<uintptr_t>& starts() const { return starts_; }

    // Smallest function start strictly greater than `addr`, or 0 when none.
    uintptr_t nextStartAfter(uintptr_t addr) const;

    // Largest function start <= addr, or 0 when none.
    uintptr_t startAtOrBefore(uintptr_t addr) const;

    size_t size() const { return starts_.size(); }

private:
    bool parseEhFrameHdr(const ElfImage& image, std::string& error);
    void parseEhFrameFdes(const ElfImage& image);

    std::vector<uintptr_t> starts_;
};

// Reads a DWARF encoded pointer using the encodings that appear in
// .eh_frame_hdr.
//
// `pcrelBase` is the address of the encoded field itself (DW_EH_PE_pcrel is
// relative to the field), `datarelBase` is the start of the containing section
// (DW_EH_PE_datarel is relative to it), `textrelBase` is the start of the text
// segment. Passing the wrong base is the classic way to get a plausible but wrong
// table, so the caller must supply all three.
bool readEncodedPointer(const uint8_t*& cursor, const uint8_t* end, uint8_t encoding,
                        uintptr_t pcrelBase, uintptr_t datarelBase, uintptr_t textrelBase,
                        uintptr_t& out);

}  // namespace coui
