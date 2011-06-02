/*
 * urcu-rbtree.c
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

#define _BSD_SOURCE
#define _LGPL_SOURCE

#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

#include <urcu/rcurbtree.h>
#include <urcu-pointer.h>
#include <urcu-call-rcu.h>
#include <urcu/compiler.h>

#ifdef DEBUG
#define dbg_printf(args...)	printf(args)
#define dbg_usleep(usecs)	usleep(usecs)
#else
#define dbg_printf(args...)
#define dbg_usleep(usecs)
#endif

/*
 * Undefine this to enable the non-RCU rotate and transplant functions
 * (for debugging).
 */
#define RBTREE_RCU_SUPPORT_ROTATE_LEFT
#define RBTREE_RCU_SUPPORT_ROTATE_RIGHT
#define RBTREE_RCU_SUPPORT_TRANSPLANT

#ifdef EXTRA_DEBUG
static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t outer_mutex = PTHREAD_MUTEX_INITIALIZER;

static
void lock_outer_mutex(void)
{
	pthread_mutex_lock(&outer_mutex);
}

static
void unlock_outer_mutex(void)
{
	pthread_mutex_unlock(&outer_mutex);
}

static
void lock_test_mutex(void)
{
	pthread_mutex_lock(&test_mutex);
}

static
void unlock_test_mutex(void)
{
	pthread_mutex_unlock(&test_mutex);
}
#endif

static
void set_parent(struct rcu_rbtree_node *node,
		struct rcu_rbtree_node *parent,
		unsigned int pos)
{
	_CMM_STORE_SHARED(node->parent, ((unsigned long) parent) | pos);
}

static
struct rcu_rbtree_node *get_parent(struct rcu_rbtree_node *node)
{
	return (struct rcu_rbtree_node *) (node->parent & ~1UL);
}

static
unsigned int get_pos(struct rcu_rbtree_node *node)
{
	return (unsigned int) (node->parent & 1UL);
}

static
struct rcu_rbtree_node *get_parent_and_pos(struct rcu_rbtree_node *node,
				unsigned int *pos)
{
	unsigned long parent_pos = rcu_dereference(node->parent);

	*pos = (unsigned int) (parent_pos & 1UL);
	return (struct rcu_rbtree_node *) (parent_pos & ~1UL);
}

static
void set_decay(struct rcu_rbtree_node *x, struct rcu_rbtree_node *xc)
{
	x->decay_next = xc;
}

static
struct rcu_rbtree_node *get_decay(struct rcu_rbtree_node *x)
{
	if (!x)
		return NULL;
	while (x->decay_next)
		x = x->decay_next;
	return x;
}

static
struct rcu_rbtree_node *is_decay(struct rcu_rbtree_node *x)
{
	return x->decay_next;
}

static
struct rcu_rbtree_node *dup_decay_node(struct rcu_rbtree *rbtree,
				struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xc;

	if (rcu_rbtree_is_nil(rbtree, x))
		return x;

	xc = rbtree->rballoc();
	memcpy(xc, x, sizeof(struct rcu_rbtree_node));
	xc->decay_next = NULL;
	set_decay(x, xc);
	call_rcu(&x->head, rbtree->rbfree);
	return xc;
}

/*
 * Info for range lookups:
 * Range lookup information is only valid when used when searching for
 * ranges. It should never be used in next/prev traversal because the
 * pointers to parents are not in sync with the parent vision of the
 * children range.
 */
static
void set_left(struct rcu_rbtree *rbtree, struct rcu_rbtree_node *node,
			struct rcu_rbtree_node *left)
{
	node->_left = left;
}

static
void set_right(struct rcu_rbtree *rbtree, struct rcu_rbtree_node *node,
			struct rcu_rbtree_node *right)
{
	node->_right = right;
}

/*
 * TODO
 * Deal with memory allocation errors.
 * Can be ensured by reserving a pool of memory entries before doing the
 * insertion, which will have to be function of number of
 * transplantations/rotations required for the operation.
 */

