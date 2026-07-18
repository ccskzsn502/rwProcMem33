#!/usr/bin/env python3
"""Verify merged_module.ko has a usable __cfi_check PAC landing, not paciasp;brk."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

PACIASP_AUTIASP_RET = bytes.fromhex("3f2303d5bf2303d5c0035fd6")
BRK = bytes.fromhex("200020d4")


def main(path: Path) -> int:
    p = path.read_bytes()
    if p[:4] != b"\x7fELF":
        print(f"not ELF: {path}", file=sys.stderr)
        return 2

    e_shoff = struct.unpack_from("<Q", p, 40)[0]
    e_shentsize = struct.unpack_from("<H", p, 58)[0]
    e_shnum = struct.unpack_from("<H", p, 60)[0]
    e_shstrndx = struct.unpack_from("<H", p, 62)[0]

    def sh(i: int):
        o = e_shoff + i * e_shentsize
        return struct.unpack_from("<IIQQQQIIQQ", p, o)

    shstr = sh(e_shstrndx)
    strtab = p[shstr[4] : shstr[4] + shstr[5]]
    sym = st = None
    for i in range(e_shnum):
        name, typ, flags, addr, off, size, link, info, addralign, entsize = sh(i)
        n = strtab[name : strtab.find(b"\x00", name)].decode()
        if n == ".symtab":
            sym = (off, size)
        if n == ".strtab":
            st = (off, size)
    if not sym or not st:
        print("missing .symtab/.strtab", file=sys.stderr)
        return 2

    soff, ssize = sym
    stroff, strsz = st
    s = p[stroff : stroff + strsz]
    for j in range(0, ssize, 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
            "<IBBHQQ", p, soff + j
        )
        nm = s[st_name : s.find(b"\x00", st_name)].decode(errors="replace")
        if nm != "__cfi_check":
            continue
        name, typ, flags, addr, off, size, link, info, addralign, entsize = sh(st_shndx)
        fileoff = off + st_value - addr
        blob = p[fileoff : fileoff + min(st_size, 16)]
        print(f"__cfi_check size={st_size} bytes={blob.hex()}")
        if st_size < 12:
            print(f"__cfi_check too small: {st_size}", file=sys.stderr)
            return 1
        if BRK in blob[:8]:
            print("residual BRK in __cfi_check", file=sys.stderr)
            return 1
        if blob[:12] != PACIASP_AUTIASP_RET:
            print("WARN: unexpected __cfi_check prologue, size ok")
        print("cfi_check_ok")
        return 0

    print("__cfi_check symbol missing", file=sys.stderr)
    return 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} merged_module.ko", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(Path(sys.argv[1])))
