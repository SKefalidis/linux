#include <linux/random.h>
#include <trace/events/sched.h>
#include "sched.h"
#include "pelt.h"
#include <linux/sched/cputime.h>
#include <linux/sched/hcs.h>

/* Macros and defines. */
/* Timeshare range = Whole range of this scheduler. */
#define	PRI_TIMESHARE_RANGE		(PRI_MAX_TIMESHARE - PRI_MIN_TIMESHARE + 1)
#define PRI_MIN_TIMESHARE		MIN_KTZ_PRIO
#define PRI_MAX_TIMESHARE		MAX_KTZ_PRIO

/* Interactive range. */
#define	PRI_INTERACT_RANGE		12
#define	PRI_MIN_INTERACT		PRI_MIN_TIMESHARE
#define	PRI_MAX_INTERACT		(PRI_MIN_TIMESHARE + PRI_INTERACT_RANGE - 1)

/* Batch range. */
#define	PRI_BATCH_RANGE			(PRI_TIMESHARE_RANGE - PRI_INTERACT_RANGE)
#define	PRI_MIN_BATCH			(PRI_MIN_TIMESHARE + PRI_INTERACT_RANGE)
#define	PRI_MAX_BATCH			PRI_MAX_TIMESHARE

/* Batch range separation. */
#define	SCHED_PRI_NRESV			(PRIO_MAX - PRIO_MIN)
#define	SCHED_PRI_NHALF			(SCHED_PRI_NRESV / 2)
#define	SCHED_PRI_MIN			(PRI_MIN_BATCH + 7)
#define	SCHED_PRI_MAX			(SCHED_PRI_MIN + 12)
#define	SCHED_PRI_RANGE			(SCHED_PRI_MAX - SCHED_PRI_MIN + 1)

/* Macros/defines used for stat computation. */
#define	SCHED_TICK_SHIFT		10	/* Used to avoid rounding errors. */
#define	SCHED_TICK_SECS			10	/* Number of secs for cpu stats. */
#define	SCHED_TICK_TARG			(HZ * SCHED_TICK_SECS)	/* 10s in ticks. */
#define	SCHED_TICK_MAX			(SCHED_TICK_TARG + HZ)
#define	SCHED_SLP_RUN_MAX		((HZ * 5) << SCHED_TICK_SHIFT)
#define	SCHED_INTERACT_MAX		(100)
#define	SCHED_INTERACT_HALF		(SCHED_INTERACT_MAX / 2)
#define	SCHED_INTERACT_THRESH	(30)

#define ktz_roundup(x, y) 		((((x)+((y)-1))/(y))*(y))
#define	SCHED_TICK_HZ(ts)		((ts)->ticks >> SCHED_TICK_SHIFT)
#define	SCHED_TICK_TOTAL(ts)	(max((ts)->ltick - (ts)->ftick, ((unsigned long long)HZ)))
#define	SCHED_PRI_TICKS(ts)	    (SCHED_TICK_HZ((ts)) / (ktz_roundup(SCHED_TICK_TOTAL((ts)), SCHED_PRI_RANGE) / SCHED_PRI_RANGE))
#define	SCHED_SLP_RUN_FORK		((HZ / 2) << SCHED_TICK_SHIFT)

/*
 * These parameters determine the slice behavior for batch work.
 */
#define	SCHED_SLICE_DEFAULT_DIVISOR	10	/* ~94 ms, 12 stathz ticks. */
#define	SCHED_SLICE_MIN_DIVISOR		6	/* DEFAULT/MIN = ~16 ms. */
#define	TDF_SLICEEND				0	/* TODO : find linux counterpart. */
#define TD_IS_IDLETHREAD(task)		false /* TODO : needed ? */

/*
 * Task states.
 */
#define TDS_INACTIVE	(2<<1)
#define TDS_INHIBITED	(2<<2)
#define TDS_CAN_RUN		(2<<3)
#define TDS_RUNQ		(2<<4)
#define TDS_RUNNING		(2<<5)

/* Flags from FreeBSD. */
#define SRQ_PREEMPTED 	(2<<1)

/* Locking stuff. */
#define TDQ_LOCK_ASSERT(tdq, flag)
#define THREAD_LOCK_ASSERT(td, flags)

/* Load balancing stuff */
#define THREAD_CAN_MIGRATE(td)	false /*TODO*/
#define BALANCING_CPU			0 /* CPU 0 is responsible for load balancing. */
#define	SCHED_AFFINITY(ts, t)	((ts)->ltick > jiffies - ((t) * affinity))
#define CG_SHARE_L1     		1
#define CG_SHARE_L2     		2
#define CG_SHARE_L3     		3

static int 		balance_ticks;
static int 		balance_interval 		= 128; /* Default set in sched_initticks(). */
unsigned int 	idle_stealing_cooldown 	= 500000UL;
static int 		affinity 				= 100; /* TODO : check validity of this. */
static int 		steal_thresh 			= 2;

/* Globals */
#define TICKINCR 			(1 << SCHED_TICK_SHIFT) /* 1 Should be correct. */
#define SCHED_SLICE 		(10) /* reset during boot. */
#define SCHED_SLICE_MIN		(1)  /* reset during boot. */
#define PREEMPT_THRESH 		(80)
#define PERIODIC_BALANCE	(1)

/* HCS VARIABLES START */

cpumask_t SMALL_CORE_GROUP_MASK;
cpumask_t BIG_CORE_GROUP_MASK;

const unsigned int SMALL_CORE_GROUP_LOAD_LIMIT = 80;
const unsigned int BIG_CORE_GROUP_LOAD_LIMIT = 90;

unsigned int hcs_score_threshold = SCHED_INTERACT_THRESH;
unsigned int hsc_score_deviation = 5;

/* HCS VARIABLES END */

unsigned int sysctl_ktz_enabled 		 = 1; /* Enabled by default */
unsigned int sysctl_ktz_forced_timeslice = 0; /* Force the value of a slice, 0 = default. */

/* Per cpu variables. */
DECLARE_PER_CPU(uint32_t, randomval);
DEFINE_PER_CPU(uint32_t, randomval);

/* Helper macros / defines. */
#define PRINT_DEBUG_INFO
#ifdef PRINT_DEBUG_INFO
#define LOG(...) 	printk_deferred(__VA_ARGS__)
#else
#define LOG(...) 	do {} while (0)
#endif // PRINT_DEBUG_INFO

#define KTZ_SE(p)	(&(p)->ktz_se)
#define TDQ(rq)		(&(rq)->ktz)
#define RQ(tdq)		(container_of(tdq, struct rq, ktz))



/* Trace functions */
static inline void 	trace_load			    (struct ktz_tdq *tdq);
static inline void 	trace_plb			    (void);
static inline void 	trace_inter			    (struct task_struct *p);

/* Print/Debug functions */
static inline void	print_groups		    (struct sched_domain *sd);
static inline void	print_sched_domain	    (int cpu);
static inline void 	print_stats			    (struct task_struct *p);
static inline void 	print_runq			    (struct runq *q);
static inline void 	print_tdq			    (struct ktz_tdq *tdq);
static inline void 	print_all_tdqs		    (void);
static inline void 	print_children		    (struct task_struct *p);
static inline void 	print_loads			    (void);

/* Utility functions */
static uint32_t 	sched_random		    (void);
static int          cpu_search              (cpumask_t *cpumask, struct cpu_search *low, struct cpu_search *high, const int match);
static inline int   cpu_search_lowest       (struct cpu_search *low);
static inline int   cpu_search_highest      (struct cpu_search *high);
static inline int   sched_lowest            (struct cpumask *mask, int pri, int maxload, int prefer);
static inline int   sched_highest           (struct cpumask *mask, int minload);
static struct sched_domain*
                    get_top_domain          (int cpu);
static inline struct task_struct*
                    ktz_task_of             (struct sched_ktz_entity *ktz_se);
static inline bool  is_enqueued             (struct task_struct *p);

/* Scheduler interface functions */
static void 		enqueue_task_ktz		(struct rq *rq, struct task_struct *p, int flags);
static void 		dequeue_task_ktz		(struct rq *rq, struct task_struct *p, int flags);
static void 		check_preempt_curr_ktz	(struct rq *rq, struct task_struct *p, int flags);
static struct task_struct*
					pick_next_task_ktz		(struct rq *rq);
static void 		put_prev_task_ktz		(struct rq *rq, struct task_struct *prev);
static void 		set_next_task_ktz		(struct rq *rq, struct task_struct *p, bool first);
static void 		task_tick_ktz			(struct rq *rq, struct task_struct *curr, int queued);
static void 		task_fork_ktz			(struct task_struct *p);
static void 		task_dead_ktz			(struct task_struct *p);
#ifdef CONFIG_SMP
static int 			balance_ktz				(struct rq *rq, struct task_struct *prev, struct rq_flags *rf);
static int 			select_task_rq_ktz		(struct task_struct *p, int cpu, int wake_flags);
static struct task_struct*
					pick_task_ktz			(struct rq *rq);
