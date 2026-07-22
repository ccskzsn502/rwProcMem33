/* Compile HWBP subsystem into the merged .ko (no own init_module / proc). */
#ifndef CONFIG_MERGED_MODULE
#define CONFIG_MERGED_MODULE 1
#endif
#include "hwbp/hwBreakpointProc_module.c"
