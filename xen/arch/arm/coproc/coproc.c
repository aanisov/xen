/*
 * xen/arch/arm/coproc/coproc.c
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

#include <xen/init.h>
#include <xen/err.h>
#include <xen/guest_access.h>

#include "coproc.h"

/* dom0_coprocs: comma-separated list of coprocs for domain 0. */
static char __initdata opt_dom0_coprocs[128] = "";
string_param("dom0_coprocs", opt_dom0_coprocs);

/* The "framework's" global coprocs list is used to keep track
 * of all coproc instances that have been registered in the framework */
static LIST_HEAD(coprocs);
static DEFINE_SPINLOCK(coprocs_lock);
static int num_coprocs;

int vcoproc_context_switch(struct vcoproc_instance *curr, struct vcoproc_instance *next)
{
    struct coproc_device *coproc;
    int ret = 0;

    if ( unlikely(curr == next) )
        return 0;

    /* It is no matter who will provide coproc */
    coproc = next ? next->coproc : curr->coproc;

    ret = coproc->ops->ctx_switch_from(curr);
    if ( ret )
        return ret;

    ret = coproc->ops->ctx_switch_to(next);
    if ( ret )
        panic("Failed to switch context to vcoproc \"%s\" (%d)\n",
              dev_path(coproc->dev), ret);

    return ret;
}

void vcoproc_continue_running(struct vcoproc_instance *same)
{
    /* nothing to do */
}

static struct coproc_device *coproc_find_by_path(const char *path)
{
    struct coproc_device *coproc;
    bool_t found = false;

    if ( !path )
        return NULL;

    spin_lock(&coprocs_lock);

    if ( list_empty(&coprocs) )
        goto out;

    list_for_each_entry(coproc, &coprocs, coproc_elem)
    {
        if ( !strcmp(dev_path(coproc->dev), path) )
        {
            found = true;
            break;
        }
    }

out:
    spin_unlock(&coprocs_lock);

    return found ? coproc : NULL;
}

static int coproc_attach_to_domain(struct domain *d, struct coproc_device *coproc)
{
    struct vcoproc *vcoproc_d = &d->arch.vcoproc;
    struct vcoproc_instance *vcoproc;
    int ret;

    if ( !coproc )
        return -EINVAL;

    spin_lock(&coprocs_lock);

    if ( coproc->ops->vcoproc_is_created(d, coproc) )
    {
        ret = -EEXIST;
        goto out;
    }

    vcoproc = coproc->ops->vcoproc_init(d, coproc);
    if ( IS_ERR(vcoproc) )
    {
        ret = PTR_ERR(vcoproc);
        goto out;
    }

    ret = vcoproc_scheduler_vcoproc_init(coproc->sched, vcoproc);
    if ( ret )
    {
        coproc->ops->vcoproc_free(d, vcoproc);
        goto out;
    }

    spin_lock(&vcoproc_d->lock);
    BUG_ON(vcoproc_d->num_instances >= num_coprocs);
    list_add_tail(&vcoproc->instance_elem, &vcoproc_d->instances);
    vcoproc_d->num_instances++;
    spin_unlock(&vcoproc_d->lock);

    printk("Created vcoproc \"%s\" for dom%u\n",
           dev_path(coproc->dev), d->domain_id);

out:
    spin_unlock(&coprocs_lock);

    return ret;
}

static int coproc_find_and_attach_to_domain(struct domain *d, const char *path)
{
    struct coproc_device *coproc;

    coproc = coproc_find_by_path(path);
    if ( !coproc )
        return -ENODEV;

    return coproc_attach_to_domain(d, coproc);
}

static int coproc_detach_from_domain(struct domain *d, struct vcoproc_instance *vcoproc)
{
    struct vcoproc *vcoproc_d = &d->arch.vcoproc;
    struct coproc_device *coproc;
    int ret;

    if ( !vcoproc )
        return 0;

    coproc = vcoproc->coproc;

    spin_lock(&coprocs_lock);

    ret = vcoproc_scheduler_vcoproc_destroy(coproc->sched, vcoproc);
    if ( ret )
    {
        if ( ret == -EBUSY )
            ret = -ERESTART;
        goto out;
    }

    spin_lock(&vcoproc_d->lock);
    BUG_ON(!vcoproc_d->num_instances);
    list_del_init(&vcoproc->instance_elem);
    vcoproc_d->num_instances--;
    spin_unlock(&vcoproc_d->lock);

    coproc->ops->vcoproc_free(d, vcoproc);

    printk("Destroyed vcoproc \"%s\" for dom%u\n",
            dev_path(coproc->dev), d->domain_id);

out:
    spin_unlock(&coprocs_lock);

    return ret;
}

bool_t coproc_is_attached_to_domain(struct domain *d, const char *path)
{
    struct coproc_device *coproc;

    coproc = coproc_find_by_path(path);
    if ( !coproc )
        return false;

    return coproc->ops->vcoproc_is_created(d, coproc);
}

