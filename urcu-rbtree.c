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
#else
#define dbg_printf(args...)
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

	if (rcu_rbtree_is_nil(x))
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
void _set_left_dup_decay(struct rcu_rbtree *rbtree,
			struct rcu_rbtree_node *node,
			struct rcu_rbtree_node *left,
			struct rcu_rbtree_node **top,
			struct rcu_rbtree_node **top_child,
			unsigned int *top_child_pos, int copy)
{
	struct rcu_rbtree_node *first_node = node;	/* already a copy */

	node->_left = left;
	do {
		void *min_child_key;

		if (rcu_rbtree_is_nil(left)) {
			min_child_key = node->key;
		} else {
			min_child_key = left->min_child_key;
			assert(rbtree->comp(left->key, left->min_child_key) >= 0);
			assert(rbtree->comp(node->key, left->min_child_key) >= 0);
		}
		if (min_child_key != node->min_child_key) {
			if (node != first_node) {
				if (copy) {
					node = dup_decay_node(rbtree, node);
					node->_left = left;
					set_parent(left, node, IS_LEFT);
				}
			}
			node->min_child_key = min_child_key;
		} else {
			if (node != first_node) {
				if (top)
					*top = node;
				if (top_child)
					*top_child = left;
				if (top_child_pos)
					*top_child_pos = get_pos(left);
			} else {
				if (top)
					*top = get_parent(node);
				if (top_child)
					*top_child = node;
				if (top_child_pos)
					*top_child_pos = get_pos(node);
			}
			return;
		}
		left = node;
	} while (get_pos(node) == IS_LEFT
		 && !rcu_rbtree_is_nil(node = get_parent(node)));

	if (rcu_rbtree_is_nil(node)) {
		if (top)
			*top = node;
		if (top_child)
			*top_child = left;
		if (top_child_pos)
			*top_child_pos = IS_LEFT;	/* arbitrary */
	} else {
		assert(get_pos(node) == IS_RIGHT);
		if (top)
			*top = get_parent(node);
		if (top_child)
			*top_child = node;
		if (top_child_pos)
			*top_child_pos = IS_RIGHT;
	}
}

static
void set_left_dup_decay(struct rcu_rbtree *rbtree,
			struct rcu_rbtree_node *node,
			struct rcu_rbtree_node *left,
			struct rcu_rbtree_node **top,
			struct rcu_rbtree_node **top_child,
			unsigned int *top_child_pos)
{
	_set_left_dup_decay(rbtree, node, left, top, top_child,
			    top_child_pos, 1);
}

static
void set_left_update_decay(struct rcu_rbtree *rbtree, struct rcu_rbtree_node *node,
	      struct rcu_rbtree_node *left)
{
	struct rcu_rbtree_node *first_node = node;	/* already a copy */

	do {
		if (node != first_node) {
			set_parent(node->_right,
				get_decay(get_parent(node->_right)), IS_RIGHT);
		}
	} while (get_pos(node) == IS_LEFT
		 && !rcu_rbtree_is_nil(node = get_parent(node)));
}

static
void set_right_dup_decay(struct rcu_rbtree *rbtree, struct rcu_rbtree_node *node,
			struct rcu_rbtree_node *right,
			struct rcu_rbtree_node **top,
			struct rcu_rbtree_node **top_child,
			unsigned int *top_child_pos)
{
	struct rcu_rbtree_node *first_node = node;	/* already a copy */

	node->_right = right;
	do {
		void *max_child_key;

		if (rcu_rbtree_is_nil(right)) {
			max_child_key = node->key;
		} else {
			max_child_key = right->max_child_key;
			assert(rbtree->comp(right->key, right->max_child_key) <= 0);
			assert(rbtree->comp(node->key, right->max_child_key) <= 0);
		}
		if (max_child_key != node->max_child_key) {
			if (node != first_node) {
				node = dup_decay_node(rbtree, node);
				node->_right = right;
				set_parent(right, node, IS_RIGHT);
			}
			node->max_child_key = max_child_key;
		} else {
			if (node != first_node) {
				if (top)
					*top = node;
				if (top_child)
					*top_child = right;
				if (top_child_pos)
					*top_child_pos = get_pos(right);
			} else {
				if (top)
					*top = get_parent(node);
				if (top_child)
					*top_child = node;
				if (top_child_pos)
					*top_child_pos = get_pos(node);
			}
			return;
		}
		right = node;
	} while (get_pos(node) == IS_RIGHT
		 && !rcu_rbtree_is_nil(node = get_parent(node)));

	if (rcu_rbtree_is_nil(node)) {
		if (top)
			*top = node;
		if (top_child)
			*top_child = right;
		if (top_child_pos)
			*top_child_pos = IS_RIGHT;	/* arbitrary */
	} else {
		assert(get_pos(node) == IS_LEFT);
		if (top)
			*top = get_parent(node);
		if (top_child)
			*top_child = node;
		if (top_child_pos)
			*top_child_pos = IS_LEFT;
	}
}

