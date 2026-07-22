# rwProcMem33 — merged GKI module

Single kernel module: process R/W, in-kernel memory search, hardware breakpoints.

## Layout

```
kernel/
  Makefile           # builds merged_module.ko
  merged_main.c      # init + single /proc node + CMD router
  rw_core.c          # R/W + search subsystem
  hwbp_core.c        # HWBP subsystem
  rw/                # R/W + search sources
  hwbp/              # HWBP sources
```

## Load (lsnbm-style)

1. Build empty-CRC `.ko` (CI does this; hide `Module.symvers` + `KBUILD_MODPOST_WARN=1`).
2. `insmod merged_module.ko` as root — **no post-link Python**.
3. Init runs CFI slowpath bypass, then creates one `/proc/<key>/<key>` node.

Auth key: `e84523d7b60d5d341a7c4d1861773ecd`  
CMDs: R/W+search `1..14`, HWBP `100..111`.

## Build kernel tree

Google Android GKI: `https://android.googlesource.com/kernel/common`  
Branch `android13-5.15`, commit pin for CI cache: `742fd6fce289`.  
API branches follow `LINUX_VERSION_CODE` from the build tree (no hard pin in source).

CI workflow: `.github/workflows/build-gki515.yml` → artifact `merged-module-gki515`.