#ifdef DEBUG
static
void show_tree(struct rcu_rbtree *rbtree)
{
	struct rcu_rbtree_node *node;

	node = rcu_rbtree_min(rbtree, rbtree->root);
	while (!rcu_rbtree_is_nil(rbtree, node)) {
		assert(!is_decay(node));
		printf("{ b:%lX e:%lX pb: %lX r:%lX l:%lX %s %s %s} ",
			(unsigned long) node->begin,
			(unsigned long) node->end,
			(unsigned long) get_parent(node)->begin,
			(unsigned long) node->_right->begin,
			(unsigned long) node->_left->begin,
			node->color ? "red" : "black",
			get_pos(node) ? "right" : "left",
			rcu_rbtree_is_nil(rbtree, node) ? "nil" : "");
		node = rcu_rbtree_next(rbtree, node);
	}
	printf("\n");
}
#else /* DEBUG */
static
void show_tree(struct rcu_rbtree *rbtree)
{
}
#endif /* DEBUG */

static
struct rcu_rbtree_node *make_nil(struct rcu_rbtree *rbtree)
{
	return &rbtree->nil_node;
}

/*
 * Iterative rbtree search.
 */
struct rcu_rbtree_node *rcu_rbtree_search(struct rcu_rbtree *rbtree,
					  struct rcu_rbtree_node *x,
					  void *k)
{
	x = rcu_dereference(x);
	int comp;

	while (!rcu_rbtree_is_nil(rbtree, x) && (comp = rbtree->comp(k, x->begin)) != 0) {
		dbg_usleep(10);
		if (comp < 0)
			x = rcu_dereference(x->_left);
		else
			x = rcu_dereference(x->_right);
	}
	return x;
}

static
struct rcu_rbtree_node *rcu_rbtree_min_dup_decay(struct rcu_rbtree *rbtree,
						 struct rcu_rbtree_node *x,
						 struct rcu_rbtree_node **zr)
{
	struct rcu_rbtree_node *xl;

	x = rcu_dereference(x);

	if (rcu_rbtree_is_nil(rbtree, x)) {
		*zr = x;
		return x;
	} else
		*zr = x = dup_decay_node(rbtree, x);

	while (!rcu_rbtree_is_nil(rbtree, xl = rcu_dereference(x->_left))) {
		x = dup_decay_node(rbtree, xl);
		set_parent(x, get_decay(get_parent(x)), get_pos(x));
		get_parent(x)->_left = get_decay(get_parent(x)->_left);
	}
	return x;
}

static
struct rcu_rbtree_node *rcu_rbtree_min_update_decay(struct rcu_rbtree *rbtree,
						    struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xl;

	x = rcu_dereference(x);

	if (rcu_rbtree_is_nil(rbtree, x))
		return x;
	else {
		set_parent(x->_right, get_decay(get_parent(x->_right)),
			   get_pos(x->_right));
		set_parent(x->_left, get_decay(get_parent(x->_left)),
			   get_pos(x->_left));
	}

	while (!rcu_rbtree_is_nil(rbtree, xl = rcu_dereference(x->_left))) {
		x = xl;
		set_parent(x->_right, get_decay(get_parent(x->_right)),
			   get_pos(x->_right));
		set_parent(x->_left, get_decay(get_parent(x->_left)),
			   get_pos(x->_left));
	}
	return x;
}

struct rcu_rbtree_node *rcu_rbtree_min(struct rcu_rbtree *rbtree,
				       struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xl;

	x = rcu_dereference(x);

	if (rcu_rbtree_is_nil(rbtree, x))
		return x;

	while (!rcu_rbtree_is_nil(rbtree, xl = rcu_dereference(x->_left)))
		x = xl;
	return x;
}

struct rcu_rbtree_node *rcu_rbtree_max(struct rcu_rbtree *rbtree,
				       struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xr;

	x = rcu_dereference(x);

	if (rcu_rbtree_is_nil(rbtree, x))
		return x;

	while (!rcu_rbtree_is_nil(rbtree, xr = rcu_dereference(x->_right)))
		x = xr;
	return x;
}

/*
 * FIXME:
 * Updates should wait for a grace period between update of the
 * redirect pointer and update of the parent child pointer. This is to make sure
 * that no reference to the old entry exist.
 */

/*
 * RCU read lock must be held across the next/prev calls to ensure validity of
 * the returned node.
 */
