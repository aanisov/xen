/*
 * xen/arch/arm/coproc/coproc.h
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

#ifndef __COPROC_H_
#define __COPROC_H_

#include <xen/types.h>
#include <xen/list.h>
#include <xen/spinlock.h>
#include <xen/sched.h>
#include <xen/device_tree.h>
#include <public/domctl.h>

#include "schedule.h"

struct mmio {
    void __iomem *base;
    u64 addr;
    u64 size;

    struct coproc_device *coproc;
};

struct coproc_device {
    struct device *dev;

    u32 num_mmios;
    struct mmio *mmios;
    u32 num_irqs;
    unsigned int *irqs;

    /* The coproc_elem list is used to append instances of coproc
     * to the "framework's" global coprocs list */
    struct list_head coproc_elem;

    spinlock_t vcoprocs_lock;
    /* The "coproc's" vcoprocs list is used to keep track of all vcoproc
     * instances that have been created from this coproc device */
    struct list_head vcoprocs;

    const struct vcoproc_ops *ops;

    struct vcoproc_scheduler *sched;
};

struct vcoproc_ops {
    struct vcoproc_instance *(*vcoproc_init)(struct domain *, struct coproc_device *);
    void (*vcoproc_free)(struct domain *, struct vcoproc_instance *);
    bool_t (*vcoproc_is_created)(struct domain *, struct coproc_device *);
    int (*ctx_switch_from)(struct vcoproc_instance *);
    int (*ctx_switch_to)(struct vcoproc_instance *);
};

enum vcoproc_state {
    VCOPROC_UNKNOWN,
    VCOPROC_SLEEPING,
    VCOPROC_WAITING,
    VCOPROC_RUNNING,
    VCOPROC_TERMINATING
};

struct vcoproc_instance {
    struct coproc_device *coproc;
    struct domain *domain;
    spinlock_t lock;
    enum vcoproc_state state;

    /* The vcoproc_elem list is used to append instances of vcoproc
     * to the "coproc's" vcoprocs list */
    struct list_head vcoproc_elem;
    /* The instance_elem list is used to append instances of vcoproc
     * to the "domain's" instances list */
    struct list_head instance_elem;

    void *sched_priv;
};

void coproc_init(void);
int coproc_register(struct coproc_device *);
int vcoproc_domain_init(struct domain *);
void vcoproc_domain_free(struct domain *);
int coproc_do_domctl(struct xen_domctl *, struct domain *, XEN_GUEST_HANDLE_PARAM(xen_domctl_t));
bool_t coproc_is_attached_to_domain(struct domain *, const char *);
int vcoproc_context_switch(struct vcoproc_instance *, struct vcoproc_instance *);
void vcoproc_continue_running(struct vcoproc_instance *);
int coproc_release_vcoprocs(struct domain *);

#define dev_path(dev) dt_node_full_name(dev_to_dt(dev))

#endif /* __COPROC_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