#endif
static void 		yield_task_ktz			(struct rq *rq);
static void 		switched_from_ktz		(struct rq *rq, struct task_struct *p);
static void 		switched_to_ktz			(struct rq *rq, struct task_struct *p);
static void 		prio_changed_ktz		(struct rq *rq, struct task_struct *p, int oldprio);
static void 		update_curr_ktz			(struct rq *rq);



/* Define Scheduler class KTZ (ULE) */
const struct sched_class ktz_sched_class;

DEFINE_SCHED_CLASS(ktz) = {
	.enqueue_task		= enqueue_task_ktz,
	.dequeue_task		= dequeue_task_ktz,

	.yield_task			= yield_task_ktz,
	.yield_to_task		= NULL,

	.check_preempt_curr	= check_preempt_curr_ktz,

	.pick_next_task		= pick_next_task_ktz,

	.put_prev_task		= put_prev_task_ktz,
	.set_next_task      = set_next_task_ktz,

#ifdef CONFIG_SMP
	.balance			= balance_ktz,
	.select_task_rq		= select_task_rq_ktz,
	.pick_task			= pick_task_ktz,
	.migrate_task_rq	= NULL,
	.task_woken			= NULL,
	.set_cpus_allowed   = set_cpus_allowed_common,

	.rq_online			= NULL,
	.rq_offline			= NULL,
	.find_lock_rq		= NULL,
#endif

	.task_tick			= task_tick_ktz,
	.task_fork			= task_fork_ktz,
	.task_dead			= task_dead_ktz,

	.switched_from		= switched_from_ktz,
	.switched_to		= switched_to_ktz,
	.prio_changed		= prio_changed_ktz,

	.update_curr		= update_curr_ktz,
};



#define PRINT(name)	printk_deferred(#name "\t\t = %d", name)

void init_ktz_tdq(struct ktz_tdq *ktz_tdq)
{
	INIT_LIST_HEAD(&ktz_tdq->queue);

	/* Init runqueues. */
	runq_init(&ktz_tdq->realtime);
	runq_init(&ktz_tdq->timeshare);
	runq_init(&ktz_tdq->idle);

	/* Print init message. */
	printk_deferred("||| STABLE WITH ORIGINAL PLB + fixed random |||\n");
	
	/* Print config. */
	PRINT(TICKINCR);
	PRINT(PRI_MIN_TIMESHARE);
	PRINT(PRI_MAX_TIMESHARE);
	PRINT(PRI_INTERACT_RANGE);
	PRINT(PRI_MIN_INTERACT);
	PRINT(PRI_MAX_INTERACT);
	PRINT(PRI_BATCH_RANGE);
	PRINT(PRI_MIN_BATCH);
	PRINT(PRI_MAX_BATCH);
	PRINT(SCHED_PRI_NRESV);
	PRINT(SCHED_PRI_NHALF);
	PRINT(SCHED_PRI_MIN);
	PRINT(SCHED_PRI_MAX);
	PRINT(SCHED_PRI_RANGE);

	if (smp_processor_id() == BALANCING_CPU)
		balance_ticks = max(balance_interval / 2, 1) + (sched_random() % balance_interval);

	/* HCS INIT */
	cpumask_clear(&SMALL_CORE_GROUP_MASK);
	cpumask_set_cpu(0, &SMALL_CORE_GROUP_MASK);
	cpumask_set_cpu(1, &SMALL_CORE_GROUP_MASK);
	cpumask_set_cpu(2, &SMALL_CORE_GROUP_MASK);
	cpumask_set_cpu(3, &SMALL_CORE_GROUP_MASK);
	printk("Small mask: %*pb\n", cpumask_pr_args(&SMALL_CORE_GROUP_MASK));

	cpumask_clear(&BIG_CORE_GROUP_MASK);
	cpumask_set_cpu(4, &BIG_CORE_GROUP_MASK);
	cpumask_set_cpu(5, &BIG_CORE_GROUP_MASK);
	cpumask_set_cpu(6, &BIG_CORE_GROUP_MASK);
	cpumask_set_cpu(7, &BIG_CORE_GROUP_MASK);
	printk("Big mask: %*pb\n", cpumask_pr_args(&BIG_CORE_GROUP_MASK));
}

#undef PRINT

static void pctcpu_update(struct sched_ktz_entity *ts, bool run)
{
	int t = jiffies;

	if ((uint)(t - ts->ltick) >= SCHED_TICK_TARG) {
		ts->ticks = 0;
		ts->ftick = t - SCHED_TICK_TARG;
	}
	else if (t - ts->ftick >= SCHED_TICK_MAX) {
		ts->ticks = (ts->ticks / (ts->ltick - ts->ftick)) *
		    (ts->ltick - (t - SCHED_TICK_TARG));
		ts->ftick = t - SCHED_TICK_TARG;
	}
	if (run)
		ts->ticks += (t - ts->ltick) << SCHED_TICK_SHIFT;
	ts->ltick = t;
}

/*
 * This routine enforces a maximum limit on the amount of scheduling history
 * kept.  It is called after either the slptime or runtime is adjusted.  This
 * function is ugly due to integer math.
 */
static void interact_update(struct task_struct *p)
{
	u_int sum;
	struct sched_ktz_entity *ke_se = KTZ_SE(p);

	sum = ke_se->runtime + ke_se->slptime;
	if (sum < SCHED_SLP_RUN_MAX)
		return;
	/*
	 * This only happens from two places:
	 * 1) We have added an unusual amount of run time from fork_exit.
	 * 2) We have added an unusual amount of sleep time from sched_sleep().
	 */
	if (sum > SCHED_SLP_RUN_MAX * 2) {
		if (ke_se->runtime > ke_se->slptime) {
			ke_se->runtime = SCHED_SLP_RUN_MAX;
			ke_se->slptime = 1;
		} else {
			ke_se->slptime = SCHED_SLP_RUN_MAX;
			ke_se->runtime = 1;
		}
		return;
	}
	/*
	 * If we have exceeded by more than 1/5th then the algorithm below
	 * will not bring us back into range.  Dividing by two here forces
	 * us into the range of [4/5 * SCHED_INTERACT_MAX, SCHED_INTERACT_MAX]
	 */
	if (sum > (SCHED_SLP_RUN_MAX / 5) * 6) {
		ke_se->runtime /= 2;
		ke_se->slptime /= 2;
		return;
	}
	ke_se->runtime = (ke_se->runtime / 5) * 4;
	ke_se->slptime = (ke_se->slptime / 5) * 4;
}

static int interact_score(struct task_struct *p)
{
	int div;
	struct sched_ktz_entity *ktz_se = KTZ_SE(p);

	/*
	 * The score is only needed if this is likely to be an interactive
	 * task.  Don't go through the expense of computing it if there's
	 * no chance.
	 */
	/*if (SCHED_INTERACT_THRESH <= SCHED_INTERACT_HALF &&
		ktz_se->runtime >= ktz_se->slptime)
			return (SCHED_INTERACT_HALF);*/

	if (ktz_se->runtime > ktz_se->slptime) {
		div = max(1ULL, ktz_se->runtime / SCHED_INTERACT_HALF);
		return (SCHED_INTERACT_HALF +
		    (SCHED_INTERACT_HALF - (ktz_se->slptime / div)));
	}
	if (ktz_se->slptime > ktz_se->runtime) {
		div = max(1ULL, ktz_se->slptime / SCHED_INTERACT_HALF);
		return (ktz_se->runtime / div);
	}
	/* runtime == slptime */
	if (ktz_se->runtime)
		return (SCHED_INTERACT_HALF);

	/*
	 * This can happen if slptime and runtime are 0.
	 */
	return (0);
}

static int hcs_score(struct task_struct *p)
{
	// use bias only if we are on the big cores to maybe move to the small cores
	// it doesn't make sense to move a task from a small core to a big core because it uses the CPU effectively, since it doesn't use it enough to warrant moving to the big core anyway

	return interact_score(p);
}

/*
 * Load is maintained for all threads RUNNING and ON_RUNQ.  Add the load
 * for this thread to the referenced thread queue.
 */
static void tdq_load_add(struct ktz_tdq *tdq, struct task_struct *p)
{
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_ASSERT(p, MA_OWNED);

	tdq->load++;
	//if ((td->td_flags & TDF_NOLOAD) == 0) /* We probably dont care. */
	tdq->sysload++;
	trace_load(tdq);
}

/*
 * Remove the load from a thread that is transitioning to a sleep state or
 * exiting.
 */
static void
tdq_load_rem(struct ktz_tdq *tdq, struct task_struct *p)
{
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_ASSERT(p, MA_OWNED);

	tdq->load--;
	//if ((td->td_flags & TDF_NOLOAD) == 0) /* We probably dont care. */
	tdq->sysload--;
	trace_load(tdq);
}