struct rcu_rbtree_node *rcu_rbtree_next(struct rcu_rbtree *rbtree,
					struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xr, *y;
	unsigned int x_pos;

	x = rcu_dereference(x);

	if (!rcu_rbtree_is_nil(rbtree, xr = rcu_dereference(x->_right)))
		return rcu_rbtree_min(rbtree, xr);
	y = get_parent_and_pos(x, &x_pos);
	while (!rcu_rbtree_is_nil(rbtree, y) && x_pos == IS_RIGHT) {
		x = y;
		y = get_parent_and_pos(y, &x_pos);
	}
	return y;
}

struct rcu_rbtree_node *rcu_rbtree_prev(struct rcu_rbtree *rbtree,
					struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xl, *y;
	unsigned int x_pos;

	x = rcu_dereference(x);

	if (!rcu_rbtree_is_nil(rbtree, xl = rcu_dereference(x->_left)))
		return rcu_rbtree_max(rbtree, xl);
	y = get_parent_and_pos(x, &x_pos);
	while (!rcu_rbtree_is_nil(rbtree, y) && x_pos == IS_LEFT) {
		x = y;
		y = get_parent_and_pos(y, &x_pos);
	}
	return y;
}

static
void *calculate_node_max_end(struct rcu_rbtree *rbtree, struct rcu_rbtree_node *node)
{
	void *max_end;

	max_end = node->end;
	if (!rcu_rbtree_is_nil(rbtree, node->_right)) {
		if (rbtree->comp(max_end, node->_right->max_end) < 0)
			max_end = node->_right->max_end;
	}
	if (!rcu_rbtree_is_nil(rbtree, node->_left)) {
		if (rbtree->comp(max_end, node->_left->max_end) < 0)
			max_end = node->_left->max_end;
	}
	return max_end;
}

/*
 * "node" needs to be non-visible by readers.
 */
static
void populate_node_end(struct rcu_rbtree *rbtree, struct rcu_rbtree_node *node,
		unsigned int copy_parents, struct rcu_rbtree_node *stop)
{
	struct rcu_rbtree_node *prev = NULL, *orig_node = node, *top;

	do {
		void *max_end;

		assert(node);
		assert(!rcu_rbtree_is_nil(rbtree, node));

		max_end = calculate_node_max_end(rbtree, node);
		if (rbtree->comp(max_end, node->max_end) != 0) {
			if (prev && copy_parents) {
				node = dup_decay_node(rbtree, node);
				if (get_pos(prev) == IS_RIGHT)
					node->_right = prev;
				else
					node->_left = prev;
				set_parent(prev, node, get_pos(prev));
			}
			node->max_end = max_end;
		} else {
			/*
			 * We did not have to change the current node,
			 * so set the pointer to the prev node. If prev
			 * was null, this means we are coming in the
			 * first loop, and must update the parent of the
			 * node received as parameter (node).
			 */
			if (prev)
				node = prev;
			top = get_parent(node);
			cmm_smp_wmb();	/* write into node before publish */
			/* make new branch visible to readers */
			if (rcu_rbtree_is_nil(rbtree, top))
				_CMM_STORE_SHARED(rbtree->root, node);
			if (get_pos(node) == IS_LEFT)
				_CMM_STORE_SHARED(top->_left, node);
			else
				_CMM_STORE_SHARED(top->_right, node);
			goto end;
		}

		/* Check for propagation stop */
		if (node == stop)
			return;

		prev = node;
		node = get_parent(node);
	} while (!rcu_rbtree_is_nil(rbtree, node));

	top = node;	/* nil */
	cmm_smp_wmb();	/* write into node before publish */
	/* make new branch visible to readers */
	_CMM_STORE_SHARED(rbtree->root, prev);

end:
	if (!copy_parents)
		return;
	/* update children */
	node = orig_node;
	do {
		assert(!rcu_rbtree_is_nil(rbtree, node));
		set_parent(node->_left, get_decay(get_parent(node->_left)), IS_LEFT);
		set_parent(node->_right, get_decay(get_parent(node->_right)), IS_RIGHT);
	} while ((node = get_parent(node)) != top);
}

/*
 * We have to ensure these assumptions are correct for prev/next
 * traversal:
 *
 * with x being a right child, the assumption that:
 *   get_parent(x)->_right == x
 * or if x is a left child, the assumption that:
 *   get_parent(x)->_left == x
 *
 * This explains why we have to allocate a vc copy of the node for left_rotate,
 * right_rotate and transplant operations.
 *
 * We always ensure that the right/left child and correct parent is set in the
 * node copies *before* we reparent the children and make the upper-level point
 * to the copy.
 */

