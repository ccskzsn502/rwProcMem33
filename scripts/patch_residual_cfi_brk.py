#!/usr/bin/env python3
"""Neutralize residual GKI CFI trap stubs in merged_module.ko.

Even with -fno-sanitize=cfi and a source __cfi_check landing, the GKI Clang
pipeline can still emit weak residual stubs like:

  __cfi_check.NN:   paciasp; brk #1
  __cfi_check_fail: ... brk #0x5502

Those run before/around init and hard-reboot the phone. This post-link pass
rewrites residual BRKs inside CFI-related symbols (and any paciasp;brk pairs)
to RET so insmod can reach init / runtime slowpath bypass.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

PACIASP = bytes.fromhex("3f2303d5")
BRK1 = bytes.fromhex("200020d4")
RET = bytes.fromhex("c0035fd6")
# common Clang CFI fail traps
BRK_TRAPS = {
    bytes.fromhex("200020d4"),  # brk #1
    bytes.fromhex("40a02ad4"),  # brk #0x5502
    bytes.fromhex("40a22ad4"),  # brk #0x5512
}


def _sh(p: bytes, e_shoff: int, e_shentsize: int, i: int):
    o = e_shoff + i * e_shentsize
    return struct.unpack_from("<IIQQQQIIQQ", p, o)


def patch(path: Path, inplace: bool = True) -> int:
    raw = bytearray(path.read_bytes())
    if raw[:4] != b"\x7fELF":
        print(f"not ELF: {path}", file=sys.stderr)
        return 2

    e_shoff = struct.unpack_from("<Q", raw, 40)[0]
    e_shentsize = struct.unpack_from("<H", raw, 58)[0]
    e_shnum = struct.unpack_from("<H", raw, 60)[0]
    e_shstrndx = struct.unpack_from("<H", raw, 62)[0]
    shstr = _sh(raw, e_shoff, e_shentsize, e_shstrndx)
    strtab = bytes(raw[shstr[4] : shstr[4] + shstr[5]])

    secs = {}
    sym = st = None
    for i in range(e_shnum):
        name, typ, flags, addr, off, size, link, info, addralign, entsize = _sh(
            raw, e_shoff, e_shentsize, i
        )
        n = strtab[name : strtab.find(b"\x00", name)].decode()
        secs[i] = (n, addr, off, size)
        if n == ".symtab":
            sym = (off, size)
        if n == ".strtab":
            st = (off, size)
    if not sym or not st:
        print("missing .symtab/.strtab", file=sys.stderr)
        return 2

    patched = 0

    # 1) every paciasp; brk#1 pair → paciasp; ret
    i = 0
    while True:
        j = bytes(raw).find(PACIASP + BRK1, i)
        if j < 0:
            break
        raw[j + 4 : j + 8] = RET
        patched += 1
        print(f"paciasp;brk -> paciasp;ret @ {j:#x}")
        i = j + 4

    # 2) any brk trap inside *cfi_check* symbols → ret
    s = bytes(raw[st[0] : st[0] + st[1]])
    for j in range(0, sym[1], 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
            "<IBBHQQ", raw, sym[0] + j
        )
        nm = s[st_name : s.find(b"\x00", st_name)].decode(errors="replace")
        if "cfi_check" not in nm:
            continue
        if st_shndx == 0 or st_shndx not in secs:
            continue
        sn, sa, so, ss = secs[st_shndx]
        fo = so + st_value - sa
        size = st_size or 16
        for k in range(0, size, 4):
            op = bytes(raw[fo + k : fo + k + 4])
            if op in BRK_TRAPS:
                raw[fo + k : fo + k + 4] = RET
                patched += 1
                print(f"{nm}+{k:#x}: brk -> ret")

    out = path if inplace else path.with_name(path.stem + "_fixed" + path.suffix)
    out.write_bytes(raw)
    print(f"patched={patched} wrote={out}")
    # hard fail if residual paciasp;brk remains
    if PACIASP + BRK1 in raw:
        print("residual paciasp;brk still present", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("ko", type=Path)
    ap.add_argument("--out", type=Path, default=None, help="write to path instead of inplace")
    args = ap.parse_args()
    if args.out:
        args.out.write_bytes(args.ko.read_bytes())
        return patch(args.out, inplace=True)
    return patch(args.ko, inplace=True)


if __name__ == "__main__":
    sys.exit(main())
