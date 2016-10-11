/*
 * xen/arch/arm/coproc/plat/coproc_xxx.h
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

#ifndef __COPROC_XXX_H_
#define __COPROC_XXX_H_

#include "common.h"

struct mmios {
	void __iomem *base;
	u64 addr;
	u64 size;
};

struct coproc_xxx_device {
	char *name;
	struct device *dev;

	u32 num_mmios;
	struct mmios *mmios;
	u32 num_irqs;
	unsigned int *irqs;
	struct list_head list;
};

#endif /* __COPROC_XXX_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
