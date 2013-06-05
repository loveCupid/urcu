#ifndef _URCU_RCUJA_H
#define _URCU_RCUJA_H

/*
 * urcu/rcuja.h
 *
 * Userspace RCU library - RCU Judy Array
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
 *
 * Include this file _after_ including your URCU flavor.
 */

#include <stdint.h>
#include <urcu/compiler.h>
#include <urcu-call-rcu.h>
#include <urcu-flavor.h>
#include <stdint.h>
#include <urcu/rcuhlist.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cds_ja_node {
	/* Linked list of nodes with same key */
	struct cds_hlist_node list;
};

struct cds_ja;

/*
 * cds_ja_node_init - initialize a judy array node
 * @node: the node to initialize.
 *
 * This function is kept to be eventually used for debugging purposes
 * (detection of memory corruption).
 */
static inline
void cds_ja_node_init(struct cds_ja_node *node)
{
}

struct cds_hlist_head cds_ja_lookup(struct cds_ja *ja, uint64_t key);
struct cds_hlist_head cds_ja_lookup_lower_equal(struct cds_ja *ja,
		uint64_t key);

int cds_ja_add(struct cds_ja *ja, uint64_t key,
		struct cds_ja_node *new_node);

struct cds_ja_node *cds_ja_add_unique(struct cds_ja *ja, uint64_t key,
		struct cds_ja_node *new_node);

int cds_ja_del(struct cds_ja *ja, uint64_t key,
		struct cds_ja_node *node);

struct cds_ja *_cds_ja_new(unsigned int key_bits,
		const struct rcu_flavor_struct *flavor);

static inline
struct cds_ja *cds_ja_new(unsigned int key_bits)
{
	return _cds_ja_new(key_bits, &rcu_flavor);
}

int cds_ja_destroy(struct cds_ja *ja,
		void (*rcu_free_node_cb)(struct cds_ja_node *node));

#ifdef __cplusplus
}
#endif

#endif /* _URCU_RCUJA_H */