/* RCU: copy x and y, atomically point to new versions. GC old. */
/* Should be eventually followed by a cmm_smp_wmc() */

#ifdef RBTREE_RCU_SUPPORT_ROTATE_LEFT

static
void left_rotate(struct rcu_rbtree *rbtree,
		 struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y, *y_left;

	y = x->_right;
	y_left = y->_left;

	/* Now operate on new copy, decay old versions */
	x = dup_decay_node(rbtree, x);
	y = dup_decay_node(rbtree, y);
	y_left = dup_decay_node(rbtree, y_left);

	/* Internal node modifications */
	set_parent(y, get_parent(x), get_pos(x));
	set_parent(x, y, IS_LEFT);
	set_left(rbtree, y, x);
	set_right(rbtree, x, y_left);

	if (!rcu_rbtree_is_nil(rbtree, y_left))
		set_parent(y_left, x, IS_RIGHT);

	/*
	 * We only changed the relative position of x and y wrt their
	 * children, and reparented y (but are keeping the same nodes in
	 * place, so its parent does not need to have end value
	 * recalculated).
	 */
	x->max_end = calculate_node_max_end(rbtree, x);
	y->max_end = calculate_node_max_end(rbtree, y);

	cmm_smp_wmb();	/* write into node before publish */

	/* External references update (visible by readers) */
	if (rcu_rbtree_is_nil(rbtree, get_parent(y)))
		_CMM_STORE_SHARED(rbtree->root, y);
	else if (get_pos(y) == IS_LEFT)
		_CMM_STORE_SHARED(get_parent(y)->_left, y);
	else
		_CMM_STORE_SHARED(get_parent(y)->_right, y);

	/* Point children to new copy (parent only used by updates/next/prev) */
	set_parent(x->_left, get_decay(get_parent(x->_left)),
		get_pos(x->_left));
	set_parent(y->_right, get_decay(get_parent(y->_right)),
		get_pos(y->_right));
	if (!rcu_rbtree_is_nil(rbtree, y_left)) {
		set_parent(y_left->_right,
			get_decay(get_parent(y_left->_right)),
			get_pos(y_left->_right));
		set_parent(y_left->_left,
			get_decay(get_parent(y_left->_left)),
			get_pos(y_left->_left));
	}

	/* Sanity checks */
	assert(y == rbtree->root || get_parent(y)->_left == y
		|| get_parent(y)->_right == y);
	assert(x == rbtree->root || get_parent(x)->_left == x
		|| get_parent(x)->_right == x);
	assert(rcu_rbtree_is_nil(rbtree, x->_right) || get_parent(x->_right) == x);
	assert(rcu_rbtree_is_nil(rbtree, x->_left) || get_parent(x->_left) == x);
	assert(rcu_rbtree_is_nil(rbtree, y->_right) || get_parent(y->_right) == y);
	assert(rcu_rbtree_is_nil(rbtree, y->_left) || get_parent(y->_left) == y);
	assert(!is_decay(rbtree->root));
	assert(!is_decay(x));
	assert(!is_decay(y));
	assert(!is_decay(x->_right));
	assert(!is_decay(x->_left));
	assert(!is_decay(y->_right));
	assert(!is_decay(y->_left));
}

#else

/* non-rcu version */
static
void left_rotate(struct rcu_rbtree *rbtree,
		 struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y;

	lock_test_mutex();
	y = x->_right;
	x->_right = y->_left;
	if (!rcu_rbtree_is_nil(rbtree, y->_left))
		set_parent(y->_left, x, IS_RIGHT);
	set_parent(y, get_parent(x), get_pos(x));
	if (rcu_rbtree_is_nil(rbtree, get_parent(x)))
		rbtree->root = y;
	else if (x == get_parent(x)->_left) {
		get_parent(x)->_left = y;
	} else {
		get_parent(x)->_right = y;
	}
	y->_left = x;
	set_parent(x, y, IS_LEFT);
	unlock_test_mutex();
}

#endif