/*
 * Bound timeshare latency by decreasing slice size as load increases.  We
 * consider the maximum latency as the sum of the threads waiting to run
 * aside from curthread and target no more than SCHED_SLICE latency but
 * no less than SCHED_SLICE_MIN runtime.
 */
static inline int compute_slice(struct ktz_tdq *tdq) 
{
	int load = tdq->sysload - 1;
	if (sysctl_ktz_forced_timeslice)
		return sysctl_ktz_forced_timeslice;
	if (load >= SCHED_SLICE_MIN_DIVISOR)
		return (SCHED_SLICE_MIN);
	if (load <= 1)
		return (SCHED_SLICE);
	return (SCHED_SLICE / load);
}

/*
 * Scale the scheduling priority according to the "interactivity" of this
 * process.
 */
static void compute_priority(struct task_struct *p)
{
	int score;
	int inter;
	int pri;
	struct sched_ktz_entity *ktz_se = KTZ_SE(p);

	/*
	 * If the score is interactive we place the thread in the realtime
	 * queue with a priority that is less than kernel and interrupt
	 * priorities.  These threads are not subject to nice restrictions.
	 *
	 * Scores greater than this are placed on the normal timeshare queue
	 * where the priority is partially decided by the most recent cpu
	 * utilization and the rest is decided by nice value.
	 *
	 * The nice value of the process has a linear effect on the calculated
	 * score.  Negative nice values make it easier for a thread to be
	 * considered interactive.
	 */
	inter = interact_score(p);
	score = max(0, inter + task_nice(p));
	if (score < SCHED_INTERACT_THRESH) {
		pri = PRI_MIN_INTERACT;
		pri += ((PRI_MAX_INTERACT - PRI_MIN_INTERACT + 1) / SCHED_INTERACT_THRESH) * score;
	} else {
		pri = SCHED_PRI_MIN;
		if (ktz_se->ticks) {
			int d;
			d = min(SCHED_PRI_TICKS(ktz_se), (unsigned long long)(SCHED_PRI_RANGE - 1));
			if (d < 0) {
				BUG();
			}
			pri += d;
		}
		pri += (int)((40 * task_nice(p)) / 104);
	}

	/* Test : */
	p->ktz_prio = pri;
	ktz_se->base_user_pri = pri;
	if (ktz_se->lend_user_pri <= pri)
		return;
	ktz_se->user_pri = pri;
}

/*
 * Add a thread to the actual run-queue.  Keeps transferable counts up to
 * date with what is actually on the run-queue.  Selects the correct
 * queue position for timeshare threads.
 */
static inline void tdq_runq_add(struct ktz_tdq *tdq, struct task_struct *td, int flags)
{
	struct sched_ktz_entity *ts =  KTZ_SE(td);
	struct runq *dest;
	u_char pri;

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_ASSERT(td, MA_OWNED);

	pri = td->ktz_prio;
	ts->state = TDS_RUNQ;	
	if (THREAD_CAN_MIGRATE(td)) {
		tdq->transferable++;
		// TODO
		//ts->flags |= TSF_XFERABLE;
	}
	if (pri < PRI_MIN_BATCH) {
		dest = &tdq->realtime;
	}
	else if (pri <= PRI_MAX_BATCH) {
		dest = &tdq->timeshare;
		if ((flags & SRQ_PREEMPTED) == 0) {
			pri = KTZ_HEADS_PER_RUNQ * (pri - PRI_MIN_BATCH) / PRI_BATCH_RANGE;
			pri = (pri + tdq->idx) % KTZ_HEADS_PER_RUNQ;
			if (tdq->ridx != tdq->idx && pri == tdq->ridx)
				pri = (unsigned char)(pri - 1) % KTZ_HEADS_PER_RUNQ;
		}
		else {
			pri = tdq->ridx;
		}
		runq_add_pri(dest, td, pri, flags);
		return;
	}
	else {
		/* Should never happen. */
		dest = &tdq->idle;
	}
	runq_add(dest, td, flags);
}

static void tdq_add(struct ktz_tdq *tdq, struct task_struct *p, int flags)
{
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	if (p->ktz_prio < tdq->lowpri)
		tdq->lowpri = p->ktz_prio;
	tdq_runq_add(tdq, p, flags);
	tdq_load_add(tdq, p);
}

/* 
 * Remove a thread from a run-queue.  This typically happens when a thread
 * is selected to run.  Running threads are not on the queue and the
 * transferable count does not reflect them.
 */
static inline void tdq_runq_rem(struct ktz_tdq *tdq, struct task_struct *td)
{
	struct sched_ktz_entity *ts = KTZ_SE(td);
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	// TODO
	/*if (ts->ts_flags & TSF_XFERABLE) {
		tdq->tdq_transferable--;
		ts->ts_flags &= ~TSF_XFERABLE;
	}*/
	if (ts->curr_runq == &tdq->timeshare) {
		if (tdq->idx != tdq->ridx)
			runq_remove_idx(ts->curr_runq, td, &tdq->ridx);
		else
			runq_remove_idx(ts->curr_runq, td, NULL);
	} 
	else {
		runq_remove(ts->curr_runq, td);
	}
}

/*
 * Pick the highest priority task we have and return it.
 */
static struct task_struct *tdq_choose(struct ktz_tdq *tdq, struct task_struct *except, bool find_lowest_priority)
{
	struct task_struct *td;

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	if (find_lowest_priority == false) {
		td = runq_choose(&tdq->realtime, except, find_lowest_priority);
		if (td != NULL) {
			return (td);
		}
		td = runq_choose_from(&tdq->timeshare, tdq->ridx, except, find_lowest_priority);
		if (td != NULL) {
			return td;
		}
	} else {
		td = runq_choose_from(&tdq->timeshare, tdq->ridx, except, find_lowest_priority);
		if (td != NULL) {
			return td;
		}
		td = runq_choose(&tdq->realtime, except, find_lowest_priority);
		if (td != NULL) {
			return (td);
		}
	}
	td = runq_choose(&tdq->idle, except, find_lowest_priority);
	if (td != NULL) {
		BUG();
		return td;
	}
	return NULL;
}

void sched_fork_thread(struct task_struct *td, struct task_struct *child)
{
	struct sched_ktz_entity *ts;
	struct sched_ktz_entity *ts2;
	struct ktz_tdq *tdq;

	tdq = &this_rq()->ktz;
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	ts = KTZ_SE(td);
	ts2 = KTZ_SE(child);
	/*child->td_oncpu = NOCPU;
	child->td_lastcpu = NOCPU;
	child->td_lock = TDQ_LOCKPTR(tdq);
	child->td_cpuset = cpuset_ref(td->td_cpuset);
	ts2->ts_cpu = ts->ts_cpu;
	ts2->ts_flags = 0;*/
	/*
	 * Grab our parents cpu estimation information.
	 */
	ts2->ticks = ts->ticks;
	ts2->ltick = ts->ltick;
	ts2->ftick = ts->ftick;
	/*
	 * Do not inherit any borrowed priority from the parent.
	 */
	//child->td_priority = child->td_base_pri;
	/*
	 * And update interactivity score.
	 */
	ts2->slptime = ts->slptime;
	ts2->runtime = ts->runtime;
	/* Attempt to quickly learn interactivity. */
	ts2->slice = compute_slice(tdq) - SCHED_SLICE_MIN;
}

static void sched_interact_fork(struct task_struct *td)
{
	struct sched_ktz_entity *ts;
	int ratio;
	int sum;

	ts = KTZ_SE(td);
	sum = ts->runtime + ts->slptime;
	if (sum > SCHED_SLP_RUN_FORK) {
		ratio = sum / SCHED_SLP_RUN_FORK;
		ts->runtime /= ratio;
		ts->slptime /= ratio;
	}
}

/*
 * Set lowpri to its exact value by searching the run-queue and
 * evaluating curthread.  curthread may be passed as an optimization.
 */
static void tdq_setlowpri(struct ktz_tdq *tdq, struct task_struct *ctd)
{
	struct task_struct *td;
	struct rq *rq = RQ(tdq);

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	if (ctd == NULL)
		ctd = rq->curr;
	td = tdq_choose(tdq, NULL, false);
	if (td == NULL || td->ktz_prio > ctd->ktz_prio)
		tdq->lowpri = ctd->ktz_prio;
	else
		tdq->lowpri = td->ktz_prio;
}

static void sched_thread_priority(struct ktz_tdq *tdq, struct task_struct *td, int prio)
{
	int oldpri;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	if (td->ktz_prio == prio)
		return;
	/*
	 * If the priority has been elevated due to priority
	 * propagation, we may have to move ourselves to a new
	 * queue.  This could be optimized to not re-add in some
	 * cases.
	 */
	if (is_enqueued(td) && prio < td->ktz_prio) {
		tdq_runq_rem(tdq, td);
		/* Don't forget to remove the load as tdq_add will later inc. it .*/
		tdq_load_rem(tdq, td);
		td->ktz_prio = prio;
		tdq_add(tdq, td, 0);
		return;
	}
	/*
	 * If the thread is currently running we may have to adjust the lowpri
	 * information so other cpus are aware of our current priority.
	 */
	if (task_curr(td)) {
		oldpri = td->ktz_prio;
		td->ktz_prio = prio;
		if (prio < tdq->lowpri)
			tdq->lowpri = prio;
		else if (tdq->lowpri == oldpri)
			tdq_setlowpri(tdq, td);
		return;
	}
	td->ktz_prio = prio;
}

