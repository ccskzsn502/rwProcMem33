// 远端进程只读页映射：控制路径固定页面，读取路径直接访问同一物理页
#include "mirror.h"

#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pagewalk.h>
#include <linux/pid.h>
#include <linux/refcount.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#ifdef kmap_local_page
#define mirror_kmap(p) kmap_local_page(p)
#define mirror_kunmap(p, a) kunmap_local(a)
#else
#define mirror_kmap(p) kmap(p)
#define mirror_kunmap(p, a) do { (void)(a); kunmap(p); } while (0)
#endif

struct mirror_target {
    struct list_head node;
    struct mm_struct *mm;
    struct mmu_notifier notifier;
    spinlock_t lock;
    struct list_head contexts;
    refcount_t refs;
    bool released;
};

struct mirror_context {
    struct mirror_target *target;
    struct list_head target_node;
    struct page *status_page;
    struct mirror_status *status;
    struct page **pages;
    unsigned long remote_addr;
    unsigned long length;
    unsigned long nr_pages;
    unsigned long pinned_pages;
    unsigned long reserved_pages;
    uint64_t invalidation_seq;
    bool target_linked;
};

static atomic64_t mirror_generation = ATOMIC64_INIT(1);
static atomic_long_t mirror_pinned_pages = ATOMIC_LONG_INIT(0);
static DEFINE_MUTEX(mirror_targets_lock);
static LIST_HEAD(mirror_targets);
static struct task_struct *mirror_pin_thread;
static DECLARE_COMPLETION(mirror_pin_pending);
static DEFINE_SPINLOCK(mirror_pin_jobs_lock);
static LIST_HEAD(mirror_pin_jobs);
static bool mirror_pin_stopping;

#define MIRROR_MAX_PINNED_BYTES (272UL * 1024UL * 1024UL)

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
/* 5.10 用 FOLL_MLOCK 的 present-only 分支阻止 slow GUP 触发缺页。 */
#define MIRROR_SLOW_GUP_NOFAULT FOLL_MLOCK
#else
#define MIRROR_SLOW_GUP_NOFAULT FOLL_NOFAULT
#endif

#ifndef FOLL_FORCE
#define FOLL_FORCE 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && \
    LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)

typedef unsigned long (*mirror_kallsyms_lookup_name_t)(const char *name);

static typeof(&mmu_notifier_register) mirror_mmu_notifier_register_fn;
static typeof(&mmu_notifier_unregister) mirror_mmu_notifier_unregister_fn;
static typeof(&pin_user_pages_fast_only) mirror_pin_user_pages_fast_only_fn;
static typeof(&kthread_use_mm) mirror_kthread_use_mm_fn;
static typeof(&kthread_unuse_mm) mirror_kthread_unuse_mm_fn;

static __nocfi unsigned long mirror_call_kallsyms_lookup_name(
    mirror_kallsyms_lookup_name_t fn, const char *name)
{
    return fn(name);
}

static unsigned long mirror_resolve_6_1_symbol(
    mirror_kallsyms_lookup_name_t lookup, const char *name)
{
    unsigned long address;

    address = mirror_call_kallsyms_lookup_name(lookup, name);
    if (!address)
        pr_err("[ook] 6.1 symbol not found: %s\n", name);
    return address;
}

static int mirror_resolve_6_1_symbols(void)
{
    struct kprobe kp = {
        .symbol_name = "kallsyms_lookup_name",
    };
    mirror_kallsyms_lookup_name_t lookup;
    unsigned long address;
    int ret;

    ret = register_kprobe(&kp);
    if (ret) {
        pr_err("[ook] cannot resolve kallsyms_lookup_name: %d\n", ret);
        return ret;
    }
    lookup = (mirror_kallsyms_lookup_name_t)(unsigned long)kp.addr;
    unregister_kprobe(&kp);
    if (!lookup)
        return -ENOENT;

    address = mirror_resolve_6_1_symbol(lookup, "mmu_notifier_register");
    if (!address)
        return -ENOENT;
    mirror_mmu_notifier_register_fn =
        (typeof(mirror_mmu_notifier_register_fn))address;

    address = mirror_resolve_6_1_symbol(lookup, "mmu_notifier_unregister");
    if (!address)
        return -ENOENT;
    mirror_mmu_notifier_unregister_fn =
        (typeof(mirror_mmu_notifier_unregister_fn))address;

    address = mirror_resolve_6_1_symbol(lookup, "pin_user_pages_fast_only");
    if (!address)
        return -ENOENT;
    mirror_pin_user_pages_fast_only_fn =
        (typeof(mirror_pin_user_pages_fast_only_fn))address;

    address = mirror_resolve_6_1_symbol(lookup, "kthread_use_mm");
    if (!address)
        return -ENOENT;
    mirror_kthread_use_mm_fn = (typeof(mirror_kthread_use_mm_fn))address;

    address = mirror_resolve_6_1_symbol(lookup, "kthread_unuse_mm");
    if (!address)
        return -ENOENT;
    mirror_kthread_unuse_mm_fn = (typeof(mirror_kthread_unuse_mm_fn))address;
    return 0;
}

