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
#include <limits.h>
#include <urcu/rcuja.h>
#include <urcu/compiler.h>
#include <urcu/arch.h>
#include <assert.h>
#include "rcuja-internal.h"
#include "bitfield.h"

enum rcu_ja_type_class {
	RCU_JA_LINEAR = 0,	/* Type A */
			/* 32-bit: 1 to 25 children, 8 to 128 bytes */
			/* 64-bit: 1 to 28 children, 16 to 256 bytes */
	RCU_JA_POOL = 1,	/* Type B */
			/* 32-bit: 26 to 100 children, 256 to 512 bytes */
			/* 64-bit: 29 to 112 children, 512 to 1024 bytes */
	RCU_JA_PIGEON = 2,	/* Type C */
			/* 32-bit: 101 to 256 children, 1024 bytes */
			/* 64-bit: 113 to 256 children, 2048 bytes */
	/* Leaf nodes are implicit from their height in the tree */
};

struct rcu_ja_type {
	enum rcu_ja_type_class type_class;
	uint16_t min_child;	/* minimum number of children: 1 to 256 */
	uint16_t max_child;	/* maximum number of children: 1 to 256 */
	uint16_t order;		/* node size is (1 << order), in bytes */
	uint16_t nr_pool_order;		/* number of pools */
	uint16_t pool_size_order;	/* pool size */
};

/*
 * Number of least significant pointer bits reserved to represent the
 * child type.
 */
#define JA_TYPE_BITS	3
#define JA_TYPE_MAX_NR	(1U << JA_TYPE_BITS)
#define JA_TYPE_MASK	(JA_TYPE_MAX_NR - 1)
#define JA_PTR_MASK	(~JA_TYPE_MASK)

#define JA_ENTRY_PER_NODE	256UL

/*
 * Iteration on the array to find the right node size for the number of
 * children stops when it reaches .max_child == 256 (this is the largest
 * possible node size, which contains 256 children).
 * The min_child overlaps with the previous max_child to provide an
 * hysteresis loop to reallocation for patterns of cyclic add/removal
 * within the same node.
 * The node the index within the following arrays is represented on 3
 * bits. It identifies the node type, min/max number of children, and
 * the size order.
 * The max_child values for the RCU_JA_POOL below result from
 * statistical approximation: over million populations, the max_child
 * covers between 97% and 99% of the populations generated. Therefore, a
 * fallback should exist to cover the rare extreme population unbalance
 * cases, but it will not have a major impact on speed nor space
 * consumption, since those are rare cases.
 */

#if (CAA_BITS_PER_LONG < 64)
/* 32-bit pointers */
const struct rcu_ja_type ja_types[] = {
	{ .type_class = RCU_JA_LINEAR, .min_child = 1, .max_child = 1, .order = 3, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 1, .max_child = 3, .order = 4, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 3, .max_child = 6, .order = 5, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 4, .max_child = 12, .order = 6, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 10, .max_child = 25, .order = 7, },

	/* Pools may fill sooner than max_child */
	{ .type_class = RCU_JA_POOL, .min_child = 20, .max_child = 48, .order = 8, .nr_pool_order = 1, .pool_size_order = 7, },
	{ .type_class = RCU_JA_POOL, .min_child = 45, .max_child = 92, .order = 9, .nr_pool_order = 2, .pool_size_order = 7, },

	/*
	 * TODO: Upon node removal below min_child, if child pool is
	 * filled beyond capacity, we need to roll back to pigeon.
	 */
	{ .type_class = RCU_JA_PIGEON, .min_child = 89, .max_child = 256, .order = 10, },
};
CAA_BUILD_BUG_ON(CAA_ARRAY_SIZE(ja_types) > JA_TYPE_MAX_NR);
#else /* !(CAA_BITS_PER_LONG < 64) */
/* 64-bit pointers */
const struct rcu_ja_type ja_types[] = {
	{ .type_class = RCU_JA_LINEAR, .min_child = 1, .max_child = 1, .order = 4, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 1, .max_child = 3, .order = 5, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 3, .max_child = 7, .order = 6, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 5, .max_child = 14, .order = 7, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 10, .max_child = 28, .order = 8, },

	/* Pools may fill sooner than max_child. */
	{ .type_class = RCU_JA_POOL, .min_child = 22, .max_child = 54, .order = 9, .nr_pool_order = 1, .pool_size_order = 8, },
	{ .type_class = RCU_JA_POOL, .min_child = 51, .max_child = 104, .order = 10, .nr_pool_order = 2, .pool_size_order = 8, },

	/*
	 * TODO: Upon node removal below min_child, if child pool is
	 * filled beyond capacity, we need to roll back to pigeon.
	 */
	{ .type_class = RCU_JA_PIGEON, .min_child = 101, .max_child = 256, .order = 11, },
};
CAA_BUILD_BUG_ON(CAA_ARRAY_SIZE(ja_types) > JA_TYPE_MAX_NR);
#endif /* !(BITS_PER_LONG < 64) */

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