#ifdef CONFIG_SMP
static void detach_task(struct rq *src_rq, struct task_struct *p, int dest_cpu)
{
	deactivate_task(src_rq, p, 0);
	set_task_cpu(p, dest_cpu);
}

static void attach_task(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_held(&rq->__lock);

	BUG_ON(task_rq(p) != rq);
	activate_task(rq, p, 0);
	check_preempt_curr(rq, p, 0);
}

static bool can_migrate(struct task_struct *p, int to_cpu)
{
	BUG_ON(p == NULL);

	/* Can't migrate running task. */
	if (task_running(task_rq(p), p))
		return false;

	/* Can't migrate if dest is not allowed. */
	if (!cpumask_test_cpu(to_cpu, &(p->cpus_mask)))
		return false;

	if (TDQ(task_rq(p))->load <= 1)
		return false;

	/* Other filters might be added in the future. */

	return true;
}

static struct task_struct *runq_steal_from(struct runq *rq, int dest_cpu, int start)
{
	unsigned long *status;
	int bit;
	int offset;
	int size = KTZ_RUNQ_BITMAP_SIZE;

	status = rq->status;
	offset = start;

again:
	while ((bit = find_next_bit(status, size, offset)) != size) {
		struct list_head *queue = &rq->queues[bit];
		struct sched_ktz_entity *tmp;
		struct task_struct *tmp_task;
		list_for_each_entry(tmp, queue, runq) {
			tmp_task = ktz_task_of(tmp);
			if (can_migrate(tmp_task, dest_cpu))
				return tmp_task;
		}
		offset = bit + 1;
	}
	if (start != 0) {
		offset = 0;
		start = 0;
		goto again;
	}
	return NULL;
}

static struct task_struct *runq_steal(struct runq *rq, int dest_cpu)
{
	return runq_steal_from(rq, dest_cpu, 0);
}

static struct task_struct *tdq_steal(struct ktz_tdq *tdq, int cpu)
{
	struct task_struct *td;

	if ((td = runq_steal(&tdq->realtime, cpu)) != NULL)
		return (td);
	if ((td = runq_steal_from(&tdq->timeshare, cpu, tdq->ridx)) != NULL)
		return (td);
	if ((td = runq_steal(&tdq->idle, cpu)) != NULL)
		BUG();
	return NULL;
}

/*
 * Transfer load between two imbalanced thread queues.
 */
static int sched_balance_pair(struct ktz_tdq *high, struct ktz_tdq *low)
{
	int dest_cpu;
	unsigned long flags;
	struct task_struct *stolen;
	struct rq *high_rq = RQ(high);
	struct rq *low_rq = RQ(low);

	/* We will move a task from high to low. */
	dest_cpu = RQ(low)->cpu;

	raw_spin_lock_irqsave(&high_rq->__lock, flags);

	/* Try to steal a task. */
	stolen = tdq_steal(high, dest_cpu);
	if (stolen) {
		detach_task(high_rq, stolen, dest_cpu);
	}

	raw_spin_unlock(&high_rq->__lock);
	local_irq_restore(flags);

	/* Attach the stolen task at the destination if needed. */
	if (stolen) {
		raw_spin_lock_irqsave(&low_rq->__lock, flags);
		attach_task(low_rq, stolen);
		raw_spin_unlock(&low_rq->__lock);
		local_irq_restore(flags);
		resched_curr(low_rq);
		return 1;
	}
	else {
		return 0;
	}
}

/*
 * Fixed version that can migrate multiple threads from the same cpu.
 */
static int sched_balance_group_fixed(struct sched_domain *sd, cpumask_t base_cpumask)
{
	struct cpumask hmask, lmask; 
	int high, low, anylow, moved;
	struct ktz_tdq *tdq_high;
	struct ktz_tdq *tdq_low;

	cpumask_copy(&hmask, &base_cpumask);
	(void) cpumask_and(&hmask, &hmask, cpu_online_mask);
	moved = 0;

	for (;;) {
		high = sched_highest(&hmask, 1);
		/* Stop if there is no more CPU with transferrable threads. */
		if (high == -1)
			break;
		tdq_high = TDQ(cpu_rq(high));
		//cpumask_clear_cpu(high, &hmask);
		cpumask_copy(&lmask, &hmask);
		cpumask_clear_cpu(high, &lmask);
		/* Stop if there is no more CPU left for low. */
		if (cpumask_empty(&lmask))
			break;
		anylow = 1;
nextlow:
		if (cpumask_empty(&lmask)) {
			cpumask_clear_cpu(high, &hmask);
			continue;
		}
		low = sched_lowest(&lmask, -1, tdq_high->load - 1, high);
		BUG_ON(low == high);
		/* Stop if we looked well and found no less loaded CPU. */
		if (anylow && low == -1)
			break;
		/* Go to next high if we found no less loaded CPU. */
		if (low == -1)
			continue;
		tdq_low = TDQ(cpu_rq(low));
		/* Transfer thread from high to low. */
		if (sched_balance_pair(tdq_high, tdq_low)) {
			/* CPU that got thread can no longer be a donor. */
			cpumask_clear_cpu(low, &hmask);
			moved ++;
		} else {
			/*
			 * If failed, then there is no threads on high
			 * that can run on this low. Drop low from low
			 * mask and look for different one.
			 */
			cpumask_clear_cpu(low, &lmask);
			anylow = 0;
			goto nextlow;
		}
	}
	return moved;
}

static void move_task_between_cores_of_different_groups(struct task_struct *p, struct rq *high_rq, struct rq *low_rq, cpumask_t *new_cpumask)
{
	unsigned long flags;
	bool running;

	/* remove process from one core-group */
	raw_spin_lock_irqsave(&high_rq->__lock, flags);
	running = task_current(high_rq, p);
	if (running) {
		BUG(); /* unsupported by the ULE port */
		put_prev_task(high_rq, p);
	}
	do_set_cpus_allowed(p, new_cpumask);
	detach_task(high_rq, p, low_rq->cpu);
	if (running)
		set_next_task(high_rq, p);
	raw_spin_unlock(&high_rq->__lock);
	local_irq_restore(flags);

	/* move process to the other core-group */
	raw_spin_lock_irqsave(&low_rq->__lock, flags);
	attach_task(low_rq, p);
	raw_spin_unlock(&low_rq->__lock);
	local_irq_restore(flags);
	resched_curr(low_rq);
}

static void move_task_to_core_group(struct task_struct *p, cpumask_t *new_cpumask)
{
	struct rq 		*old_rq;
	struct rq 	 	*new_rq;
	int				 old;
	int				 new;

	old = p->on_cpu;
	new = sched_lowest(new_cpumask, -1, INT_MAX, -1);

	old_rq = cpu_rq(old);
	new_rq = cpu_rq(new);

	move_task_between_cores_of_different_groups(p, old_rq, new_rq, new_cpumask);
}

