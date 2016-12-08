/*
 * xen/drivers/passthrough/arm/ipmmu-vmsa.c
 *
 * Driver for the Renesas IPMMU-VMSA found in R-Car Gen3 SoCs.
 *
 * The IPMMU-VMSA is VMSA-compatible I/O Memory Management Unit (IOMMU)
 * which provides address translation and access protection functionalities
 * to processing units and interconnect networks.
 *
 * Please note, current driver is supposed to work only with newest Gen3 SoCs
 * revisions which IPMMU hardware supports stage 2 translation table format and
 * is able to use CPU's P2M table as is.
 *
 * Based on Linux's IPMMU-VMSA driver from Renesas BSP:
 *    drivers/iommu/ipmmu-vmsa.c
 * you can found at:
 *    url: git://git.kernel.org/pub/scm/linux/kernel/git/horms/renesas-bsp.git
 *    branch: v4.14.75-ltsi/rcar-3.9.2
 *    commit: a5266d298124874c2c06b8b13d073f6ecc2ee355
 * and Xen's SMMU driver:
 *    xen/drivers/passthrough/arm/smmu.c
 *
 * Copyright (C) 2016-2019 EPAM Systems Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms and conditions of the GNU General Public
 * License, version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/delay.h>
#include <xen/err.h>
#include <xen/irq.h>
#include <xen/lib.h>
#include <xen/list.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/vmap.h>
#include <asm/atomic.h>
#include <asm/device.h>
#include <asm/io.h>

#define dev_name(dev) dt_node_full_name(dev_to_dt(dev))

/* Device logger functions */
#define dev_print(dev, lvl, fmt, ...)    \
    printk(lvl "ipmmu: %s: " fmt, dev_name(dev), ## __VA_ARGS__)

#define dev_info(dev, fmt, ...)    \
    dev_print(dev, XENLOG_INFO, fmt, ## __VA_ARGS__)
#define dev_warn(dev, fmt, ...)    \
    dev_print(dev, XENLOG_WARNING, fmt, ## __VA_ARGS__)
#define dev_err(dev, fmt, ...)     \
    dev_print(dev, XENLOG_ERR, fmt, ## __VA_ARGS__)
#define dev_err_ratelimited(dev, fmt, ...)    \
    dev_print(dev, XENLOG_ERR, fmt, ## __VA_ARGS__)

/*
 * Gen3 SoCs make use of up to 8 IPMMU contexts (sets of page table) and
 * these can be managed independently. Each context is mapped to one Xen domain.
 */
#define IPMMU_CTX_MAX     8
/* Gen3 SoCs make use of up to 48 micro-TLBs per IPMMU device. */
#define IPMMU_UTLB_MAX    48

/* IPMMU context supports IPA size up to 40 bit. */
#define IPMMU_MAX_P2M_IPA_BITS    40

/*
 * Xen's domain IPMMU information stored in dom_iommu(d)->arch.priv
 *
 * As each context (set of page table) is mapped to one Xen domain,
 * all associated IPMMU domains use the same context mapped to this Xen domain.
 * This makes all master devices being attached to the same Xen domain share
 * the same context (P2M table).
 */
struct ipmmu_vmsa_xen_domain
{
    spinlock_t lock;
    /* One or more Cache IPMMU domains associated with this Xen domain */
    struct list_head cache_domains;
    /* Root IPMMU domain associated with this Xen domain */
    struct ipmmu_vmsa_domain *root_domain;
};

/* Xen master device's IPMMU information stored in dev->archdata.iommu */
struct ipmmu_vmsa_xen_device
{
    /* Cache IPMMU domain this master device is attached to */
    struct ipmmu_vmsa_domain *domain;
    /* Master device's IPMMU configuration information */
    struct ipmmu_vmsa_master_cfg *cfg;
};

#define dev_archdata(dev) ((struct ipmmu_vmsa_xen_device *)dev->archdata.iommu)

/* Root/Cache IPMMU device's information */
struct ipmmu_vmsa_device
{
    struct device *dev;
    void __iomem *base;
    struct ipmmu_vmsa_device *root;
    struct list_head list;
    unsigned int num_utlbs;
    unsigned int num_ctx;
    spinlock_t lock;    /* Protects ctx and domains[] */
    DECLARE_BITMAP(ctx, IPMMU_CTX_MAX);
    struct ipmmu_vmsa_domain *domains[IPMMU_CTX_MAX];
};

/*
 * Root/Cache IPMMU domain's information
 *
 * Root IPMMU device is assigned to Root IPMMU domain while Cache IPMMU device
 * is assigned to Cache IPMMU domain. Master devices are connected to Cache
 * IPMMU devices through specific ports called micro-TLBs.
 * All Cache IPMMU devices, in turn, are connected to Root IPMMU device
 * which manages IPMMU context.
 */
struct ipmmu_vmsa_domain
{
    /*
     * IPMMU device assigned to this IPMMU domain.
     * Either Root device which is located at the main memory bus domain or
     * Cache device which is located at each hierarchy bus domain.
     */
    struct ipmmu_vmsa_device *mmu;

    /* Context used for this IPMMU domain */
    unsigned int context_id;

    /* Xen domain associated with this IPMMU domain */
    struct domain *d;

    /* The fields below are used for Cache IPMMU domain only */

    /*
     * Used to keep track of the master devices which are attached to this
     * IPMMU domain (domain users). Master devices behind the same IPMMU device
     * are grouped together by putting into the same IPMMU domain.
     * Only when the refcount reaches 0 this IPMMU domain can be destroyed.
     */
    int refcount;
    /* Used to link this IPMMU domain for the same Xen domain */
    struct list_head list;
};

/* Master device's IPMMU configuration information */
struct ipmmu_vmsa_master_cfg
{
    /* Cache IPMMU this master device is connected to */
    struct ipmmu_vmsa_device *mmu;
    /*
     * These fields describe master device's connection to Cache IPMMU.
     * Each master device gets micro-TLB assignment via the "iommus" property
     * in DT.
     *
     * Some kind of an equivalent to Linux's device IDs which present
     * in struct iommu_fwspec.
     */
    unsigned int *utlbs;
    unsigned int num_utlbs;
};

/* Used to keep track of registered IPMMU devices */
static LIST_HEAD(ipmmu_devices);
static DEFINE_SPINLOCK(ipmmu_devices_lock);

#define TLB_LOOP_TIMEOUT    100 /* 100us */

/* Registers Definition */
#define IM_CTX_SIZE    0x40

#define IMCTR                0x0000
/*
 * These fields are implemented in IPMMU-MM only. So, can be set for
 * Root IPMMU only.
 */
#define IMCTR_VA64           (1 << 29)
#define IMCTR_TRE            (1 << 17)
#define IMCTR_AFE            (1 << 16)
#define IMCTR_RTSEL_MASK     (3 << 4)
#define IMCTR_RTSEL_SHIFT    4
#define IMCTR_TREN           (1 << 3)
/*
 * These fields are common for all IPMMU devices. So, can be set for
 * Cache IPMMUs as well.
 */
#define IMCTR_INTEN          (1 << 2)
#define IMCTR_FLUSH          (1 << 1)
#define IMCTR_MMUEN          (1 << 0)
#define IMCTR_COMMON_MASK    (7 << 0)

#define IMCAAR               0x0004

#define IMTTBCR                        0x0008
#define IMTTBCR_EAE                    (1 << 31)
#define IMTTBCR_PMB                    (1 << 30)
#define IMTTBCR_SH1_NON_SHAREABLE      (0 << 28)
#define IMTTBCR_SH1_OUTER_SHAREABLE    (2 << 28)
#define IMTTBCR_SH1_INNER_SHAREABLE    (3 << 28)
#define IMTTBCR_SH1_MASK               (3 << 28)
#define IMTTBCR_ORGN1_NC               (0 << 26)
#define IMTTBCR_ORGN1_WB_WA            (1 << 26)
#define IMTTBCR_ORGN1_WT               (2 << 26)
#define IMTTBCR_ORGN1_WB               (3 << 26)
#define IMTTBCR_ORGN1_MASK             (3 << 26)
#define IMTTBCR_IRGN1_NC               (0 << 24)
#define IMTTBCR_IRGN1_WB_WA            (1 << 24)
#define IMTTBCR_IRGN1_WT               (2 << 24)
#define IMTTBCR_IRGN1_WB               (3 << 24)
#define IMTTBCR_IRGN1_MASK             (3 << 24)
#define IMTTBCR_TSZ1_MASK              (1f << 16)
#define IMTTBCR_TSZ1_SHIFT             16
#define IMTTBCR_SH0_NON_SHAREABLE      (0 << 12)
#define IMTTBCR_SH0_OUTER_SHAREABLE    (2 << 12)
#define IMTTBCR_SH0_INNER_SHAREABLE    (3 << 12)
#define IMTTBCR_SH0_MASK               (3 << 12)
#define IMTTBCR_ORGN0_NC               (0 << 10)
#define IMTTBCR_ORGN0_WB_WA            (1 << 10)
#define IMTTBCR_ORGN0_WT               (2 << 10)
#define IMTTBCR_ORGN0_WB               (3 << 10)
#define IMTTBCR_ORGN0_MASK             (3 << 10)
#define IMTTBCR_IRGN0_NC               (0 << 8)
#define IMTTBCR_IRGN0_WB_WA            (1 << 8)
#define IMTTBCR_IRGN0_WT               (2 << 8)
#define IMTTBCR_IRGN0_WB               (3 << 8)
#define IMTTBCR_IRGN0_MASK             (3 << 8)
#define IMTTBCR_SL0_LVL_2              (0 << 6)
#define IMTTBCR_SL0_LVL_1              (1 << 6)
#define IMTTBCR_TSZ0_MASK              (0x1f << 0)
#define IMTTBCR_TSZ0_SHIFT             0

#define IMTTLBR0              0x0010
#define IMTTLBR0_TTBR_MASK    (0xfffff << 12)
#define IMTTUBR0              0x0014
#define IMTTUBR0_TTBR_MASK    (0xff << 0)
#define IMTTLBR1              0x0018
#define IMTTLBR1_TTBR_MASK    (0xfffff << 12)
#define IMTTUBR1              0x001c
#define IMTTUBR1_TTBR_MASK    (0xff << 0)

#define IMSTR                          0x0020
#define IMSTR_ERRLVL_MASK              (3 << 12)
#define IMSTR_ERRLVL_SHIFT             12
#define IMSTR_ERRCODE_TLB_FORMAT       (1 << 8)
#define IMSTR_ERRCODE_ACCESS_PERM      (4 << 8)
#define IMSTR_ERRCODE_SECURE_ACCESS    (5 << 8)
#define IMSTR_ERRCODE_MASK             (7 << 8)
#define IMSTR_MHIT                     (1 << 4)
#define IMSTR_ABORT                    (1 << 2)
#define IMSTR_PF                       (1 << 1)
#define IMSTR_TF                       (1 << 0)

#define IMELAR    0x0030
#define IMEUAR    0x0034

#define IMUCTR(n)              ((n) < 32 ? IMUCTR0(n) : IMUCTR32(n))
#define IMUCTR0(n)             (0x0300 + ((n) * 16))
#define IMUCTR32(n)            (0x0600 + (((n) - 32) * 16))
#define IMUCTR_FIXADDEN        (1 << 31)
#define IMUCTR_FIXADD_MASK     (0xff << 16)
#define IMUCTR_FIXADD_SHIFT    16
#define IMUCTR_TTSEL_MMU(n)    ((n) << 4)
#define IMUCTR_TTSEL_PMB       (8 << 4)
#define IMUCTR_TTSEL_MASK      (15 << 4)
#define IMUCTR_FLUSH           (1 << 1)
#define IMUCTR_MMUEN           (1 << 0)

#define IMUASID(n)             ((n) < 32 ? IMUASID0(n) : IMUASID32(n))
#define IMUASID0(n)            (0x0308 + ((n) * 16))
#define IMUASID32(n)           (0x0608 + (((n) - 32) * 16))
#define IMUASID_ASID8_MASK     (0xff << 8)
#define IMUASID_ASID8_SHIFT    8
#define IMUASID_ASID0_MASK     (0xff << 0)
#define IMUASID_ASID0_SHIFT    0

#define IMSAUXCTLR          0x0504
#define IMSAUXCTLR_S2PTE    (1 << 3)

/* Root device handling */
static bool ipmmu_is_root(struct ipmmu_vmsa_device *mmu)
{
    return mmu->root == mmu;
}

static struct ipmmu_vmsa_device *ipmmu_find_root(void)
{
    struct ipmmu_vmsa_device *mmu = NULL;
    bool found = false;

    spin_lock(&ipmmu_devices_lock);

    list_for_each_entry( mmu, &ipmmu_devices, list )
    {
        if ( ipmmu_is_root(mmu) )
        {
            found = true;
            break;
        }
    }

    spin_unlock(&ipmmu_devices_lock);

    return found ? mmu : NULL;
}

/* Read/Write Access */
static u32 ipmmu_read(struct ipmmu_vmsa_device *mmu, unsigned int offset)
{
    return readl(mmu->base + offset);
}

static void ipmmu_write(struct ipmmu_vmsa_device *mmu, unsigned int offset,
                        u32 data)
{
    writel(data, mmu->base + offset);
}

static u32 ipmmu_ctx_read_root(struct ipmmu_vmsa_domain *domain,
                               unsigned int reg)
{
    return ipmmu_read(domain->mmu->root,
                      domain->context_id * IM_CTX_SIZE + reg);
}

static void ipmmu_ctx_write_root(struct ipmmu_vmsa_domain *domain,
                                 unsigned int reg, u32 data)
{
    ipmmu_write(domain->mmu->root,
                domain->context_id * IM_CTX_SIZE + reg, data);
}

static void ipmmu_ctx_write_cache(struct ipmmu_vmsa_domain *domain,
                                  unsigned int reg, u32 data)
{
    ASSERT(reg == IMCTR);

    /* Mask fields which are implemented in IPMMU-MM only. */
    if ( !ipmmu_is_root(domain->mmu) )
        ipmmu_write(domain->mmu, domain->context_id * IM_CTX_SIZE + reg,
                    data & IMCTR_COMMON_MASK);
}

/*
 * Write the context to both Root IPMMU and all Cache IPMMUs assigned
 * to this Xen domain.
 */
static void ipmmu_ctx_write_all(struct ipmmu_vmsa_domain *domain,
                                unsigned int reg, u32 data)
{
    struct ipmmu_vmsa_xen_domain *xen_domain = dom_iommu(domain->d)->arch.priv;
    struct ipmmu_vmsa_domain *cache_domain;

    list_for_each_entry( cache_domain, &xen_domain->cache_domains, list )
        ipmmu_ctx_write_cache(cache_domain, reg, data);

    ipmmu_ctx_write_root(domain, reg, data);
}

/* TLB and micro-TLB Management */

/* Wait for any pending TLB invalidations to complete. */
static void ipmmu_tlb_sync(struct ipmmu_vmsa_domain *domain)
{
    unsigned int count = 0;

    while ( ipmmu_ctx_read_root(domain, IMCTR) & IMCTR_FLUSH )
    {
        cpu_relax();
        if ( ++count == TLB_LOOP_TIMEOUT )
        {
            dev_err_ratelimited(domain->mmu->dev, "TLB sync timed out -- MMU may be deadlocked\n");
            return;
        }
        udelay(1);
    }
}

static void ipmmu_tlb_invalidate(struct ipmmu_vmsa_domain *domain)
{
    u32 reg;

    reg = ipmmu_ctx_read_root(domain, IMCTR);
    reg |= IMCTR_FLUSH;
    ipmmu_ctx_write_all(domain, IMCTR, reg);

    ipmmu_tlb_sync(domain);
}

/* Enable MMU translation for the micro-TLB. */
static void ipmmu_utlb_enable(struct ipmmu_vmsa_domain *domain,
                              unsigned int utlb)
{
    struct ipmmu_vmsa_device *mmu = domain->mmu;

    /*
     * TODO: Reference-count the micro-TLB as several bus masters can be
     * connected to the same micro-TLB.
     */
    ipmmu_write(mmu, IMUASID(utlb), 0);
    ipmmu_write(mmu, IMUCTR(utlb), ipmmu_read(mmu, IMUCTR(utlb)) |
                IMUCTR_TTSEL_MMU(domain->context_id) | IMUCTR_MMUEN);
}

/* Disable MMU translation for the micro-TLB. */
static void ipmmu_utlb_disable(struct ipmmu_vmsa_domain *domain,
                               unsigned int utlb)
{
    struct ipmmu_vmsa_device *mmu = domain->mmu;

    ipmmu_write(mmu, IMUCTR(utlb), 0);
}

/* Domain/Context Management */
static int ipmmu_domain_allocate_context(struct ipmmu_vmsa_device *mmu,
                                         struct ipmmu_vmsa_domain *domain)
{
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&mmu->lock, flags);

    ret = find_first_zero_bit(mmu->ctx, mmu->num_ctx);
    if ( ret != mmu->num_ctx )
    {
        mmu->domains[ret] = domain;
        set_bit(ret, mmu->ctx);
    }
    else
        ret = -EBUSY;

    spin_unlock_irqrestore(&mmu->lock, flags);

    return ret;
}

static void ipmmu_domain_free_context(struct ipmmu_vmsa_device *mmu,
                                      unsigned int context_id)
{
    unsigned long flags;

    spin_lock_irqsave(&mmu->lock, flags);

    clear_bit(context_id, mmu->ctx);
    mmu->domains[context_id] = NULL;

    spin_unlock_irqrestore(&mmu->lock, flags);
}

static int ipmmu_domain_init_context(struct ipmmu_vmsa_domain *domain)
{
    u64 ttbr;
    u32 tsz0;
    int ret;

    /* Find an unused context. */
    ret = ipmmu_domain_allocate_context(domain->mmu->root, domain);
    if ( ret < 0 )
        return ret;

    domain->context_id = ret;

    /*
     * TTBR0
     * Use P2M table for this Xen domain.
     */
    ASSERT(domain->d != NULL);
    ttbr = page_to_maddr(domain->d->arch.p2m.root);

    dev_info(domain->mmu->root->dev, "d%d: Set IPMMU context %u (pgd 0x%"PRIx64")\n",
             domain->d->domain_id, domain->context_id, ttbr);

    ipmmu_ctx_write_root(domain, IMTTLBR0, ttbr & IMTTLBR0_TTBR_MASK);
    ipmmu_ctx_write_root(domain, IMTTUBR0, (ttbr >> 32) & IMTTUBR0_TTBR_MASK);

    /*
     * TTBCR
     * We use long descriptors with inner-shareable WBWA tables and allocate
     * the whole "p2m_ipa_bits" IPA space to TTBR0. Use 4KB page granule.
     * Start page table walks at first level. Bypass stage 1 translation
     * when only stage 2 translation is performed.
     */
    tsz0 = (64 - p2m_ipa_bits) << IMTTBCR_TSZ0_SHIFT;
    ipmmu_ctx_write_root(domain, IMTTBCR, IMTTBCR_EAE | IMTTBCR_PMB |
                         IMTTBCR_SH0_INNER_SHAREABLE | IMTTBCR_ORGN0_WB_WA |
                         IMTTBCR_IRGN0_WB_WA | IMTTBCR_SL0_LVL_1 | tsz0);

    /*
     * IMSTR
     * Clear all interrupt flags.
     */
    ipmmu_ctx_write_root(domain, IMSTR, ipmmu_ctx_read_root(domain, IMSTR));

    /*
     * IMCTR
     * Enable the MMU and interrupt generation. The long-descriptor
     * translation table format doesn't use TEX remapping. Don't enable AF
     * software management as we have no use for it. Use VMSAv8-64 mode.
     * Enable the context for Root IPMMU only. Flush the TLB as required
     * when modifying the context registers.
     */
    ipmmu_ctx_write_root(domain, IMCTR,
                         IMCTR_VA64 | IMCTR_INTEN | IMCTR_FLUSH | IMCTR_MMUEN);

    return 0;
}

static void ipmmu_domain_destroy_context(struct ipmmu_vmsa_domain *domain)
{
    if ( !domain->mmu )
        return;

    /*
     * Disable the context for Root IPMMU only. Flush the TLB as required
     * when modifying the context registers.
     */
    ipmmu_ctx_write_root(domain, IMCTR, IMCTR_FLUSH);
    ipmmu_tlb_sync(domain);

    ipmmu_domain_free_context(domain->mmu->root, domain->context_id);
}

/* Fault Handling */
static void ipmmu_domain_irq(struct ipmmu_vmsa_domain *domain)
{
    const u32 err_mask = IMSTR_MHIT | IMSTR_ABORT | IMSTR_PF | IMSTR_TF;
    struct ipmmu_vmsa_device *mmu = domain->mmu;
    u32 status;
    u64 iova;

    status = ipmmu_ctx_read_root(domain, IMSTR);
    if ( !(status & err_mask) )
        return;

    iova = ipmmu_ctx_read_root(domain, IMELAR) |
                               ((u64)ipmmu_ctx_read_root(domain, IMEUAR) << 32);

    /*
     * Clear the error status flags. Unlike traditional interrupt flag
     * registers that must be cleared by writing 1, this status register
     * seems to require 0. The error address register must be read before,
     * otherwise its value will be 0.
     */
    ipmmu_ctx_write_root(domain, IMSTR, 0);

    /* Log fatal errors. */
    if ( status & IMSTR_MHIT )
        dev_err_ratelimited(mmu->dev, "d%d: Multiple TLB hits @0x%"PRIx64"\n",
                            domain->d->domain_id, iova);
    if ( status & IMSTR_ABORT )
        dev_err_ratelimited(mmu->dev, "d%d: Page Table Walk Abort @0x%"PRIx64"\n",
                            domain->d->domain_id, iova);

    /* Return if it is neither Permission Fault nor Translation Fault. */
    if ( !(status & (IMSTR_PF | IMSTR_TF)) )
        return;

    /* Flush the TLB as required when IPMMU translation error occurred. */
    ipmmu_tlb_invalidate(domain);

    dev_err_ratelimited(mmu->dev, "d%d: Unhandled fault: status 0x%08x iova 0x%"PRIx64"\n",
                        domain->d->domain_id, status, iova);
}

static void ipmmu_irq(int irq, void *dev, struct cpu_user_regs *regs)
{
    struct ipmmu_vmsa_device *mmu = dev;
    unsigned int i;
    unsigned long flags;

    spin_lock_irqsave(&mmu->lock, flags);

    /*
     * When interrupt arrives, we don't know the context it is related to.
     * So, check interrupts for all active contexts to locate a context
     * with status bits set.
    */
    for ( i = 0; i < mmu->num_ctx; i++ )
    {
        if ( !mmu->domains[i] )
            continue;
        ipmmu_domain_irq(mmu->domains[i]);
    }

    spin_unlock_irqrestore(&mmu->lock, flags);
}

/* Master devices management */
static int ipmmu_attach_device(struct ipmmu_vmsa_domain *domain,
                               struct device *dev)
{
    struct ipmmu_vmsa_master_cfg *cfg = dev_archdata(dev)->cfg;
    struct ipmmu_vmsa_device *mmu = cfg->mmu;
    unsigned int i;

    if ( !mmu )
    {
        dev_err(dev, "Cannot attach to IPMMU\n");
        return -ENXIO;
    }

    if ( !domain->mmu )
    {
        /* The domain hasn't been used yet, initialize it. */
        domain->mmu = mmu;

        /*
         * We have already enabled context for Root IPMMU assigned to this
         * Xen domain in ipmmu_domain_init_context().
         * Enable the context for Cache IPMMU only. Flush the TLB as required
         * when modifying the context registers.
         */
        ipmmu_ctx_write_cache(domain, IMCTR, IMCTR_INTEN | IMCTR_FLUSH |
                              IMCTR_MMUEN);

        dev_info(dev, "Using IPMMU context %u\n", domain->context_id);
    }
    else if ( domain->mmu != mmu )
    {
        /*
         * Something is wrong, we can't attach two master devices using
         * different IOMMUs to the same IPMMU domain.
         */
        dev_err(dev, "Can't attach IPMMU %s to domain on IPMMU %s\n",
                dev_name(mmu->dev), dev_name(domain->mmu->dev));
        return -EINVAL;
    }
    else
        dev_info(dev, "Reusing IPMMU context %u\n", domain->context_id);

    for ( i = 0; i < cfg->num_utlbs; ++i )
        ipmmu_utlb_enable(domain, cfg->utlbs[i]);

    return 0;
}

static void ipmmu_detach_device(struct ipmmu_vmsa_domain *domain,
                                struct device *dev)
{
    struct ipmmu_vmsa_master_cfg *cfg = dev_archdata(dev)->cfg;
    unsigned int i;

    for ( i = 0; i < cfg->num_utlbs; ++i )
        ipmmu_utlb_disable(domain, cfg->utlbs[i]);
}

static int ipmmu_get_utlbs(struct ipmmu_vmsa_device *mmu, struct device *dev,
                           unsigned int *utlbs, unsigned int num_utlbs)
{
    unsigned int i;

    for ( i = 0; i < num_utlbs; ++i )
    {
        struct dt_phandle_args args;
        int ret;

        ret = dt_parse_phandle_with_args(dev->of_node, "iommus",
                                         "#iommu-cells", i, &args);
        if ( ret < 0 )
            return ret;

        if ( args.np != mmu->dev->of_node || args.args_count != 1 )
            return -EINVAL;

        utlbs[i] = args.args[0];
    }

    return 0;
}

static int ipmmu_init_master(struct device *dev)
{
    struct ipmmu_vmsa_master_cfg *cfg;
    struct ipmmu_vmsa_device *mmu;
    unsigned int *utlbs, i;
    int num_utlbs, ret = -ENODEV;

    /* Get the number of micro-TLBs this master device is connected through. */
    num_utlbs = dt_count_phandle_with_args(dev->of_node, "iommus",
                                           "#iommu-cells");
    if ( num_utlbs <= 0 )
        return -ENODEV;

    if ( num_utlbs > IPMMU_UTLB_MAX )
        return -EINVAL;

    utlbs = xzalloc_array(unsigned int, num_utlbs);
    if ( !utlbs )
        return -ENOMEM;

    spin_lock(&ipmmu_devices_lock);

    /*
     * Loop through all Cache IPMMUs to find an IPMMU device this master
     * device is connected to and get the micro-TLB assignment.
     * Make sure this master device doesn't refer to multiple different
     * IOMMU devices. It can have multiple master interfaces (micro-TLBs),
     * but to one IPMMU device only.
     */
    list_for_each_entry( mmu, &ipmmu_devices, list )
    {
        if ( ipmmu_is_root(mmu) )
            continue;

        ret = ipmmu_get_utlbs(mmu, dev, utlbs, num_utlbs);
        if ( !ret )
            break;
    }

    spin_unlock(&ipmmu_devices_lock);

    if ( ret < 0 )
        goto error;

    for ( i = 0; i < num_utlbs; ++i )
    {
        if ( utlbs[i] >= mmu->num_utlbs )
        {
            ret = -EINVAL;
            goto error;
        }
    }

    cfg = xzalloc(struct ipmmu_vmsa_master_cfg);
    if ( !cfg )
    {
        ret = -ENOMEM;
        goto error;
    }

    /* Establish the link between IPMMU device and master device */
    cfg->mmu = mmu;
    cfg->utlbs = utlbs;
    cfg->num_utlbs = num_utlbs;
    dev_archdata(dev)->cfg = cfg;

    dev_info(dev, "Initialized master device (IPMMU %s micro-TLBs %u)\n",
             dev_name(mmu->dev), num_utlbs);

    return 0;

error:
    xfree(utlbs);
    return ret;
}

static void ipmmu_protect_masters(struct ipmmu_vmsa_device *mmu)
{
    struct dt_device_node *node;

    dt_for_each_device_node( dt_host, node )
    {
        if ( mmu->dev->of_node != dt_parse_phandle(node, "iommus", 0) )
            continue;

        /* Let Xen know that the master device is protected by an IOMMU. */
        dt_device_set_protected(node);

        dev_info(mmu->dev, "Found master device %s\n", dt_node_full_name(node));
    }
}

static void ipmmu_device_reset(struct ipmmu_vmsa_device *mmu)
{
    unsigned int i;

    /* Disable all contexts. */
    for ( i = 0; i < mmu->num_ctx; ++i )
        ipmmu_write(mmu, i * IM_CTX_SIZE + IMCTR, 0);
}

/*
 * This function relies on the fact that Root IPMMU device is being probed
 * the first. If not the case, it denies further Cache IPMMU device probes
 * (returns the -ENODEV) until the Root IPMMU device has been registered
 * for sure.
 */
static int ipmmu_probe(struct dt_device_node *node)
{
    struct ipmmu_vmsa_device *mmu;
    u64 addr, size;
    int irq, ret;

    mmu = xzalloc(struct ipmmu_vmsa_device);
    if ( !mmu )
    {
        dev_err(&node->dev, "Cannot allocate device data\n");
        return -ENOMEM;
    }

    mmu->dev = &node->dev;
    mmu->num_utlbs = IPMMU_UTLB_MAX;
    mmu->num_ctx = IPMMU_CTX_MAX;
    spin_lock_init(&mmu->lock);
    bitmap_zero(mmu->ctx, IPMMU_CTX_MAX);

    /* Map I/O memory and request IRQ. */
    ret = dt_device_get_address(node, 0, &addr, &size);
    if ( ret )
    {
        dev_err(&node->dev, "Failed to get MMIO\n");
        goto out;
    }

    mmu->base = ioremap_nocache(addr, size);
    if ( !mmu->base )
    {
        dev_err(&node->dev, "Failed to ioremap MMIO (addr 0x%"PRIx64" size 0x%"PRIx64")\n",
                addr, size);
        ret = -ENOMEM;
        goto out;
    }

    /*
     * Determine if this IPMMU node is a Root device by checking for
     * the lack of renesas,ipmmu-main property.
     */
    if ( !dt_find_property(node, "renesas,ipmmu-main", NULL) )
        mmu->root = mmu;
    else
        mmu->root = ipmmu_find_root();

    /* Wait until the Root device has been registered for sure. */
    if ( !mmu->root )
    {
        dev_err(&node->dev, "Root IPMMU hasn't been registered yet\n");
        return -ENODEV;
    }

    /* Root devices have mandatory IRQs. */
    if ( ipmmu_is_root(mmu) )
    {
        irq = platform_get_irq(node, 0);
        if ( irq < 0 )
        {
            dev_err(&node->dev, "No IRQ found\n");
            ret = irq;
            goto out;
        }

        ret = request_irq(irq, 0, ipmmu_irq, dev_name(&node->dev), mmu);
        if ( ret < 0 )
        {
            dev_err(&node->dev, "Failed to request IRQ %d\n", irq);
            goto out;
        }

        ipmmu_device_reset(mmu);

        /*
         * Use stage 2 translation table format when stage 2 translation
         * enabled.
         */
        ipmmu_write(mmu, IMSAUXCTLR,
                    ipmmu_read(mmu, IMSAUXCTLR) | IMSAUXCTLR_S2PTE);

        dev_info(&node->dev, "IPMMU context 0 is reserved\n");
        set_bit(0, mmu->ctx);
    }

    spin_lock(&ipmmu_devices_lock);
    list_add(&mmu->list, &ipmmu_devices);
    spin_unlock(&ipmmu_devices_lock);

    dev_info(&node->dev, "Registered %s IPMMU\n",
             ipmmu_is_root(mmu) ? "Root" : "Cache");

    /*
     * Mark all master devices that connected to this Cache IPMMU
     * as protected.
     */
    if ( !ipmmu_is_root(mmu) )
        ipmmu_protect_masters(mmu);

    return 0;

out:
    if ( mmu->base )
        iounmap(mmu->base);
    xfree(mmu);

    return ret;
}

/* Xen IOMMU ops */
static int __must_check ipmmu_iotlb_flush_all(struct domain *d)
{
    struct ipmmu_vmsa_xen_domain *xen_domain = dom_iommu(d)->arch.priv;

    if ( !xen_domain || !xen_domain->root_domain )
        return 0;

    spin_lock(&xen_domain->lock);
    ipmmu_tlb_invalidate(xen_domain->root_domain);
    spin_unlock(&xen_domain->lock);

    return 0;
}

static int __must_check ipmmu_iotlb_flush(struct domain *d, dfn_t dfn,
                                          unsigned int page_count,
                                          unsigned int flush_flags)
{
    ASSERT(flush_flags);

    /* The hardware doesn't support selective TLB flush. */
    return ipmmu_iotlb_flush_all(d);
}

static struct ipmmu_vmsa_domain *ipmmu_get_cache_domain(struct domain *d,
                                                        struct device *dev)
{
    struct ipmmu_vmsa_xen_domain *xen_domain = dom_iommu(d)->arch.priv;
    struct ipmmu_vmsa_master_cfg *cfg = dev_archdata(dev)->cfg;
    struct ipmmu_vmsa_device *mmu = cfg->mmu;
    struct ipmmu_vmsa_domain *domain;

    if ( !mmu )
        return NULL;

    /*
     * Loop through all Cache IPMMU domains associated with this Xen domain
     * to locate an IPMMU domain this IPMMU device is assigned to.
     */
    list_for_each_entry( domain, &xen_domain->cache_domains, list )
    {
        if ( domain->mmu == mmu )
            return domain;
    }

    return NULL;
}

static struct ipmmu_vmsa_domain *ipmmu_alloc_cache_domain(struct domain *d)
{
    struct ipmmu_vmsa_xen_domain *xen_domain = dom_iommu(d)->arch.priv;
    struct ipmmu_vmsa_domain *domain;

    domain = xzalloc(struct ipmmu_vmsa_domain);
    if ( !domain )
        return ERR_PTR(-ENOMEM);

    /*
     * We don't assign the Cache IPMMU device here, it will be assigned when
     * attaching master device to this domain in ipmmu_attach_device().
     * domain->mmu = NULL;
     */

    domain->d = d;
    /* Use the same context mapped to this Xen domain. */
    domain->context_id = xen_domain->root_domain->context_id;

    return domain;
}

static void ipmmu_free_cache_domain(struct ipmmu_vmsa_domain *domain)
{
    list_del(&domain->list);
    /*
     * Disable the context for Cache IPMMU only. Flush the TLB as required
     * when modifying the context registers.
     */
    ipmmu_ctx_write_cache(domain, IMCTR, IMCTR_FLUSH);
    xfree(domain);
}

static struct ipmmu_vmsa_domain *ipmmu_alloc_root_domain(struct domain *d)
{
    struct ipmmu_vmsa_domain *domain;
    struct ipmmu_vmsa_device *root;
    int ret;

    root = ipmmu_find_root();
    if ( !root )
    {
        printk(XENLOG_ERR "ipmmu: Unable to locate Root IPMMU\n");
        return ERR_PTR(-EAGAIN);
    }

    domain = xzalloc(struct ipmmu_vmsa_domain);
    if ( !domain )
        return ERR_PTR(-ENOMEM);

    domain->mmu = root;
    domain->d = d;

    /* Initialize the context to be mapped to this Xen domain. */
    ret = ipmmu_domain_init_context(domain);
    if ( ret < 0 )
    {
        dev_err(root->dev, "d%d: Unable to initialize IPMMU context\n",
                d->domain_id);
        xfree(domain);
        return ERR_PTR(ret);
    }

    return domain;
}

static void ipmmu_free_root_domain(struct ipmmu_vmsa_domain *domain)
{
    ipmmu_domain_destroy_context(domain);
    xfree(domain);
}

static int ipmmu_assign_device(struct domain *d, u8 devfn, struct device *dev,
                               u32 flag)
{
    struct ipmmu_vmsa_xen_domain *xen_domain = dom_iommu(d)->arch.priv;
    struct ipmmu_vmsa_domain *domain;
    int ret;

    if ( !xen_domain )
        return -EINVAL;

    spin_lock(&xen_domain->lock);

    /*
     * The IPMMU context for the Xen domain is not allocated beforehand
     * (at the Xen domain creation time), but on demand only, when the first
     * master device being attached to it.
     * Create Root IPMMU domain which context will be mapped to this Xen domain
     * if not exits yet.
     */
    if ( !xen_domain->root_domain )
    {
        domain = ipmmu_alloc_root_domain(d);
        if ( IS_ERR(domain) )
        {
            ret = PTR_ERR(domain);
            goto out;
        }

        xen_domain->root_domain = domain;
    }

    if ( !dev->archdata.iommu )
    {
        dev->archdata.iommu = xzalloc(struct ipmmu_vmsa_xen_device);
        if ( !dev->archdata.iommu )
        {
            ret = -ENOMEM;
            goto out;
        }
    }

    if ( !dev_archdata(dev)->cfg )
    {
        ret = ipmmu_init_master(dev);
        if ( ret )
        {
            dev_err(dev, "Failed to initialize master device\n");
            goto out;
        }
    }

    if ( dev_archdata(dev)->domain )
    {
        dev_err(dev, "Already attached to IPMMU domain\n");
        ret = -EEXIST;
        goto out;
    }

    /*
     * Master devices behind the same Cache IPMMU can be attached to the same
     * Cache IPMMU domain.
     * Before creating new IPMMU domain check to see if the required one
     * already exists for this Xen domain.
     */
    domain = ipmmu_get_cache_domain(d, dev);
    if ( !domain )
    {
        /* Create new IPMMU domain this master device will be attached to. */
        domain = ipmmu_alloc_cache_domain(d);
        if ( IS_ERR(domain) )
        {
            ret = PTR_ERR(domain);
            goto out;
        }

        /* Chain new IPMMU domain to the Xen domain. */
        list_add(&domain->list, &xen_domain->cache_domains);
    }

    ret = ipmmu_attach_device(domain, dev);
    if ( ret )
    {
        /*
         * Destroy Cache IPMMU domain only if there are no master devices
         * attached to it.
         */
        if ( !domain->refcount )
            ipmmu_free_cache_domain(domain);
    }
    else
    {
        domain->refcount++;
        dev_archdata(dev)->domain = domain;
    }

out:
    spin_unlock(&xen_domain->lock);

    return ret;
}

static int ipmmu_deassign_device(struct domain *d, struct device *dev)
{
    struct ipmmu_vmsa_xen_domain *xen_domain = dom_iommu(d)->arch.priv;
    struct ipmmu_vmsa_domain *domain = dev_archdata(dev)->domain;

    if ( !domain || domain->d != d )
    {
        dev_err(dev, "Not attached to domain %d\n", d->domain_id);
        return -ESRCH;
    }

    spin_lock(&xen_domain->lock);

    ipmmu_detach_device(domain, dev);
    dev_archdata(dev)->domain = NULL;
    domain->refcount--;

    /*
     * Destroy Cache IPMMU domain only if there are no master devices
     * attached to it.
     */
    if ( !domain->refcount )
        ipmmu_free_cache_domain(domain);

    spin_unlock(&xen_domain->lock);

    return 0;
}

static int ipmmu_reassign_device(struct domain *s, struct domain *t,
                                 u8 devfn,  struct device *dev)
{
    int ret = 0;

    /* Don't allow remapping on other domain than hwdom */
    if ( t && t != hardware_domain )
        return -EPERM;

    if ( t == s )
        return 0;

    ret = ipmmu_deassign_device(s, dev);
    if ( ret )
        return ret;

    if ( t )
    {
        /* No flags are defined for ARM. */
        ret = ipmmu_assign_device(t, devfn, dev, 0);
        if ( ret )
            return ret;
    }

    return 0;
}

static int ipmmu_iommu_domain_init(struct domain *d)
{
    struct ipmmu_vmsa_xen_domain *xen_domain;

    xen_domain = xzalloc(struct ipmmu_vmsa_xen_domain);
    if ( !xen_domain )
        return -ENOMEM;

    spin_lock_init(&xen_domain->lock);
    INIT_LIST_HEAD(&xen_domain->cache_domains);
    /*
     * We don't create Root IPMMU domain here, it will be created on demand
     * only, when attaching the first master device to this Xen domain in
     * ipmmu_assign_device().
     * xen_domain->root_domain = NULL;
    */

    dom_iommu(d)->arch.priv = xen_domain;

    return 0;
}

static void __hwdom_init ipmmu_iommu_hwdom_init(struct domain *d)
{
    /* Set to false options not supported on ARM. */
    if ( iommu_hwdom_inclusive )
        printk(XENLOG_WARNING "ipmmu: map-inclusive dom0-iommu option is not supported on ARM\n");
    iommu_hwdom_inclusive = false;
    if ( iommu_hwdom_reserved == 1 )
        printk(XENLOG_WARNING "ipmmu: map-reserved dom0-iommu option is not supported on ARM\n");
    iommu_hwdom_reserved = 0;

    arch_iommu_hwdom_init(d);
}

static void ipmmu_iommu_domain_teardown(struct domain *d)
{
    struct ipmmu_vmsa_xen_domain *xen_domain = dom_iommu(d)->arch.priv;

    if ( !xen_domain )
        return;

    spin_lock(&xen_domain->lock);
    /*
     * Destroy Root IPMMU domain which context is mapped to this Xen domain
     * if exits.
     */
    if ( xen_domain->root_domain )
        ipmmu_free_root_domain(xen_domain->root_domain);

    spin_unlock(&xen_domain->lock);

    /*
     * We assume that all master devices have already been detached from
     * this Xen domain and there must be no associated Cache IPMMU domains
     * in use.
     */
    ASSERT(list_empty(&xen_domain->cache_domains));
    xfree(xen_domain);
    dom_iommu(d)->arch.priv = NULL;
}

static int __must_check ipmmu_map_page(struct domain *d, dfn_t dfn, mfn_t mfn,
                                       unsigned int flags,
                                       unsigned int *flush_flags)
{
    p2m_type_t t;

    /*
     * Grant mappings can be used for DMA requests. The dev_bus_addr
     * returned by the hypercall is the MFN (not the IPA). For device
     * protected by an IOMMU, Xen needs to add a 1:1 mapping in the domain
     * p2m to allow DMA request to work.
     * This is only valid when the domain is directed mapped. Hence this
     * function should only be used by gnttab code with gfn == mfn == dfn.
     */
    BUG_ON(!is_domain_direct_mapped(d));
    BUG_ON(mfn_x(mfn) != dfn_x(dfn));

    /* We only support readable and writable flags */
    if ( !(flags & (IOMMUF_readable | IOMMUF_writable)) )
        return -EINVAL;

    t = (flags & IOMMUF_writable) ? p2m_iommu_map_rw : p2m_iommu_map_ro;

    /*
     * The function guest_physmap_add_entry replaces the current mapping
     * if there is already one...
     */
    return guest_physmap_add_entry(d, _gfn(dfn_x(dfn)), _mfn(dfn_x(dfn)), 0, t);
}

static int __must_check ipmmu_unmap_page(struct domain *d, dfn_t dfn,
                                         unsigned int *flush_flags)
{
    /*
     * This function should only be used by gnttab code when the domain
     * is direct mapped (i.e. gfn == mfn == dfn).
     */
    if ( !is_domain_direct_mapped(d) )
        return -EINVAL;

    return guest_physmap_remove_page(d, _gfn(dfn_x(dfn)), _mfn(dfn_x(dfn)), 0);
}

static const struct iommu_ops ipmmu_iommu_ops =
{
    .init            = ipmmu_iommu_domain_init,
    .hwdom_init      = ipmmu_iommu_hwdom_init,
    .teardown        = ipmmu_iommu_domain_teardown,
    .iotlb_flush     = ipmmu_iotlb_flush,
    .iotlb_flush_all = ipmmu_iotlb_flush_all,
    .assign_device   = ipmmu_assign_device,
    .reassign_device = ipmmu_reassign_device,
    .map_page        = ipmmu_map_page,
    .unmap_page      = ipmmu_unmap_page,
};

/* RCAR GEN3 product and cut information. */
#define RCAR_PRODUCT_MASK    0x00007F00
#define RCAR_PRODUCT_H3      0x00004F00
#define RCAR_PRODUCT_M3      0x00005200
#define RCAR_PRODUCT_M3N     0x00005500
#define RCAR_CUT_MASK        0x000000FF
#define RCAR_CUT_VER30       0x00000020

static __init bool ipmmu_stage2_supported(void)
{
    struct dt_device_node *np;
    u64 addr, size;
    void __iomem *base;
    u32 product, cut;
    static enum
	{
        UNKNOWN,
        SUPPORTED,
        NOTSUPPORTED
    } stage2_supported = UNKNOWN;

    /* Use the flag to avoid checking for the compatibility more then once. */
    switch ( stage2_supported )
    {
    case SUPPORTED:
        return true;

    case NOTSUPPORTED:
        return false;

    case UNKNOWN:
    default:
        stage2_supported = NOTSUPPORTED;
        break;
    }

    np = dt_find_compatible_node(NULL, NULL, "renesas,prr");
    if ( !np )
    {
        printk(XENLOG_ERR "ipmmu: Failed to find PRR node\n");
        return false;
    }

    if ( dt_device_get_address(np, 0, &addr, &size) )
    {
        printk(XENLOG_ERR "ipmmu: Failed to get PRR MMIO\n");
        return false;
    }

    base = ioremap_nocache(addr, size);
    if ( !base )
    {
        printk(XENLOG_ERR "ipmmu: Failed to ioremap PRR MMIO\n");
        return false;
    }

    product = readl(base);
    cut = product & RCAR_CUT_MASK;
    product &= RCAR_PRODUCT_MASK;

    switch ( product )
    {
    case RCAR_PRODUCT_H3:
    case RCAR_PRODUCT_M3:
        if ( cut >= RCAR_CUT_VER30 )
            stage2_supported = SUPPORTED;
        break;

    case RCAR_PRODUCT_M3N:
        stage2_supported = SUPPORTED;
        break;

    default:
        printk(XENLOG_ERR "ipmmu: Unsupported SoC version\n");
        break;
    }

    iounmap(base);

    return stage2_supported == SUPPORTED;
}

static const struct dt_device_match ipmmu_dt_match[] __initconst =
{
    DT_MATCH_COMPATIBLE("renesas,ipmmu-r8a7795"),
    DT_MATCH_COMPATIBLE("renesas,ipmmu-r8a77965"),
    DT_MATCH_COMPATIBLE("renesas,ipmmu-r8a7796"),
    { /* sentinel */ },
};

static __init int ipmmu_init(struct dt_device_node *node, const void *data)
{
    static struct dt_device_node *root_node = NULL;
    static bool init_once = true;
    int ret;

    /*
     * Even if the device can't be initialized, we don't want to give
     * the IPMMU device to dom0.
     */
    dt_device_set_used_by(node, DOMID_XEN);

    if ( !iommu_hap_pt_share )
    {
        dev_err(&node->dev, "P2M table must always be shared between the CPU and the IPMMU\n");
        return -EINVAL;
    }

    if ( !ipmmu_stage2_supported() )
    {
        dev_err(&node->dev, "P2M sharing is not supported in current SoC revision\n");
        return -EOPNOTSUPP;
    }
    else
    {
        /*
         * As 4-level translation table is not supported in IPMMU, we need
         * to check IPA size used for P2M table beforehand to be sure it is
         * 3-level and the IPMMU will be able to use it.
         *
         * In case of using 4KB page granule we should use two concatenated
         * translation tables at level 1 in order to support 40 bit IPA
         * with 3-level translation table.
         *
         * TODO: Probably, when determing the "pa_range" in setup_virt_paging()
         * we should take into the account the IPMMU ability as well.
         */
        if ( IPMMU_MAX_P2M_IPA_BITS < p2m_ipa_bits )
        {
            dev_err(&node->dev, "P2M IPA size is not supported (P2M=%u IPMMU=%u)!\n",
                    p2m_ipa_bits, IPMMU_MAX_P2M_IPA_BITS);
            return -EOPNOTSUPP;
        }
    }

    if ( init_once )
    {
        /*
        * Loop through all IPMMU nodes to find Root IPMMU device. It must
        * be probed the first.
        * Determine if this IPMMU node is a Root device by checking for
        * the lack of "renesas,ipmmu-main" property.
        */
        while ( (root_node = dt_find_matching_node(root_node, ipmmu_dt_match)) )
        {
            if ( !dt_find_property(root_node, "renesas,ipmmu-main", NULL) )
                break;
        }

        init_once = false;

        if ( !root_node )
        {
            dev_err(&node->dev, "Failed to find Root node\n");
            return -ENODEV;
        }

        /*
         * Probe Root IPMMU beforehand despite what IPMMU device is being
         * processed now.
         */
        ret = ipmmu_probe(root_node);
        if ( ret )
        {
            dev_err(&root_node->dev, "Failed to init Root IPMMU (%d)\n", ret);
            root_node = NULL;
            return ret;
        }
    }

    /* There is no sense in initializing Cache IPMMUs without Root IPMMU. */
    if ( !root_node )
        return -ENODEV;

    /* Probe Cache IPMMU and skip already register Root IPMMU if such. */
    if ( root_node != node )
    {
        ret = ipmmu_probe(node);
        if ( ret )
        {
            dev_err(&node->dev, "Failed to init Cache IPMMU (%d)\n", ret);
            return ret;
        }
    }

    iommu_set_ops(&ipmmu_iommu_ops);

    return 0;
}

DT_DEVICE_START(ipmmu, "Renesas IPMMU-VMSA", DEVICE_IOMMU)
    .dt_match = ipmmu_dt_match,
    .init = ipmmu_init,
DT_DEVICE_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