static
void set_right_update_decay(struct rcu_rbtree *rbtree, struct rcu_rbtree_node *node,
	      struct rcu_rbtree_node *right)
{
	struct rcu_rbtree_node *first_node = node;	/* already a copy */

	do {
		if (node != first_node) {
			set_parent(node->_left,
				get_decay(get_parent(node->_left)), IS_LEFT);
		}
	} while (get_pos(node) == IS_RIGHT
		 && !rcu_rbtree_is_nil(node = get_parent(node)));
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
	while (!rcu_rbtree_is_nil(node)) {
		assert(!is_decay(node));
		printf("{ 0x%lX p:%lX r:%lX l:%lX %s %s %s} ",
			(unsigned long)node->key,
			(unsigned long) get_parent(node)->key,
			(unsigned long) node->_right->key,
			(unsigned long) node->_left->key,
			node->color ? "red" : "black",
			get_pos(node) ? "right" : "left",
			node->nil ? "nil" : "");
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

	while (!rcu_rbtree_is_nil(x) && (comp = rbtree->comp(k, x->key)) != 0) {
		usleep(10);
		if (comp < 0)
			x = rcu_dereference(x->_left);
		else
			x = rcu_dereference(x->_right);
	}
	return x;
}

struct rcu_rbtree_node *rcu_rbtree_search_min(struct rcu_rbtree *rbtree,
					  struct rcu_rbtree_node *x,
					  void *range_low, void *range_high)
{
	struct rcu_rbtree_node *xl;
	x = rcu_dereference(x);

	dbg_printf("start search min x %lx low %lx high %lx\n",
		(unsigned long) x->key,
		(unsigned long) range_low, (unsigned long) range_high);
	while (!rcu_rbtree_is_nil(x)) {
		usleep(10);
		xl = rcu_dereference(x->_left);
		dbg_printf("search min x %lx\n", (unsigned long) x->key);
		dbg_printf("search min xl %lx\n", (unsigned long) xl->key);
		if (!rcu_rbtree_is_nil(xl)
		    && (rbtree->comp(xl->max_child_key, range_low) >= 0
			|| rbtree->comp(xl->key, range_low) == 0)) {
			dbg_printf("go left\n");
			x = xl;
		} else if (rbtree->comp(x->key, range_low) >= 0
			   && rbtree->comp(x->key, range_high) <= 0) {
			dbg_printf("got it!\n");
			break;
		} else if (rbtree->comp(range_low, x->min_child_key) >= 0) {
			dbg_printf("go right\n");
			x = rcu_dereference(x->_right);
		} else {
			dbg_printf("not found!\n");
			x = make_nil(rbtree);
		}
	}
	return x;
}

struct rcu_rbtree_node *rcu_rbtree_search_max(struct rcu_rbtree *rbtree,
					  struct rcu_rbtree_node *x,
					  void *range_low, void *range_high)
{
	struct rcu_rbtree_node *xr;
	x = rcu_dereference(x);

	dbg_printf("start search max x %lx low %lx high %lx\n",
		(unsigned long) x->key,
		(unsigned long) range_low, (unsigned long) range_high);
	while (!rcu_rbtree_is_nil(x)) {
		usleep(10);
		xr = rcu_dereference(x->_right);
		dbg_printf("search max x %lx\n", (unsigned long) x->key);
		dbg_printf("search max xl %lx\n", (unsigned long) xr->key);
		if (!rcu_rbtree_is_nil(xr)
		    && (rbtree->comp(xr->min_child_key, range_high) <= 0
			|| rbtree->comp(xr->key, range_high) == 0)) {
			dbg_printf("go right\n");
			x = xr;
		} else if (rbtree->comp(x->key, range_low) >= 0
			   && rbtree->comp(x->key, range_high) <= 0) {
			dbg_printf("got it!\n");
			break;
		} else if (rbtree->comp(range_high, x->max_child_key) <= 0) {
			dbg_printf("go left\n");
			x = rcu_dereference(x->_left);
		} else {
			dbg_printf("not found!\n");
			x = make_nil(rbtree);
		}
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

	if (rcu_rbtree_is_nil(x)) {
		*zr = x;
		return x;
	} else
		*zr = x = dup_decay_node(rbtree, x);

	while (!rcu_rbtree_is_nil(xl = rcu_dereference(x->_left))) {
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

	if (rcu_rbtree_is_nil(x))
		return x;
	else {
		set_parent(x->_right, get_decay(get_parent(x->_right)),
			   get_pos(x->_right));
		set_parent(x->_left, get_decay(get_parent(x->_left)),
			   get_pos(x->_left));
	}

	while (!rcu_rbtree_is_nil(xl = rcu_dereference(x->_left))) {
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

	if (rcu_rbtree_is_nil(x))
		return x;

	while (!rcu_rbtree_is_nil(xl = rcu_dereference(x->_left)))
		x = xl;
	return x;
}

struct rcu_rbtree_node *rcu_rbtree_max(struct rcu_rbtree *rbtree,
				       struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xr;

	x = rcu_dereference(x);

	if (rcu_rbtree_is_nil(x))
		return x;

	while (!rcu_rbtree_is_nil(xr = rcu_dereference(x->_right)))
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

	if (!rcu_rbtree_is_nil(xr = rcu_dereference(x->_right)))
		return rcu_rbtree_min(rbtree, xr);
	y = get_parent_and_pos(x, &x_pos);
	while (!rcu_rbtree_is_nil(y) && x_pos == IS_RIGHT) {
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

	if (!rcu_rbtree_is_nil(xl = rcu_dereference(x->_left)))
		return rcu_rbtree_max(rbtree, xl);
	y = get_parent_and_pos(x, &x_pos);
	while (!rcu_rbtree_is_nil(y) && x_pos == IS_LEFT) {
		x = y;
		y = get_parent_and_pos(y, &x_pos);
	}
	return y;
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
	struct rcu_rbtree_node *y, *y_left, *top, *top_child;
	unsigned int top_child_pos;

	y = x->_right;
	y_left = y->_left;

	/* Now operate on new copy, decay old versions */
	x = dup_decay_node(rbtree, x);
	y = dup_decay_node(rbtree, y);
	y_left = dup_decay_node(rbtree, y_left);

	/* Internal node modifications */
	set_parent(y, get_parent(x), get_pos(x));
	set_parent(x, y, IS_LEFT);
	set_left_dup_decay(rbtree, y, x, &top, &top_child, &top_child_pos);
	set_right_dup_decay(rbtree, x, y_left, NULL, NULL, NULL);
	assert(!is_decay(top));
	assert(!is_decay(top_child));

	if (!rcu_rbtree_is_nil(y_left))
		set_parent(y_left, x, IS_RIGHT);

	cmm_smp_wmb();	/* write into node before publish */

	/* External references update (visible by readers) */
	if (rcu_rbtree_is_nil(top))
		_CMM_STORE_SHARED(rbtree->root, top_child);
	else if (top_child_pos == IS_LEFT)
		_CMM_STORE_SHARED(top->_left, top_child);
	else
		_CMM_STORE_SHARED(top->_right, top_child);

	/* Point children to new copy (parent only used by updates/next/prev) */
	set_parent(x->_left, get_decay(get_parent(x->_left)),
		get_pos(x->_left));
	set_parent(y->_right, get_decay(get_parent(y->_right)),
		get_pos(y->_right));
	if (!rcu_rbtree_is_nil(y_left)) {
		set_parent(y_left->_right,
			get_decay(get_parent(y_left->_right)),
			get_pos(y_left->_right));
		set_parent(y_left->_left,
			get_decay(get_parent(y_left->_left)),
			get_pos(y_left->_left));
	}
	set_left_update_decay(rbtree, y, x);

	/* Sanity checks */
	assert(y == rbtree->root || get_parent(y)->_left == y
		|| get_parent(y)->_right == y);
	assert(x == rbtree->root || get_parent(x)->_left == x
		|| get_parent(x)->_right == x);
	assert(rcu_rbtree_is_nil(x->_right) || get_parent(x->_right) == x);
	assert(rcu_rbtree_is_nil(x->_left) || get_parent(x->_left) == x);
	assert(rcu_rbtree_is_nil(y->_right) || get_parent(y->_right) == y);
	assert(rcu_rbtree_is_nil(y->_left) || get_parent(y->_left) == y);
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
	if (!rcu_rbtree_is_nil(y->_left))
		set_parent(y->_left, x, IS_RIGHT);
	set_parent(y, get_parent(x), get_pos(x));
	if (rcu_rbtree_is_nil(get_parent(x)))
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
	struct rcu_rbtree_node *y, *y_right, *top, *top_child;
	unsigned int top_child_pos;

	y = x->_left;
	y_right = y->_right;

	/* Now operate on new copy, decay old versions */
	x = dup_decay_node(rbtree, x);
	y = dup_decay_node(rbtree, y);
	y_right = dup_decay_node(rbtree, y_right);

	/* Internal node modifications */
	set_parent(y, get_parent(x), get_pos(x));
	set_parent(x, y, IS_RIGHT);
	set_right_dup_decay(rbtree, y, x, &top, &top_child, &top_child_pos);
	set_left_dup_decay(rbtree, x, y_right, NULL, NULL, NULL);
	assert(!is_decay(top));
	assert(!is_decay(top_child));

	if (!rcu_rbtree_is_nil(y_right))
		set_parent(y_right, x, IS_LEFT);

	cmm_smp_wmb();	/* write into node before publish */

	/* External references update (visible by readers) */
	if (rcu_rbtree_is_nil(top))
		_CMM_STORE_SHARED(rbtree->root, top_child);
	else if (top_child_pos == IS_RIGHT)
		_CMM_STORE_SHARED(top->_right, top_child);
	else
		_CMM_STORE_SHARED(top->_left, top_child);

	/* Point children to new copy (parent only used by updates/next/prev) */
	set_parent(x->_right, get_decay(get_parent(x->_right)),
		get_pos(x->_right));
	set_parent(y->_left, get_decay(get_parent(y->_left)),
		get_pos(y->_left));
	if (!rcu_rbtree_is_nil(y_right)) {
		set_parent(y_right->_left,
			get_decay(get_parent(y_right->_left)),
			get_pos(y_right->_left));
		set_parent(y_right->_right,
			get_decay(get_parent(y_right->_right)),
			get_pos(y_right->_right));
	}
	set_right_update_decay(rbtree, y, x);
	

	/* Sanity checks */
	assert(y == rbtree->root || get_parent(y)->_right == y
		|| get_parent(y)->_left == y);
	assert(x == rbtree->root || get_parent(x)->_right == x
		|| get_parent(x)->_left == x);
	assert(rcu_rbtree_is_nil(x->_left) || get_parent(x->_left) == x);
	assert(rcu_rbtree_is_nil(x->_right) || get_parent(x->_right) == x);
	assert(rcu_rbtree_is_nil(y->_left) || get_parent(y->_left) == y);
	assert(rcu_rbtree_is_nil(y->_right) || get_parent(y->_right) == y);
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
	if (!rcu_rbtree_is_nil(y->_right))
		set_parent(y->_right, x, IS_LEFT);
	set_parent(y, get_parent(x), get_pos(x));
	if (rcu_rbtree_is_nil(get_parent(x)))
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

	dbg_printf("insert fixup %p\n", z->key);
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
	struct rcu_rbtree_node *x, *y, *top, *top_child;
	unsigned int top_child_pos;

	dbg_printf("insert %p\n", z->key);
	assert(!is_decay(rbtree->root));

	y = make_nil(rbtree);
	x = rbtree->root;
	while (!rcu_rbtree_is_nil(x)) {
		y = x;
		if (rbtree->comp(z->key, x->key) < 0)
			x = x->_left;
		else
			x = x->_right;
	}

	z->_left = make_nil(rbtree);
	z->_right = make_nil(rbtree);
	z->min_child_key = z->key;
	z->max_child_key = z->key;
	z->color = COLOR_RED;
	z->nil = 0;
	z->decay_next = NULL;

	if (rcu_rbtree_is_nil(y))
		set_parent(z, y, IS_RIGHT); /* pos arbitrary for root node */
	else if (rbtree->comp(z->key, y->key) < 0)
		set_parent(z, y, IS_LEFT);
	else
		set_parent(z, y, IS_RIGHT);

	/*
	 * Order stores to z (children/parents) before stores that will make it
	 * visible to the rest of the tree.
	 */
	cmm_smp_wmb();

	if (rcu_rbtree_is_nil(y)) {
		_CMM_STORE_SHARED(rbtree->root, z);
	} else if (rbtree->comp(z->key, y->key) < 0) {
		set_left_dup_decay(rbtree, y, z, &top, &top_child,
				&top_child_pos);
		if (rcu_rbtree_is_nil(top))
			_CMM_STORE_SHARED(rbtree->root, top_child);
		else if (top_child_pos == IS_LEFT)
			_CMM_STORE_SHARED(top->_left, top_child);
		else
			_CMM_STORE_SHARED(top->_right, top_child);
		set_left_update_decay(rbtree, y, z);
	} else {
		set_right_dup_decay(rbtree, y, z, &top, &top_child,
				&top_child_pos);
		if (rcu_rbtree_is_nil(top))
			_CMM_STORE_SHARED(rbtree->root, top_child);
		else if (top_child_pos == IS_LEFT)
			_CMM_STORE_SHARED(top->_left, top_child);
		else
			_CMM_STORE_SHARED(top->_right, top_child);
		set_right_update_decay(rbtree, y, z);
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
			unsigned int copy_parents)
{
	struct rcu_rbtree_node *top, *top_child;
	unsigned int top_child_pos;

	dbg_printf("transplant %p\n", v->key);

	if (!rcu_rbtree_is_nil(v))
		v = dup_decay_node(rbtree, v);

	if (rcu_rbtree_is_nil(get_parent(u))) {
		/* pos is arbitrary for root node */
		set_parent(v, get_parent(u), IS_RIGHT);
		cmm_smp_wmb();	/* write into node before publish */
		_CMM_STORE_SHARED(rbtree->root, v);
	} else {
		set_parent(v, get_parent(u), get_pos(u));
		cmm_smp_wmb();	/* write into node before publish */

		if (get_pos(u) == IS_LEFT) {
			_set_left_dup_decay(rbtree, get_parent(u), v,
					&top, &top_child, &top_child_pos,
					copy_parents);
		} else {
			assert(copy_parents);
			set_right_dup_decay(rbtree, get_parent(u), v,
					&top, &top_child, &top_child_pos);
		}

		if (rcu_rbtree_is_nil(top))
			_CMM_STORE_SHARED(rbtree->root, top_child);
		else if (top_child_pos == IS_LEFT)
			_CMM_STORE_SHARED(top->_left, top_child);
		else
			_CMM_STORE_SHARED(top->_right, top_child);

		/* Point children to new copy (parent only used by updates/next/prev) */
		if (get_pos(u) == IS_LEFT) {
			if (copy_parents)
				set_left_update_decay(rbtree, get_parent(u), v);
		} else {
			assert(copy_parents);
			set_right_update_decay(rbtree, get_parent(u), v);
		}
	}

	/* Point children to new copy (parent only used by updates/next/prev) */
	if (!rcu_rbtree_is_nil(v)) {
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
			   unsigned int copy_parents)
{
	dbg_printf("transplant %p\n", v->key);

	lock_test_mutex();
	if (rcu_rbtree_is_nil(get_parent(u)))
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
	dbg_printf("remove fixup %p\n", x->key);

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
	struct rcu_rbtree_node *x, *top, *top_child;
	unsigned int top_child_pos;

	dbg_printf("remove nonil %p\n", z->key);
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
		set_left_dup_decay(rbtree, y, z->_left,
				&top, &top_child, &top_child_pos);
		assert(top_child == y);
		rcu_rbtree_transplant(rbtree, z, y, 1);
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
		 * The max child key of z_right does not change, because
		 * we're only changing its left children.
		 */
		y->_right = z_right;
		set_parent(y->_right, y, IS_RIGHT);
		if (rcu_rbtree_is_nil(y->_right))
			y->max_child_key = y->key;
		else
			y->max_child_key = y->_right->max_child_key;

		assert(!is_decay(z->_left));
		y->_left = z->_left;
		if (rcu_rbtree_is_nil(y->_left))
			y->min_child_key = y->key;
		else
			y->min_child_key = y->_left->min_child_key;

		assert(!is_decay(oy_right));
		/*
		 * Transplant of oy_right to old y's location will only
		 * trigger a min/max update of the already copied branch
		 * (which is not visible yet). We are transplanting
		 * oy_right as a left child of old y's parent, so the
		 * min values update propagated upward necessarily stops
		 * at z_right.
		 */
		rcu_rbtree_transplant(rbtree, y, oy_right, 0);
		rcu_rbtree_transplant(rbtree, z, y, 1);
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
	dbg_printf("remove %p\n", z->key);
	show_tree(rbtree);

	assert(!is_decay(z));
	y = z;
	y_original_color = y->color;

	if (rcu_rbtree_is_nil(z->_left)) {
		rcu_rbtree_transplant(rbtree, z, z->_right, 1);
		assert(!is_decay(z));
		x = get_decay(z->_right);
		show_tree(rbtree);
	} else if (rcu_rbtree_is_nil(z->_right)) {
		rcu_rbtree_transplant(rbtree, z, z->_left, 1);
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
