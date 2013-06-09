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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cds_ja_range *cds_ja_range_lookup(struct cds_ja *ja, uint64_t key);

struct cds_ja_range *cds_ja_range_lock(struct cds_ja_range *range);

void cds_ja_range_unlock(struct cds_ja_range *range);

int cds_ja_range_add(struct cds_ja *ja,
		uint64_t start,		/* inclusive */
		uint64_t end,		/* inclusive */
		void *priv);

int cds_ja_range_del(struct cds_ja *ja, struct cds_ja_range *range);

struct cds_ja *_cds_ja_range_new(const struct rcu_flavor_struct *flavor);

static inline
struct cds_ja *cds_ja_range_new(void)
{
	return _cds_ja_range_new(&rcu_flavor);
}

int cds_ja_range_destroy(struct cds_ja *ja,
		void (*free_priv)(void *ptr));

#ifdef __cplusplus
}
#endif

#endif /* _URCU_RCUJA_RANGE_H */
