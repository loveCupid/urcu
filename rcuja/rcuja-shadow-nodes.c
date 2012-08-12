/*
 * rcuja/rcuja-hashtable.c
 *
 * Userspace RCU library - RCU Judy Array Shadow Node Hash Table
 *
 * Copyright 2012 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
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
 */

#define _LGPL_SOURCE
#include <stdint.h>
#include <errno.h>
#include <limits.h>

/*
 * The hash table used by judy array updates only for the shadow node
 * mapping rely on standard urcu_mb flavor. It does not put any
 * requirement on the RCU flavor used by applications using the judy
 * array.
 */
#include <urcu.h>

#include <urcu/rcuja.h>
#include <urcu/compiler.h>
#include <urcu/arch.h>
#include <assert.h>
#include <urcu-pointer.h>

#include "rcuja-internal.h"
#include "bitfield.h"

__attribute__((visibility("protected")))
struct cds_lfht *rcuja_create_ht(void)
{
	return cds_lfht_new(1, 1, 0,
		CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING,
		NULL);
}

__attribute__((visibility("protected")))
void rcuja_delete_ht(struct cds_lfht *ht)
{
	int ret;

	ret = cds_lfht_destroy(ht, NULL);
	assert(!ret);
}
