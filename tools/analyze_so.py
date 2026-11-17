#!/usr/bin/env python3
"""Offline cross-check of the runtime discovery algorithm against a real
libminecraftpe.so, without loading the game.

Mirrors src/discover.cpp: collect route strings from read-only segments, find
`lea reg, [rip+disp]` references in .text, group them per function using the
.eh_frame_hdr binary search table, and rank by distinct route count.

Usage: python3 tools/analyze_so.py /path/to/libminecraftpe.so
"""
from __future__ import annotations

import struct
import sys
from collections import defaultdict

ROUTE_HINTS = [
    b"/oreui-marketplace/search",
    b"/oreui-marketplace/wishlist",
    b"/start-from-template",
    b"/create-new-world",
    b"/player-permissions",
    b"/send-invites",
    b"/gameplay/bedtime",
    b"/pre-start",
    b"/main-menu",
]


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"\x7fELF":
            raise SystemExit("not an ELF")
        if d[4] != 2:
            raise SystemExit("not ELF64")
        self.e_phoff, = struct.unpack_from("<Q", d, 0x20)
        self.e_phentsize, self.e_phnum = struct.unpack_from("<HH", d, 0x36)
        self.segments = []   # (vaddr, filesz, memsz, flags)
        self.eh_frame_hdr = None
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            p_type, p_flags = struct.unpack_from("<II", d, off)
            p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from(
                "<QQQQQ", d, off + 8
            )
            if p_type == 1:  # PT_LOAD
                self.segments.append((p_vaddr, p_offset, p_filesz, p_memsz, p_flags))
            elif p_type == 0x6474E550:  # PT_GNU_EH_FRAME
                self.eh_frame_hdr = (p_vaddr, p_offset, p_filesz)

    def vaddr_to_offset(self, vaddr):
        for p_vaddr, p_offset, p_filesz, p_memsz, flags in self.segments:
            if p_vaddr <= vaddr < p_vaddr + p_memsz:
                return p_offset + (vaddr - p_vaddr)
        return None

    def read_at_vaddr(self, vaddr, n):
        off = self.vaddr_to_offset(vaddr)
        if off is None:
            return None
        return self.data[off:off + n]

    def exec_segment(self):
        for seg in self.segments:
            if seg[4] & 1:
                return seg
        return None


def read_uleb(buf, pos):
    result = 0
    shift = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            break
        shift += 7
    return result, pos


def read_sleb(buf, pos):
    result = 0
    shift = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            if b & 0x40:
                result -= 1 << shift
            break
    return result, pos


def decode_eh_ptr(buf, pos, enc, field_vaddr, datarel_base, text_base):
    if enc == 0xFF:
        return 0, pos
    fmt = enc & 0x0F
    if fmt == 0x00:
        val, = struct.unpack_from("<Q", buf, pos); pos += 8
    elif fmt == 0x01:
        val, pos = read_uleb(buf, pos)
    elif fmt == 0x02:
        val, = struct.unpack_from("<H", buf, pos); pos += 2
    elif fmt == 0x03:
        val, = struct.unpack_from("<I", buf, pos); pos += 4
    elif fmt == 0x04:
        val, = struct.unpack_from("<Q", buf, pos); pos += 8
    elif fmt == 0x09:
        val, pos = read_sleb(buf, pos); val &= (1 << 64) - 1
    elif fmt == 0x0A:
        val, = struct.unpack_from("<h", buf, pos); pos += 2; val &= (1 << 64) - 1
    elif fmt == 0x0B:
        val, = struct.unpack_from("<i", buf, pos); pos += 4; val &= (1 << 64) - 1
    elif fmt == 0x0C:
        val, = struct.unpack_from("<q", buf, pos); pos += 8; val &= (1 << 64) - 1
    else:
        raise ValueError(f"unsupported format {fmt:#x}")

    app = enc & 0x70
    if app == 0x10:      # pcrel
        val += field_vaddr
    elif app == 0x30:    # datarel
        val += datarel_base
    elif app == 0x20:    # textrel
        val += text_base
    return val, pos


def function_starts(elf):
    if elf.eh_frame_hdr is None:
        return []
    vaddr, offset, filesz = elf.eh_frame_hdr
    buf = elf.data[offset:offset + filesz]
    if buf[0] != 1:
        return []
    enc_ptr, enc_cnt, enc_tab = buf[1], buf[2], buf[3]
    text = elf.exec_segment()
    text_base = text[0] if text else 0
    pos = 4
    eh_frame_addr, pos = decode_eh_ptr(buf, pos, enc_ptr, vaddr + pos, vaddr, text_base)
    count, pos = decode_eh_ptr(buf, pos, enc_cnt, vaddr + pos, vaddr, text_base)

    starts = []
    table_off = pos
    stride_probe = pos
    _, stride_probe = decode_eh_ptr(buf, stride_probe, enc_tab, vaddr + stride_probe, vaddr, text_base)
    stride = stride_probe - pos
    for i in range(count):
        p = table_off + i * stride * 2
        if p + stride * 2 > len(buf):
            break
        loc, p2 = decode_eh_ptr(buf, p, enc_tab, vaddr + p, vaddr, text_base)
        if loc:
            starts.append(loc)
    starts.sort()
    return starts