static int sched_hcs_balance_groups(struct sched_domain *sd)
{
	/* get utilization of core-groups */
	unsigned int 		 small_core_group_util;
	unsigned int 		 big_core_group_util;
	int 		 		 cpu;
	struct ktz_tdq 		*tdq_high;
	struct ktz_tdq 		*tdq_low;
	struct rq 			*high_rq;
	struct rq 			*low_rq;
	struct task_struct 	*p;
	int 				 high, low;
	int					 moved = 0;
	int 				 last_moved_hcs_score = -1;
	bool				 moved_from_no_mans_land = false;

	for_each_cpu(cpu, &SMALL_CORE_GROUP_MASK) {
		small_core_group_util += cpu_util_ktz_percentage(cpu);
	}
	small_core_group_util /= cpumask_weight (&SMALL_CORE_GROUP_MASK);

	for_each_cpu(cpu, &BIG_CORE_GROUP_MASK) {
		big_core_group_util += cpu_util_ktz_percentage(cpu);
	}
	big_core_group_util /= cpumask_weight (&BIG_CORE_GROUP_MASK);

	/* if neither group is extremely loaded we don't need to do anything */
	if (small_core_group_util < SMALL_CORE_GROUP_LOAD_LIMIT && big_core_group_util < BIG_CORE_GROUP_LOAD_LIMIT)
		return 0;

	/* if both groups are overloaded, use lottery scheduling:
	1. Try to keep a balanced load between core groups. Currently, because of the way HCS/ULE works, we migrate some processes from the big-core group to the small-core group. New processes
	are added in the big-core group which also contains the batch processes, so it is almost certain that it is far more loaded than the small-core group (TODO: Use PELT).
		ex1: 4/4 system: make -j6 -> 6 heavy processes are added in the big-core group -> at most 4 of the lightest rpocesses get moved to the small core group, if the big-core group contains
		no other processes that means that 2 compile jobs are move to the small core group.	
		ex2: 4/4 system: sudden burst of activity in 2 cores of the small core group -> processes get moved to the big core group (interactivity mehcanism), make -j6 -> 6 more heavy processes for a total of 8 batch tasks
		-> 4 of them get moved to the small-core group.
	The main issues with this approach is that because of ULE's load balancing small-cores would probably work more efficiently if all small tasks were moved away from small cores which are under heavy load (either because
	of this load balancing or because of some other reason). That's why rewriting ULE's load balancing using PELT could prove advantegeous.
	2. Run the lottery algorithm (only on batch processes).
	*/
	/* heuristic for tasks in the lottery? Is there a way to tell if a task is likely to be mischeduled by the bias? */
	if (small_core_group_util > SMALL_CORE_GROUP_LOAD_LIMIT && big_core_group_util > BIG_CORE_GROUP_LOAD_LIMIT) {
		/* 1. Don't do anything if the small-core group is already overutilized (95 chosen as a placeholder value until proper load-tracking is implemented for the runqueue) */
		if (small_core_group_util < 95) {
			unsigned int small_core_group_process_count = 0;
			unsigned int big_core_group_process_count = 0;

			for_each_cpu(cpu, &SMALL_CORE_GROUP_MASK) {
				struct rq *rq = cpu_rq(cpu);
				small_core_group_process_count += rq->nr_running;
			}
			for_each_cpu(cpu, &BIG_CORE_GROUP_MASK) {
				struct rq *rq = cpu_rq(cpu);
				big_core_group_process_count += rq->nr_running;
			}

			while (big_core_group_process_count > cpumask_weight (&BIG_CORE_GROUP_MASK) && moved < cpumask_weight (&SMALL_CORE_GROUP_MASK)) {
				/* migrate one process to the small core group */
				big_core_group_process_count--;
				moved++;
			}
		}

#ifdef HCS_LOTTERY_SCHEDULING
		/* 2. Run when both core groups are under extremely heavy load (TODO: Use PELT) caused by a number of heavy processes. */
		if (small_core_group_util > 99 && big_core_group_util > 99) {
			/* move BIG_CORE_GROUP_n processes between the 2 groups. To select the processes:
			1. Do not choose running processes (it's not supported by the ULE port for now).
			2. Randomly select processes based on the bias of the processes and the runtime on each core-type:
			- Each runqueue knows the amount of tickets on each core.
			- Select BIG_CORE_GROUP_n tickets.
			- Split the tickets between the cores of the small-core group.
			- As the task_tick function runs on each core, count tickets. When you reach a selected ticket number, mark the current process to be moved to the big core group and steal a process from the big core group.
			*/
		}
#endif // HCS_LOTTERY_SCHEDULING

		return 0;
	}

	/* if one group is under heavy load and the other one is not do the following (general idea):
	 1. Move any tasks that are in the "no-man's land" of hcs-score away from the overloaded group 
	    Alternatively we could set hcs_score_deviation to null for the overloaded group (maybe we can do both, since we have an upper bound for processes moved in this function).
	 2. If there are no such tasks, move tasks from the overloaded group:
	 	- If the small-score group is overloaded, move multiple tasks to the big-core group.
		- If the big-core group is overloaded, move one (or maybe two) task(s) for each small-core to the small-core group.
		In this case, the group which is not overloaded also expands its ownership over the range of hcs-score values. Specifically,
		it takes ownership in such a way that the last stolen process is placed in the middle of the "no-man's land" of hsc-scores.
	*/
	if (small_core_group_util > SMALL_CORE_GROUP_LOAD_LIMIT) {
		/* move from no man's land
			Stop when there are either no more processes in no-man's land or we have moved enough processes.
			If you have moved a number of processes equal or larger than the number of small cores, then stop migrating processes,
			otherwise, continue migrating proceses in the second step 
		*/
		while (true) {
			high = sched_highest(&SMALL_CORE_GROUP_MASK, 1); /* FIXME: searches for the CPU with the most number of processes, instead of that
																we should search for processes in no-man's land in all CPUs */
			low = sched_lowest(&BIG_CORE_GROUP_MASK, -1, INT_MAX, -1);
			if (high == -1 || low == -1)
				return 0;

			high_rq = cpu_rq(high);
			low_rq = cpu_rq(low);
			tdq_high = TDQ(high_rq);
			tdq_low = TDQ(low_rq);

			p = tdq_choose(tdq_high, task_current(high_rq, p), true); /* FIXME: possible race condition? we are selecting a process different than the current, but that might change if we don't lock the RQ */
			if (interact_score(p) > hcs_score_threshold) {
				move_task_between_cores_of_different_groups(p, high_rq, low_rq, &BIG_CORE_GROUP_MASK);
				moved_from_no_mans_land = true;
			} else if (moved_from_no_mans_land && moved >= cpumask_weight (&SMALL_CORE_GROUP_MASK)) {
				/* there are no more processes in no man's land, but we have moved enough processes already */
				return 0;
			} else {
				/* there are no processes in no man's land */
				break;
			}

			moved++;
			/* we have moved twice as many processes as there are small cores, this should be enough */
			if (moved == cpumask_weight (&SMALL_CORE_GROUP_MASK) * 2)
				return 0;
		}

		/* expand the ownership of the big-core group over the hcs-score range
			If you have moved a number of processes equal or larger than the number of small cores, then stop migrating processes,
			otherwise, continue migrating proceses in the second step 
		*/
		while (true) {
			high = sched_highest(&SMALL_CORE_GROUP_MASK, 1); /* FIXME: searches for the CPU with the most number of processes, instead of that
																we should search for the heaviest processes in all CPUs */
			low = sched_lowest(&BIG_CORE_GROUP_MASK, -1, INT_MAX, -1);
			if (high == -1 || low == -1)
				return 0;

			high_rq = cpu_rq(high);
			low_rq = cpu_rq(low);
			tdq_high = TDQ(high_rq);
			tdq_low = TDQ(low_rq);

			p = tdq_choose(tdq_high, task_current(high_rq, p), true);
			last_moved_hcs_score = hcs_score(p);
			move_task_between_cores_of_different_groups(p, high_rq, low_rq, &BIG_CORE_GROUP_MASK);

			moved++;
			/* we have moved twice as many processes as there are small cores, this should be enough */
			if (moved == cpumask_weight (&SMALL_CORE_GROUP_MASK) * 2)
				break;
		}

		hcs_score_threshold = last_moved_hcs_score - hsc_score_deviation / 2;
	} else {
		/* move from no man's land
			Stop when there are either no more processes in no-man's land or we have moved enough processes.
			If you have moved a number of processes equal or larger than the number of small cores, then stop migrating processes,
			otherwise, continue migrating proceses in the second step 
		*/
		while (true) {
			high = sched_highest(&BIG_CORE_GROUP_MASK, 1); /* FIXME: searches for the CPU with the most number of processes, instead of that
																we should search for processes in no-man's land in all CPUs */
			low = sched_lowest(&SMALL_CORE_GROUP_MASK, -1, INT_MAX, -1);
			if (high == -1 || low == -1)
				return 0;

			high_rq = cpu_rq(high);
			low_rq = cpu_rq(low);
			tdq_high = TDQ(high_rq);
			tdq_low = TDQ(low_rq);

			p = tdq_choose(tdq_high, task_current(high_rq, p), true); /* FIXME: possible race condition? we are selecting a process different than the current, but that might change if we don't lock the RQ */
			if (interact_score(p) > hcs_score_threshold) {
				move_task_between_cores_of_different_groups(p, high_rq, low_rq, &SMALL_CORE_GROUP_MASK);
				moved_from_no_mans_land = true;
			} else {
				/* there are no processes in no man's land */
				break;
			}

			moved++;
			/* we have moved as many processes as there are small cores, this should be enough */
			if (moved == cpumask_weight (&SMALL_CORE_GROUP_MASK))
				return 0;
		}

		/* expand the ownership of the small-core group over the hcs-score range
			If you have moved a number of processes equal or larger than the number of small cores, then stop migrating processes,
			otherwise, continue migrating proceses in the second step 
		*/
		while (true) {
			high = sched_highest(&BIG_CORE_GROUP_MASK, 1); /* FIXME: searches for the CPU with the most number of processes, instead of that
																we should search for the heaviest processes in all CPUs */
			low = sched_lowest(&SMALL_CORE_GROUP_MASK, -1, INT_MAX, -1);
			if (high == -1 || low == -1)
				return 0;

			high_rq = cpu_rq(high);
			low_rq = cpu_rq(low);
			tdq_high = TDQ(high_rq);
			tdq_low = TDQ(low_rq);

			p = tdq_choose(tdq_high, task_current(high_rq, p), true);
			last_moved_hcs_score = hcs_score(p);
			move_task_between_cores_of_different_groups(p, high_rq, low_rq, &SMALL_CORE_GROUP_MASK);

			moved++;
			/* we have moved twice as many processes as there are small cores, this should be enough */
			if (moved == cpumask_weight (&SMALL_CORE_GROUP_MASK))
				break;
		}

		hcs_score_threshold = last_moved_hcs_score + hsc_score_deviation / 2;
	}

	return 0;
}

