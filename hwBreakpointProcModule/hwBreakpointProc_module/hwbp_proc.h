#ifndef _HWBP_PROC_H_
#define _HWBP_PROC_H_
#include "ver_control.h"

#define HWBP_MAX_STACK_FRAMES 16

#pragma pack(1)
struct my_user_pt_regs {
	uint64_t regs[31];
	uint64_t sp;
	uint64_t pc;
	uint64_t pstate;
	uint64_t orig_x0;
	uint64_t syscallno;
};
struct HWBP_HIT_ITEM {
	uint64_t task_id;
	uint64_t hit_addr;
	uint64_t hit_time;
	struct my_user_pt_regs regs_info;
	uint64_t bp_addr;
	uint32_t hit_type;
	uint32_t stack_count;
	uint64_t stack_pcs[HWBP_MAX_STACK_FRAMES];
};
#pragma pack()

struct HWBP_HANDLE_INFO {
	uint64_t task_id;
	struct perf_event * sample_hbp;
	struct perf_event_attr original_attr;
	bool is_32bit_task;
#ifdef CONFIG_MODIFY_HIT_NEXT_MODE
	struct perf_event_attr next_instruction_attr;
#endif
	size_t hit_total_count;
	cvector hit_item_arr;
};

#endif