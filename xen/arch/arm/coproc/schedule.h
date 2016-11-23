/*
 * xen/arch/arm/coproc/schedule.h
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __SCHEDULE_H_
#define __SCHEDULE_H_

#include <xen/timer.h>
#include <xen/types.h>
#include <xen/list.h>
#include <xen/spinlock.h>

struct vcoproc_instance;

struct vcoproc_task_slice {
    struct vcoproc_instance *task;
    s_time_t time;
};

struct vcoproc_schedule_data {
    struct timer s_timer;
    struct vcoproc_instance *curr;
    spinlock_t schedule_lock;
};

struct vcoproc_scheduler {
    char *name;
    char *opt_name;
    unsigned int sched_id;
    void *sched_data;

    int (*init)(struct vcoproc_scheduler *);
    void (*deinit)(struct vcoproc_scheduler *);
    void *(*alloc_vdata)(const struct vcoproc_scheduler *, struct vcoproc_instance *);
    void (*free_vdata)(const struct vcoproc_scheduler *, void *);

    void (*sleep)(const struct vcoproc_scheduler *, struct vcoproc_instance *);
    void (*wake)(const struct vcoproc_scheduler *, struct vcoproc_instance *);
    void (*yield)(const struct vcoproc_scheduler *, struct vcoproc_instance *);

    struct vcoproc_task_slice (*do_schedule)(const struct vcoproc_scheduler *, s_time_t);
    void (*schedule_completed)(const struct vcoproc_scheduler *, struct vcoproc_instance *, int);

    /* TODO Here the scheduler core stores *schedule_data to interact with.
     * The algorithm shouldn't touch it and even know about it. So, it would be correctly
     * to remove it from here, but where to keep it?! */
    void *sched_priv;
};

struct coproc_device;

struct vcoproc_scheduler *vcoproc_scheduler_init(struct coproc_device *);
int vcoproc_scheduler_vcoproc_init(struct vcoproc_scheduler *, struct vcoproc_instance *);
int vcoproc_scheduler_vcoproc_destroy(struct vcoproc_scheduler *, struct vcoproc_instance *);
bool_t vcoproc_scheduler_vcoproc_is_destroyed(struct vcoproc_scheduler *, struct vcoproc_instance *);
void vcoproc_schedule(struct vcoproc_scheduler *);
void vcoproc_sheduler_vcoproc_wake(struct vcoproc_scheduler *, struct vcoproc_instance *);
void vcoproc_sheduler_vcoproc_sleep(struct vcoproc_scheduler *, struct vcoproc_instance *);
void vcoproc_sheduler_vcoproc_yield(struct vcoproc_scheduler *, struct vcoproc_instance *);

#endif /* __SCHEDULE_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