def is_route(raw):
    if not raw or raw[0:1] != b"/":
        return False
    if len(raw) < 4 or len(raw) > 120:
        return False
    if b"%" in raw:
        return False
    return all(
        (0x30 <= b <= 0x39) or (0x61 <= b <= 0x7A) or (0x41 <= b <= 0x5A)
        or b in b"/_-.:"
        for b in raw
    )


def collect_routes(elf):
    routes = {}
    for p_vaddr, p_offset, p_filesz, p_memsz, p_flags in elf.segments:
        # Must be readable; do NOT exclude executable segments. This binary has a
        # single `R E` LOAD holding both .text and the read-only data.
        if not (p_flags & 4):
            continue
        blob = elf.data[p_offset:p_offset + p_filesz]
        i = 0
        n = len(blob)
        while i < n:
            if not (0x20 <= blob[i] <= 0x7E):
                i += 1
                continue
            start = i
            while i < n and 0x20 <= blob[i] <= 0x7E:
                i += 1
            if i < n and blob[i] == 0:
                raw = blob[start:i]
                if is_route(raw):
                    routes[p_vaddr + start] = raw.decode("ascii", "replace")
            if i < n:
                i += 1
    return routes


def find_refs(elf, routes, starts):
    text = elf.exec_segment()
    if text is None:
        return []
    t_vaddr, t_offset, t_filesz, t_memsz, _ = text
    blob = elf.data[t_offset:t_offset + t_filesz]

    import bisect
    refs = []
    n = len(blob)
    for i in range(n - 6):
        if blob[i] != 0x8D:
            continue
        modrm = blob[i + 1]
        if (modrm & 0xC7) != 0x05:
            continue
        insn_off = i
        insn_len = 6
        if i >= 1 and (blob[i - 1] & 0xF0) == 0x40:
            insn_off = i - 1
            insn_len = 7
        disp, = struct.unpack_from("<i", blob, i + 2)
        target = t_vaddr + insn_off + insn_len + disp
        if target not in routes:
            continue
        abs_addr = t_vaddr + insn_off
        idx = bisect.bisect_right(starts, abs_addr) - 1
        if idx < 0:
            continue
        refs.append((starts[idx], target))
    return refs


def main():
    path = sys.argv[1]
    elf = Elf(path)
    print(f"segments: {len(elf.segments)}")
    text = elf.exec_segment()
    print(f".text: vaddr={text[0]:#x} size={text[3]:#x}")

    starts = function_starts(elf)
    print(f"function starts from .eh_frame_hdr: {len(starts)}")

    routes = collect_routes(elf)
    print(f"route-like strings: {len(routes)}")

    hints_found = [h.decode() for h in ROUTE_HINTS if any(h in r.encode() for r in routes.values())]
    print(f"anchor hints present: {len(hints_found)}/{len(ROUTE_HINTS)}")
    for h in hints_found:
        print(f"   + {h}")

    refs = find_refs(elf, routes, starts)
    print(f"lea references into route strings: {len(refs)}")

    per_fn = defaultdict(set)
    for fn, target in refs:
        per_fn[fn].add(target)

    # Rank by TOP-LEVEL screen routes ("/main-menu"), not by raw reference count.
    # Measured on 1.26.62.1: the function with the most route references overall
    # was a name mapper that also touches in-world routes like
    # "/gameplay/block-actors/anvil". The registration function is the one that
    # covers the most single-segment screens.
    def is_top_level(name):
        return name.find("/", 1) == -1

    entry_set = set(starts)
    ranked = []
    for fn, tgts in per_fn.items():
        if fn not in entry_set:
            continue  # must be a real function entry, not the middle of one
        names = [routes[t] for t in tgts]
        ranked.append((fn, sum(1 for n in names if is_top_level(n)), len(names), names))
    ranked.sort(key=lambda r: (-r[1], -r[2]))

    print("\ntop candidates (ranked by top-level screen routes):")
    print(f"  {'address':>12}  {'top':>4} {'all':>4}  sample")
    for fn, top, total, names in ranked[:8]:
        top_names = [n for n in names if is_top_level(n)]
        print(f"  {fn:#12x}  {top:4d} {total:4d}  {', '.join(top_names[:5])}")

    if ranked:
        best_fn, best_top, best_total, names = ranked[0]
        print(f"\ndiscovered: {best_fn:#x}  top-level screens={best_top}  routes={best_total}")
        pro = elf.read_at_vaddr(best_fn, 16)
        print(f"prologue bytes: {pro[:12].hex(' ')}")


if __name__ == "__main__":
    main()