static int sched_balance(struct rq *rq)
{
	int moved;
	struct sched_domain *top;

	balance_ticks = max(balance_interval / 2, 1) + (sched_random() % balance_interval);

	if (!rq->sd)
		return 0;
	top = get_top_domain(smp_processor_id());

	if (!top)
		return 0;

	trace_plb();

	raw_spin_unlock(&rq->__lock);
#ifdef HCS_BALANCE
	sched_hcs_balance_groups(top);
#endif // HCS_BALANCE
	moved = sched_balance_group_fixed(top, BIG_CORE_GROUP_MASK) + sched_balance_group_fixed(top, SMALL_CORE_GROUP_MASK);
	raw_spin_lock(&rq->__lock);

	return moved;
}

/*
 * This cpu is idle, try to steal some tasks.
 */ 
static int tdq_idled(struct ktz_tdq *this_tdq)
{
	int this_cpu, victim_cpu;
	unsigned long flags;
	struct ktz_tdq *victim_tdq;
	struct cpumask cpus;
	struct sched_domain *sd;
	struct rq *victim_rq, *this_rq;
	struct task_struct *stolen;

	this_rq = RQ(this_tdq);
	this_cpu = smp_processor_id();

	if(!cpu_active(this_cpu))
		return 0;

	cpumask_setall(&cpus);
	/* Don't steal from oursleves. */
	/* TODO: Should we only steal from our group (right now we are stealing from all CPUs)? */
	cpumask_clear_cpu(this_cpu, &cpus);
	(void) cpumask_and(&cpus, &cpus, cpu_online_mask);

	for_each_domain(this_cpu, sd) {
		/* Maybe we received some task(s) during the stealing via
		 * select_task_rq or load balacing. */
		if (this_tdq->load)
			return 0;

		victim_cpu = sched_highest(&cpus, steal_thresh);
		if (victim_cpu == -1)
			continue;

		/* Remove the victim for next iterations. */
		cpumask_clear_cpu(victim_cpu, &cpus);
		victim_rq = cpu_rq(victim_cpu);
		victim_tdq = TDQ(victim_rq);

		raw_spin_lock_irqsave(&victim_rq->__lock, flags);
		/* Make sure the load of the victim still permits us to steal. */
		if (victim_tdq->load <= 1) {
			raw_spin_unlock(&victim_rq->__lock);
			local_irq_restore(flags);
			continue;
		}
		//moved = tdq_move(victim_tdq, this_tdq);
		stolen = tdq_steal(victim_tdq, this_cpu);
		if (stolen) {
			detach_task(victim_rq, stolen, this_cpu);
		}
		raw_spin_unlock(&victim_rq->__lock);
		local_irq_restore(flags);

		if (stolen) {
			raw_spin_lock_irqsave(&this_rq->__lock, flags);
			attach_task(this_rq, stolen);
			raw_spin_unlock(&this_rq->__lock);
			local_irq_restore(flags);
			return 1;
		}
	}

	/* Failed to steal. */
	return 0;
}
#endif /* CONFIG_SMP */

static void enqueue_task_ktz(struct rq *rq, struct task_struct *p, int flags)
{
	struct ktz_tdq *tdq = TDQ(rq);
	struct sched_ktz_entity *ktz_se = KTZ_SE(p);

	add_nr_running(rq,1);
	if (p->ktz_prio == 0)
		p->ktz_prio = p->prio;
	if (flags & ENQUEUE_WAKEUP) {
		/* Count sleeping ticks. */
		ktz_se->slptime += (jiffies - ktz_se->slptick) << SCHED_TICK_SHIFT;
		ktz_se->slptick = 0;
		interact_update(p);
		pctcpu_update(ktz_se, false);
	}

	/* Update prio before putting into runq. */
	compute_priority(p);
	ktz_se->slice = 0;
	tdq_add(tdq, p, 0);
	p->on_rq = TASK_ON_RQ_QUEUED;

	trace_inter(p);
}

static void dequeue_task_ktz(struct rq *rq, struct task_struct *p, int flags)
{
	struct ktz_tdq *tdq = TDQ(rq);
	struct sched_ktz_entity *ktz_se = KTZ_SE(p);

	BUG_ON(!ktz_se->curr_runq);
	sub_nr_running(rq,1);
	if (flags & DEQUEUE_SLEEP) {
		ktz_se->slptick = jiffies;
	}
	//list_del_init(&ktz_se->run_list);
	BUG_ON(!ktz_se->curr_runq);
	tdq_runq_rem(tdq, p);
	ktz_se->curr_runq = NULL;
	tdq_load_rem(tdq, p);
	if (p->ktz_prio == tdq->lowpri)
		tdq_setlowpri(tdq, NULL);
	p->on_rq = 0;

	trace_inter(p);
}

/*
 * Very simplified version.
 */
static void check_preempt_curr_ktz(struct rq *rq, struct task_struct *p, int flags)
{
	struct task_struct *curr = rq->curr;
	struct sched_ktz_entity *ktz_se = KTZ_SE(curr);
	int pri = p->sched_class == &ktz_sched_class ? p->ktz_prio : p->prio;
	int cpri = curr->ktz_prio;

	if (pri >= cpri)
		return;

	if (!PREEMPT_THRESH)
		return;

	if (pri <= PREEMPT_THRESH) {
		if (curr->sched_class == &ktz_sched_class) {
			/* Mark the task being preempted as SRQ_PREEMPTED. */
			ktz_se->preempted = 1;
		}
		resched_curr(rq);
	}

	// TODO : Add when adding SMP support. ?
	/*if (remote && pri <= PRI_MAX_INTERACT && cpri > PRI_MAX_INTERACT)
		return (1);*/
}

static struct task_struct *pick_next_task_ktz(struct rq *rq)
{
	struct ktz_tdq *tdq = TDQ(rq);
	struct task_struct *next_task;
#ifdef CONFIG_SMP
	int steal;
	int again = 0;
#endif

redo:
	if (tdq->load) {
#ifdef CONFIG_SMP
		rq->idle_stamp = 0;
#endif
		next_task = tdq_choose(tdq, NULL, false);

		update_ktz_rq_load_avg(rq_clock_pelt(rq), rq, 0);
		cpufreq_update_util (rq, 0);

		rq->ktz.curr = next_task;

		return next_task;
	}
	else {
#ifdef CONFIG_SMP
		BUG_ON(again);

		rq->idle_stamp = rq_clock(rq);
		/*if (rq->avg_idle < idle_stealing_cooldown)
			return NULL;*/

		/* Steal something. */
		// lockdep_unpin_lock(&rq->__lock, cookie);
		raw_spin_unlock(&rq->__lock);
		steal = tdq_idled(tdq);
		raw_spin_lock(&rq->__lock);
		// lockdep_repin_lock(&rq->__lock, cookie);

		/* Either we managed to steal a task, or $BALANCING_CPU gave us
		 * one while we were trying. In both case we retry. */
		if (steal || tdq->load) {
			again = 1;
			goto redo;
		}
		else {
			update_idle_rq_clock_pelt(rq);

			return NULL;	
		}
#else	/* !CONFIG_SMP */
		return NULL;
#endif
	}
}

static void put_prev_task_ktz(struct rq *rq, struct task_struct *prev)
{
	struct ktz_tdq *tdq = TDQ(rq);
	struct sched_ktz_entity *ktz_se = KTZ_SE(prev);

	if (is_enqueued(prev)) {
		tdq_runq_rem(tdq, prev);
		tdq_runq_add(tdq, prev, ktz_se->preempted ? SRQ_PREEMPTED : 0);
		ktz_se->preempted = 0; /* Reset preempted bit. */
	}

	update_ktz_rq_load_avg(rq_clock_pelt(rq), rq, 1);
	cpufreq_update_util (rq, 0);

	rq->ktz.curr = NULL;
}

static void set_next_task_ktz(struct rq *rq, struct task_struct *p, bool first)
{
	update_ktz_rq_load_avg(rq_clock_pelt(rq), rq, 0);

	rq->ktz.curr = p;
}

#ifdef CONFIG_SMP
static void check_balance(struct rq *rq)
{
	printk("Check balance %d %d\n", PERIODIC_BALANCE, balance_ticks);
	if (PERIODIC_BALANCE && balance_ticks && --balance_ticks == 0) {
		sched_balance(rq);
	}
}
#endif