#ifdef RBTREE_RCU_SUPPORT_ROTATE_RIGHT
static
void right_rotate(struct rcu_rbtree *rbtree,
		  struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y, *y_right;

	y = x->_left;
	y_right = y->_right;

	/* Now operate on new copy, decay old versions */
	x = dup_decay_node(rbtree, x);
	y = dup_decay_node(rbtree, y);
	y_right = dup_decay_node(rbtree, y_right);

	/* Internal node modifications */
	set_parent(y, get_parent(x), get_pos(x));
	set_parent(x, y, IS_RIGHT);
	set_right(rbtree, y, x);
	set_left(rbtree, x, y_right);

	if (!rcu_rbtree_is_nil(rbtree, y_right))
		set_parent(y_right, x, IS_LEFT);

	/*
	 * We only changed the relative position of x and y wrt their
	 * children, and reparented y (but are keeping the same nodes in
	 * place, so its parent does not need to have end value
	 * recalculated).
	 */
	x->max_end = calculate_node_max_end(rbtree, x);
	y->max_end = calculate_node_max_end(rbtree, y);

	cmm_smp_wmb();	/* write into node before publish */

	/* External references update (visible by readers) */
	if (rcu_rbtree_is_nil(rbtree, get_parent(y)))
		_CMM_STORE_SHARED(rbtree->root, y);
	else if (get_pos(y) == IS_RIGHT)
		_CMM_STORE_SHARED(get_parent(y)->_right, y);
	else
		_CMM_STORE_SHARED(get_parent(y)->_left, y);

	/* Point children to new copy (parent only used by updates/next/prev) */
	set_parent(x->_right, get_decay(get_parent(x->_right)),
		get_pos(x->_right));
	set_parent(y->_left, get_decay(get_parent(y->_left)),
		get_pos(y->_left));
	if (!rcu_rbtree_is_nil(rbtree, y_right)) {
		set_parent(y_right->_left,
			get_decay(get_parent(y_right->_left)),
			get_pos(y_right->_left));
		set_parent(y_right->_right,
			get_decay(get_parent(y_right->_right)),
			get_pos(y_right->_right));
	}

	/* Sanity checks */
	assert(y == rbtree->root || get_parent(y)->_right == y
		|| get_parent(y)->_left == y);
	assert(x == rbtree->root || get_parent(x)->_right == x
		|| get_parent(x)->_left == x);
	assert(rcu_rbtree_is_nil(rbtree, x->_left) || get_parent(x->_left) == x);
	assert(rcu_rbtree_is_nil(rbtree, x->_right) || get_parent(x->_right) == x);
	assert(rcu_rbtree_is_nil(rbtree, y->_left) || get_parent(y->_left) == y);
	assert(rcu_rbtree_is_nil(rbtree, y->_right) || get_parent(y->_right) == y);
	assert(!is_decay(rbtree->root));
	assert(!is_decay(x));
	assert(!is_decay(y));
	assert(!is_decay(x->_left));
	assert(!is_decay(x->_right));
	assert(!is_decay(y->_left));
	assert(!is_decay(y->_right));
}

#else

/* non-rcu version */
static
void right_rotate(struct rcu_rbtree *rbtree,
		  struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y;

	lock_test_mutex();
	y = x->_left;
	x->_left = y->_right;
	if (!rcu_rbtree_is_nil(rbtree, y->_right))
		set_parent(y->_right, x, IS_LEFT);
	set_parent(y, get_parent(x), get_pos(x));
	if (rcu_rbtree_is_nil(rbtree, get_parent(x)))
		rbtree->root = y;
	else if (x == get_parent(x)->_right) {
		get_parent(x)->_right = y;
	} else {
		get_parent(x)->_left = y;
	}
	y->_right = x;
	set_parent(x, y, IS_RIGHT);
	unlock_test_mutex();
}

#endif

static void rcu_rbtree_insert_fixup(struct rcu_rbtree *rbtree,
				    struct rcu_rbtree_node *z)
{
	struct rcu_rbtree_node *y;

	dbg_printf("insert fixup %p\n", z->begin);
	assert(!is_decay(rbtree->root));

