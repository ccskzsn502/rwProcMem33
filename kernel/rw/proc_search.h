#ifndef PROC_SEARCH_H_
#define PROC_SEARCH_H_

#include <linux/types.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#if MY_LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,83)
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#endif

#include "api_proxy.h"
#include "phy_mem.h"
#include "proc_maps.h"
#include "ver_control.h"

/* Value type for exact search */
enum {
	SEARCH_VAL_U8 = 1,
	SEARCH_VAL_U16 = 2,
	SEARCH_VAL_U32 = 4,
	SEARCH_VAL_U64 = 8,
	SEARCH_VAL_FLOAT = 0x14,   /* 4-byte float exact (bit compare) */
	SEARCH_VAL_DOUBLE = 0x18,  /* 8-byte double exact (bit compare) */
	SEARCH_VAL_BYTES = 0x20,   /* raw pattern, optional mask in tail */
};

/* Protection mask bits for region filter (match flags[0..3] rwxp style loosely) */
enum {
	SEARCH_PROT_R = 1,
	SEARCH_PROT_W = 2,
	SEARCH_PROT_X = 4,
	SEARCH_PROT_ANY = 0x7,
};

#pragma pack(push, 1)
/* Input header in ioctl buf; followed by value[value_size], optional mask[value_size] if has_mask */
struct search_request {
	uint64_t start_addr;     /* inclusive; 0 = no lower bound */
	uint64_t end_addr;       /* exclusive; 0 = no upper bound */
	uint32_t value_type;     /* SEARCH_VAL_* */
	uint32_t value_size;     /* 1..64 for bytes; for typed values size must match type */
	uint32_t alignment;      /* 0 = value_size; scan step */
	uint32_t max_results;    /* cap result count */
	uint32_t prot_mask;      /* SEARCH_PROT_* ; 0 = any readable */
	uint32_t flags;          /* bit0: force_read like CMD_READ; bit1: has_mask */
	/* value bytes follow immediately after this header (value_size)
	 * if flags&2: mask bytes follow value (value_size), 0xff=match 0x00=wildcard */
};
/* Output: first uint64_t = hit_count (or truncated flag in high bit), then hit_count * uint64_t addresses */
#pragma pack(pop)

#define SEARCH_FLAG_FORCE_READ  (1u << 0)
#define SEARCH_FLAG_HAS_MASK    (1u << 1)
#define SEARCH_TRUNCATED_BIT    (1ull << 63)
#define SEARCH_MAX_VALUE_SIZE   64
#define SEARCH_DEFAULT_MAX_HITS 4096
#define SEARCH_HARD_MAX_HITS    65536

static inline int search_match_bytes(const unsigned char *mem,
	const unsigned char *pat, const unsigned char *mask, uint32_t n)
{
	uint32_t i;
	if (mask) {
		for (i = 0; i < n; i++) {
			if ((mem[i] & mask[i]) != (pat[i] & mask[i]))
				return 0;
		}
		return 1;
	}
	return memcmp(mem, pat, n) == 0;
}

static inline int search_vma_ok(struct vm_area_struct *vma, uint32_t prot_mask)
{
	unsigned long f;
	if (!vma)
		return 0;
	f = vma->vm_flags;
	if (!(f & (VM_READ | VM_WRITE | VM_EXEC)))
		return 0;
	if (prot_mask == 0 || prot_mask == SEARCH_PROT_ANY)
		return 1;
	if ((prot_mask & SEARCH_PROT_R) && !(f & VM_READ))
		return 0;
	if ((prot_mask & SEARCH_PROT_W) && !(f & VM_WRITE))
		return 0;
	if ((prot_mask & SEARCH_PROT_X) && !(f & VM_EXEC))
		return 0;
	return 1;
}

/* Reuse shared quiet VA->PA from phy_mem.h */
#define search_va_to_pa va_to_pa_mm

/*
 * Kernel-side scan: compare in ko, copy only addresses to user.
 * buf layout in:  search_request + value [+ mask]
 * buf layout out: uint64_t count_or_flags, uint64_t addrs[]
 * return: 0 on success, negative errno, or positive = unused
 * hdr->buf_size must be large enough for output (8 + 8*max_results at least for count)
 */
