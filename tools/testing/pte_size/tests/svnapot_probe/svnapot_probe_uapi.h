/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SVNAPOT_PROBE_UAPI_H
#define SVNAPOT_PROBE_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SVNAPOT_PROBE_PTES	16
#define SVNAPOT_PROBE_SIZE	(64 * 1024)
#define SVNAPOT_PROBE_PIN_WRITE	1U

struct svnapot_probe_query {
	__u64 addr;
	__u64 base;
	__u64 raw[SVNAPOT_PROBE_PTES];
	__u64 pfn[SVNAPOT_PROBE_PTES];
	__u32 seen_mask;
	__u32 present_mask;
	__u32 napot_mask;
	__u32 pte_shift;
	__u32 pg_shift;
	__u32 reserved;
};

struct svnapot_probe_pin {
	__u64 addr;
	__u64 length;
	__u32 flags;
	__s32 pinned;
};

#define SVNAPOT_PROBE_IOC_MAGIC	'N'
#define SVNAPOT_PROBE_QUERY	_IOWR(SVNAPOT_PROBE_IOC_MAGIC, 0x01, \
				      struct svnapot_probe_query)
#define SVNAPOT_PROBE_PIN	_IOWR(SVNAPOT_PROBE_IOC_MAGIC, 0x02, \
				      struct svnapot_probe_pin)

#endif /* SVNAPOT_PROBE_UAPI_H */
