#ifndef _URCU_RCUJA_RANGE_H
#define _URCU_RCUJA_RANGE_H

/*
 * urcu/rcuja-range.h
 *
 * Userspace RCU library - RCU Judy Array Range Support
 *
 * Copyright 2012-2013 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Include this file _after_ including your URCU flavor.
 */

#include <urcu/rcuja.h>
#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum cds_ja_range_type {
	CDS_JA_RANGE_ALLOCATED,
	CDS_JA_RANGE_FREE,
	CDS_JA_RANGE_REMOVED,
};

/*
 * Range goes from start (inclusive) to end (inclusive).
 * Range start is used as node key in the Judy array.
 */
struct cds_ja_range {
	uint64_t start, end;
	struct cds_ja_node ja_node;
	pthread_mutex_t lock;
	enum cds_ja_range_type type;
	struct rcu_head head;
};

int cds_ja_range_init(struct cds_ja *ja);
int cds_ja_range_fini(struct cds_ja *ja);

struct cds_ja_range *cds_ja_range_lookup(struct cds_ja *ja, uint64_t key);

struct cds_ja_range *cds_ja_range_add(struct cds_ja *ja,
		uint64_t start,		/* inclusive */
		uint64_t end);		/* inclusive */

int cds_ja_range_del(struct cds_ja *ja, struct cds_ja_range *range);

#ifdef __cplusplus
}
#endif

#endif /* _URCU_RCUJA_H */
