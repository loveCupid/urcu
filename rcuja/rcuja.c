/*
 * rcuja/rcuja.c
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
 */

#include <stdint.h>
#include <urcu/rcuja.h>
#include "rcuja-internal.h"

enum rcu_ja_child_type {
	RCU_JA_LINEAR = 0,	/* Type A */
			/* 32-bit: 1 to 12 children, 8 to 64 bytes */
			/* 64-bit: 1 to 14 children, 16 to 128 bytes */
	RCU_JA_BITMAP = 1,	/* Type B */
			/* 32-bit: 13 to 120 children, 128 to 512 bytes */
			/* 64-bit: 15 to 124 children, 256 to 1024 bytes */
	RCU_JA_PIGEON = 2,	/* Type C */
			/* 32-bit: 121 to 256 children, 1024 bytes */
			/* 64-bit: 125 to 256 children, 2048 bytes */
	/* Leaf nodes are implicit from their height in the tree */
};

struct rcu_ja_type_select {
	enum rcu_ja_child_type type;
	uint16_t max_nr_child;	/* 1 to 256 */
	uint16_t node_order;	/* node size is (1 << node_order), in bytes */
};

/*
 * Iteration on the array to find the right node size for the number of
 * children stops when it reaches max_nr_child == 0 (this is the largest
 * possible node size, which contains 256 children).
 */
const struct rcu_ja_type_select ja_select_u32[] = {
	{ .type = RCU_JA_LINEAR, .max_nr_child = 1, .node_order = 3, },
	{ .type = RCU_JA_LINEAR, .max_nr_child = 3, .node_order = 4, },
	{ .type = RCU_JA_LINEAR, .max_nr_child = 6, .node_order = 5, },
	{ .type = RCU_JA_LINEAR, .max_nr_child = 12, .node_order = 6, },

	{ .type = RCU_JA_BITMAP, .max_nr_child = 24, .node_order = 7, },
	{ .type = RCU_JA_BITMAP, .max_nr_child = 56, .node_order = 8, },
	{ .type = RCU_JA_BITMAP, .max_nr_child = 120, .node_order = 9, },

	{ .type = RCU_JA_PIGEON, .max_nr_child = 256, .node_order = 10, },
};

const struct rcu_ja_type_select ja_select_u64[] = {
	{ .type = RCU_JA_LINEAR, .max_nr_child = 1, .node_order = 4, },
	{ .type = RCU_JA_LINEAR, .max_nr_child = 3, .node_order = 5, },
	{ .type = RCU_JA_LINEAR, .max_nr_child = 7, .node_order = 6, },
	{ .type = RCU_JA_LINEAR, .max_nr_child = 14, .node_order = 7, },

	{ .type = RCU_JA_BITMAP, .max_nr_child = 28, .node_order = 8, },
	{ .type = RCU_JA_BITMAP, .max_nr_child = 60, .node_order = 9, },
	{ .type = RCU_JA_BITMAP, .max_nr_child = 124, .node_order = 10, },

	{ .type = RCU_JA_PIGEON, .max_nr_child = 256, .node_order = 11, },
};

/*
 * The rcu_ja_node starts with a byte counting the number of children in
 * the node. Then, the node-specific data is placed.
 * TODO: where should we put the mutex for the node ?
 *  -> mutex could be a 0-value node count.
 * TODO: where do we keep nr children for pigeon ?
 */
struct rcu_ja_node {
	char data[];
};

struct rcu_ja_node *create_rcu_ja_node(struct rcu_ja_type_select *sel)
{
	return zmalloc(1 << sel->node_order);
}

void free_rcu_ja_node(struct rcu_ja_node *node)
{
	free(node);
}



