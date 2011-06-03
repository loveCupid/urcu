#ifndef URCU_RBTREE_H
#define URCU_RBTREE_H

/*
 * urcu-rbtree.h
 *
 * Userspace RCU library - Red-Black Tree
 *
 * Copyright (c) 2010 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
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
 * Implementation of RCU-adapted data structures and operations based on the RB
 * tree algorithms found in chapter 12 of:
 *
 * Thomas H. Cormen, Charles E. Leiserson, Ronald L. Rivest, and
 * Clifford Stein. Introduction to Algorithms, Third Edition. The MIT
 * Press, September 2009.
 */

#include <pthread.h>
#include <urcu-call-rcu.h>

#define COLOR_BLACK	0
#define COLOR_RED	1

#define IS_LEFT		0
#define IS_RIGHT	1

/*
 * Node key comparison function.
 * < 0 : a lower than b.
 * > 0 : a greater than b.
 * == 0 : a equals b.
 */
typedef int (*rcu_rbtree_comp)(void *a, void *b);

/*
 * Allocation and deletion functions.
 */
typedef void *(*rcu_rbtree_alloc)(size_t size);
typedef void (*rcu_rbtree_free)(void *ptr);

/*
 * struct rcu_rbtree_node must be aligned at least on 2 bytes.
 * Lowest bit reserved for position (left/right) in pointer to parent.
 *
 * Set "high" to key + 1 to insert single-value nodes.
 */
struct rcu_rbtree_node {
	/* internally reserved */
	void *begin;		/* Start of range (inclusive) */
	void *end;		/* range end (exclusive) */
	void *max_end;		/* max range end of node and children */
	/* parent uses low bit for "0 -> is left, 1 -> is right" */
	unsigned long parent;
	/* _left and _right must be updated with set_left(), set_right() */
	struct rcu_rbtree_node *_left, *_right;
	struct rcu_rbtree_node *decay_next;
	struct rcu_rbtree *rbtree;
	struct rcu_head head;		/* For delayed free */
	unsigned int color:1;
};

struct rcu_rbtree {
	struct rcu_rbtree_node *root;
	struct rcu_rbtree_node nil_node;
	rcu_rbtree_comp comp;
	rcu_rbtree_alloc rballoc;
	rcu_rbtree_free rbfree;
};

#define DEFINE_RCU_RBTREE(x, _comp, _rballoc, _rbfree)	\
	struct rcu_rbtree x =				\
		{					\
			.comp = _comp,			\
			.rballoc = _rballoc,		\
			.rbfree = _rbfree,		\
			.nil_node = {			\
				.color = COLOR_BLACK,	\
			},				\
			.root = &(x).nil_node,		\
		};

/*
 * Each of the search primitive and "prev"/"next" iteration must be called with
 * the RCU read-side lock held.
 *
 * Insertion and removal must be protected by a mutex. At the moment, insertion
 * and removal use defer_rcu, so calling them with rcu read-lock held is
 * prohibited.
 */

/*
 * Node insertion. Returns 0 on success. May fail with -ENOMEM.
 * Caller must have exclusive write access and hold RCU read-side lock.
 */
int rcu_rbtree_insert(struct rcu_rbtree *rbtree,
		      void *begin, void *end);

/*
 * Remove node from tree.
 * Must wait for a grace period after removal before performing deletion of the
 * node. Note: it is illegal to re-use the same node pointer passed to "insert"
 * also to "remove", because it may have been copied and garbage-collected since
 * the insertion. A "search" for the key in the tree should be done to get
 * "node".
 * Returns 0 on success. May fail with -ENOMEM.
 *
 * Caller must have exclusive write access and hold RCU read-side lock
 * across "search" and "remove".
 */
int rcu_rbtree_remove(struct rcu_rbtree *rbtree,
		      struct rcu_rbtree_node *node);

/* RCU read-side */

/*
 * For all these read-side privimitives, RCU read-side lock must be held
 * across the duration for which the search is done and the returned
 * rbtree node is expected to be valid.
 */

/*
 * Search point in range starting from node x (node x is typically the
 * rbtree root node). Returns nil node if not found.
 */
struct rcu_rbtree_node *rcu_rbtree_search(struct rcu_rbtree *rbtree,
					  struct rcu_rbtree_node *x,
					  void *point);

/*
 * Search range starting from node x (typically the rbtree root node).
 * Returns the first range containing the range specified as parameters.
 * Returns nil node if not found.
 *
 * Note: ranges in the rbtree should not partially overlap when this search
 * range function is used. Otherwise, a range matching the low value (but not
 * containing the high value) could hide a range that would match this query.
 * It is OK for the ranges to overlap entirely though.
 */
struct rcu_rbtree_node *rcu_rbtree_search_range(struct rcu_rbtree *rbtree,
					  struct rcu_rbtree_node *x,
					  void *begin, void *end);

/*
 * Search exact range begin value starting from node x (typically rbtree
 * root node). Returns nil node if not found.
 * This function is only useful if you do not use the range feature at
 * all and only care about range begin values.
 */
struct rcu_rbtree_node *rcu_rbtree_search_begin_key(struct rcu_rbtree *rbtree,
					  struct rcu_rbtree_node *x,
					  void *key);

/*
 * Search for minimum node of the tree under node x.
 */
struct rcu_rbtree_node *rcu_rbtree_min(struct rcu_rbtree *rbtree,
				       struct rcu_rbtree_node *x);

/*
 * Search for maximum node of the tree under node x.
 */
struct rcu_rbtree_node *rcu_rbtree_max(struct rcu_rbtree *rbtree,
				       struct rcu_rbtree_node *x);

/*
 * Get next node after node x.
 */
struct rcu_rbtree_node *rcu_rbtree_next(struct rcu_rbtree *rbtree,
					struct rcu_rbtree_node *x);

/*
 * Get previous node before node x.
 */
struct rcu_rbtree_node *rcu_rbtree_prev(struct rcu_rbtree *rbtree,
					struct rcu_rbtree_node *x);

/*
 * Sentinel (bottom nodes).
 * Don't care about p, left, right, pos and key values.
 */
static
int rcu_rbtree_is_nil(struct rcu_rbtree *rbtree, struct rcu_rbtree_node *node)
{
	return node == &rbtree->nil_node;
}

#endif /* URCU_RBTREE_H */
