#ifndef PROC_SEARCH_H_
#define PROC_SEARCH_H_

#include <linux/types.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/sched/mm.h>
#include <linux/rcupdate.h>
#include <linux/sched/task.h>

#include "api_proxy.h"
#include "handle_table.h"
#include "mirror.h"
#include "proc_maps_auto_offset.h"
#include "ver_control.h"

/* Keep in sync with userspace MemoryReaderWriter39.h */
#define SEARCH_VAL_U8     1
#define SEARCH_VAL_U16    2
#define SEARCH_VAL_U32    4
#define SEARCH_VAL_U64    8
#define SEARCH_VAL_FLOAT  0x14
#define SEARCH_VAL_DOUBLE 0x18
#define SEARCH_VAL_BYTES  0x20

#define SEARCH_PROT_R   1
#define SEARCH_PROT_W   2
#define SEARCH_PROT_X   4
#define SEARCH_PROT_ANY 0x7

#define SEARCH_FLAG_FORCE_READ (1u << 0)
#define SEARCH_FLAG_HAS_MASK   (1u << 1)

#define SEARCH_TRUNCATED_BIT   (1ull << 63)
#define SEARCH_MAX_VALUE_SIZE  64
#define SEARCH_DEFAULT_MAX_HITS 4096
#define SEARCH_HARD_MAX_HITS   65536
#define SEARCH_CHUNK_SIZE      (64UL * 1024UL)

#pragma pack(push, 1)
struct search_request {
	uint64_t start_addr;
	uint64_t end_addr;
	uint32_t value_type;
	uint32_t value_size;
	uint32_t alignment;
	uint32_t max_results;
	uint32_t prot_mask;
	uint32_t flags;
};
#pragma pack(pop)

static inline uint32_t search_default_align(uint32_t value_type, uint32_t value_size)
{
	switch (value_type) {
	case SEARCH_VAL_U8:
		return 1;
	case SEARCH_VAL_U16:
		return 2;
	case SEARCH_VAL_U32:
	case SEARCH_VAL_FLOAT:
		return 4;
	case SEARCH_VAL_U64:
	case SEARCH_VAL_DOUBLE:
		return 8;
	default:
		if (value_size >= 8)
			return 4;
		if (value_size >= 4)
			return 4;
		if (value_size >= 2)
			return 2;
		return 1;
	}
}

static inline int search_prot_ok(unsigned long vm_flags, uint32_t prot_mask)
{
	uint32_t have = 0;

	if (!prot_mask || prot_mask == SEARCH_PROT_ANY)
		return 1;
	if (vm_flags & VM_READ)
		have |= SEARCH_PROT_R;
	if (vm_flags & VM_WRITE)
		have |= SEARCH_PROT_W;
	if (vm_flags & VM_EXEC)
		have |= SEARCH_PROT_X;
	/* Require all bits set in mask to be present on the VMA. */
	return (have & prot_mask) == prot_mask;
}

static inline int search_bytes_match(const u8 *hay, const u8 *needle,
				     const u8 *mask, uint32_t n)
{
	uint32_t i;

	if (!mask) {
		for (i = 0; i < n; i++) {
			if (hay[i] != needle[i])
				return 0;
		}
		return 1;
	}
	for (i = 0; i < n; i++) {
		if ((hay[i] & mask[i]) != (needle[i] & mask[i]))
			return 0;
	}
	return 1;
}

/*
 * In-kernel memory scan. Buffer layout (same buffer used for in/out):
 *  IN:  search_request + value[+mask]
 *  OUT: uint64_t count_word (bit63=truncated) + count * uint64_t addresses
 *
 * Returns 0 on success, negative errno on hard failure.
 * Soft failures (no hits) still return 0 with count=0.
 */
