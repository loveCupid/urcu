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

#define COLOR_BLACK	0
#define COLOR_RED	1

/*
 * Node key comparison function.
 * < 0 : a lower than b.
 * > 0 : a greater than b.
 * == 0 : a equals b.
 */
typedef int (*rcu_rbtree_comp)(void *a, void *b);

/*
 * Node allocation and deletion functions.
 */
typedef struct rcu_rbtree_node *(*rcu_rbtree_alloc)(void);
typedef void (*rcu_rbtree_free)(struct rcu_rbtree_node *node);

struct rcu_rbtree_node;

struct rcu_rbtree_node {
	/* must be set upon insertion */
	void *key;

	/* internally reserved */
	struct rcu_rbtree_node *p, *left, *right;
	unsigned int color:1;
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
 */
int rcu_rbtree_insert(struct rcu_rbtree_node **root,
		      struct rcu_rbtree_node *node,
		      rcu_rbtree_comp comp,
		      rcu_rbtree_alloc rballoc,
		      rcu_rbtree_free rbfree);

/*
 * Remove node from tree.
 * Must wait for a grace period after removal before performing deletion of the
 * node.
 * Returns 0 on success. May fail with -ENOMEM.
 */
int rcu_rbtree_remove(struct rcu_rbtree_node **root,
		      struct rcu_rbtree_node *node,
		      rcu_rbtree_comp comp,
		      rcu_rbtree_alloc rballoc,
		      rcu_rbtree_free rbfree);

/* RCU read-side */

/*
 * Search key starting from node x. Returns NULL if not found.
 */
struct rcu_rbtree_node* rcu_rbtree_search(struct rcu_rbtree_node *x,
					  void *key, rcu_rbtree_comp comp);

struct rcu_rbtree_node *rcu_rbtree_min(struct rcu_rbtree_node *x,
				       rcu_rbtree_comp comp);

struct rcu_rbtree_node *rcu_rbtree_max(struct rcu_rbtree_node *x,
				       rcu_rbtree_comp comp);

struct rcu_rbtree_node *rcu_rbtree_next(struct rcu_rbtree_node *x,
					rcu_rbtree_comp comp);

struct rcu_rbtree_node *rcu_rbtree_prev(struct rcu_rbtree_node *x,
					rcu_rbtree_comp comp);

#endif /* URCU_RBTREE_H */
