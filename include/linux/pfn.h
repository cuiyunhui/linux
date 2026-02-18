/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PFN_H_
#define _LINUX_PFN_H_

#ifndef __ASSEMBLY__
#include <linux/types.h>
#endif

#define PFN_ALIGN(x)	(((unsigned long)(x) + (PTE_SIZE - 1)) & PTE_MASK)
#define PFN_UP(x)	(((x) + PTE_SIZE-1) >> PTE_SHIFT)
#define PFN_DOWN(x)	((x) >> PTE_SHIFT)
#define PFN_PHYS(x)	((phys_addr_t)(x) << PTE_SHIFT)
#define PHYS_PFN(x)	((unsigned long)((x) >> PTE_SHIFT))

#endif