static int vcoproc_dom0_init(struct domain *d)
{
    const char *curr, *next;
    int len, ret = 0;

    if ( !strcmp(opt_dom0_coprocs, "") )
        return 0;

    printk("Got list of coprocs \"%s\" for dom%u\n",
           opt_dom0_coprocs, d->domain_id);

    /*
     * For the moment, we'll create vcoproc for each registered coproc
     * which is described in the list of coprocs for domain 0 in bootargs.
     */
    for ( curr = opt_dom0_coprocs; curr; curr = next )
    {
        struct dt_device_node *node = NULL;
        char *buf;
        bool_t is_alias = false;

        next = strchr(curr, ',');
        if ( next )
        {
            len = next - curr;
            next++;
        }
        else
            len = strlen(curr);

        if ( *curr != '/' )
            is_alias = true;

        buf = xmalloc_array(char, len + 1);
        if ( !buf )
        {
            ret = -ENOMEM;
            break;
        }

        strlcpy(buf, curr, len + 1);
        if ( is_alias )
            node = dt_find_node_by_alias(buf);
        else
            node = dt_find_node_by_path(buf);
        if ( !node )
        {
            printk("Unable to find node by %s \"%s\"\n",
                   is_alias ? "alias" : "path", buf);
            ret = -EINVAL;
        }
        xfree(buf);
        if ( ret )
            break;

        curr = dt_node_full_name(node);

        ret = coproc_find_and_attach_to_domain(d, curr);
        if (ret)
        {
            printk("Failed to attach coproc \"%s\" to dom%u (%d)\n",
                   curr, d->domain_id, ret);
            break;
        }
    }

    return ret;
}

int vcoproc_domain_init(struct domain *d)
{
    struct vcoproc *vcoproc_d = &d->arch.vcoproc;
    int ret = 0;

    vcoproc_d->num_instances = 0;
    INIT_LIST_HEAD(&vcoproc_d->instances);
    spin_lock_init(&vcoproc_d->lock);

    /*
     * We haven't known yet if the guest domain are going to use coprocs.
     * So, just return okay for the moment. It won't be an issue later if
     * guest domain doesn't request any.
     * But we definitely know when domain 0 is being created.
     */
    if ( !num_coprocs )
    {
        if ( d->domain_id == 0 && strcmp(opt_dom0_coprocs, "") )
        {
            printk("There is no registered coproc for creating vcoproc\n");
            return -ENODEV;
        }

        return 0;
    }

    /* We already have the list of coprocs for domain 0 only. */
    if ( d->domain_id == 0 )
        ret = vcoproc_dom0_init(d);

    return ret;
}

void vcoproc_domain_free(struct domain *d)
{
    coproc_release_vcoprocs(d);
}

int coproc_release_vcoprocs(struct domain *d)
{
    struct vcoproc *vcoproc_d = &d->arch.vcoproc;
    struct vcoproc_instance *vcoproc, *temp;
    int ret = 0;

    spin_lock(&vcoproc_d->lock);

    if ( list_empty(&vcoproc_d->instances) )
        goto out;

    list_for_each_entry_safe( vcoproc, temp, &vcoproc_d->instances, instance_elem )
    {
        spin_unlock(&vcoproc_d->lock);

        ret = coproc_detach_from_domain(d, vcoproc);
        if ( ret )
            return ret;

        spin_lock(&vcoproc_d->lock);
    }

out:
    spin_unlock(&vcoproc_d->lock);

    return ret;
}

int coproc_do_domctl(struct xen_domctl *domctl, struct domain *d,
                     XEN_GUEST_HANDLE_PARAM(xen_domctl_t) u_domctl)
{
    char *path;
    int ret;

    switch ( domctl->cmd )
    {
    case XEN_DOMCTL_attach_coproc:
        if ( unlikely(d->is_dying) )
        {
            ret = -EINVAL;
            break;
        }

        path = safe_copy_string_from_guest(domctl->u.attach_coproc.path,
                                           domctl->u.attach_coproc.size,
                                           PAGE_SIZE);
        if ( IS_ERR(path) )
        {
            ret = PTR_ERR(path);
            break;
        }

        printk("Got coproc \"%s\" for dom%u\n", path, d->domain_id);

        ret = coproc_find_and_attach_to_domain(d, path);
        if ( ret )
            printk("Failed to attach coproc \"%s\" to dom%u (%d)\n",
                   path, d->domain_id, ret);
        xfree(path);
        break;

    default:
        ret = -ENOSYS;
        break;
    }

    return ret;
}

int __init coproc_register(struct coproc_device *coproc)
{
    if ( !coproc || !coproc->ops )
        return -EINVAL;

    if ( coproc_find_by_path(dev_path(coproc->dev)) )
        return -EEXIST;

    coproc->sched = vcoproc_scheduler_init(coproc);
    if ( IS_ERR(coproc->sched) )
        return PTR_ERR(coproc->sched);

    spin_lock(&coprocs_lock);
    list_add_tail(&coproc->coproc_elem, &coprocs);
    num_coprocs++;
    spin_unlock(&coprocs_lock);

    printk("Registered new coproc \"%s\"\n", dev_path(coproc->dev));

    return 0;
}

void __init coproc_init(void)
{
    struct dt_device_node *node;
    unsigned int num_coprocs = 0;
    int ret;

    /*
     * For the moment, we'll create coproc for each device that presents
     * in the device tree and has "xen,coproc" property.
     */
    dt_for_each_device_node(dt_host, node)
    {
        if ( !dt_get_property(node, "xen,coproc", NULL) )
            continue;

        ret = device_init(node, DEVICE_COPROC, NULL);
        if ( !ret )
            num_coprocs++;
    }

    if ( !num_coprocs )
        printk("Unable to find compatible coprocs in the device tree\n");
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