static inline ssize_t do_proc_search_memory(struct pid *pid_struct,
					   char __user *buf,
					   size_t buf_size)
{
	struct search_request req;
	u8 value[SEARCH_MAX_VALUE_SIZE];
	u8 mask[SEARCH_MAX_VALUE_SIZE];
	u8 *chunk = NULL;
	uint64_t *hits = NULL;
	uint64_t hit_count = 0;
	uint64_t count_word;
	bool truncated = false;
	bool force;
	bool has_mask;
	uint32_t align;
	size_t need_in;
	struct task_struct *task;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	ssize_t ret = 0;

	if (!pid_struct || !buf)
		return -EINVAL;
	if (buf_size < sizeof(struct search_request) + 1)
		return -EINVAL;

	if (x_copy_from_user(&req, buf, sizeof(req)))
		return -EFAULT;

	if (req.value_size == 0 || req.value_size > SEARCH_MAX_VALUE_SIZE)
		return -EINVAL;
	if (req.max_results == 0)
		req.max_results = SEARCH_DEFAULT_MAX_HITS;
	if (req.max_results > SEARCH_HARD_MAX_HITS)
		req.max_results = SEARCH_HARD_MAX_HITS;

	has_mask = (req.flags & SEARCH_FLAG_HAS_MASK) != 0;
	force = (req.flags & SEARCH_FLAG_FORCE_READ) != 0;
	need_in = sizeof(req) + req.value_size + (has_mask ? req.value_size : 0);
	if (buf_size < need_in)
		return -EINVAL;
	/* Output needs count_word + max_results addresses. */
	if (buf_size < sizeof(uint64_t) + (size_t)req.max_results * sizeof(uint64_t))
		return -EINVAL;

	if (x_copy_from_user(value, buf + sizeof(req), req.value_size))
		return -EFAULT;
	if (has_mask) {
		if (x_copy_from_user(mask, buf + sizeof(req) + req.value_size, req.value_size))
			return -EFAULT;
	}

	align = req.alignment;
	if (align == 0)
		align = search_default_align(req.value_type, req.value_size);
	if (align == 0 || (align & (align - 1)))
		align = 1;

	hits = x_kmalloc(sizeof(uint64_t) * req.max_results, GFP_KERNEL);
	chunk = x_kmalloc(SEARCH_CHUNK_SIZE, GFP_KERNEL);
	if (!hits || !chunk) {
		ret = -ENOMEM;
		goto out_free;
	}

	task = get_task_from_pid_struct(pid_struct);
	if (!task) {
		ret = -ESRCH;
		goto out_free;
	}
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm) {
		ret = -ESRCH;
		goto out_free;
	}

	if (down_read_mmap_lock(mm) != 0) {
		mmput(mm);
		ret = -EBUSY;
		goto out_free;
	}

	/*
	 * Snapshot VMA ranges under lock into a small stack list of intervals,
	 * then drop lock and pin/copy outside (mirror path takes its own locks).
	 * For simplicity and low memory, walk VMAs once and scan each range
	 * while still holding the read lock only for enumeration; actual page
	 * pin happens in mirror which re-validates.
	 */
	{
		struct {
			unsigned long start;
			unsigned long end;
			unsigned long flags;
		} *ranges = NULL;
		size_t nranges = 0, cap = 0;
#if MY_LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
		VMA_ITERATOR(vmi, mm, 0);
		for_each_vma(vmi, vma) {
#else
		for (vma = mm->mmap; vma; vma = vma->vm_next) {
#endif
			unsigned long rs, re;

			if (!(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)))
				continue;
			if (vma->vm_flags & (VM_IO | VM_PFNMAP))
				continue;
			if (!search_prot_ok(vma->vm_flags, req.prot_mask))
				continue;

			rs = vma->vm_start;
			re = vma->vm_end;
			if (req.start_addr && re <= (unsigned long)req.start_addr)
				continue;
			if (req.end_addr && rs >= (unsigned long)req.end_addr)
				continue;
			if (req.start_addr && rs < (unsigned long)req.start_addr)
				rs = (unsigned long)req.start_addr;
			if (req.end_addr && re > (unsigned long)req.end_addr)
				re = (unsigned long)req.end_addr;
			if (re <= rs)
				continue;

			if (nranges >= cap) {
				size_t ncap = cap ? cap * 2 : 64;
				void *nbuf = krealloc(ranges, ncap * sizeof(*ranges), GFP_KERNEL);
				if (!nbuf) {
					kfree(ranges);
					up_read_mmap_lock(mm);
					mmput(mm);
					ret = -ENOMEM;
					goto out_free;
				}
				ranges = nbuf;
				cap = ncap;
			}
			ranges[nranges].start = rs;
			ranges[nranges].end = re;
			ranges[nranges].flags = vma->vm_flags;
			nranges++;
		}
		up_read_mmap_lock(mm);
		mmput(mm);

		/* Scan ranges outside mmap_lock. */
		{
			size_t ri;
			for (ri = 0; ri < nranges && !truncated; ri++) {
				unsigned long cur = ranges[ri].start;
				unsigned long rend = ranges[ri].end;

				/* Align start. */
				if (align > 1 && (cur % align))
					cur += align - (cur % align);

				while (cur + req.value_size <= rend && !truncated) {
					size_t want = rend - cur;
					ssize_t got;
					size_t off;

					if (want > SEARCH_CHUNK_SIZE)
						want = SEARCH_CHUNK_SIZE;
					/* Keep enough room for last pattern. */
					if (want < req.value_size)
						break;

					got = mirror_copy_from_remote_kbuf(pid_struct, cur,
									  chunk, want, force);
					if (got <= 0) {
						/* Skip this page-sized hole and continue. */
						unsigned long next = (cur + PAGE_SIZE) & PAGE_MASK;
						if (next <= cur)
							break;
						cur = next;
						if (align > 1 && (cur % align))
							cur += align - (cur % align);
						continue;
					}

					for (off = 0;
					     off + req.value_size <= (size_t)got && !truncated;
					     off += align) {
						if (search_bytes_match(chunk + off, value,
								       has_mask ? mask : NULL,
								       req.value_size)) {
							if (hit_count < req.max_results) {
								hits[hit_count++] = (uint64_t)(cur + off);
							} else {
								truncated = true;
								break;
							}
						}
					}

					/*
					 * Advance by got - (value_size-1) to avoid missing
					 * patterns spanning chunk boundary, then realign.
					 */
					if ((size_t)got >= req.value_size) {
						cur += (size_t)got - (req.value_size - 1);
					} else {
						cur += (size_t)got;
					}
					if (align > 1 && (cur % align))
						cur += align - (cur % align);
					if (need_resched())
						cond_resched();
				}
			}
		}
		kfree(ranges);
	}

	count_word = hit_count;
	if (truncated)
		count_word |= SEARCH_TRUNCATED_BIT;

	if (x_copy_to_user(buf, &count_word, sizeof(count_word))) {
		ret = -EFAULT;
		goto out_free;
	}
	if (hit_count) {
		if (x_copy_to_user(buf + sizeof(uint64_t), hits,
				   sizeof(uint64_t) * hit_count)) {
			ret = -EFAULT;
			goto out_free;
		}
	}
	ret = 0;

out_free:
	kfree(hits);
	kfree(chunk);
	return ret;
}

static inline ssize_t search_process_memory(struct pid *pid_struct,
                                          char __user *buf, size_t buf_size)
{
        return do_proc_search_memory(pid_struct, buf, buf_size);
}

#endif /* PROC_SEARCH_H_ */

