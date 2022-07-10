/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_HCS_H
#define _LINUX_SCHED_HCS_H

#include <linux/sched.h>

struct task_struct;

void                hcs_init                (void);

void                enqueue_task_hcs        (struct rq *rq, struct task_struct *p, int flags);
void                dequeue_task_hcs        (struct rq *rq, struct task_struct *p, int flags);

void                yield_task_hcs          (struct rq *rq);

void                check_preempt_wakeup_hcs(struct rq *rq, struct task_struct *p, int wake_flags);

struct task_struct *pick_next_task_hcs      (struct rq *rq);
void                put_prev_task_hcs       (struct rq *rq, struct task_struct *prev);
void                set_next_task_hcs       (struct rq *rq, struct task_struct *p, bool first);

void                task_tick_hcs           (struct rq *rq, struct task_struct *curr, int queued);

void                task_fork_hcs           (struct task_struct *p);

void                prio_changed_hcs        (struct rq *rq, struct task_struct *p, int oldprio);

void                switched_from_hcs       (struct rq *rq, struct task_struct *p);
void                switched_to_hcs         (struct rq *rq, struct task_struct *p);

unsigned int        get_rr_interval_hcs     (struct rq *rq, struct task_struct *task);

void                update_curr_hcs         (struct rq *rq);

/*
 * SMP
 */

int                 balance_hcs         (struct rq *rq, struct task_struct *prev, struct rq_flags *rf);

struct task_struct *pick_task_hcs       (struct rq *rq);

int                 select_task_rq_hcs  (struct task_struct *p, int prev_cpu, int wake_flags);

void                migrate_task_rq_hcs (struct task_struct *p, int new_cpu);

void                rq_online_hcs       (struct rq *rq);
void                rq_offline_hcs      (struct rq *rq);

void                task_dead_hcs       (struct task_struct *p);

#endif /* _LINUX_SCHED_HCS_H */
