# K60 Pro GKI 5.15 CI

## Device
- Redmi K60 Pro
- uname: `5.15.189-android13-8-00012-g742fd6fce289-ab14327796`
- Google GKI branch: `android13-5.15`, commit: `742fd6fce289`

## Fork
https://github.com/ccskzsn502/rwProcMem33

## Run
1. https://github.com/ccskzsn502/rwProcMem33/actions
2. Workflow: **Build GKI 5.15 Modules (K60 Pro)**
3. Run workflow -> download `k60pro-gki515-ko` and `k60pro-userspace-arm64`

## Version macros
Both modules use `KERNEL_VERSION(5,15,189)`.

## Load (root)
```bash
adb push *.ko /data/local/tmp/
adb shell su -c "insmod /data/local/tmp/rwProcMem_module.ko"
adb shell su -c "insmod /data/local/tmp/hwBreakpointProc_module.ko"
adb shell su -c "dmesg | tail -50"
```

## Note
CI builds OOT modules against Google `kernel/common` with **CFI+LTO enabled** to match the K60 Pro GKI image.
If `insmod` still fails with `disagrees about version of symbol module_layout`, the OEM `Module.symvers` CRC differs from Google common; need the exact device/GKI `Module.symvers` used for `ab14327796`.