static void task_tick_ktz(struct rq *rq, struct task_struct *curr, int queued)
{
	struct ktz_tdq *tdq = TDQ(rq);
	struct sched_ktz_entity *ktz_se = KTZ_SE(curr);
	int hscore;

	tdq->oldswitchcnt = tdq->switchcnt;
	tdq->switchcnt = tdq->load;

#ifdef CONFIG_SMP
	if (smp_processor_id() == BALANCING_CPU) {
		check_balance(rq);
	}
	trace_load(tdq);
#endif

	/*
	 * Advance the insert index once for each tick to ensure that all
	 * threads get a chance to run.
	 */
	if (tdq->idx == tdq->ridx) {
		tdq->idx = (tdq->idx + 1) % KTZ_HEADS_PER_RUNQ;
		if (list_empty(&tdq->timeshare.queues[tdq->ridx]))
			tdq->ridx = tdq->idx;
	}

	/* Update CPU stats. */
	pctcpu_update(ktz_se, true);

	/* Account runtime. */
	ktz_se->runtime += TICKINCR;
	interact_update(curr);
	compute_priority(curr);
	trace_inter(curr);

	/* utilization */
	update_ktz_rq_load_avg(rq_clock_pelt(rq), rq, 1);
	cpufreq_update_util (rq, 0);

	if (!TD_IS_IDLETHREAD(curr) && ++ktz_se->slice >= compute_slice(tdq)) {
		ktz_se->slice = 0;
		//ktz_se->flags |= TDF_SLICEEND;
		resched_curr(rq);
	}

	/* HCS: core-group assignment
	 * If the running process must change group mark it to be rescheduled.
	 * When it is being rescheduled the scheduler will transfer it to the correct core-group.
	 */
	hscore = hcs_score(curr);
	if (hscore > hcs_score_threshold + hsc_score_deviation && cpumask_bits(&curr->cpus_mask) != cpumask_bits(&BIG_CORE_GROUP_MASK)) {
		resched_curr(rq); /* FIXME: does this really lead to a change of core-group? */
	} else if (hscore < hcs_score_threshold - hsc_score_deviation  && cpumask_bits(&curr->cpus_mask) != cpumask_bits(&SMALL_CORE_GROUP_MASK)) {
		resched_curr(rq);
	}
}

static void task_fork_ktz(struct task_struct *p)
{
	struct task_struct *child = p;
	struct task_struct *parent = current;
	struct sched_ktz_entity *pktz_se = KTZ_SE(parent);

	/* Update parent stats. */
	pctcpu_update(pktz_se, task_curr(parent));	
	sched_fork_thread(parent, child);

	sched_interact_fork(child);
	compute_priority(child);

	pktz_se->runtime += TICKINCR;
	interact_update(parent);
	compute_priority(parent);
}

static void task_dead_ktz(struct task_struct *p)
{
	struct task_struct *parent = p->parent;
	struct task_struct *child = p;
	struct sched_ktz_entity *pktz_se = KTZ_SE(parent);
	struct sched_ktz_entity *cktz_se = KTZ_SE(child);

	/* Add runtime of child to the parent. This penalizes parents that
	 * spawn batch children such as make. */
	pktz_se->runtime += cktz_se->runtime;
	interact_update(parent);
	compute_priority(parent);
}

#ifdef CONFIG_SMP
static int select_task_rq_ktz(struct task_struct *p, int cpu, int wake_flags)
{
	int curr_cpu, this_cpu, pri;
	struct ktz_tdq *curr_tdq;
	struct sched_ktz_entity *ktz_se;
	struct sched_domain *sd;
	struct sched_domain *last_domain;
	struct sched_domain *root_domain;
	struct ktz_tdq *this_tdq;
	struct rq *rq0;

	ktz_se = KTZ_SE(p);

	/* 
	 * HCS: Interactivity based core-group assignment.
	 */

	/* update interactivity score, FIXME: also happens in enqueue */
	if (wake_flags & WF_TTWU) {
		ktz_se->slptime += (jiffies - ktz_se->slptick) << SCHED_TICK_SHIFT;
		ktz_se->slptick = 0;
		interact_update(p);
		pctcpu_update(ktz_se, false);
	}

	/* set core-group */
	if (wake_flags & WF_TTWU) {
		struct rq_flags rf;
		int hscore = hcs_score(p);
		if (hscore > hcs_score_threshold + hsc_score_deviation && cpumask_bits(&p->cpus_mask) != cpumask_bits(&BIG_CORE_GROUP_MASK)) {
			do_set_cpus_allowed(p, &BIG_CORE_GROUP_MASK);
		} else if (hscore < hcs_score_threshold - hsc_score_deviation  && cpumask_bits(&p->cpus_mask) != cpumask_bits(&SMALL_CORE_GROUP_MASK)) {
			do_set_cpus_allowed(p, &SMALL_CORE_GROUP_MASK);
		}
	} else if (wake_flags & WF_FORK) {
		do_set_cpus_allowed(p, &BIG_CORE_GROUP_MASK);
		ktz_se->slptime = 0;
		ktz_se->runtime = 0; /* TODO: deal with this in task_fork */
	}

	/* 
	 * ULE: select_task_rq 
	 */
	rq0 = cpu_rq(0);
	if (!rq0->sd) {
		return 0;
	}

	curr_cpu = task_cpu(p);
	curr_tdq = TDQ(task_rq(p));
	this_cpu = smp_processor_id();
	pri = p->ktz_prio;
	root_domain = get_top_domain(this_cpu);
	this_tdq = TDQ(cpu_rq(this_cpu));
	
	/*
	 * If the task can run on the last cpu and the affinity has not
	 * expired or it is idle run it there.
	 */
	if (cpumask_test_cpu(curr_cpu, &(p->cpus_mask)) &&
	   curr_tdq->load == 0 && SCHED_AFFINITY(ktz_se, CG_SHARE_L2)) {
		return curr_cpu;
	}

	/*
	 * Search for the last level cache CPU group in the tree.
	 * Skip caches with expired affinity time and SMT groups.
	 * Affinity to higher level caches will be handled less aggressively.
	 */
	last_domain = NULL;
	for_each_domain(this_cpu, sd) {
		if (!SCHED_AFFINITY(ktz_se, sd->level))
			continue;
		last_domain = sd;
	}

	cpu = -1;

	/* If not top domain. */
	if (last_domain && last_domain != root_domain) {
		cpumask_t cpumask;
		cpumask_and(&cpumask, sched_domain_span(last_domain), &p->cpus_mask);
		cpu = sched_lowest(&cpumask, max(pri, PRI_MAX_TIMESHARE), INT_MAX, curr_cpu);
	}

	/* Search globally for the less loaded CPU we can run now. */
	if (cpu == -1) {
		cpu = sched_lowest(&(p->cpus_mask), pri, INT_MAX, curr_cpu);
	}
	/* Search globally for the less loaded CPU. */
	if (cpu == -1) {
		cpu = sched_lowest(&(p->cpus_mask), -1, INT_MAX, curr_cpu);
	}

	if (cpu == -1) {
		LOG("Can't find a CPU for task %d\n", p->pid);
		LOG("allowed : %*pbl\n", cpumask_pr_args(&(p->cpus_mask)));
		LOG("root domain : %*pbl\n", cpumask_pr_args(sched_domain_span(root_domain)));
		BUG();
	}
	/*
	 * Compare the lowest loaded cpu to current cpu.
	 */
	if (cpumask_test_cpu(this_cpu, &(p->cpus_mask)) &&
	    this_tdq->lowpri > pri &&
	    curr_tdq->load &&
	    this_tdq->load <= curr_tdq->load + 1) {
		cpu = this_cpu;
	}

	return cpu;
}
#endif

static void yield_task_ktz(struct rq *rq)
{
	; /* No neeed to renqueue here as we will do it in put_prev_task. */
}

static void switched_from_ktz(struct rq *rq, struct task_struct *p)
{
	;
}

static void switched_to_ktz(struct rq *rq, struct task_struct *p)
{
	;
}

static void prio_changed_ktz(struct rq*rq, struct task_struct *p, int oldprio)
{
	sched_thread_priority(TDQ(rq), p, p->prio);
}

static void update_curr_ktz(struct rq*rq)
{
	;
}

static int balance_ktz(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	if (rq->nr_running)
		return 1;
	else
		return 0; // WIP, see balance_fair
}

static struct task_struct *pick_task_ktz(struct rq *rq)
{
	BUG();
}

/*
 *  Utility functions
 */

/* As defined in FreeBSD */
static uint32_t sched_random(void)
{
	uint32_t *rnd = &get_cpu_var(randomval);
	uint32_t res;

	*rnd = *rnd * 69069 + 5;
	res = *rnd >> 16;
	put_cpu_var(randomval);

	return *rnd >> 16;
}

static inline struct task_struct *ktz_task_of(struct sched_ktz_entity *ktz_se)
{
	return container_of(ktz_se, struct task_struct, ktz_se);
}

static inline bool is_enqueued(struct task_struct *p)
{
	return KTZ_SE(p)->curr_runq;
}