	while (get_parent(z)->color == COLOR_RED) {
		if (get_parent(z) == get_parent(get_parent(z))->_left) {
			y = get_parent(get_parent(z))->_right;
			if (y->color == COLOR_RED) {
				get_parent(z)->color = COLOR_BLACK;
				y->color = COLOR_BLACK;
				get_parent(get_parent(z))->color = COLOR_RED;
				z = get_parent(get_parent(z));
			} else {
				if (z == get_parent(z)->_right) {
					z = get_parent(z);
					left_rotate(rbtree, z);
					z = get_decay(z);
					assert(!is_decay(rbtree->root));
				}
				get_parent(z)->color = COLOR_BLACK;
				get_parent(get_parent(z))->color = COLOR_RED;
				assert(!is_decay(z));
				assert(!is_decay(get_parent(z)));
				assert(!is_decay(get_parent(get_parent(z))));
				right_rotate(rbtree, get_parent(get_parent(z)));
				assert(!is_decay(z));
				assert(!is_decay(rbtree->root));
			}
		} else {
			y = get_parent(get_parent(z))->_left;
			if (y->color == COLOR_RED) {
				get_parent(z)->color = COLOR_BLACK;
				y->color = COLOR_BLACK;
				get_parent(get_parent(z))->color = COLOR_RED;
				z = get_parent(get_parent(z));
			} else {
				if (z == get_parent(z)->_left) {
					z = get_parent(z);
					right_rotate(rbtree, z);
					z = get_decay(z);
					assert(!is_decay(rbtree->root));
				}
				get_parent(z)->color = COLOR_BLACK;
				get_parent(get_parent(z))->color = COLOR_RED;
				left_rotate(rbtree, get_parent(get_parent(z)));
				assert(!is_decay(z));
				assert(!is_decay(rbtree->root));
			}
		}
	}
	rbtree->root->color = COLOR_BLACK;
}

/*
 * rcu_rbtree_insert - Insert a node in the RCU rbtree
 *
 * Returns 0 on success, or < 0 on error.
 */
int rcu_rbtree_insert(struct rcu_rbtree *rbtree,
		      struct rcu_rbtree_node *z)
{
	struct rcu_rbtree_node *x, *y;

	dbg_printf("insert %p\n", z->begin);
	assert(!is_decay(rbtree->root));

	y = make_nil(rbtree);
	x = rbtree->root;
	while (!rcu_rbtree_is_nil(rbtree, x)) {
		y = x;
		if (rbtree->comp(z->begin, x->begin) < 0)
			x = x->_left;
		else
			x = x->_right;
	}

	z->_left = make_nil(rbtree);
	z->_right = make_nil(rbtree);
	z->color = COLOR_RED;
	z->decay_next = NULL;
	z->max_end = z->end;

	if (rcu_rbtree_is_nil(rbtree, y)) {
		set_parent(z, y, IS_RIGHT); /* pos arbitrary for root node */
		/*
		 * Order stores to z (children/parents) before stores
		 * that will make it visible to the rest of the tree.
		 */
		cmm_smp_wmb();
		_CMM_STORE_SHARED(rbtree->root, z);
	} else if (rbtree->comp(z->begin, y->begin) < 0) {
		y = dup_decay_node(rbtree, y);
		set_parent(z, y, IS_LEFT);
		if (get_pos(z) == IS_LEFT)
			_CMM_STORE_SHARED(y->_left, z);
		else
			_CMM_STORE_SHARED(y->_right, z);
		populate_node_end(rbtree, y, 1, NULL);
	} else {
		y = dup_decay_node(rbtree, y);
		set_parent(z, y, IS_RIGHT);
		if (get_pos(z) == IS_LEFT)
			_CMM_STORE_SHARED(y->_left, z);
		else
			_CMM_STORE_SHARED(y->_right, z);
		populate_node_end(rbtree, y, 1, NULL);
	}
	rcu_rbtree_insert_fixup(rbtree, z);
	/*
	 * Make sure to commit all _CMM_STORE_SHARED() for non-coherent caches.
	 */
	cmm_smp_wmc();
	show_tree(rbtree);

	return 0;
}

/*
 * Transplant v into u position.
 */

#ifdef RBTREE_RCU_SUPPORT_TRANSPLANT

static
void rcu_rbtree_transplant(struct rcu_rbtree *rbtree,
			struct rcu_rbtree_node *u,
			struct rcu_rbtree_node *v,
			unsigned int copy_parents,
			struct rcu_rbtree_node *stop)
{
	dbg_printf("transplant %p\n", v->begin);

	if (!rcu_rbtree_is_nil(rbtree, v))
		v = dup_decay_node(rbtree, v);

