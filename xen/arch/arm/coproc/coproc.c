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
#include <xen/sched.h>
#include <xen/list.h>
#include <xen/err.h>
#include <xen/guest_access.h>
#include <asm/device.h>

#include "coproc.h"

/* dom0_coprocs: comma-separated list of coprocs for domain 0. */
static char __initdata opt_dom0_coprocs[128] = "";
string_param("dom0_coprocs", opt_dom0_coprocs);

static DEFINE_SPINLOCK(coproc_devices_lock);
static LIST_HEAD(coproc_devices);
static int num_coprocs_devices;

#define dev_path(dev) dt_node_full_name(dev_to_dt(dev))

int vcoproc_context_switch(struct vcoproc_info *prev, struct vcoproc_info *next)
{
    struct coproc_device *coproc;
    int ret = 0;

    if ( unlikely(prev == next) )
        return 0;

    coproc = next ? next->coproc : prev->coproc;

    ret = coproc->ops->ctx_switch_from(prev);
    if ( ret )
        return ret;

    ret = coproc->ops->ctx_switch_to(next);
    if ( ret )
        panic("Failed to switch context to coproc \"%s\" (%d)\n",
              dev_path(coproc->dev), ret);

    return ret;
}

void vcoproc_continue_running(struct vcoproc_info *same)
{
    /* nothing to do */
}

int vcoproc_attach(struct domain *d, struct vcoproc_info *info)
{
    struct vcoproc *vcoproc = &d->arch.vcoproc;
    struct vcoproc_instance *instance;

    if ( !info )
        return -EINVAL;

    BUG_ON(vcoproc->num_instances >= num_coprocs_devices);

    instance = &vcoproc->instances[vcoproc->num_instances];
    instance->idx = vcoproc->num_instances;
    instance->info = info;
    vcoproc->num_instances++;

    printk("Attached vcoproc \"%s\" to dom%u\n",
           dev_path(info->coproc->dev), d->domain_id);

    return 0;
}

static struct coproc_device *find_coproc_by_path(const char *path)
{
    struct coproc_device *coproc;
    bool_t found = false;

    if ( !path )
        return NULL;

    if ( !num_coprocs_devices )
        return NULL;

    spin_lock(&coproc_devices_lock);
    list_for_each_entry(coproc, &coproc_devices, list)
    {
        if ( !strcmp(dev_path(coproc->dev), path) )
        {
            found = true;
            break;
        }
    }
    spin_unlock(&coproc_devices_lock);

    return found ? coproc : NULL;
}

static int coproc_attach_to_domain(struct domain *d, struct coproc_device *coproc)
{
    struct vcoproc_info *info;
    int ret;

    if ( !coproc )
        return -EINVAL;

    if ( coproc->ops->vcoproc_is_created(d, coproc) )
        return -EEXIST;

    info = coproc->ops->vcoproc_init(d, coproc);
    if ( IS_ERR(info) )
        return PTR_ERR(info);

    ret = info->ops->domain_init(d, info);
    if ( ret )
        coproc->ops->vcoproc_free(d, info);

    return ret;
}

static int coproc_find_and_attach_to_domain(struct domain *d, const char *path)
{
    struct coproc_device *coproc;
    int ret;

    coproc = find_coproc_by_path(path);
    if ( !coproc )
        return -ENODEV;

    spin_lock(&coproc_devices_lock);
    ret = coproc_attach_to_domain(d, coproc);
    spin_unlock(&coproc_devices_lock);

    return ret;
}

static void coproc_detach_from_domain(struct domain *d, struct vcoproc_info *info)
{
    struct coproc_device *coproc = info->coproc;

    if ( !info )
        return;

    info->ops->domain_free(d, info);
    coproc->ops->vcoproc_free(d, info);
}

bool_t coproc_is_attached_to_domain(struct domain *d, const char *path)
{
    struct vcoproc *vcoproc = &d->arch.vcoproc;
    struct coproc_device *coproc;
    struct vcoproc_info *info;
    bool_t found = false;
    int i;

    coproc = find_coproc_by_path(path);
    if ( !coproc )
        return false;

    for ( i = 0; i < vcoproc->num_instances; ++i )
    {
        info = vcoproc->instances[i].info;
        if ( info->coproc == coproc )
        {
            found = true;
            break;
        }
    }

    return found;
}

static int dom0_vcoproc_init(struct domain *d)
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

int domain_vcoproc_init(struct domain *d)
{
    struct vcoproc *vcoproc = &d->arch.vcoproc;
    int ret = 0;

    vcoproc->num_instances = 0;

    /*
     * We haven't known yet if the domain are going to use coprocs.
     * So, just return okay for the moment.
     */
    if ( !num_coprocs_devices )
        return 0;

    vcoproc->instances = xzalloc_array(struct vcoproc_instance, num_coprocs_devices);
    if ( !vcoproc->instances )
        return -ENOMEM;
    spin_lock_init(&vcoproc->lock);

    /* We already have the list of coprocs for domain 0. */
    if ( d->domain_id == 0 )
        ret = dom0_vcoproc_init(d);

    return ret;
}

void domain_vcoproc_free(struct domain *d)
{
    struct vcoproc *vcoproc = &d->arch.vcoproc;
    struct vcoproc_info *info;
    int i;

    spin_lock(&coproc_devices_lock);
    for ( i = 0; i < vcoproc->num_instances; ++i )
    {
        info = vcoproc->instances[i].info;
        coproc_detach_from_domain(d, info);
    }
    spin_unlock(&coproc_devices_lock);

    vcoproc->num_instances = 0;
    xfree(vcoproc->instances);
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
    if ( !coproc )
        return -EINVAL;

    if ( find_coproc_by_path(dev_path(coproc->dev)) )
        return -EEXIST;

    spin_lock(&coproc_devices_lock);
    list_add_tail(&coproc->list, &coproc_devices);
    spin_unlock(&coproc_devices_lock);

    num_coprocs_devices++;

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
