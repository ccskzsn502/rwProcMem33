#ifndef HANDLE_TABLE_H_
#define HANDLE_TABLE_H_

#include <linux/pid.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/task.h>

#define PROC_HANDLE_MAX 512

struct proc_handle_slot {
	struct pid *pid;
	u32 gen;
	u8 used;
};

static struct proc_handle_slot g_proc_handles[PROC_HANDLE_MAX];
static DEFINE_SPINLOCK(g_proc_handle_lock);
static u32 g_proc_handle_gen = 1;

static inline uint64_t proc_handle_pack(u32 slot, u32 gen)
{
	return ((uint64_t)gen << 32) | (uint64_t)slot;
}

static inline void proc_handle_unpack(uint64_t h, u32 *slot, u32 *gen)
{
	*slot = (u32)(h & 0xffffffffu);
	*gen = (u32)(h >> 32);
}

/* Takes ownership of pid ref on success. Returns 0 on failure (caller must put_pid). */
static inline uint64_t proc_handle_alloc(struct pid *pid)
{
	unsigned long flags;
	u32 i;
	uint64_t handle = 0;

	if (!pid)
		return 0;

	spin_lock_irqsave(&g_proc_handle_lock, flags);
	for (i = 1; i < PROC_HANDLE_MAX; i++) {
		if (!g_proc_handles[i].used) {
			u32 gen = g_proc_handle_gen++;
			if (gen == 0)
				gen = g_proc_handle_gen++;
			g_proc_handles[i].pid = pid;
			g_proc_handles[i].gen = gen;
			g_proc_handles[i].used = 1;
			handle = proc_handle_pack(i, gen);
			break;
		}
	}
	spin_unlock_irqrestore(&g_proc_handle_lock, flags);
	return handle;
}

/* Returns pid with extra get_pid ref; caller must put_pid. NULL if invalid. */
static inline struct pid *proc_handle_get(uint64_t h)
{
	unsigned long flags;
	u32 slot, gen;
	struct pid *pid = NULL;

	proc_handle_unpack(h, &slot, &gen);
	if (slot == 0 || slot >= PROC_HANDLE_MAX || gen == 0)
		return NULL;

	spin_lock_irqsave(&g_proc_handle_lock, flags);
	if (g_proc_handles[slot].used &&
	    g_proc_handles[slot].gen == gen &&
	    g_proc_handles[slot].pid) {
		pid = g_proc_handles[slot].pid;
		get_pid(pid);
	}
	spin_unlock_irqrestore(&g_proc_handle_lock, flags);
	return pid;
}

/* Removes handle and returns pid (caller put_pid). NULL if invalid. */
static inline struct pid *proc_handle_put_remove(uint64_t h)
{
	unsigned long flags;
	u32 slot, gen;
	struct pid *pid = NULL;

	proc_handle_unpack(h, &slot, &gen);
	if (slot == 0 || slot >= PROC_HANDLE_MAX || gen == 0)
		return NULL;

	spin_lock_irqsave(&g_proc_handle_lock, flags);
	if (g_proc_handles[slot].used &&
	    g_proc_handles[slot].gen == gen) {
		pid = g_proc_handles[slot].pid;
		g_proc_handles[slot].pid = NULL;
		g_proc_handles[slot].gen = 0;
		g_proc_handles[slot].used = 0;
	}
	spin_unlock_irqrestore(&g_proc_handle_lock, flags);
	return pid;
}

static inline void proc_handle_table_destroy(void)
{
	unsigned long flags;
	u32 i;

	spin_lock_irqsave(&g_proc_handle_lock, flags);
	for (i = 0; i < PROC_HANDLE_MAX; i++) {
		if (g_proc_handles[i].used && g_proc_handles[i].pid) {
			struct pid *pid = g_proc_handles[i].pid;
			g_proc_handles[i].pid = NULL;
			g_proc_handles[i].used = 0;
			g_proc_handles[i].gen = 0;
			spin_unlock_irqrestore(&g_proc_handle_lock, flags);
			put_pid(pid);
			spin_lock_irqsave(&g_proc_handle_lock, flags);
		}
	}
	spin_unlock_irqrestore(&g_proc_handle_lock, flags);
}

/* RCU-safe task lookup; returns task with ref, or NULL. Caller put_task_struct. */
static inline struct task_struct *get_task_from_pid_struct(struct pid *pid)
{
	struct task_struct *task;

	if (!pid)
		return NULL;
	rcu_read_lock();
	task = pid_task(pid, PIDTYPE_PID);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	return task;
}

#endif /* HANDLE_TABLE_H_ */