static ssize_t search_process_memory(struct pid *proc_pid_struct,
	char __user *user_buf, size_t buf_size)
{
	struct search_request req;
	struct task_struct *task;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned char *value = NULL;
	unsigned char *mask = NULL;
	unsigned char *page_buf = NULL;
	uint64_t *hits = NULL;
	uint32_t hit_count = 0;
	uint32_t max_hits;
	uint32_t align;
	uint32_t vsize;
	int force_read;
	int has_mask;
	int truncated = 0;
	ssize_t ret = 0;
	size_t need_in;
	size_t need_out;
	unsigned long start_bound, end_bound;

	if (!proc_pid_struct || !user_buf || buf_size < sizeof(struct search_request) + 8)
		return -EINVAL;

	if (x_copy_from_user(&req, user_buf, sizeof(req)))
		return -EFAULT;

	vsize = req.value_size;
	if (vsize == 0 || vsize > SEARCH_MAX_VALUE_SIZE)
		return -EINVAL;

	/* Normalize typed sizes */
	if (req.value_type == SEARCH_VAL_U8) vsize = 1;
	else if (req.value_type == SEARCH_VAL_U16) vsize = 2;
	else if (req.value_type == SEARCH_VAL_U32 || req.value_type == SEARCH_VAL_FLOAT) vsize = 4;
	else if (req.value_type == SEARCH_VAL_U64 || req.value_type == SEARCH_VAL_DOUBLE) vsize = 8;
	else if (req.value_type != SEARCH_VAL_BYTES)
		return -EINVAL;

	req.value_size = vsize;
	has_mask = (req.flags & SEARCH_FLAG_HAS_MASK) ? 1 : 0;
	force_read = (req.flags & SEARCH_FLAG_FORCE_READ) ? 1 : 0;
	need_in = sizeof(req) + vsize + (has_mask ? vsize : 0);
	if (buf_size < need_in)
		return -EINVAL;

	max_hits = req.max_results ? req.max_results : SEARCH_DEFAULT_MAX_HITS;
	if (max_hits > SEARCH_HARD_MAX_HITS)
		max_hits = SEARCH_HARD_MAX_HITS;

	need_out = sizeof(uint64_t) + (size_t)max_hits * sizeof(uint64_t);
	if (buf_size < need_out) {
		/* shrink max_hits to fit output buffer */
		if (buf_size < sizeof(uint64_t) + sizeof(uint64_t))
			return -EINVAL;
		max_hits = (uint32_t)((buf_size - sizeof(uint64_t)) / sizeof(uint64_t));
		if (max_hits == 0)
			return -EINVAL;
	}

	align = req.alignment ? req.alignment : vsize;
	if (align == 0)
		align = 1;

	value = x_kmalloc(vsize, GFP_KERNEL);
	if (!value)
		return -ENOMEM;
	if (x_copy_from_user(value, user_buf + sizeof(req), vsize)) {
		ret = -EFAULT;
		goto out_free;
	}
	if (has_mask) {
		mask = x_kmalloc(vsize, GFP_KERNEL);
		if (!mask) {
			ret = -ENOMEM;
			goto out_free;
		}
		if (x_copy_from_user(mask, user_buf + sizeof(req) + vsize, vsize)) {
			ret = -EFAULT;
			goto out_free;
		}
	}

	page_buf = x_kmalloc(PAGE_SIZE, GFP_KERNEL);
	hits = x_kmalloc((size_t)max_hits * sizeof(uint64_t), GFP_KERNEL);
	if (!page_buf || !hits) {
		ret = -ENOMEM;
		goto out_free;
	}

	task = pid_task(proc_pid_struct, PIDTYPE_PID);
	if (!task) {
		ret = -ESRCH;
		goto out_free;
	}
	mm = get_task_mm(task);
	if (!mm) {
		ret = -EINVAL;
		goto out_free;
	}
	if (down_read_mmap_lock(mm) != 0) {
		mmput(mm);
		ret = -EBUSY;
		goto out_free;
	}

	start_bound = (unsigned long)req.start_addr;
	end_bound = (unsigned long)req.end_addr;
	if (end_bound == 0)
		end_bound = (unsigned long)-1;

#if MY_LINUX_VERSION_CODE == KERNEL_VERSION(3,10,0)
	for (vma = mm->mmap; vma; vma = vma->vm_next)
#else
	/* Prefer mm->mmap walk for older; for newer kernels with maple tree
	 * this project still uses mmap-linked list helpers elsewhere. */
	for (vma = mm->mmap; vma; vma = vma->vm_next)
#endif
	{
		unsigned long vstart, vend, addr;
		if (truncated)
			break;
		if (!search_vma_ok(vma, req.prot_mask))
			continue;
		if (!(vma->vm_flags & VM_READ) && !force_read)
			continue;

		vstart = vma->vm_start;
		vend = vma->vm_end;
		if (vend <= start_bound || vstart >= end_bound)
			continue;
		if (vstart < start_bound)
			vstart = start_bound;
		if (vend > end_bound)
			vend = end_bound;
		if (vstart >= vend)
			continue;

		/* Align start */
				if (align > 1) {
					unsigned long rem = vstart % align;
					if (rem)
						vstart += (align - rem);
				}

				{
					/* Last (vsize-1) bytes of previous readable chunk for page-span hits */
					unsigned char tail[SEARCH_MAX_VALUE_SIZE];
					uint32_t tail_len = 0;
					unsigned long tail_base = 0;

					for (addr = vstart; addr < vend && !truncated; ) {
						size_t page_off = addr & (PAGE_SIZE - 1);
						size_t chunk = PAGE_SIZE - page_off;
						size_t phy_addr;
						pte_t *pte = NULL;
						size_t got;
						size_t i;
						uint32_t need_tail = (vsize > 1) ? (vsize - 1) : 0;

						cond_resched();

						if (addr + chunk > vend)
							chunk = vend - addr;
						if (chunk == 0)
							break;

						phy_addr = search_va_to_pa(mm, addr, &pte);
						if (phy_addr == 0 || (!force_read && !is_pte_can_read(pte))) {
							tail_len = 0;
							addr = (addr + PAGE_SIZE) & ~(PAGE_SIZE - 1);
							if (align > 1 && (addr % align))
								addr += align - (addr % align);
							continue;
						}

						{
							size_t inside = size_inside_page(phy_addr, chunk);
							if (inside == 0) {
								tail_len = 0;
								addr = (addr + PAGE_SIZE) & ~(PAGE_SIZE - 1);
								continue;
							}
							chunk = inside;
						}

						got = read_ram_physical_addr_bounce(true, phy_addr, (char *)page_buf, chunk, NULL);
						if (got == 0) {
							tail_len = 0;
							addr = (addr + PAGE_SIZE) & ~(PAGE_SIZE - 1);
							continue;
						}

						/* Patterns that start in previous chunk and end in this page */
						if (tail_len && need_tail) {
							unsigned char win[SEARCH_MAX_VALUE_SIZE * 2];
							uint32_t take = (got < need_tail) ? (uint32_t)got : need_tail;
							uint32_t wlen;
							unsigned long hit_addr;
							unsigned long rem;

							memcpy(win, tail, tail_len);
							memcpy(win + tail_len, page_buf, take);
							wlen = tail_len + take;

							hit_addr = tail_base;
							rem = hit_addr % align;
							if (rem)
								hit_addr += (align - rem);
							for (; hit_addr < addr &&
							       (hit_addr - tail_base) + vsize <= wlen &&
							       !truncated;
							     hit_addr += align) {
								uint32_t off = (uint32_t)(hit_addr - tail_base);
								/* only report starts that cross into this page */
								if (hit_addr + vsize <= addr)
									continue;
								if (hit_addr < vstart || hit_addr + vsize > vend)
									continue;
								if (search_match_bytes(win + off, value, mask, vsize)) {
									if (hit_count < max_hits)
										hits[hit_count++] = (uint64_t)hit_addr;
									else
										truncated = 1;
								}
							}
						}
						if (truncated)
							break;

						/* Fully in-page matches */
						for (i = 0; i + vsize <= got; i += align) {
							if (search_match_bytes(page_buf + i, value, mask, vsize)) {
								if (hit_count < max_hits)
									hits[hit_count++] = (uint64_t)(addr + i);
								else {
									truncated = 1;
									break;
								}
							}
						}
						if (truncated)
							break;

						/* Keep last (vsize-1) bytes for next page */
						if (need_tail && got > 0) {
							if (got >= need_tail) {
								memcpy(tail, page_buf + (got - need_tail), need_tail);
								tail_len = need_tail;
								tail_base = addr + (got - need_tail);
							} else if (tail_len + got >= need_tail) {
								uint32_t keep = need_tail - (uint32_t)got;
								if (tail_len > keep) {
									memmove(tail, tail + (tail_len - keep), keep);
									tail_base = addr - keep;
									tail_len = keep;
								}
								memcpy(tail + tail_len, page_buf, got);
								tail_len += (uint32_t)got;
							} else {
								memcpy(tail + tail_len, page_buf, got);
								if (tail_len == 0)
									tail_base = addr;
								tail_len += (uint32_t)got;
							}
						} else {
							tail_len = 0;
						}

						addr += got;
					}
				}
			}

	up_read_mmap_lock(mm);
	mmput(mm);

	/* Write results to user: [count|trunc_flag][addrs...] */
	{
		uint64_t meta = (uint64_t)hit_count;
		if (truncated)
			meta |= SEARCH_TRUNCATED_BIT;
		if (x_copy_to_user(user_buf, &meta, sizeof(meta))) {
			ret = -EFAULT;
			goto out_free;
		}
		if (hit_count > 0) {
			if (x_copy_to_user(user_buf + sizeof(meta), hits,
					(size_t)hit_count * sizeof(uint64_t))) {
				ret = -EFAULT;
				goto out_free;
			}
		}
	}
	ret = 0;

out_free:
	if (hits)
		kfree(hits);
	if (page_buf)
		kfree(page_buf);
	if (mask)
		kfree(mask);
	if (value)
		kfree(value);
	return ret;
}

#endif /* PROC_SEARCH_H_ */
