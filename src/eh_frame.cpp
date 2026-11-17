#include "eh_frame.h"

#include "log.h"

#include <algorithm>
#include <cstring>

namespace coui {
namespace {

uint64_t readLE(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

int64_t signExtend(uint64_t v, size_t bytes) {
    if (bytes == 0 || bytes >= 8) {
        return static_cast<int64_t>(v);
    }
    size_t shift = 64 - bytes * 8;
    return static_cast<int64_t>(v << shift) >> shift;
}

// DW_EH_PE_* encodings.
constexpr uint8_t kAbsptr = 0x00;
constexpr uint8_t kUleb128 = 0x01;
constexpr uint8_t kUdata2 = 0x02;
constexpr uint8_t kUdata4 = 0x03;
constexpr uint8_t kUdata8 = 0x04;
constexpr uint8_t kSleb128 = 0x09;
constexpr uint8_t kSdata2 = 0x0A;
constexpr uint8_t kSdata4 = 0x0B;
constexpr uint8_t kSdata8 = 0x0C;
constexpr uint8_t kPcrel = 0x10;
constexpr uint8_t kTextrel = 0x20;
constexpr uint8_t kDatarel = 0x30;
constexpr uint8_t kFuncrel = 0x40;
constexpr uint8_t kAligned = 0x50;
constexpr uint8_t kIndirect = 0x80;
constexpr uint8_t kOmit = 0xFF;

bool readULEB(const uint8_t*& cursor, const uint8_t* end, uint64_t& out) {
    uint64_t result = 0;
    int shift = 0;
    while (cursor < end && shift < 64) {
        uint8_t byte = *cursor++;
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            out = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool readSLEB(const uint8_t*& cursor, const uint8_t* end, int64_t& out) {
    uint64_t result = 0;
    int shift = 0;
    uint8_t byte = 0;
    while (cursor < end && shift < 64) {
        byte = *cursor++;
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) {
            if (shift < 64 && (byte & 0x40)) {
                result |= ~0ULL << shift;
            }
            out = static_cast<int64_t>(result);
            return true;
        }
    }
    return false;
}

}  // namespace

bool readEncodedPointer(const uint8_t*& cursor, const uint8_t* end, uint8_t encoding,
                        uintptr_t pcrelBase, uintptr_t datarelBase, uintptr_t textrelBase,
                        uintptr_t& out) {
    if (encoding == kOmit) {
        out = 0;
        return true;
    }

    uint8_t format = encoding & 0x0F;
    uintptr_t value = 0;
    uint64_t raw = 0;
    int64_t sraw = 0;

    switch (format) {
        case kAbsptr:
            if (end - cursor < 8) return false;
            raw = readLE(cursor, 8);
            cursor += 8;
            value = static_cast<uintptr_t>(raw);
            break;
        case kUleb128:
            if (!readULEB(cursor, end, raw)) return false;
            value = static_cast<uintptr_t>(raw);
            break;
        case kUdata2:
            if (end - cursor < 2) return false;
            value = static_cast<uintptr_t>(readLE(cursor, 2));
            cursor += 2;
            break;
        case kUdata4:
            if (end - cursor < 4) return false;
            value = static_cast<uintptr_t>(readLE(cursor, 4));
            cursor += 4;
            break;
        case kUdata8:
            if (end - cursor < 8) return false;
            value = static_cast<uintptr_t>(readLE(cursor, 8));
            cursor += 8;
            break;
        case kSleb128:
            if (!readSLEB(cursor, end, sraw)) return false;
            value = static_cast<uintptr_t>(sraw);
            break;
        case kSdata2:
            if (end - cursor < 2) return false;
            sraw = signExtend(readLE(cursor, 2), 2);
            value = static_cast<uintptr_t>(sraw);
            cursor += 2;
            break;
        case kSdata4:
            if (end - cursor < 4) return false;
            sraw = signExtend(readLE(cursor, 4), 4);
            value = static_cast<uintptr_t>(sraw);
            cursor += 4;
            break;
        case kSdata8:
            if (end - cursor < 8) return false;
            value = static_cast<uintptr_t>(readLE(cursor, 8));
            cursor += 8;
            break;
        default:
            return false;
    }

    switch (encoding & 0x70) {
        case kPcrel:
            value += pcrelBase;
            break;
        case kTextrel:
            value += textrelBase;
            break;
        case kDatarel:
            value += datarelBase;
            break;
        case kFuncrel:
            value += pcrelBase;
            break;
        case kAligned:
            // No base adjustment.
            break;
        default:
            break;
    }

    if (encoding & kIndirect) {
        if (value == 0) {
            return false;
        }
        value = *reinterpret_cast<uintptr_t*>(value);
    }

    out = value;
    return true;
}

bool FunctionTable::build(const ElfImage& image, std::string& error) {
    starts_.clear();

    std::string hdrError;
    if (!parseEhFrameHdr(image, hdrError)) {
        logWarn("no usable .eh_frame_hdr (%s), falling back to FDE scan", hdrError.c_str());
    }
    parseEhFrameFdes(image);

    std::sort(starts_.begin(), starts_.end());
    starts_.erase(std::unique(starts_.begin(), starts_.end()), starts_.end());

    if (starts_.empty()) {
        error = ".eh_frame yielded no function starts";
        return false;
    }
    return true;
}

bool FunctionTable::parseEhFrameHdr(const ElfImage& image, std::string& error) {
    Segment hdrSeg;
    if (!image.ehFrameHeaderSegment(hdrSeg)) {
        error = "no PT_GNU_EH_FRAME segment";
        return false;
    }

    Segment textSeg;
    uintptr_t textBase = 0;
    if (image.executableSegment(textSeg)) {
        textBase = textSeg.vaddr;
    }
    const uintptr_t datarelBase = hdrSeg.vaddr;

    const uint8_t* start = reinterpret_cast<const uint8_t*>(hdrSeg.vaddr);
    const uint8_t* end = start + hdrSeg.memSize;
    if (end - start < 4) {
        error = "segment too small";
        return false;
    }
    if (start[0] != 1) {
        error = "unsupported eh_frame_hdr version";
        return false;
    }

    const uint8_t* cursor = start + 4;
    uintptr_t ehFramePtr = 0;
    uintptr_t count = 0;

    if (!readEncodedPointer(cursor, end, start[1], reinterpret_cast<uintptr_t>(cursor),
                            datarelBase, textBase, ehFramePtr)) {
        error = "bad eh_frame_ptr encoding";
        return false;
    }
    if (!readEncodedPointer(cursor, end, start[2], reinterpret_cast<uintptr_t>(cursor),
                            datarelBase, textBase, count)) {
        error = "bad fde_count encoding";
        return false;
    }

    if (count == 0 || count > 8u * 1000u * 1000u) {
        error = "implausible fde_count";
        return false;
    }
    if (start[3] == kOmit) {
        error = "no binary search table encoding";
        return false;
    }

    const uint8_t* table = cursor;
    size_t entryStride = 0;
    {
        const uint8_t* probe = table;
        uintptr_t dummy = 0;
        const uint8_t* before = probe;
        if (!readEncodedPointer(probe, end, start[3], reinterpret_cast<uintptr_t>(probe),
                                datarelBase, textBase, dummy)) {
            error = "bad table entry encoding";
            return false;
        }
        entryStride = static_cast<size_t>(probe - before) * 2;
    }
    if (entryStride == 0) {
        error = "zero table entry stride";
        return false;
    }

    size_t available = static_cast<size_t>(end - table);
    size_t usable = available / entryStride;
    if (usable < count) {
        count = usable;
    }

    starts_.reserve(static_cast<size_t>(count) + 1024);

    const uint8_t* entry = table;
    for (uintptr_t i = 0; i < count; ++i) {
        uintptr_t location = 0;
        uintptr_t address = 0;
        if (!readEncodedPointer(entry, end, start[3], reinterpret_cast<uintptr_t>(entry),
                                datarelBase, textBase, location)) {
            break;
        }
        if (!readEncodedPointer(entry, end, start[3], reinterpret_cast<uintptr_t>(entry),
                                datarelBase, textBase, address)) {
            break;
        }
        (void) address;
        if (location != 0) {
            starts_.push_back(location);
        }
    }

    if (starts_.empty()) {
        error = "table contained no usable entries";
        return false;
    }
    return true;
}

void FunctionTable::parseEhFrameFdes(const ElfImage& image) {
    Segment hdrSeg;
    if (!image.ehFrameHeaderSegment(hdrSeg)) {
        return;
    }

    Segment textSeg;
    uintptr_t textBase = 0;
    if (image.executableSegment(textSeg)) {
        textBase = textSeg.vaddr;
    }

    const uint8_t* hdrStart = reinterpret_cast<const uint8_t*>(hdrSeg.vaddr);
    const uint8_t* hdrEnd = hdrStart + hdrSeg.memSize;
    if (hdrEnd - hdrStart < 4) {
        return;
    }

    const uint8_t* cursor = hdrStart + 4;
    uintptr_t ehFrameAddr = 0;
    if (!readEncodedPointer(cursor, hdrEnd, hdrStart[1], reinterpret_cast<uintptr_t>(cursor),
                            hdrSeg.vaddr, textBase, ehFrameAddr)) {
        return;
    }
    if (ehFrameAddr == 0) {
        return;
    }

    // Bound the walk by the PT_LOAD that contains .eh_frame.
    Segment ehSeg;
    bool haveSegment = false;
    for (const Segment& seg : image.segments()) {
        if (ehFrameAddr >= seg.vaddr && ehFrameAddr < seg.vaddr + seg.memSize) {
            ehSeg = seg;
            haveSegment = true;
            break;
        }
    }
    if (!haveSegment) {
        return;
    }

    const uintptr_t textStart = textBase;
    const uintptr_t textEnd = textSeg.vaddr + textSeg.memSize;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(ehFrameAddr);
    const uint8_t* limit = reinterpret_cast<const uint8_t*>(ehSeg.vaddr + ehSeg.memSize);
    if (limit > hdrEnd) {
        limit = hdrEnd;
    }

    size_t scanned = 0;
    constexpr size_t kMaxScan = 64u * 1024u * 1024u;

    while (p + 4 <= limit && scanned < kMaxScan) {
        uint32_t length = static_cast<uint32_t>(readLE(p, 4));
        size_t headerSize = 4;

        if (length == 0) {
            break;
        }
        if (length == 0xFFFFFFFFu) {
            if (p + 12 > limit) {
                break;
            }
            uint64_t len64 = readLE(p + 4, 8);
            if (len64 > static_cast<uint64_t>(limit - p - 12)) {
                break;
            }
            length = static_cast<uint32_t>(len64);
            headerSize = 12;
        }

        if (length < 4 || length > static_cast<uint32_t>(limit - p - headerSize)) {
            break;
        }

        const uint8_t* body = p + headerSize;
        const uint8_t* next = body + length;
        scanned += length + headerSize;

        uint32_t cieId = static_cast<uint32_t>(readLE(body, 4));
        if (cieId != 0) {
            // FDE: [CiePointer][InitialLocation][AddressRange].
            //
            // The initial-location encoding lives in the CIE. For every Android
            // and GNU toolchain target it is pcrel|sdata4, so decode that and
            // reject anything that does not land inside .text — a wrong guess
            // then costs nothing instead of corrupting the table.
            const uint8_t* field = body + 4;
            if (next - field >= 4) {
                int64_t rel = signExtend(readLE(field, 4), 4);
                uintptr_t pc = reinterpret_cast<uintptr_t>(field) + static_cast<uintptr_t>(rel);
                if (pc >= textStart && pc < textEnd) {
                    starts_.push_back(pc);
                }
            }
        }

        p = next;
    }
}

bool FunctionTable::isFunctionStart(uintptr_t addr) const {
    if (starts_.empty()) {
        return false;
    }
    return std::binary_search(starts_.begin(), starts_.end(), addr);
}

uintptr_t FunctionTable::nextStartAfter(uintptr_t addr) const {
    auto it = std::upper_bound(starts_.begin(), starts_.end(), addr);
    return it == starts_.end() ? 0 : *it;
}

uintptr_t FunctionTable::startAtOrBefore(uintptr_t addr) const {
    auto it = std::upper_bound(starts_.begin(), starts_.end(), addr);
    if (it == starts_.begin()) {
        return 0;
    }
    --it;
    return *it;
}

}  // namespace coui