/* CPU search */
#ifdef CONFIG_SMP
#define	CPU_SEARCH_LOWEST	0x1
#define	CPU_SEARCH_HIGHEST	0x2

struct cpu_search {
	struct cpumask *cs_mask;
	int	cs_prefer;
	int	cs_pri;		/* Min priority for low. */
	int	cs_limit;	/* Max load for low, min load for high. */
	int	cs_cpu;
	int	cs_load;
};

static struct sched_domain *get_top_domain(int cpu)
{
	struct sched_domain *curr = rcu_dereference(per_cpu(sd_llc, cpu));
	while (curr->parent) {
		curr = curr->parent;
	}
	return curr;
}

static int cpu_search(cpumask_t *cpumask, struct cpu_search *low, struct cpu_search *high, const int match)
{
	struct cpu_search lgroup;
	struct cpu_search hgroup;
	struct cpumask cpumask_c;
	struct ktz_tdq *tdq;
	int cpu, hload, lload, load, total, rnd;

	cpumask_copy(&cpumask_c, cpumask);

	/* Avoid warnings. */
	hgroup.cs_cpu = -1;
	lgroup.cs_cpu = -1;
	lgroup.cs_load = INT_MAX;
	hgroup.cs_load = INT_MIN;

	total = 0;

	if (match & CPU_SEARCH_LOWEST) {
		lload = INT_MAX;
		lgroup = *low;
	}
	if (match & CPU_SEARCH_HIGHEST) {
		hload = INT_MIN;
		hgroup = *high;
	}

	/* Iterate through the child CPU groups and then remaining CPUs. */
	for_each_cpu(cpu, &cpumask_c) {
		if (match & CPU_SEARCH_LOWEST)
			lgroup.cs_cpu = -1;
		if (match & CPU_SEARCH_HIGHEST)
			hgroup.cs_cpu = -1;
		cpumask_clear_cpu(cpu, &cpumask_c);
		tdq = TDQ(cpu_rq(cpu));
		load = tdq->load * 256;
		rnd = sched_random() % 32;
		if (match & CPU_SEARCH_LOWEST) {
			if (cpu == low->cs_prefer)
				load -= 64;
			/* If that CPU is allowed and get data. */
			if (tdq->lowpri > lgroup.cs_pri &&
			    tdq->load <= lgroup.cs_limit &&
			    cpumask_test_cpu(cpu, lgroup.cs_mask)) {
				lgroup.cs_cpu = cpu;
				lgroup.cs_load = load - rnd;
			}
		}
		if (match & CPU_SEARCH_HIGHEST)
			if (tdq->load >= hgroup.cs_limit &&
			    cpumask_test_cpu(cpu, hgroup.cs_mask)) {
				hgroup.cs_cpu = cpu;
				hgroup.cs_load = load - rnd;
			}
		total += load;

		/* We have info about child item. Compare it. */
		if (match & CPU_SEARCH_LOWEST) {
			if (lgroup.cs_cpu >= 0 &&
			    (load < lload ||
			     (load == lload && lgroup.cs_load < low->cs_load))) {
				lload = load;
				low->cs_cpu = lgroup.cs_cpu;
				low->cs_load = lgroup.cs_load;
			}
		}
		if (match & CPU_SEARCH_HIGHEST) {
			if (hgroup.cs_cpu >= 0 &&
			    (load > hload ||
			     (load == hload && hgroup.cs_load > high->cs_load))) {
				hload = load;
				high->cs_cpu = hgroup.cs_cpu;
				high->cs_load = hgroup.cs_load;
			}
		}
	}
	return (total);
}

static inline int cpu_search_lowest(struct cpu_search *low)
{
	return cpu_search(low->cs_mask, low, NULL, CPU_SEARCH_LOWEST);
}

static inline int cpu_search_highest(struct cpu_search *high)
{
	return cpu_search(high->cs_mask, NULL, high, CPU_SEARCH_HIGHEST);
}

static inline int sched_lowest(struct cpumask *mask, int pri, int maxload, int prefer)
{
	struct cpu_search low;

	low.cs_cpu = -1;
	low.cs_prefer = prefer;
	low.cs_mask = mask;
	low.cs_pri = pri;
	low.cs_limit = maxload;
	cpu_search_lowest(&low);

	return low.cs_cpu;
}

static inline int sched_highest(struct cpumask *mask, int minload)
{
	struct cpu_search high;

	high.cs_cpu = -1;
	high.cs_mask = mask;
	high.cs_limit = minload;
	cpu_search_highest(&high);

	return high.cs_cpu;
}
#endif // CONFIG_SMP

/*
 * Trace functions
 */

static inline void trace_load(struct ktz_tdq *tdq)
{
#ifdef CONFIG_SMP
	struct rq *rq = RQ(tdq);
	trace_sched_load_changed(rq->cpu, rq->nr_running);
#endif
}

static void trace_plb(void)
{
	trace_sched_plb(jiffies);
}

static inline void trace_inter(struct task_struct *p)
{
	int inter, score;

	inter = interact_score(p);
	score = max(0, inter + task_nice(p));
	trace_sched_interactivity(p, inter, score);
}

/*
 * Print functions
 */

static void print_groups(struct sched_domain *sd)
{
	struct sched_group *first = sd->groups;
	struct sched_group *g;
	int i;

	if (!first)
		return;

	i = 0;
	g = first;
	do {
		LOG("g%d : %p", i, g);
		g = g->next;
		i ++;
	} while (g != first);
}

static void print_sched_domain(int cpu)
{
	struct sched_domain *sd;
	LOG("Domains for CPU %d : ", cpu);
	LOG("top : %p", get_top_domain(cpu));
	for_each_domain(cpu, sd) {
		LOG("sd : %p", sd);
		LOG("span : %*pbl", cpumask_pr_args(sched_domain_span(sd)));
		LOG("gr 0 : %p", sd->groups);
	}
	LOG("###################");
}

static inline void print_stats(struct task_struct *p)
{
	struct sched_ktz_entity *kse = KTZ_SE(p);
	unsigned long long st = kse->slptime >> SCHED_TICK_SHIFT;
	unsigned long long rt = kse->runtime >> SCHED_TICK_SHIFT;
	int interact = interact_score(p);
	LOG("Task %d : ", p->pid);
	LOG("\t| slptime\t\t= %llu ms", st);
	LOG("\t| runtime\t\t= %llu ms", rt);
	LOG("\t| interact\t\t= %d", interact);
	LOG("\t| ticks\t\t= %llu", kse->ticks);
	LOG("\t| lticks\t\t= %llu", kse->ltick);
	LOG("\t| fticks\t\t= %llu", kse->ftick);
}


static inline void print_runq(struct runq *q)
{
	int i;
	struct sched_ktz_entity *pos;
	struct task_struct *t;

	for (i = 0; i < KTZ_HEADS_PER_RUNQ; ++i) {
		if (!list_empty(&q->queues[i])) {
			list_for_each_entry(pos, &q->queues[i], runq) {
				t = ktz_task_of(pos);
				LOG("\t_ %d", t->pid);
			}
		}
	}
}

static inline void print_tdq(struct ktz_tdq *tdq)
{
	LOG("tdq %p\n", tdq);
	LOG("idx : %d", tdq->idx);
	LOG("ridx : %d", tdq->ridx);
	LOG("load : %d", tdq->load);
	LOG("Realtime runq :\n");
	print_runq(&tdq->realtime);
	LOG("Timeshare runq :\n");
	print_runq(&tdq->timeshare);
	LOG("Idle runq :\n");
	print_runq(&tdq->idle);
	LOG("##################\n");
}

static inline void print_all_tdqs(void)
{
	int cpu;
	for (cpu = 0; cpu < nr_cpu_ids; ++cpu) {
		LOG("CPU %d TDQ :", cpu);
		print_tdq(TDQ(cpu_rq(cpu)));
	}
	LOG("##################\n");
}

static inline void print_children(struct task_struct *p)
{
	struct task_struct *pos;
	struct list_head *head;
	head = &(p->children);

	LOG("Children of %d\n", p->pid);
	if (list_empty(head)) {
		LOG("\tnone");
	}
	else {
		list_for_each_entry(pos, head, sibling) {
			LOG("\t%d\n", pos->pid);
		}
	}
}

static inline void print_loads(void)
{
	int cpu;
	struct cpumask mask;
	unsigned long flags;
	cpumask_setall(&mask);
	LOG("[%d] CPU loads : ", smp_processor_id());
	for_each_cpu(cpu, &mask) {
		struct rq *rq = cpu_rq(cpu);
		struct ktz_tdq *tdq = TDQ(rq);
		if (smp_processor_id() != cpu)
			raw_spin_lock_irqsave(&rq->__lock, flags);
		LOG("\t| Cpu %d, load = %d, nr_running = %lu\n", cpu, tdq->load, rq->nr_running);
		if (smp_processor_id() != cpu) {
			raw_spin_unlock(&rq->__lock);
			local_irq_restore(flags);
		}
	}
	LOG("##################\n");
}