	if (rcu_rbtree_is_nil(rbtree, get_parent(u))) {
		/* pos is arbitrary for root node */
		set_parent(v, get_parent(u), IS_RIGHT);
		cmm_smp_wmb();	/* write into node before publish */
		_CMM_STORE_SHARED(rbtree->root, v);
	} else {
		struct rcu_rbtree_node *vp;

		vp = get_parent(u);
		if (copy_parents)
			vp = dup_decay_node(rbtree, vp);
		set_parent(v, vp, get_pos(u));
		if (get_pos(v) == IS_LEFT)
			_CMM_STORE_SHARED(vp->_left, v);
		else
			_CMM_STORE_SHARED(vp->_right, v);
		populate_node_end(rbtree, vp, copy_parents, stop);
	}

	/* Point children to new copy (parent only used by updates/next/prev) */
	if (!rcu_rbtree_is_nil(rbtree, v)) {
		set_parent(v->_right, get_decay(get_parent(v->_right)),
			get_pos(v->_right));
		set_parent(v->_left, get_decay(get_parent(v->_left)),
			get_pos(v->_left));
	}
	assert(!is_decay(rbtree->root));
}

#else

/* Non-RCU version */
static
void rcu_rbtree_transplant(struct rcu_rbtree *rbtree,
			   struct rcu_rbtree_node *u,
			   struct rcu_rbtree_node *v,
			   unsigned int copy_parents,
			   struct rcu_rbtree_node *stop)
{
	dbg_printf("transplant %p\n", v->begin);

	lock_test_mutex();
	if (rcu_rbtree_is_nil(rbtree, get_parent(u)))
		rbtree->root = v;
	else if (u == get_parent(u)->_left)
		get_parent(u)->_left = v;
	else
		get_parent(u)->_right = v;
	set_parent(v, get_parent(u), get_pos(u));
	unlock_test_mutex();
}

#endif

static void rcu_rbtree_remove_fixup(struct rcu_rbtree *rbtree,
				    struct rcu_rbtree_node *x)
{
	dbg_printf("remove fixup %p\n", x->begin);

	while (x != rbtree->root && x->color == COLOR_BLACK) {
		assert(!is_decay(get_parent(x)));
		assert(!is_decay(get_parent(x)->_left));
		if (x == get_parent(x)->_left) {
			struct rcu_rbtree_node *w;

			w = get_parent(x)->_right;

			if (w->color == COLOR_RED) {
				w->color = COLOR_BLACK;
				get_parent(x)->color = COLOR_RED;
				left_rotate(rbtree, get_parent(x));
				x = get_decay(x);
				assert(!is_decay(rbtree->root));
				w = get_parent(x)->_right;
			}
			if (w->_left->color == COLOR_BLACK
			    && w->_right->color == COLOR_BLACK) {
				w->color = COLOR_RED;
				x = get_parent(x);
				assert(!is_decay(rbtree->root));
				assert(!is_decay(x));
			} else {
				if (w->_right->color == COLOR_BLACK) {
					w->_left->color = COLOR_BLACK;
					w->color = COLOR_RED;
					right_rotate(rbtree, w);
					assert(!is_decay(rbtree->root));
					x = get_decay(x);
					w = get_parent(x)->_right;
				}
				w->color = get_parent(x)->color;
				get_parent(x)->color = COLOR_BLACK;
				w->_right->color = COLOR_BLACK;
				left_rotate(rbtree, get_parent(x));
				assert(!is_decay(rbtree->root));
				x = rbtree->root;
			}
		} else {
			struct rcu_rbtree_node *w;

			w = get_parent(x)->_left;

			if (w->color == COLOR_RED) {
				w->color = COLOR_BLACK;
				get_parent(x)->color = COLOR_RED;
				right_rotate(rbtree, get_parent(x));
				assert(!is_decay(rbtree->root));
				x = get_decay(x);
				w = get_parent(x)->_left;
			}
			if (w->_right->color == COLOR_BLACK
			    && w->_left->color == COLOR_BLACK) {
				w->color = COLOR_RED;
				x = get_parent(x);
				assert(!is_decay(rbtree->root));
				assert(!is_decay(x));
			} else {
				if (w->_left->color == COLOR_BLACK) {
					w->_right->color = COLOR_BLACK;
					w->color = COLOR_RED;
					left_rotate(rbtree, w);
					assert(!is_decay(rbtree->root));
					x = get_decay(x);
					w = get_parent(x)->_left;
				}
				w->color = get_parent(x)->color;
				get_parent(x)->color = COLOR_BLACK;
				w->_left->color = COLOR_BLACK;
				right_rotate(rbtree, get_parent(x));
				assert(!is_decay(rbtree->root));
				x = rbtree->root;
			}
		}
	}
	x->color = COLOR_BLACK;
}

