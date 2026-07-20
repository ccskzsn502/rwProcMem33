#!/usr/bin/env python3
"""Verify merged_module.ko has a usable __cfi_check PAC landing, not residual BRK traps."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

PACIASP = bytes.fromhex("3f2303d5")
PACIASP_AUTIASP_RET = bytes.fromhex("3f2303d5bf2303d5c0035fd6")
BRK = bytes.fromhex("200020d4")
BRK_TRAPS = {
    bytes.fromhex("200020d4"),  # brk #1
    bytes.fromhex("40a02ad4"),  # brk #0x5502
    bytes.fromhex("40a22ad4"),  # brk #0x5512
}


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
    secs = {}
    for i in range(e_shnum):
        name, typ, flags, addr, off, size, link, info, addralign, entsize = sh(i)
        n = strtab[name : strtab.find(b"\x00", name)].decode()
        secs[i] = (n, addr, off, size)
        if n == ".symtab":
            sym = (off, size)
        if n == ".strtab":
            st = (off, size)
    if not sym or not st:
        print("missing .symtab/.strtab", file=sys.stderr)
        return 2

    # Residual weak stubs (__cfi_check.NN = paciasp;brk) hard-reboot on load.
    if PACIASP + BRK in p:
        print("residual paciasp;brk present anywhere in ko", file=sys.stderr)
        return 1

    soff, ssize = sym
    stroff, strsz = st
    s = p[stroff : stroff + strsz]
    found_main = False
    residual_names: list[str] = []
    for j in range(0, ssize, 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
            "<IBBHQQ", p, soff + j
        )
        nm = s[st_name : s.find(b"\x00", st_name)].decode(errors="replace")
        if "cfi_check" not in nm:
            continue
        # Numbered compiler stubs: __cfi_check.18 / __cfi_check_fail.17
        if nm not in ("__cfi_check", "__cfi_check_fail") and (
            nm.startswith("__cfi_check.") or nm.startswith("__cfi_check_fail.")
        ):
            residual_names.append(nm)
        if st_shndx == 0 or st_shndx not in secs:
            continue
        sn, sa, so, ss = secs[st_shndx]
        fileoff = so + st_value - sa
        size = st_size or 16
        body = p[fileoff : fileoff + size]
        for k in range(0, len(body), 4):
            if body[k : k + 4] in BRK_TRAPS:
                print(f"residual BRK trap in {nm}+{k:#x}", file=sys.stderr)
                return 1
        if nm == "__cfi_check":
            found_main = True
            blob = body[:16]
            print(f"__cfi_check size={st_size} bytes={blob.hex()}")
            if st_size < 12:
                print(f"__cfi_check too small: {st_size}", file=sys.stderr)
                return 1
            if blob[:12] != PACIASP_AUTIASP_RET and blob[4:16] != PACIASP_AUTIASP_RET:
                # allow BTI + paciasp;autiasp;ret
                if not (blob[:4] == bytes.fromhex("5f2403d5") and blob[4:16] == PACIASP_AUTIASP_RET):
                    print("WARN: unexpected __cfi_check prologue, size ok")
            print("cfi_check_ok")

    if residual_names:
        print(
            "residual numbered CFI stubs (compile flags incomplete): "
            + ", ".join(sorted(set(residual_names))),
            file=sys.stderr,
        )
        return 1
    if not found_main:
        print("__cfi_check symbol missing", file=sys.stderr)
        return 1
    print("no residual cfi brk stubs")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} merged_module.ko", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(Path(sys.argv[1])))
