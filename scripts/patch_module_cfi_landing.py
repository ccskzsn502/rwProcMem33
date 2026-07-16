#!/usr/bin/env python3
"""Patch module __cfi_check / __cfi_check_fail for PAC/BTI-safe accept-all.

On GKI 5.15 CFI kernels, the loader invokes the module's __cfi_check before
calling init_module. Our ThinLTO OOT __cfi_check is incomplete vs OEM type
hashes; a bare RET also reboots because the call site expects a PAC-compatible
landing pad.

Device-proven working stub (K60 Pro / 5.15.189):
  paciasp ; autiasp ; ret

Usage:
  python3 scripts/patch_module_cfi_landing.py path/to/module.ko [more.ko...]
  python3 scripts/patch_module_cfi_landing.py --in-place out/ko/*.ko
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


# AArch64: paciasp; autiasp; ret
LANDING_RET = bytes.fromhex("3f2303d5 bf2303d5 c0035fd6")
# Optional fail path can use the same landing.
TARGETS = ("__cfi_check", "__cfi_check_fail")


def read_elf_syms(data: bytes):
    if data[:4] != b"\x7fELF":
        raise ValueError("not ELF")
    ei_class = data[4]
    ei_data = data[5]
    if ei_class != 2 or ei_data != 1:
        raise ValueError("only little-endian ELF64 supported")

    e_shoff = struct.unpack_from("<Q", data, 40)[0]
    e_shentsize = struct.unpack_from("<H", data, 58)[0]
    e_shnum = struct.unpack_from("<H", data, 60)[0]
    e_shstrndx = struct.unpack_from("<H", data, 62)[0]

    def shdr(i: int):
        off = e_shoff + i * e_shentsize
        # name, type, flags, addr, offset, size, link, info, addralign, entsize
        return struct.unpack_from("<IIQQQQIIQQ", data, off)

    shstr_off = shdr(e_shstrndx)[4]
    shstr_size = shdr(e_shstrndx)[5]
    shstr = data[shstr_off : shstr_off + shstr_size]

    def sec_name(name_off: int) -> str:
        end = shstr.find(b"\x00", name_off)
        if end < 0:
            end = len(shstr)
        return shstr[name_off:end].decode("utf-8", "replace")

    sections = []
    symtab_idx = None
    for i in range(e_shnum):
        n, t, fl, addr, off, size, link, info, al, es = shdr(i)
        name = sec_name(n)
        sections.append(
            {
                "name": name,
                "type": t,
                "offset": off,
                "size": size,
                "link": link,
                "entsize": es,
            }
        )
        if name == ".symtab" and t == 2:  # SHT_SYMTAB
            symtab_idx = i

    if symtab_idx is None:
        raise ValueError("no .symtab")

    st = sections[symtab_idx]
    strtab = sections[st["link"]]
    strdata = data[strtab["offset"] : strtab["offset"] + strtab["size"]]
    entsize = st["entsize"] or 24
    count = st["size"] // entsize
    syms = []
    for i in range(count):
        off = st["offset"] + i * entsize
        # st_name, st_info, st_other, st_shndx, st_value, st_size
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
            "<IBBHQQ", data, off
        )
        end = strdata.find(b"\x00", st_name)
        if end < 0:
            end = len(strdata)
        name = strdata[st_name:end].decode("utf-8", "replace")
        syms.append(
            {
                "name": name,
                "shndx": st_shndx,
                "value": st_value,
                "size": st_size,
                "info": st_info,
            }
        )
    return sections, syms


def file_off(sections, sym) -> int | None:
    shndx = sym["shndx"]
    if not isinstance(shndx, int) or shndx == 0 or shndx >= len(sections):
        return None
    sec = sections[shndx]
    return sec["offset"] + sym["value"]


def patch_ko(path: Path, in_place: bool, suffix: str) -> bool:
    raw = bytearray(path.read_bytes())
    sections, syms = read_elf_syms(bytes(raw))
    patched_any = False
    for target in TARGETS:
        matches = [
            s
            for s in syms
            if s["name"] == target and s["size"] > 0 and isinstance(s["shndx"], int)
        ]
        if not matches:
            print(f"  SKIP {path.name}: no symbol {target}")
            continue
        for s in matches:
            off = file_off(sections, s)
            if off is None:
                print(f"  SKIP {path.name}: {target} bad section")
                continue
            if s["size"] < len(LANDING_RET):
                print(
                    f"  SKIP {path.name}: {target} size 0x{s['size']:x} < landing stub"
                )
                continue
            old = bytes(raw[off : off + min(16, s["size"])])
            # Already patched?
            if raw[off : off + len(LANDING_RET)] == LANDING_RET:
                print(f"  OK   {path.name}: {target} already landing-pad @0x{off:x}")
                patched_any = True
                continue
            raw[off : off + len(LANDING_RET)] = LANDING_RET
            print(
                f"  PATCH {path.name}: {target} file=0x{off:x} size=0x{s['size']:x} "
                f"old={old.hex()} new={LANDING_RET.hex()}"
            )
            patched_any = True
            break
    if not patched_any:
        print(f"  FAIL {path.name}: nothing patched")
        return False

    if in_place:
        out = path
    else:
        out = path.with_name(path.stem + suffix + path.suffix)
    out.write_bytes(raw)
    print(f"  wrote {out} ({len(raw)} bytes)")
    return True


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("kos", nargs="+", type=Path, help="module .ko files")
    ap.add_argument(
        "--in-place",
        action="store_true",
        help="overwrite input files (default for CI)",
    )
    ap.add_argument(
        "--suffix",
        default=".landing",
        help="output suffix when not --in-place (default: .landing)",
    )
    args = ap.parse_args(argv)

    ok = True
    for ko in args.kos:
        if not ko.is_file():
            print(f"missing {ko}", file=sys.stderr)
            ok = False
            continue
        try:
            if not patch_ko(ko, args.in_place, args.suffix):
                ok = False
        except Exception as e:
            print(f"error {ko}: {e}", file=sys.stderr)
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
