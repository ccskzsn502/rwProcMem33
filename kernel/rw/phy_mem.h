#ifndef PHY_MEM_H_
#define PHY_MEM_H_
//声明
//////////////////////////////////////////////////////////////////////////
#include <linux/fs.h>
#include <linux/pid.h>
#include <asm/page.h>
#include "phy_mem_auto_offset.h"
#include "api_proxy.h"
#include "ver_control.h"

static inline int is_pte_can_read(pte_t* pte);
static inline int is_pte_can_write(pte_t* pte);
static inline int is_pte_can_exec(pte_t* pte);
static inline int change_pte_read_status(pte_t* pte, bool can_read);
static inline int change_pte_write_status(pte_t* pte, bool can_write);
static inline int change_pte_exec_status(pte_t* pte, bool can_exec);

static inline size_t va_to_pa_mm(struct mm_struct *mm, size_t virt_addr, pte_t **out_pte);
static inline size_t get_proc_phy_addr(struct pid* proc_pid_struct, size_t virt_addr, pte_t** out_pte);
static inline size_t read_ram_physical_addr(bool is_kernel_buf, size_t phy_addr, char* lpBuf, size_t read_size);
static inline size_t read_ram_physical_addr_bounce(bool is_kernel_buf, size_t phy_addr, char* lpBuf, size_t read_size, void *bounce);
static inline size_t write_ram_physical_addr(size_t phy_addr, char* lpBuf, bool is_kernel_buf, size_t write_size);

//实现
//////////////////////////////////////////////////////////////////////////
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include "proc_maps_auto_offset.h"

#if MY_LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,83)
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#endif


#define RETURN_VALUE(size_t_ptr___out_ret, size_t___value) *size_t_ptr___out_ret=size_t___value;break;

#include <asm/pgtable.h>

/*
 * Quiet VA->PA walk on an already-held mm (no get_task_mm/mmput, no printk).
 * Used by multi-page R/W and kernel search hot paths.
 */
static inline size_t va_to_pa_mm(struct mm_struct *mm, size_t virt_addr, pte_t **out_pte)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	unsigned long page_addr;
	unsigned long page_offset;

	if (out_pte)
		*out_pte = NULL;
	if (!mm)
		return 0;

	pgd = x_pgd_offset(mm, virt_addr);
	if (!pgd || pgd_none(*pgd))
		return 0;
	p4d = p4d_offset(pgd, virt_addr);
	if (p4d_none(*p4d))
		return 0;
	pud = pud_offset(p4d, virt_addr);
	if (pud_none(*pud))
		return 0;
	pmd = pmd_offset(pud, virt_addr);
	if (pmd_none(*pmd))
		return 0;
	pte = pte_offset_kernel(pmd, virt_addr);
	if (pte_none(*pte))
		return 0;

	page_addr = page_to_phys(pte_page(*pte));
	page_offset = virt_addr & ~PAGE_MASK;
	if (out_pte)
		*out_pte = pte;
	return page_addr | page_offset;
}

static inline size_t get_proc_phy_addr(struct pid* proc_pid_struct, size_t virt_addr, pte_t** out_pte) {
		struct task_struct* task;
		struct mm_struct *mm;
		size_t paddr;

		if (out_pte)
			*out_pte = NULL;
		if (!proc_pid_struct)
			return 0;

		rcu_read_lock();
		task = pid_task(proc_pid_struct, PIDTYPE_PID);
		if (task)
			get_task_struct(task);
		rcu_read_unlock();
		if (!task)
			return 0;

		mm = get_task_mm(task);
		put_task_struct(task);
		if (!mm)
			return 0;

		if (down_read_mmap_lock(mm) != 0) {
			mmput(mm);
			return 0;
		}
		paddr = va_to_pa_mm(mm, virt_addr, out_pte);
		if (out_pte && *out_pte) {
			if (pte_none(**out_pte) || !pte_present(**out_pte)) {
				*out_pte = NULL;
				paddr = 0;
			}
		}
		up_read_mmap_lock(mm);
		mmput(mm);
		return paddr;
	} else {
		set_pte(pte, pte_wrprotect(*pte));
	}
	return 1;
}
static inline int change_pte_exec_status(pte_t* pte, bool can_exec) {
	if (!pte) { return 0; }
	if (can_exec) {
#ifdef pte_mknexec
		set_pte(pte, x_pte_mkwrite(*pte));
#endif
	} else {
#ifdef pte_mkexec
		set_pte(pte, x_pte_mkwrite(*pte));
#endif
	}
	return 1;
}

