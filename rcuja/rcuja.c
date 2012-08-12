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
#include <errno.h>
#include <limits.h>
#include <urcu/rcuja.h>
#include <urcu/compiler.h>
#include <urcu/arch.h>
#include <assert.h>
#include <urcu-pointer.h>

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
	RCU_JA_NR_TYPES,
};

struct rcu_ja_type {
	enum rcu_ja_type_class type_class;
	uint16_t min_child;		/* minimum number of children: 1 to 256 */
	uint16_t max_child;		/* maximum number of children: 1 to 256 */
	uint16_t max_linear_child;	/* per-pool max nr. children: 1 to 256 */
	uint16_t order;			/* node size is (1 << order), in bytes */
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
enum {
	ja_type_0_max_child = 1,
	ja_type_1_max_child = 3,
	ja_type_2_max_child = 6,
	ja_type_3_max_child = 12,
	ja_type_4_max_child = 25,
	ja_type_5_max_child = 48,
	ja_type_6_max_child = 92,
	ja_type_7_max_child = 256,
};

enum {
	ja_type_0_max_linear_child = 1,
	ja_type_1_max_linear_child = 3,
	ja_type_2_max_linear_child = 6,
	ja_type_3_max_linear_child = 12,
	ja_type_4_max_linear_child = 25,
	ja_type_5_max_linear_child = 24,
	ja_type_6_max_linear_child = 23,
};

enum {
	ja_type_5_nr_pool_order = 1,
	ja_type_6_nr_pool_order = 2,
};

const struct rcu_ja_type ja_types[] = {
	{ .type_class = RCU_JA_LINEAR, .min_child = 1, .max_child = ja_type_0_max_child, .max_linear_child = ja_type_0_max_linear_child, .order = 3, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 1, .max_child = ja_type_1_max_child, .max_linear_child = ja_type_1_max_linear_child, .order = 4, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 3, .max_child = ja_type_2_max_child, .max_linear_child = ja_type_2_max_linear_child, .order = 5, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 4, .max_child = ja_type_3_max_child, .max_linear_child = ja_type_3_max_linear_child, .order = 6, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 10, .max_child = ja_type_4_max_child, .max_linear_child = ja_type_4_max_linear_child, .order = 7, },

	/* Pools may fill sooner than max_child */
	{ .type_class = RCU_JA_POOL, .min_child = 20, .max_child = ja_type_5_max_child, .max_linear_child = ja_type_5_max_linear_child, .order = 8, .nr_pool_order = ja_type_5_nr_pool_order, .pool_size_order = 7, },
	{ .type_class = RCU_JA_POOL, .min_child = 45, .max_child = ja_type_6_max_child, .max_linear_child = ja_type_6_max_linear_child, .order = 9, .nr_pool_order = ja_type_6_nr_pool_order, .pool_size_order = 7, },

	/*
	 * TODO: Upon node removal below min_child, if child pool is
	 * filled beyond capacity, we need to roll back to pigeon.
	 */
	{ .type_class = RCU_JA_PIGEON, .min_child = 89, .max_child = ja_type_7_max_child, .order = 10, },
};
#else /* !(CAA_BITS_PER_LONG < 64) */
/* 64-bit pointers */
enum {
	ja_type_0_max_child = 1,
	ja_type_1_max_child = 3,
	ja_type_2_max_child = 7,
	ja_type_3_max_child = 14,
	ja_type_4_max_child = 28,
	ja_type_5_max_child = 54,
	ja_type_6_max_child = 104,
	ja_type_7_max_child = 256,
};

enum {
	ja_type_0_max_linear_child = 1,
	ja_type_1_max_linear_child = 3,
	ja_type_2_max_linear_child = 7,
	ja_type_3_max_linear_child = 14,
	ja_type_4_max_linear_child = 28,
	ja_type_5_max_linear_child = 27,
	ja_type_6_max_linear_child = 26,
};

enum {
	ja_type_5_nr_pool_order = 1,
	ja_type_6_nr_pool_order = 2,
};

const struct rcu_ja_type ja_types[] = {
	{ .type_class = RCU_JA_LINEAR, .min_child = 1, .max_child = ja_type_0_max_child, .max_linear_child = ja_type_0_max_linear_child, .order = 4, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 1, .max_child = ja_type_1_max_child, .max_linear_child = ja_type_1_max_linear_child, .order = 5, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 3, .max_child = ja_type_2_max_child, .max_linear_child = ja_type_2_max_linear_child, .order = 6, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 5, .max_child = ja_type_3_max_child, .max_linear_child = ja_type_3_max_linear_child, .order = 7, },
	{ .type_class = RCU_JA_LINEAR, .min_child = 10, .max_child = ja_type_4_max_child, .max_linear_child = ja_type_4_max_linear_child, .order = 8, },

	/* Pools may fill sooner than max_child. */
	{ .type_class = RCU_JA_POOL, .min_child = 22, .max_child = ja_type_5_max_child, .max_linear_child = ja_type_5_max_linear_child, .order = 9, .nr_pool_order = ja_type_5_nr_pool_order, .pool_size_order = 8, },
	{ .type_class = RCU_JA_POOL, .min_child = 51, .max_child = ja_type_6_max_child, .max_linear_child = ja_type_6_max_linear_child, .order = 10, .nr_pool_order = ja_type_6_nr_pool_order, .pool_size_order = 8, },

	/*
	 * TODO: Upon node removal below min_child, if child pool is
	 * filled beyond capacity, we need to roll back to pigeon.
	 */
	{ .type_class = RCU_JA_PIGEON, .min_child = 101, .max_child = ja_type_7_max_child, .order = 11, },
};
#endif /* !(BITS_PER_LONG < 64) */

static inline __attribute__((unused))
void static_array_size_check(void)
{
	CAA_BUILD_BUG_ON(CAA_ARRAY_SIZE(ja_types) > JA_TYPE_MAX_NR);
}

/*
 * The rcu_ja_node contains the compressed node data needed for
 * read-side. For linear and pool node configurations, it starts with a
 * byte counting the number of children in the node.  Then, the
 * node-specific data is placed.
 * The node mutex, if any is needed, protecting concurrent updated of
 * each node is placed in a separate hash table indexed by node address.
 * For the pigeon configuration, the number of children is also kept in
 * a separate hash table, indexed by node address, because it is only
 * required for updates.
 */

#define DECLARE_LINEAR_NODE(index)								\
	struct {										\
		uint8_t nr_child;								\
		uint8_t child_value[ja_type_## index ##_max_linear_child];			\
		struct rcu_ja_node_flag *child_ptr[ja_type_## index ##_max_linear_child];	\
	}

#define DECLARE_POOL_NODE(index)								\
	struct {										\
		struct {									\
			uint8_t nr_child;							\
			uint8_t child_value[ja_type_## index ##_max_linear_child];		\
			struct rcu_ja_node_flag *child_ptr[ja_type_## index ##_max_linear_child]; \
		} linear[1U << ja_type_## index ##_nr_pool_order];				\
	}

struct rcu_ja_node {
	union {
		/* Linear configuration */
		DECLARE_LINEAR_NODE(0) conf_0;
		DECLARE_LINEAR_NODE(1) conf_1;
		DECLARE_LINEAR_NODE(2) conf_2;
		DECLARE_LINEAR_NODE(3) conf_3;
		DECLARE_LINEAR_NODE(4) conf_4;

		/* Pool configuration */
		DECLARE_POOL_NODE(5) conf_5;
		DECLARE_POOL_NODE(6) conf_6;

		/* Pigeon configuration */
		struct {
			struct rcu_ja_node_flag *child[ja_type_7_max_child];
		} conf_7;
		/* data aliasing nodes for computed accesses */
		uint8_t data[sizeof(struct rcu_ja_node_flag *) * ja_type_7_max_child];
	} u;
};

static
struct rcu_ja_node_flag *ja_node_flag(struct rcu_ja_node *node,
		unsigned int type)
{
	assert(type < RCU_JA_NR_TYPES);
	return (struct rcu_ja_node_flag *) (((unsigned long) node) | type);
}

static
unsigned int ja_node_type(struct rcu_ja_node_flag *node)
{
	unsigned int type;

	type = (unsigned int) ((unsigned long) node & JA_TYPE_MASK);
	assert(type < RCU_JA_NR_TYPES);
	return type;
}

static
struct rcu_ja_node *ja_node_ptr(struct rcu_ja_node_flag *node)
{
	return (struct rcu_ja_node *) (((unsigned long) node) | JA_PTR_MASK);
}

struct rcu_ja_node *alloc_rcu_ja_node(const struct rcu_ja_type *ja_type)
{
	return calloc(1U << ja_type->order, sizeof(char));
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
uint8_t *align_ptr_size(uint8_t *ptr)
{
	return (uint8_t *) JA_ALIGN((unsigned long) ptr, sizeof(void *));
}

/*
 * The order in which values and pointers are does does not matter: if
 * a value is missing, we return NULL. If a value is there, but its
 * associated pointers is still NULL, we return NULL too.
 */
static
struct rcu_ja_node_flag *ja_linear_node_get_nth(const struct rcu_ja_type *type,
		struct rcu_ja_node *node,
		uint8_t n)
{
	uint8_t nr_child;
	uint8_t *values;
	struct rcu_ja_node_flag **pointers;
	struct rcu_ja_node_flag *ptr;
	unsigned int i;

	assert(type->type_class == RCU_JA_LINEAR || type->type_class == RCU_JA_POOL);

	nr_child = CMM_LOAD_SHARED(node->u.data[0]);
	cmm_smp_rmb();	/* read nr_child before values and pointers */
	assert(nr_child <= type->max_linear_child);
	assert(type->type_class != RCU_JA_LINEAR || nr_child >= type->min_child);

	values = &node->u.data[1];
	for (i = 0; i < nr_child; i++) {
		if (CMM_LOAD_SHARED(values[i]) == n)
			break;
	}
	if (i >= nr_child)
		return NULL;
	pointers = (struct rcu_ja_node_flag **) align_ptr_size(&values[type->max_linear_child]);
	ptr = rcu_dereference(pointers[i]);
	assert(ja_node_ptr(ptr) != NULL);
	return ptr;
}

static
struct rcu_ja_node_flag *ja_pool_node_get_nth(const struct rcu_ja_type *type,
		struct rcu_ja_node *node,
		uint8_t n)
{
	struct rcu_ja_node *linear;

	assert(type->type_class == RCU_JA_POOL);
	linear = (struct rcu_ja_node *)
		&node->u.data[((unsigned long) n >> (CHAR_BIT - type->nr_pool_order)) << type->pool_size_order];
	return ja_linear_node_get_nth(type, linear, n);
}

static
struct rcu_ja_node_flag *ja_pigeon_node_get_nth(const struct rcu_ja_type *type,
		struct rcu_ja_node *node,
		uint8_t n)
{
	assert(type->type_class == RCU_JA_PIGEON);
	return rcu_dereference(((struct rcu_ja_node_flag **) node->u.data)[n]);
}

/*
 * ja_node_get_nth: get nth item from a node.
 * node_flag is already rcu_dereference'd.
 */
static
struct rcu_ja_node_flag *ja_node_get_nth(struct rcu_ja_node_flag *node_flag,
		uint8_t n)
{
	unsigned int type_index;
	struct rcu_ja_node *node;
	const struct rcu_ja_type *type;

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

static
int ja_linear_node_set_nth(const struct rcu_ja_type *type,
		struct rcu_ja_node *node,
		uint8_t n,
		struct rcu_ja_node_flag *child_node_flag)
{
	uint8_t nr_child;
	uint8_t *values, *nr_child_ptr;
	struct rcu_ja_node_flag **pointers;
	unsigned int i;

	assert(type->type_class == RCU_JA_LINEAR || type->type_class == RCU_JA_POOL);

	nr_child_ptr = &node->u.data[0];
	nr_child = *nr_child_ptr;
	assert(nr_child <= type->max_linear_child);
	assert(type->type_class != RCU_JA_LINEAR || nr_child >= type->min_child);

	values = &node->u.data[1];
	for (i = 0; i < nr_child; i++) {
		if (values[i] == n)
			return -EEXIST;
	}
	if (nr_child >= type->max_linear_child) {
		/* No space left in this node type */
		return -ENOSPC;
	}
	pointers = (struct rcu_ja_node_flag **) align_ptr_size(&values[type->max_linear_child]);
	assert(pointers[nr_child] == NULL);
	rcu_assign_pointer(pointers[nr_child], child_node_flag);
	CMM_STORE_SHARED(values[nr_child], n);
	cmm_smp_wmb();	/* write value and pointer before nr_child */
	CMM_STORE_SHARED(*nr_child_ptr, nr_child + 1);
	return 0;
}

static
int ja_pool_node_set_nth(const struct rcu_ja_type *type,
		struct rcu_ja_node *node,
		uint8_t n,
		struct rcu_ja_node_flag *child_node_flag)
{
	struct rcu_ja_node *linear;

	assert(type->type_class == RCU_JA_POOL);
	linear = (struct rcu_ja_node *)
		&node->u.data[((unsigned long) n >> (CHAR_BIT - type->nr_pool_order)) << type->pool_size_order];
	return ja_linear_node_set_nth(type, linear, n, child_node_flag);
}

static
int ja_pigeon_node_set_nth(const struct rcu_ja_type *type,
		struct rcu_ja_node *node,
		uint8_t n,
		struct rcu_ja_node_flag *child_node_flag)
{
	struct rcu_ja_node_flag **ptr;

	assert(type->type_class == RCU_JA_PIGEON);
	ptr = &((struct rcu_ja_node_flag **) node->u.data)[n];
	if (*ptr != NULL)
		return -EEXIST;
	rcu_assign_pointer(*ptr, child_node_flag);
	return 0;
}

/*
 * _ja_node_set_nth: set nth item within a node. Return an error
 * (negative error value) if it is already there.
 * TODO: exclusive access on node.
 */
static
int _ja_node_set_nth(struct rcu_ja_node_flag *node_flag, uint8_t n,
		struct rcu_ja_node_flag *child_node_flag)
{
	unsigned int type_index;
	struct rcu_ja_node *node;
	const struct rcu_ja_type *type;

	node = ja_node_ptr(node_flag);
	assert(node != NULL);
	type_index = ja_node_type(node_flag);
	type = &ja_types[type_index];

	switch (type->type_class) {
	case RCU_JA_LINEAR:
		return ja_linear_node_set_nth(type, node, n,
				child_node_flag);
	case RCU_JA_POOL:
		return ja_pool_node_set_nth(type, node, n,
				child_node_flag);
	case RCU_JA_PIGEON:
		return ja_pigeon_node_set_nth(type, node, n,
				child_node_flag);
	default:
		assert(0);
		return -EINVAL;
	}

	return 0;
}

/*
 * ja_node_recompact_add: recompact a node, adding a new child.
 */
static
int ja_node_recompact_add(struct rcu_ja_node_flag **old_node_flag, uint8_t n,
		struct rcu_ja_node_flag *child_node_flag)

{
	unsigned int old_type_index, new_type_index;
	struct rcu_ja_node *old_node, *new_node;
	const struct rcu_ja_type *old_type, *new_type;
	struct rcu_ja_node_flag *new_node_flag;
	unsigned int i;
	int ret;

	old_node = ja_node_ptr(*old_node_flag);
	if (old_node == NULL) {
		new_type_index = 0;
	} else {
		old_type_index = ja_node_type(*old_node_flag);
		old_type = &ja_types[old_type_index];
		new_type_index = old_type_index + 1;
	}
	new_type = &ja_types[new_type_index];
	new_node = alloc_rcu_ja_node(new_type);
	if (!new_node)
		return -ENOMEM;
	new_node_flag = ja_node_flag(new_node, new_type_index);

	for (i = 0; i < old_type->max_child; i++) {
		struct rcu_ja_node_flag *iter;

		iter = ja_node_get_nth(*old_node_flag, i);
		if (!iter)
			continue;
		ret = _ja_node_set_nth(new_node_flag, i, iter);
		assert(!ret);
	}
	/* add node */
	ret = _ja_node_set_nth(new_node_flag, n, child_node_flag);
	assert(!ret);
	/* Replace the old node with the new recompacted one */
	rcu_assign_pointer(*old_node_flag, new_node_flag);
	/* TODO: free old_node (call_rcu) */
	return 0;
}

static
int ja_node_set_nth(struct rcu_ja_node_flag **node_flag, uint8_t n,
		struct rcu_ja_node_flag *child_node_flag)
{
	int ret;

	ret = _ja_node_set_nth(*node_flag, n, child_node_flag);
	if (ret == -ENOSPC) {
		/* Not enough space in node, need to recompact */
		ret = ja_node_recompact_add(node_flag, n,
				child_node_flag);
		if (ret < 0)
			return ret;
	}
	return ret;
}