/*
 * Delete z. All non-copied children left/right positions are unchanged.
 */
static
void rcu_rbtree_remove_nonil(struct rcu_rbtree *rbtree,
			     struct rcu_rbtree_node *z,
			     struct rcu_rbtree_node *y)
{
	struct rcu_rbtree_node *x;

	dbg_printf("remove nonil %p\n", z->begin);
	show_tree(rbtree);

	assert(!is_decay(z));
	assert(!is_decay(y));
	assert(!is_decay(y->_right));
	assert(!is_decay(get_parent(y)));
	x = y->_right;
	assert(!is_decay(x));
	if (get_parent(y) == z) {
		y = dup_decay_node(rbtree, y);
		set_parent(x, y, get_pos(x));	/* parent for nil */
		/* y is z's right node: set left will just update y */
		set_left(rbtree, y, z->_left);
		rcu_rbtree_transplant(rbtree, z, y, 1, NULL);
	} else {
		struct rcu_rbtree_node *oy_right, *z_right;

		/*
		 * Need to make sure y is always visible by readers.
		 */
		y = rcu_rbtree_min_dup_decay(rbtree, z->_right, &z_right);
		assert(!is_decay(y));
		assert(!is_decay(z));
		oy_right = y->_right;

		/*
		 * The max child begin of z_right does not change, because
		 * we're only changing its left children.
		 */
		y->_right = z_right;
		set_parent(y->_right, y, IS_RIGHT);
		assert(!is_decay(z->_left));
		y->_left = z->_left;
		assert(!is_decay(oy_right));
		/*
		 * Transplant of oy_right to old y's location will only
		 * trigger a "end" value update of the already copied branch
		 * (which is not visible yet). We are transplanting
		 * oy_right as a left child of old y's parent, so the
		 * min values update propagated upward necessarily stops
		 * at z_right.
		 */
		rcu_rbtree_transplant(rbtree, y, oy_right, 0, y);
		rcu_rbtree_transplant(rbtree, z, y, 1, NULL);
		/* Update children */
		(void) rcu_rbtree_min_update_decay(rbtree, y->_right);
	}
	y = get_decay(y);
	assert(!is_decay(z));
	assert(!is_decay(z->_left));
	y->color = z->color;
	set_parent(y->_left, y, IS_LEFT);
	set_parent(y->_right, get_decay(get_parent(y->_right)), IS_RIGHT);
	assert(!is_decay(y->_left));
	assert(!is_decay(y->_right));
}

int rcu_rbtree_remove(struct rcu_rbtree *rbtree,
		      struct rcu_rbtree_node *z)
{
	struct rcu_rbtree_node *x, *y;
	unsigned int y_original_color;

	assert(!is_decay(rbtree->root));
	dbg_printf("remove %p\n", z->begin);
	show_tree(rbtree);

	assert(!is_decay(z));
	y = z;
	y_original_color = y->color;

	if (rcu_rbtree_is_nil(rbtree, z->_left)) {
		rcu_rbtree_transplant(rbtree, z, z->_right, 1, NULL);
		assert(!is_decay(z));
		x = get_decay(z->_right);
		show_tree(rbtree);
	} else if (rcu_rbtree_is_nil(rbtree, z->_right)) {
		rcu_rbtree_transplant(rbtree, z, z->_left, 1, NULL);
		assert(!is_decay(z));
		x = get_decay(z->_left);
		show_tree(rbtree);
	} else {
		y = rcu_rbtree_min(rbtree, z->_right);
		assert(!is_decay(y));
		y_original_color = y->color;
		x = y->_right;
		rcu_rbtree_remove_nonil(rbtree, z, y);
		x = get_decay(x);
		show_tree(rbtree);
	}
	if (y_original_color == COLOR_BLACK)
		rcu_rbtree_remove_fixup(rbtree, x);
	show_tree(rbtree);
	/*
	 * Commit all _CMM_STORE_SHARED().
	 */
	cmm_smp_wmc();

	return 0;
}