static inline unsigned long size_inside_page(unsigned long start,
	unsigned long size) {
	unsigned long sz;

	sz = PAGE_SIZE - (start & (PAGE_SIZE - 1));

	return min(sz, size);
}


static inline int check_phys_addr_valid_range(size_t addr, size_t count) {
	if (g_phy_total_memory_size == 0) {
		init_phy_total_memory_size();
	}
	return (addr + count) <= g_phy_total_memory_size;
}


/*
 * bounce: optional PAGE_SIZE scratch for user copies. Kernel dest reads
 * go straight into lpBuf (no bounce). If bounce is NULL and user dest,
 * a temporary page is allocated.
 */
static inline size_t read_ram_physical_addr_bounce(bool is_kernel_buf, size_t phy_addr, char* lpBuf, size_t read_size, void *bounce) {
	void *owned = NULL;
	size_t realRead = 0;
	if (!check_phys_addr_valid_range(phy_addr, read_size)) {
				return 0;
	}
	if (!is_kernel_buf && !bounce) {
				owned = x_kmalloc(PAGE_SIZE, GFP_KERNEL);
				if (!owned)
					return 0;
				bounce = owned;
	}

	while (read_size > 0) {
				size_t sz = size_inside_page(phy_addr, read_size);
				char *ptr = xlate_dev_mem_ptr(phy_addr);
				int probe;

				if (!ptr)
			break;
		if (is_kernel_buf) {
					probe = x_probe_kernel_read(lpBuf, ptr, sz);
					unxlate_dev_mem_ptr(phy_addr, ptr);
					if (probe)
						break;
				} else {
					probe = x_probe_kernel_read(bounce, ptr, sz);
					unxlate_dev_mem_ptr(phy_addr, ptr);
					if (probe)
						break;
					if (x_copy_to_user(lpBuf, bounce, sz))
						break;
				}
				lpBuf += sz;
				phy_addr += sz;
				read_size -= sz;
				realRead += sz;
	}
	if (owned)
				kfree(owned);
	return realRead;
}

static inline size_t read_ram_physical_addr(bool is_kernel_buf, size_t phy_addr, char* lpBuf, size_t read_size) {
	return read_ram_physical_addr_bounce(is_kernel_buf, phy_addr, lpBuf, read_size, NULL);
}

static inline size_t write_ram_physical_addr(size_t phy_addr, char* lpBuf, bool is_kernel_buf, size_t write_size) {
	size_t realWrite = 0;
	if (!check_phys_addr_valid_range(phy_addr, write_size)) {
		return 0;
	}

	while (write_size > 0) {
		size_t sz = size_inside_page(phy_addr, write_size);
		char *ptr = xlate_dev_mem_ptr(phy_addr);
		if (!ptr)
			break;
		if (is_kernel_buf) {
			memcpy(ptr, lpBuf, sz);
		} else {
			unsigned long copied = x_copy_from_user(ptr, lpBuf, sz);
			if (copied) {
				unxlate_dev_mem_ptr(phy_addr, ptr);
				realWrite += sz - copied;
				break;
			}
		}
		unxlate_dev_mem_ptr(phy_addr, ptr);

		lpBuf += sz;
		phy_addr += sz;
		write_size -= sz;
		realWrite += sz;
	}
	return realWrite;
}
#endif /* PHY_MEM_H_ */