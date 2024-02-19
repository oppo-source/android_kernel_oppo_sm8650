/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 */

#ifndef _OPLUS_LOCKING_MAIN_H_
#define _OPLUS_LOCKING_MAIN_H_

#define cond_trace_printk(cond, fmt, ...)	\
do {										\
	if (cond)								\
		trace_printk(fmt, ##__VA_ARGS__);	\
} while (0)



#define MAGIC_NUM       (0xdead0000)
#define MAGIC_MASK      (0xffff0000)
#define MAGIC_SHIFT     (16)
#define OWNER_BIT       (1 << 0)
#define THREAD_INFO_BIT (1 << 1)
#define TYPE_BIT        (1 << 2)

#define UX_FLAG_BIT       (1<<0)
#define SS_FLAG_BIT       (1<<1)
#define GRP_SHIFT         (2)
#define GRP_FLAG_MASK     (7 << GRP_SHIFT)
#define U_GRP_OTHER       (1 << GRP_SHIFT)
#define U_GRP_BACKGROUND  (2 << GRP_SHIFT)
#define U_GRP_FRONDGROUD  (3 << GRP_SHIFT)
#define U_GRP_TOP_APP     (4 << GRP_SHIFT)

#define LOCK_TYPE_SHIFT (30)
#define INVALID_TYPE    (0)
#define LOCK_ART        (1)
#define LOCK_JUC        (2)

#define lk_err(fmt, ...) \
		pr_err("[oplus_locking][%s]"fmt, __func__, ##__VA_ARGS__)
#define lk_warn(fmt, ...) \
		pr_warn("[oplus_locking][%s]"fmt, __func__, ##__VA_ARGS__)
#define lk_info(fmt, ...) \
		pr_info("[oplus_locking][%s]"fmt, __func__, ##__VA_ARGS__)

#define OTS_IDX			0
#define MAX_CLUSTER		(3)


struct futex_uinfo {
	u32 cmd;
	u32 owner_tid;
	u32 type;
	u64 inform_user;
};

enum {
	CGROUP_RESV = 0,
	CGROUP_DEFAULT,
	CGROUP_FOREGROUND,
	CGROUP_BACKGROUND,
	CGROUP_TOP_APP,

	CGROUP_NRS,
};

enum rwsem_waiter_type {
	RWSEM_WAITING_FOR_WRITE,
	RWSEM_WAITING_FOR_READ
};

struct rwsem_waiter {
	struct list_head list;
	struct task_struct *task;
	enum rwsem_waiter_type type;
	unsigned long timeout;
	bool handoff_set;
};

#define LK_MUTEX_ENABLE (1 << 0)
#define LK_RWSEM_ENABLE (1 << 1)
#define LK_FUTEX_ENABLE (1 << 2)
#define LK_OSQ_ENABLE   (1 << 3)

#ifdef CONFIG_OPLUS_LOCKING_MONITOR
/*
 * The bit definitions of the g_opt_enable:
 * bit 0-7: reserved bits for other locking optimation.
 * bit8 ~ bit10(each monitor version is exclusive):
 * 1 : monitor control, level-0(internal version).
 * 2 : monitor control, level-1(trial version).
 * 3 : monitor control, level-2(official version).
 */
#define LK_MONITOR_SHIFT  (8)
#define LK_MONITOR_MASK   (7 << LK_MONITOR_SHIFT)
#define LK_MONITOR_LEVEL0 (1 << LK_MONITOR_SHIFT)
#define LK_MONITOR_LEVEL1 (2 << LK_MONITOR_SHIFT)
#define LK_MONITOR_LEVEL2 (3 << LK_MONITOR_SHIFT)
#endif

#define LK_DEBUG_PRINTK (1 << 0)
#define LK_DEBUG_FTRACE (1 << 1)

extern unsigned int g_opt_enable;
extern unsigned int g_opt_debug;

extern atomic64_t futex_inherit_set_times;
extern atomic64_t futex_inherit_unset_times;
extern atomic64_t futex_inherit_useless_times;
extern atomic64_t futex_low_count;
extern atomic64_t futex_high_count;

static inline bool locking_opt_enable(unsigned int enable)
{
	return g_opt_enable & enable;
}

#ifdef CONFIG_OPLUS_LOCKING_MONITOR
static inline bool lock_supp_level(int level)
{
	return (g_opt_enable & LK_MONITOR_MASK) == level;
}
#endif

static inline bool locking_opt_debug(int debug)
{
	return g_opt_debug & debug;
}

void register_rwsem_vendor_hooks(void);
void register_mutex_vendor_hooks(void);
void register_futex_vendor_hooks(void);
void register_monitor_vendor_hooks(void);
void lk_sysfs_init(void);
#ifdef CONFIG_OPLUS_LOCKING_MONITOR
int kern_lstat_init(void);
#endif

void unregister_rwsem_vendor_hooks(void);
void unregister_mutex_vendor_hooks(void);
void unregister_futex_vendor_hooks(void);
void unregister_monitor_vendor_hooks(void);
void lk_sysfs_exit(void);
#ifdef CONFIG_OPLUS_LOCKING_MONITOR
void kern_lstat_exit(void);
#endif
#endif /* _OPLUS_LOCKING_MAIN_H_ */

struct task_record {
#define RECOED_WINSIZE			(1 << 8)
#define RECOED_WINIDX_MASK		(RECOED_WINSIZE - 1)
	u8 winidx;
	u8 count;
};

struct locking_info {
	u64 waittime_stamp;
	u64 holdtime_stamp;
	/* Used in torture acquire latency statistic.*/
	u64 acquire_stamp;
	/*
	 * mutex or rwsem optimistic spin start time. Because a task
	 * can't spin both on mutex and rwsem at one time, use one common
	 * threshold time is OK.
	 */
	u64 opt_spin_start_time;
	struct task_struct *holder;
	u32 waittype;
	bool ux_contrib;
	/*
	 * Whether task is ux when it's going to be added to mutex or
	 * rwsem waiter list. It helps us check whether there is ux
	 * task on mutex or rwsem waiter list. Also, a task can't be
	 * added to both mutex and rwsem at one time, so use one common
	 * field is OK.
	 */
	bool is_block_ux;
	u32 kill_flag;
	/* for cfs enqueue smoothly.*/
	struct list_head node;
	struct task_struct *owner;
	struct list_head lock_head;
	u64 clear_seq;
	atomic_t lock_depth;
};

/* Please add your own members of task_struct here :) */
struct oplus_task_struct {
	/* CONFIG_OPLUS_FEATURE_SCHED_ASSIST */
	struct list_head ux_entry;

	struct task_struct *task;
	atomic64_t inherit_ux;
	u64 enqueue_time;
	u64 inherit_ux_start;
	u64 sum_exec_baseline;
	u64 total_exec;
	int ux_state;
	int ux_depth;
	int abnormal_flag;
	int im_flag;
	int tpd; /* task placement decision */
	/* CONFIG_OPLUS_FEATURE_SCHED_SPREAD */
	int lb_state;
	int ld_flag;
	/* CONFIG_OPLUS_FEATURE_TASK_LOAD */
	int is_update_runtime;
	int target_process;
	u64 wake_tid;
	u64 running_start_time;
	bool update_running_start_time;
	u64 exec_calc_runtime;
	struct task_record record[MAX_CLUSTER];	/* 2*u64 */
	/* CONFIG_OPLUS_FEATURE_FRAME_BOOST */
	struct list_head fbg_list;
	raw_spinlock_t fbg_list_entry_lock;
	unsigned int fbg_state;
	int fbg_depth;
	bool fbg_running; /* task belongs to a group, and in running */
	int preferred_cluster_id;
	u64 last_wake_ts;
/*#if IS_ENABLED(CONFIG_OPLUS_FEATURE_FDLEAK_CHECK)*/
	unsigned int fdleak_flag;
/*#endif*/

/*#if IS_ENABLED(CONFIG_OPLUS_LOCKING_STRATEGY)*/
	struct locking_info lkinfo;
/*#endif*/
#ifdef CONFIG_LOCKING_PROTECT
	unsigned long locking_start_time;
	struct list_head locking_entry;
	int locking_depth;
#endif /*CONFIG_LOCKING_PROTECT*/

	/* for loadbalance */
	u64 runnable_time;
	u64 exec_time;
	u64 exec_baseline;
	struct plist_node rtb;		/* rt boost task */

	/* for oplus secure guard */
	int sg_flag;
	int sg_scno;
	uid_t sg_uid;
	uid_t sg_euid;
	gid_t sg_gid;
	gid_t sg_egid;
} ____cacheline_aligned;

#define INVALID_PID						(-1)
struct oplus_lb {
	/* used for active_balance to record the running task. */
	pid_t pid;
};

static inline struct oplus_task_struct *get_oplus_task_struct(struct task_struct *t)
{
	struct oplus_task_struct *ots = NULL;

	/* not Skip idle thread */
	if (!t)
		return NULL;

	ots = (struct oplus_task_struct *) READ_ONCE(t->android_oem_data1[OTS_IDX]);
	if (IS_ERR_OR_NULL(ots))
		return NULL;

	return ots;
}

void locking_record_switch_in_cs(struct task_struct *tsk);

