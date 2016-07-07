/*
 * xen/arch/arm/optee/smc.c
 *
 * OPTEE SMC calls proxy/handler
 *
 * Volodymyr Babchuk <volodymyr.babchuk@globallogic.com>
 * Copyright (c) 2016 GlobalLogic Inc.
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

#include <xen/config.h>
#include <xen/lib.h>

#include <asm/p2m.h>
#include <asm/system.h>
#include <asm/processor.h>
#include <asm/current.h>

#include <xen/sched.h>
#include <asm/gic.h>
#include <asm/event.h>
#include <public/xen.h>

#include "optee.h"
#include "optee_smc.h"

struct domain_shmem_info {
	paddr_t maddr;
	paddr_t gaddr;
	size_t size;
	domid_t domain_id;
	bool_t valid;
};

static struct domain_shmem_info domain_shmem_info[OPTEE_MAX_DOMAINS];

struct optee_shmem_info {
	paddr_t maddr;
	size_t size;
	bool_t valid;
} optee_shmem_info;

static void execute_smc(struct cpu_user_regs *regs)
{
	register_t retval[4];

	call_smc_ext(regs->x0,
	             regs->x1,
	             regs->x2,
	             regs->x3,
	             regs->x4,
	             regs->x5,
	             regs->x6,
	             current->domain->domain_id + 1,
	             retval);
	regs->x0 = retval[0];
	regs->x1 = retval[1];
	regs->x2 = retval[2];
	regs->x3 = retval[3];
}

static void do_process_get_shm_config(struct cpu_user_regs *regs)
{
	if (!optee_shmem_info.valid) {
		size_t domain_shmem_size;
		paddr_t maddr;

		/* Get config from OPTEE */
		execute_smc(regs);
		optee_shmem_info.maddr = regs->x1;
		optee_shmem_info.size = regs->x2;
		optee_shmem_info.valid = true;
		/* Split OP-TEE shmem region for domains */
		domain_shmem_size =
			(optee_shmem_info.size/OPTEE_MAX_DOMAINS);
		domain_shmem_size -= domain_shmem_size%PAGE_SIZE;
		maddr = optee_shmem_info.maddr;
		for (int i = 0; i < OPTEE_MAX_DOMAINS; i++) {
			domain_shmem_info[i].valid = false;
			domain_shmem_info[i].size = domain_shmem_size;
			domain_shmem_info[i].maddr = maddr;
			domain_shmem_info[i].gaddr = maddr;
			maddr += domain_shmem_size;
		}
	}

	/* Check if memory is already maped for this domain */
	for (int i = 0; i < OPTEE_MAX_DOMAINS; i++) {
		if (domain_shmem_info[i].valid &&
		    domain_shmem_info[i].domain_id == current->domain->domain_id) {
			regs->x0 = OPTEE_SMC_RETURN_OK;
			regs->x1 = domain_shmem_info[i].gaddr;
			regs->x2 = domain_shmem_info[i].size;
			regs->x3 = OPTEE_SMC_SHM_CACHED;
			return;
		}
	}

	/* Find free slot and map memory */
	for (int i = 0; i < OPTEE_MAX_DOMAINS; i++) {
		if (domain_shmem_info[i].valid == false) {
			int ret = guest_physmap_add_entry_range(
				current->domain,
				paddr_to_pfn(domain_shmem_info[i].gaddr),
				domain_shmem_info[i].size / PAGE_SIZE,
				paddr_to_pfn(domain_shmem_info[i].maddr),
				p2m_ram_rw);
			if (ret == 0) {
				regs->x0 = OPTEE_SMC_RETURN_OK;
				regs->x1 = domain_shmem_info[i].gaddr;
				regs->x2 = domain_shmem_info[i].size;
				regs->x3 = OPTEE_SMC_SHM_CACHED;
				domain_shmem_info[i].domain_id =
					current->domain->domain_id;
				domain_shmem_info[i].valid = true;
				return;
			} else {
				regs->x0 = OPTEE_SMC_RETURN_ENOMEM;
				return;
			}
		}
	}

	/* There are no free slots */
	regs->x0 = OPTEE_SMC_RETURN_ENOMEM;
}

int optee_handle_smc(struct cpu_user_regs *regs)
{
	uint32_t smc_code = regs->r0;
	switch(smc_code){
	case OPTEE_SMC_GET_SHM_CONFIG:
		do_process_get_shm_config(regs);
		break;
	default:
		/* Just forward request to OPTEE */
		execute_smc(regs);
		break;
	}
	return 0;
}


void optee_domain_destroy(struct domain *d)
{
	/* Mark domain's shared memory as free */
	for (int i = 0; i < OPTEE_MAX_DOMAINS; i++) {
		if (domain_shmem_info[i].valid &&
		    domain_shmem_info[i].domain_id == d->domain_id) {
			domain_shmem_info[i].valid = false;
		}
	}
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