/* Never declared. Opaque type used to store flagged node pointers. */
struct rcu_ja_node_flag;

static
struct rcu_ja_node_flag *ja_node_flag(struct rcu_ja_node *node, unsigned int type)
{
	assert(type < JA_TYPE_NR);
	return (struct rcu_ja_node_flag *) (((unsigned long) node) | type);
}

static
unsigned int ja_node_type(struct rcu_ja_node_flag *node)
{
	unsigned int type;

	type = (unsigned int) ((unsigned long) node & JA_TYPE_MASK);
	assert(type < JA_TYPE_NR);
	return type;
}

static
struct rcu_ja_node *ja_node_ptr(struct rcu_ja_node_flag *node)
{
	return (struct rcu_ja_node *) (((unsigned long) node) | JA_PTR_MASK);
}

struct rcu_ja_node *alloc_rcu_ja_node(struct rcu_ja_type *ja_type)
{
	return zmalloc(1 << ja_type->node_order);
}

void free_rcu_ja_node(struct rcu_ja_node *node)
{
	free(node);
}

#define __JA_ALIGN_MASK(v, mask)	(((v) + (mask)) & ~(mask))
#define JA_ALIGN(v, align)		__JA_ALIGN_MASK(v, (typeof(v)) (align) - 1)
#define __JA_FLOOR_MASK(v, mask)	((v) & ~(mask))
#define JA_FLOOR(v, align)		__JA_FLOOR_MASK(v, (typeof(v)) (align) - 1)

static
char *align_ptr_size(char *ptr)
{
	return JA_ALIGN(ptr, sizeof(ptr));
}

static
struct rcu_ja_node_flag *ja_linear_node_get_nth(const struct rcu_ja_type *type,
					struct rcu_ja_node *node,
					uint8_t n)
{
	uint8_t nr_child;
	uint8_t *values;
	struct rcu_ja_node_flag *pointers;
	struct rcu_ja_node_flag *ptr;
	unsigned int i;

	assert(!type || type->type_class == RCU_JA_LINEAR);

	nr_child = node->data[0];
	cmm_smp_rmb();	/* read nr_child before values */
	assert(!type || nr_child <= type->max_child);
	assert(!type || nr_child >= type->min_child);

	values = &node[1];
	for (i = 0; i < nr_child; i++) {
		if (values[i] == n)
			break;
	}
	if (i >= nr_child)
		return NULL;
	cmm_smp_rmb();	/* read values before pointer */
	pointers = align_ptr_size(&values[nr_child]);
	ptr = pointers[i];
	assert(ja_node_ptr(ptr) != NULL);
	return ptr;
}

static
struct rcu_ja_node_flag *ja_pool_node_get_nth(const struct rcu_ja_type *type,
					struct rcu_ja_node *node,
					uint8_t n)
{
	struct rcu_ja_node_flag *ptr;
	struct rcu_ja_node *linear;

	assert(type->type_class == RCU_JA_POOL);
	linear = (struct rcu_ja_node *)
		&node->data[((unsigned long) n >> (CHAR_BIT - type->nr_pool_order)) << type->pool_size_order];
	return ja_linear_node_get_nth(NULL, linear, n);
}

static
struct rcu_ja_node_flag *ja_pigeon_node_get_nth(const struct rcu_ja_type *type,
					struct rcu_ja_node *node,
					uint8_t n)
{
	assert(type->type_class == RCU_JA_PIGEON);
	return ((struct rcu_ja_node_flag *) node->data)[n];
}

/* ja_node_get_nth: get nth item from a node */
static
struct rcu_ja_node_flag *ja_node_get_nth(struct rcu_ja_node_flag *node_flag,
					uint8_t n)
{
	unsigned int type_index;
	struct rcu_ja_node *node;
	const struct rcu_ja_type *type;

	node_flag = rcu_dereference(node_flag);
	node = ja_node_ptr(node_flag);
	assert(node != NULL);
	type_index = ja_node_type(node_flag);
	type = &ja_types[type_index];

	switch (type->type_class) {
	case RCU_JA_LINEAR:
		return ja_linear_node_get_nth(type, node, n);
	case RCU_JA_POOL:
		return ja_pool_node_get_nth(type, node, n);
	case RCU_JA_PIGEON:
		return ja_pigeon_node_get_nth(type, node, n);
	default:
		assert(0);
		return (void *) -1UL;
	}
}

/*
 * ja_node_set_nth: set nth item within a node. asserts that it is not
 * there yet.
 */