static __nocfi int mirror_mmu_notifier_register(
    struct mmu_notifier *notifier, struct mm_struct *mm)
{
    return mirror_mmu_notifier_register_fn(notifier, mm);
}

static __nocfi void mirror_mmu_notifier_unregister(
    struct mmu_notifier *notifier, struct mm_struct *mm)
{
    mirror_mmu_notifier_unregister_fn(notifier, mm);
}

static __nocfi int mirror_pin_user_pages_fast_only(
    unsigned long start, int nr_pages, unsigned int gup_flags,
    struct page **pages)
{
    return mirror_pin_user_pages_fast_only_fn(start, nr_pages, gup_flags,
                                              pages);
}

static __nocfi void mirror_kthread_use_mm(struct mm_struct *mm)
{
    mirror_kthread_use_mm_fn(mm);
}

static __nocfi void mirror_kthread_unuse_mm(struct mm_struct *mm)
{
    mirror_kthread_unuse_mm_fn(mm);
}

#else

static inline int mirror_resolve_6_1_symbols(void)
{
    return 0;
}

static inline int mirror_mmu_notifier_register(
    struct mmu_notifier *notifier, struct mm_struct *mm)
{
    return mmu_notifier_register(notifier, mm);
}

static inline void mirror_mmu_notifier_unregister(
    struct mmu_notifier *notifier, struct mm_struct *mm)
{
    mmu_notifier_unregister(notifier, mm);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
static inline int mirror_pin_user_pages_fast_only(
    unsigned long start, int nr_pages, unsigned int gup_flags,
    struct page **pages)
{
    return pin_user_pages_fast_only(start, nr_pages, gup_flags, pages);
}
#endif

static inline void mirror_kthread_use_mm(struct mm_struct *mm)
{
    kthread_use_mm(mm);
}

static inline void mirror_kthread_unuse_mm(struct mm_struct *mm)
{
    kthread_unuse_mm(mm);
}

#endif

static void mirror_mark_invalid(struct mirror_context *context)
{
    if (context->status)
        smp_store_release(&context->status->state, MIRROR_STATE_INVALID);
}

static bool mirror_range_overlaps(const struct mirror_context *context,
                                  unsigned long start, unsigned long end)
{
    return start < context->remote_addr + context->length &&
           end > context->remote_addr;
}

static void mirror_invalidate_context_locked(struct mirror_context *context)
{
    context->invalidation_seq++;
    mirror_mark_invalid(context);
}

static int mirror_target_invalidate_start(
    struct mmu_notifier *notifier,
    const struct mmu_notifier_range *range)
{
    struct mirror_target *target =
        container_of(notifier, struct mirror_target, notifier);
    struct mirror_context *context;
    unsigned long flags;

    spin_lock_irqsave(&target->lock, flags);
    list_for_each_entry(context, &target->contexts, target_node) {
        if (!mirror_range_overlaps(context, range->start, range->end))
            continue;
        mirror_invalidate_context_locked(context);
    }
    spin_unlock_irqrestore(&target->lock, flags);
    return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
static void mirror_target_change_pte(struct mmu_notifier *notifier,
                                     struct mm_struct *mm,
                                     unsigned long address, pte_t pte)
{
    struct mmu_notifier_range range = {
        .mm = mm,
        .start = address,
        .end = address + PAGE_SIZE,
    };

    (void)pte;
    mirror_target_invalidate_start(notifier, &range);
}
#endif

static void mirror_target_release(struct mmu_notifier *notifier,
                                  struct mm_struct *mm)
{
    struct mirror_target *target =
        container_of(notifier, struct mirror_target, notifier);
    struct mirror_context *context;
    unsigned long flags;

    (void)mm;
    spin_lock_irqsave(&target->lock, flags);
    target->released = true;
    list_for_each_entry(context, &target->contexts, target_node)
        mirror_invalidate_context_locked(context);
    spin_unlock_irqrestore(&target->lock, flags);
}

static const struct mmu_notifier_ops mirror_target_notifier_ops = {
    .release = mirror_target_release,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
    .change_pte = mirror_target_change_pte,
#endif
    .invalidate_range_start = mirror_target_invalidate_start,
};

static struct mirror_target *mirror_target_get(struct mm_struct *mm)
{
    struct mirror_target *target;
    unsigned long flags;
    int ret;

    mutex_lock(&mirror_targets_lock);
    list_for_each_entry(target, &mirror_targets, node) {
        if (target->mm != mm)
            continue;
        spin_lock_irqsave(&target->lock, flags);
        if (target->released) {
            spin_unlock_irqrestore(&target->lock, flags);
            mutex_unlock(&mirror_targets_lock);
            return ERR_PTR(-ESRCH);
        }
        refcount_inc(&target->refs);
        spin_unlock_irqrestore(&target->lock, flags);
        mutex_unlock(&mirror_targets_lock);
        return target;
    }

    target = kzalloc(sizeof(*target), GFP_KERNEL);
    if (!target) {
        mutex_unlock(&mirror_targets_lock);
        return ERR_PTR(-ENOMEM);
    }
    target->mm = mm;
    target->notifier.ops = &mirror_target_notifier_ops;
    spin_lock_init(&target->lock);
    INIT_LIST_HEAD(&target->contexts);
    refcount_set(&target->refs, 1);

    ret = mirror_mmu_notifier_register(&target->notifier, mm);
    if (ret) {
        kfree(target);
        mutex_unlock(&mirror_targets_lock);
        return ERR_PTR(ret);
    }
    list_add(&target->node, &mirror_targets);
    mutex_unlock(&mirror_targets_lock);
    return target;
}

static void mirror_target_put(struct mirror_target *target)
{
    if (!target)
        return;

    mutex_lock(&mirror_targets_lock);
    if (!refcount_dec_and_test(&target->refs)) {
        mutex_unlock(&mirror_targets_lock);
        return;
    }
    list_del(&target->node);
    mirror_mmu_notifier_unregister(&target->notifier, target->mm);
    mutex_unlock(&mirror_targets_lock);
    kfree(target);
}

static void mirror_unpin_pages(struct mirror_context *context)
{
    if (context->pinned_pages) {
        unpin_user_pages(context->pages, context->pinned_pages);
        context->pinned_pages = 0;
    }
    if (context->reserved_pages) {
        atomic_long_sub(context->reserved_pages, &mirror_pinned_pages);
        context->reserved_pages = 0;
    }
}

static bool mirror_reserve_pages(struct mirror_context *context)
{
    unsigned long limit = MIRROR_MAX_PINNED_BYTES >> PAGE_SHIFT;
    long total;

    if (context->reserved_pages)
        return true;

    total = atomic_long_add_return(context->nr_pages, &mirror_pinned_pages);
    if (total > (long)limit) {
        atomic_long_sub(context->nr_pages, &mirror_pinned_pages);
        return false;
    }

    context->reserved_pages = context->nr_pages;
    return true;
}

static void mirror_destroy(struct mirror_context *context)
{
    struct mirror_target *target;
    unsigned long flags;

    if (!context)
        return;

    target = context->target;
    if (target && context->target_linked) {
        spin_lock_irqsave(&target->lock, flags);
        list_del_init(&context->target_node);
        context->target_linked = false;
        mirror_mark_invalid(context);
        spin_unlock_irqrestore(&target->lock, flags);
    } else {
        mirror_mark_invalid(context);
    }
    mirror_unpin_pages(context);
    if (context->status_page)
        __free_page(context->status_page);
    kfree(context->pages);
    mirror_target_put(target);
    kfree(context);
}

static int mirror_release(struct inode *inode, struct file *file)
{
    struct mirror_context *context = file->private_data;

    (void)inode;
    mirror_destroy(context);
    file->private_data = NULL;
    return 0;
}

static vm_fault_t mirror_fault(struct vm_fault *vmf)
{
    struct mirror_context *context = vmf->vma->vm_private_data;
    unsigned long address = vmf->address & PAGE_MASK;
    unsigned long index;
    struct page *page;

    if (!context || address < vmf->vma->vm_start)
        return VM_FAULT_SIGBUS;

    index = (address - vmf->vma->vm_start) >> PAGE_SHIFT;
    if (index == 0) {
        page = context->status_page;
    } else if (index <= context->nr_pages) {
        page = context->pages[index - 1];
    } else {
        return VM_FAULT_SIGBUS;
    }

    return vmf_insert_pfn(vmf->vma, address, page_to_pfn(page));
}

static const struct vm_operations_struct mirror_vm_ops = {
    .fault = mirror_fault,
};

static int mirror_prepare_vma(struct mirror_context *context,
                              struct vm_area_struct *vma)
{
    unsigned long expected_size = PAGE_SIZE + context->length;

    if (vma->vm_pgoff != 0 || vma->vm_end - vma->vm_start != expected_size)
        return -EINVAL;
    if (vma->vm_flags & (VM_WRITE | VM_EXEC))
        return -EPERM;
    if (smp_load_acquire(&context->status->state) != MIRROR_STATE_ACTIVE)
        return -ESTALE;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
    vm_flags_set(vma, VM_PFNMAP | VM_DONTCOPY |
                 VM_DONTEXPAND | VM_DONTDUMP);
    vm_flags_clear(vma, VM_MAYWRITE | VM_MAYEXEC);
#else
    vma->vm_flags |= VM_PFNMAP | VM_DONTCOPY |
                     VM_DONTEXPAND | VM_DONTDUMP;
    vma->vm_flags &= ~(VM_MAYWRITE | VM_MAYEXEC);
#endif

    vma->vm_private_data = context;
    vma->vm_ops = &mirror_vm_ops;
    return 0;
}

static int mirror_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct mirror_context *context = file->private_data;

    if (!context)
        return -ENODEV;
    return mirror_prepare_vma(context, vma);
}

static const struct file_operations mirror_file_ops = {
    .owner = THIS_MODULE,
    .mmap = mirror_mmap,
    .release = mirror_release,
};

static int mirror_present_pud_entry(pud_t *pud, unsigned long addr,
                                    unsigned long next, struct mm_walk *walk)
{
    (void)addr;
    (void)next;
    if (!pud_present(*pud))
        return -ENOENT;
    if (pud_leaf(*pud) || pud_trans_huge(*pud) || pud_devmap(*pud))
        walk->action = ACTION_CONTINUE;
    return 0;
}

static int mirror_present_pmd_entry(pmd_t *pmd, unsigned long addr,
                                    unsigned long next, struct mm_walk *walk)
{
    (void)addr;
    (void)next;
    if (!pmd_present(*pmd))
        return -ENOENT;
    if (pmd_leaf(*pmd) || pmd_trans_huge(*pmd) || pmd_devmap(*pmd))
        walk->action = ACTION_CONTINUE;
    return 0;
}

static int mirror_present_pte_entry(pte_t *pte, unsigned long addr,
                                    unsigned long next, struct mm_walk *walk)
{
    (void)addr;
    (void)next;
    (void)walk;
    return pte_present(*pte) ? 0 : -ENOENT;
}

static int mirror_present_pte_hole(unsigned long addr, unsigned long next,
                                   int depth, struct mm_walk *walk)
{
    (void)addr;
    (void)next;
    (void)depth;
    (void)walk;
    return -ENOENT;
}

static int mirror_present_hugetlb_entry(pte_t *pte, unsigned long hmask,
                                        unsigned long addr,
                                        unsigned long next,
                                        struct mm_walk *walk)
{
    (void)hmask;
    (void)addr;
    (void)next;
    (void)walk;
    return pte_present(*pte) ? 0 : -ENOENT;
}

static const struct mm_walk_ops mirror_present_walk_ops = {
    .pud_entry = mirror_present_pud_entry,
    .pmd_entry = mirror_present_pmd_entry,
    .pte_entry = mirror_present_pte_entry,
    .pte_hole = mirror_present_pte_hole,
    .hugetlb_entry = mirror_present_hugetlb_entry,
};

struct mirror_pin_work {
    struct list_head node;
    struct completion done;
    struct mm_struct *mm;
    unsigned long address;
    int nr_pages;
    struct page **pages;
    int result;
};

static void mirror_pin_worker(struct mirror_pin_work *job)
{
    int pinned;
    int ret;

    mirror_kthread_use_mm(job->mm);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
    pinned = mirror_pin_user_pages_fast_only(job->address, job->nr_pages,
                                             FOLL_LONGTERM, job->pages);
#else
    pinned = pin_user_pages_fast(job->address, job->nr_pages,
                                 FOLL_LONGTERM | FOLL_NOFAULT, job->pages);
#endif
    if (pinned != job->nr_pages) {
        if (pinned > 0)
            unpin_user_pages(job->pages, pinned);

        /* 再次确认驻留状态，避免排队期间的换出触发远程缺页。 */
        mmap_read_lock(job->mm);
        ret = walk_page_range(job->mm, job->address,
                              job->address +
                                  (unsigned long)job->nr_pages * PAGE_SIZE,
                              &mirror_present_walk_ops, NULL);
        if (ret) {
            pinned = ret;
        } else {
            /* slow GUP 仅迁移已驻留 CMA 页，仍要求 long-term pin。 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
            pinned = pin_user_pages_remote(job->mm, job->address,
                                           job->nr_pages,
                                           FOLL_LONGTERM |
                                               MIRROR_SLOW_GUP_NOFAULT,
                                           job->pages, NULL);
#else
            pinned = pin_user_pages_remote(job->mm, job->address,
                                           job->nr_pages,
                                           FOLL_LONGTERM |
                                               MIRROR_SLOW_GUP_NOFAULT,
                                           job->pages, NULL, NULL);
#endif
        }
        mmap_read_unlock(job->mm);
    }
    job->result = pinned;
    mirror_kthread_unuse_mm(job->mm);
}

static int mirror_pin_thread_fn(void *data)
{
    (void)data;

    for (;;) {
        wait_for_completion(&mirror_pin_pending);

        for (;;) {
            struct mirror_pin_work *job;
            unsigned long flags;

            spin_lock_irqsave(&mirror_pin_jobs_lock, flags);
            if (list_empty(&mirror_pin_jobs)) {
                spin_unlock_irqrestore(&mirror_pin_jobs_lock, flags);
                break;
            }
            job = list_first_entry(&mirror_pin_jobs,
                                   struct mirror_pin_work, node);
            list_del_init(&job->node);
            spin_unlock_irqrestore(&mirror_pin_jobs_lock, flags);

            mirror_pin_worker(job);
            complete(&job->done);
        }

        if (READ_ONCE(mirror_pin_stopping) || kthread_should_stop())
            break;
    }
    return 0;
}

static long mirror_pin_present_pages(struct mm_struct *mm,
                                     unsigned long address,
                                     unsigned long nr_pages,
                                     struct page **pages)
{
    struct mirror_pin_work job = {
        .mm = mm,
        .address = address,
        .nr_pages = (int)nr_pages,
        .pages = pages,
        .result = -EFAULT,
    };
    unsigned long flags;

    INIT_LIST_HEAD(&job.node);
    init_completion(&job.done);

    spin_lock_irqsave(&mirror_pin_jobs_lock, flags);
    if (!mirror_pin_thread || mirror_pin_stopping) {
        spin_unlock_irqrestore(&mirror_pin_jobs_lock, flags);
        return -ENODEV;
    }
    list_add_tail(&job.node, &mirror_pin_jobs);
    spin_unlock_irqrestore(&mirror_pin_jobs_lock, flags);

    complete(&mirror_pin_pending);
    wait_for_completion(&job.done);
    return job.result;
}

static long mirror_pin_remote_pages(struct mirror_context *context)
{
    struct mirror_target *target = context->target;
    unsigned long flags;
    struct vm_area_struct *vma;
    long pinned;
    unsigned long nr_pages = context->nr_pages;
    int attempt;

    // 镜像可能由持久视图长期持有，因此只允许内核认可的 long-term pin。
    if (!mirror_reserve_pages(context))
        return -ENOSPC;

    for (attempt = 0; attempt < 3; ++attempt) {
        uint64_t seq;

        spin_lock_irqsave(&target->lock, flags);
        if (target->released) {
            spin_unlock_irqrestore(&target->lock, flags);
            return -ESRCH;
        }
        seq = context->invalidation_seq;
        spin_unlock_irqrestore(&target->lock, flags);

        mmap_read_lock(target->mm);
        vma = find_vma(target->mm, context->remote_addr);
        if (!vma || vma->vm_start > context->remote_addr ||
            vma->vm_end < context->remote_addr + context->length ||
            !(vma->vm_flags & (VM_READ | VM_WRITE)) ||
            (vma->vm_flags & (VM_IO | VM_PFNMAP))) {
            mmap_read_unlock(target->mm);
            return -EFAULT;
        }
        mmap_read_unlock(target->mm);
        // fast GUP 本身禁止缺页；失败回退时再检查驻留页表，避免冷路径重复遍历。
        pinned = mirror_pin_present_pages(target->mm,
                                          context->remote_addr,
                                          nr_pages, context->pages);
        if (pinned != nr_pages) {
            if (pinned > 0)
                unpin_user_pages(context->pages, pinned);
            spin_lock_irqsave(&target->lock, flags);
            if (target->released) {
                spin_unlock_irqrestore(&target->lock, flags);
                return -ESRCH;
            }
            if (context->invalidation_seq != seq) {
                spin_unlock_irqrestore(&target->lock, flags);
                continue;
            }
            spin_unlock_irqrestore(&target->lock, flags);
            if (pinned >= 0 || pinned == -EFAULT)
                return -ENOENT;
            return pinned < 0 ? pinned : -EFAULT;
        }

        spin_lock_irqsave(&target->lock, flags);
        if (!target->released && context->invalidation_seq == seq) {
            context->pinned_pages = pinned;
            smp_store_release(&context->status->state, MIRROR_STATE_ACTIVE);
            spin_unlock_irqrestore(&target->lock, flags);
            return pinned;
        }
        spin_unlock_irqrestore(&target->lock, flags);
        unpin_user_pages(context->pages, pinned);
    }
    return -EAGAIN;
}

static struct mm_struct *mirror_get_mm(pid_t pid)
{
    struct pid *pid_struct;
    struct task_struct *task;
    struct mm_struct *mm = NULL;

    pid_struct = find_get_pid(pid);
    if (!pid_struct)
        return NULL;
    task = get_pid_task(pid_struct, PIDTYPE_PID);
    put_pid(pid_struct);
    if (!task)
        return NULL;

    mm = get_task_mm(task);
    put_task_struct(task);
    return mm;
}

int mirror_create_fd(pid_t pid, unsigned long remote_addr,
                     unsigned long length)
{
    struct mirror_context *context;
    struct mirror_target *target;
    struct mm_struct *mm;
    unsigned long flags;
    long pinned;
    int ret;

    if (pid <= 0 || !length || length > MIRROR_MAX_SIZE ||
        !PAGE_ALIGNED(remote_addr) || !PAGE_ALIGNED(length) ||
        remote_addr + length < remote_addr)
        return -EINVAL;

    mm = mirror_get_mm(pid);
    if (!mm)
        return -ESRCH;

    context = kzalloc(sizeof(*context), GFP_KERNEL);
    if (!context) {
        mmput(mm);
        return -ENOMEM;
    }

    context->pages = kcalloc(length >> PAGE_SHIFT,
                             sizeof(*context->pages), GFP_KERNEL);
    context->status_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!context->pages || !context->status_page) {
        mmput(mm);
        mirror_destroy(context);
        return -ENOMEM;
    }

    target = mirror_target_get(mm);
    if (IS_ERR(target)) {
        ret = PTR_ERR(target);
        mmput(mm);
        mirror_destroy(context);
        return ret;
    }

    context->target = target;
    INIT_LIST_HEAD(&context->target_node);
    context->remote_addr = remote_addr;
    context->length = length;
    context->nr_pages = length >> PAGE_SHIFT;
    context->status = page_address(context->status_page);
    context->status->magic = MIRROR_STATUS_MAGIC;
    context->status->version = MIRROR_STATUS_VERSION;
    context->status->state = MIRROR_STATE_INVALID;
    context->status->remote_addr = remote_addr;
    context->status->length = length;
    context->status->generation =
        (uint64_t)atomic64_inc_return(&mirror_generation);

    spin_lock_irqsave(&target->lock, flags);
    if (target->released) {
        spin_unlock_irqrestore(&target->lock, flags);
        mmput(mm);
        mirror_destroy(context);
        return -ESRCH;
    }
    list_add_tail(&context->target_node, &target->contexts);
    context->target_linked = true;
    spin_unlock_irqrestore(&target->lock, flags);

    pinned = mirror_pin_remote_pages(context);
    mmput(mm);
    if (pinned < 0) {
        mirror_destroy(context);
        return (int)pinned;
    }

    ret = anon_inode_getfd("[ook-mirror]", &mirror_file_ops, context,
                           O_RDONLY | O_CLOEXEC);
    if (ret < 0)
        mirror_destroy(context);
    return ret;
}


/* -------- one-shot pin+copy (primary R/W path) -------- */

static long mirror_pin_remote_range(struct mm_struct *mm,
                                    unsigned long start,
                                    unsigned long nr_pages,
                                    struct page **pages,
                                    bool for_write,
                                    bool force)
{
    unsigned int gup_flags = FOLL_LONGTERM;
    long pinned;
    int ret;

    if (for_write)
        gup_flags |= FOLL_WRITE;
    if (force)
        gup_flags |= FOLL_FORCE;

    mirror_kthread_use_mm(mm);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
    pinned = mirror_pin_user_pages_fast_only(start, (int)nr_pages, gup_flags, pages);
#else
    pinned = pin_user_pages_fast(start, (int)nr_pages, gup_flags | FOLL_NOFAULT, pages);
#endif
    if (pinned != (long)nr_pages) {
        if (pinned > 0)
            unpin_user_pages(pages, pinned);
        mmap_read_lock(mm);
        ret = walk_page_range(mm, start,
                              start + nr_pages * PAGE_SIZE,
                              &mirror_present_walk_ops, NULL);
        if (ret) {
            pinned = ret;
        } else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
            pinned = pin_user_pages_remote(mm, start, (int)nr_pages,
                                           gup_flags | MIRROR_SLOW_GUP_NOFAULT,
                                           pages, NULL);
#else
            pinned = pin_user_pages_remote(mm, start, (int)nr_pages,
                                           gup_flags | MIRROR_SLOW_GUP_NOFAULT,
                                           pages, NULL, NULL);
#endif
        }
        mmap_read_unlock(mm);
    }
    mirror_kthread_unuse_mm(mm);
    return pinned;
}

static ssize_t mirror_copy_pages_to_user(struct page **pages,
                                         unsigned long page_off,
                                         char __user *ubuf,
                                         size_t size)
{
    size_t done = 0;
    unsigned long idx = 0;

    while (done < size) {
        size_t chunk = min_t(size_t, size - done, PAGE_SIZE - page_off);
        void *kaddr = mirror_kmap(pages[idx]);
        unsigned long left = copy_to_user(ubuf + done, (char *)kaddr + page_off, chunk);
        mirror_kunmap(pages[idx], kaddr);
        if (left)
            return done ? (ssize_t)done : -EFAULT;
        done += chunk;
        page_off = 0;
        idx++;
    }
    return (ssize_t)done;
}

static ssize_t mirror_copy_pages_to_kbuf(struct page **pages,
                                         unsigned long page_off,
                                         void *kbuf,
                                         size_t size)
{
    size_t done = 0;
    unsigned long idx = 0;

    while (done < size) {
        size_t chunk = min_t(size_t, size - done, PAGE_SIZE - page_off);
        void *kaddr = mirror_kmap(pages[idx]);
        memcpy((char *)kbuf + done, (char *)kaddr + page_off, chunk);
        mirror_kunmap(pages[idx], kaddr);
        done += chunk;
        page_off = 0;
        idx++;
    }
    return (ssize_t)done;
}

static ssize_t mirror_copy_user_to_pages(struct page **pages,
                                         unsigned long page_off,
                                         const char __user *ubuf,
                                         size_t size)
{
    size_t done = 0;
    unsigned long idx = 0;

    while (done < size) {
        size_t chunk = min_t(size_t, size - done, PAGE_SIZE - page_off);
        void *kaddr = mirror_kmap(pages[idx]);
        unsigned long left = copy_from_user((char *)kaddr + page_off, ubuf + done, chunk);
        mirror_kunmap(pages[idx], kaddr);
        if (left)
            return done ? (ssize_t)done : -EFAULT;
        done += chunk;
        page_off = 0;
        idx++;
    }
    return (ssize_t)done;
}

/*
 * Pin present pages for [remote_addr, remote_addr+size) and copy to/from user.
 * Does not require page alignment of remote_addr/size (internally expands).
 */
static ssize_t mirror_copy_remote_common(struct pid *pid_struct,
                                         unsigned long remote_addr,
                                         char __user *ubuf, size_t size,
                                         bool force, bool to_remote)
{
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    struct page **pages;
    unsigned long start_page, end_addr, nr_pages, page_off;
    long pinned;
    ssize_t copied;
    int ret = 0;

    if (!pid_struct || !ubuf || !size)
        return -EINVAL;
    if (size > (16UL * 1024UL * 1024UL))
        return -EINVAL;
    end_addr = remote_addr + size;
    if (end_addr < remote_addr)
        return -EINVAL;

    rcu_read_lock();
    task = pid_task(pid_struct, PIDTYPE_PID);
    if (task)
        get_task_struct(task);
    rcu_read_unlock();
    if (!task)
        return -ESRCH;

    mm = get_task_mm(task);
    put_task_struct(task);
    if (!mm)
        return -ESRCH;

    if (!force) {
        mmap_read_lock(mm);
        vma = find_vma(mm, remote_addr);
        if (!vma || vma->vm_start > remote_addr ||
            vma->vm_end < end_addr) {
            mmap_read_unlock(mm);
            mmput(mm);
            return -EFAULT;
        }
        if (to_remote) {
            if (!(vma->vm_flags & VM_WRITE)) {
                mmap_read_unlock(mm);
                mmput(mm);
                return -EFAULT;
            }
        } else if (!(vma->vm_flags & VM_READ)) {
            mmap_read_unlock(mm);
            mmput(mm);
            return -EFAULT;
        }
        if (vma->vm_flags & (VM_IO | VM_PFNMAP)) {
            mmap_read_unlock(mm);
            mmput(mm);
            return -EFAULT;
        }
        mmap_read_unlock(mm);
    }

    start_page = remote_addr & PAGE_MASK;
    page_off = remote_addr & ~PAGE_MASK;
    nr_pages = ((page_off + size + PAGE_SIZE - 1) >> PAGE_SHIFT);
    if (nr_pages == 0 || nr_pages > (16UL * 1024UL * 1024UL / PAGE_SIZE) + 2) {
        mmput(mm);
        return -EINVAL;
    }

    pages = kcalloc(nr_pages, sizeof(*pages), GFP_KERNEL);
    if (!pages) {
        mmput(mm);
        return -ENOMEM;
    }

    pinned = mirror_pin_remote_range(mm, start_page, nr_pages, pages, to_remote, force);
    if (pinned != (long)nr_pages) {
        if (pinned > 0)
            unpin_user_pages(pages, pinned);
        kfree(pages);
        mmput(mm);
        /* Not present / pin failed: return 0-style short read for non-force consumers */
        if (pinned == -ENOENT || pinned == -EFAULT || pinned == 0)
            return 0;
        return pinned < 0 ? pinned : -EFAULT;
    }

    if (to_remote)
        copied = mirror_copy_user_to_pages(pages, page_off, ubuf, size);
    else
        copied = mirror_copy_pages_to_user(pages, page_off, ubuf, size);

    unpin_user_pages(pages, nr_pages);
    kfree(pages);
    mmput(mm);
    return copied;
}


ssize_t mirror_copy_from_remote_kbuf(struct pid *pid_struct,
                                     unsigned long remote_addr,
                                     void *kbuf, size_t size,
                                     bool force)
{
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    struct page **pages;
    unsigned long start_page, end_addr, nr_pages, page_off;
    long pinned;
    ssize_t copied;

    if (!pid_struct || !kbuf || !size)
        return -EINVAL;
    if (size > (16UL * 1024UL * 1024UL))
        return -EINVAL;
    end_addr = remote_addr + size;
    if (end_addr < remote_addr)
        return -EINVAL;

    rcu_read_lock();
    task = pid_task(pid_struct, PIDTYPE_PID);
    if (task)
        get_task_struct(task);
    rcu_read_unlock();
    if (!task)
        return -ESRCH;

    mm = get_task_mm(task);
    put_task_struct(task);
    if (!mm)
        return -ESRCH;

    if (!force) {
        mmap_read_lock(mm);
        vma = find_vma(mm, remote_addr);
        if (!vma || vma->vm_start > remote_addr ||
            vma->vm_end < end_addr ||
            !(vma->vm_flags & VM_READ) ||
            (vma->vm_flags & (VM_IO | VM_PFNMAP))) {
            mmap_read_unlock(mm);
            mmput(mm);
            return -EFAULT;
        }
        mmap_read_unlock(mm);
    }

    start_page = remote_addr & PAGE_MASK;
    page_off = remote_addr & ~PAGE_MASK;
    nr_pages = ((page_off + size + PAGE_SIZE - 1) >> PAGE_SHIFT);
    if (nr_pages == 0 || nr_pages > (16UL * 1024UL * 1024UL / PAGE_SIZE) + 2) {
        mmput(mm);
        return -EINVAL;
    }

    pages = kcalloc(nr_pages, sizeof(*pages), GFP_KERNEL);
    if (!pages) {
        mmput(mm);
        return -ENOMEM;
    }

    pinned = mirror_pin_remote_range(mm, start_page, nr_pages, pages, false, force);
    if (pinned != (long)nr_pages) {
        if (pinned > 0)
            unpin_user_pages(pages, pinned);
        kfree(pages);
        mmput(mm);
        if (pinned == -ENOENT || pinned == -EFAULT || pinned == 0)
            return 0;
        return pinned < 0 ? pinned : -EFAULT;
    }

    copied = mirror_copy_pages_to_kbuf(pages, page_off, kbuf, size);
    unpin_user_pages(pages, nr_pages);
    kfree(pages);
    mmput(mm);
    return copied;
}

ssize_t mirror_copy_from_remote(struct pid *pid_struct,
                                unsigned long remote_addr,
                                char __user *ubuf, size_t size,
                                bool force)
{
    return mirror_copy_remote_common(pid_struct, remote_addr, ubuf, size, force, false);
}

ssize_t mirror_copy_to_remote(struct pid *pid_struct,
                              unsigned long remote_addr,
                              const char __user *ubuf, size_t size,
                              bool force)
{
    return mirror_copy_remote_common(pid_struct, remote_addr,
                                     (char __user *)ubuf, size, force, true);
}


int mirror_init(void)
{
    int ret;

    if (mirror_pin_thread)
        return 0;

    ret = mirror_resolve_6_1_symbols();
    if (ret)
        return ret;

    mirror_pin_stopping = false;
    reinit_completion(&mirror_pin_pending);
    mirror_pin_thread = kthread_run(mirror_pin_thread_fn, NULL,
                                    "ook_mirror");
    if (IS_ERR(mirror_pin_thread)) {
        int ret = PTR_ERR(mirror_pin_thread);

        mirror_pin_thread = NULL;
        return ret;
    }
    return 0;
}

void mirror_exit(void)
{
    unsigned long flags;

    if (!mirror_pin_thread)
        return;

    spin_lock_irqsave(&mirror_pin_jobs_lock, flags);
    mirror_pin_stopping = true;
    spin_unlock_irqrestore(&mirror_pin_jobs_lock, flags);
    complete(&mirror_pin_pending);
    kthread_stop(mirror_pin_thread);
    mirror_pin_thread = NULL;
}

struct task_struct *mirror_worker_task(void)
{
    return mirror_pin_thread;
}
