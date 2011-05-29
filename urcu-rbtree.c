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

#define DEBUG

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
	node->parent = ((unsigned long) parent) | pos;
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
 * TODO
 * Deal with memory allocation errors.
 * Can be ensured by reserving a pool of memory entries before doing the
 * insertion, which will have to be function of number of
 * transplantations/rotations required for the operation.
 */

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
			(unsigned long) node->right->key,
			(unsigned long) node->left->key,
			node->color ? "red" : "black",
			get_pos(node) ? "right" : "left",
			node->nil ? "nil" : "");
		node = rcu_rbtree_next(rbtree, node);
	}
	printf("\n");
}

static
struct rcu_rbtree_node *make_nil(struct rcu_rbtree *rbtree)
{
	return &rbtree->nil_node;
}

/*
 * Iterative rbtree search.
 */
struct rcu_rbtree_node* rcu_rbtree_search(struct rcu_rbtree *rbtree,
					  struct rcu_rbtree_node *x,
					  void *k)
{
	x = rcu_dereference(x);

	while (!rcu_rbtree_is_nil(x) && k != x->key) {
		usleep(10);
		if (rbtree->comp(k, x->key) < 0)
			x = rcu_dereference(x->left);
		else
			x = rcu_dereference(x->right);
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

	while (!rcu_rbtree_is_nil(xl = rcu_dereference(x->left))) {
		x = dup_decay_node(rbtree, xl);
		set_parent(x, get_decay(get_parent(x)), get_pos(x));
		get_parent(x)->left = get_decay(get_parent(x)->left);
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
		set_parent(x->right, get_decay(get_parent(x->right)),
			   get_pos(x->right));
		set_parent(x->left, get_decay(get_parent(x->left)),
			   get_pos(x->left));
	}

	while (!rcu_rbtree_is_nil(xl = rcu_dereference(x->left))) {
		x = xl;
		set_parent(x->right, get_decay(get_parent(x->right)),
			   get_pos(x->right));
		set_parent(x->left, get_decay(get_parent(x->left)),
			   get_pos(x->left));
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

	while (!rcu_rbtree_is_nil(xl = rcu_dereference(x->left)))
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

	while (!rcu_rbtree_is_nil(xr = rcu_dereference(x->right)))
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

	if (!rcu_rbtree_is_nil(xr = rcu_dereference(x->right)))
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

	if (!rcu_rbtree_is_nil(xl = rcu_dereference(x->left)))
		return rcu_rbtree_min(rbtree, xl);
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
 *   get_parent(x)->right == x
 * or if x is a left child, the assumption that:
 *   get_parent(x)->left == x
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
	struct rcu_rbtree_node *y, *y_left, *x_p;
	unsigned int x_pos;

	y = x->right;
	y_left = y->left;

	/* Now operate on new copy, decay old versions */
	x = dup_decay_node(rbtree, x);
	y = dup_decay_node(rbtree, y);
	y_left = dup_decay_node(rbtree, y_left);

	x_pos = get_pos(x);
	x_p = get_parent(x);

	/* Internal node modifications */
	x->right = y_left;
	set_parent(y, get_parent(x), get_pos(x));
	set_parent(x, y, IS_LEFT);
	y->left = x;
	if (!rcu_rbtree_is_nil(y_left))
		set_parent(y_left, x, IS_RIGHT);

	cmm_smp_wmb();	/* write into node before publish */

	/* External references update (visible by readers) */
	if (rcu_rbtree_is_nil(x_p))
		_CMM_STORE_SHARED(rbtree->root, y);
	else if (x_pos == IS_LEFT)
		_CMM_STORE_SHARED(x_p->left, y);
	else
		_CMM_STORE_SHARED(x_p->right, y);

	/* Point children to new copy (parent only used by updates/next/prev) */
	set_parent(x->left, get_decay(get_parent(x->left)),
		get_pos(x->left));
	set_parent(y->right, get_decay(get_parent(y->right)),
		get_pos(y->right));
	if (!rcu_rbtree_is_nil(y_left)) {
		set_parent(y_left->right, get_decay(get_parent(y_left->right)),
			get_pos(y_left->right));
		set_parent(y_left->left, get_decay(get_parent(y_left->left)),
			get_pos(y_left->left));
	}

	/* Sanity checks */
	assert(y == rbtree->root || get_parent(y)->left == y || get_parent(y)->right == y);
	assert(x == rbtree->root || get_parent(x)->left == x || get_parent(x)->right == x);
	assert(rcu_rbtree_is_nil(x->right) || get_parent(x->right) == x);
	assert(rcu_rbtree_is_nil(x->left) || get_parent(x->left) == x);
	assert(rcu_rbtree_is_nil(y->right) || get_parent(y->right) == y);
	assert(rcu_rbtree_is_nil(y->left) || get_parent(y->left) == y);
	assert(!is_decay(rbtree->root));
	assert(!is_decay(x));
	assert(!is_decay(y));
	assert(!is_decay(x->right));
	assert(!is_decay(x->left));
	assert(!is_decay(y->right));
	assert(!is_decay(y->left));
}

#else

/* non-rcu version */
static
void left_rotate(struct rcu_rbtree *rbtree,
		 struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y;

	lock_test_mutex();
	y = x->right;
	x->right = y->left;
	if (!rcu_rbtree_is_nil(y->left))
		set_parent(y->left, x, IS_RIGHT);
	set_parent(y, get_parent(x), get_pos(x));
	if (rcu_rbtree_is_nil(get_parent(x)))
		rbtree->root = y;
	else if (x == get_parent(x)->left) {
		get_parent(x)->left = y;
	} else {
		get_parent(x)->right = y;
	}
	y->left = x;
	set_parent(x, y, IS_LEFT);
	unlock_test_mutex();
}

#endif

#ifdef RBTREE_RCU_SUPPORT_ROTATE_RIGHT
static
void right_rotate(struct rcu_rbtree *rbtree,
		  struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y, *y_right, *x_p;
	unsigned int x_pos;

	y = x->left;
	y_right = y->right;

	/* Now operate on new copy, decay old versions */
	x = dup_decay_node(rbtree, x);
	y = dup_decay_node(rbtree, y);
	y_right = dup_decay_node(rbtree, y_right);

	x_pos = get_pos(x);
	x_p = get_parent(x);

	/* Internal node modifications */
	x->left = y_right;
	set_parent(y, get_parent(x), get_pos(x));
	set_parent(x, y, IS_RIGHT);
	y->right = x;
	if (!rcu_rbtree_is_nil(y_right))
		set_parent(y_right, x, IS_LEFT);

	cmm_smp_wmb();	/* write into node before publish */

	/* External references update (visible by readers) */
	if (rcu_rbtree_is_nil(x_p))
		_CMM_STORE_SHARED(rbtree->root, y);
	else if (x_pos == IS_RIGHT)
		_CMM_STORE_SHARED(x_p->right, y);
	else
		_CMM_STORE_SHARED(x_p->left, y);

	/* Point children to new copy (parent only used by updates/next/prev) */
	set_parent(x->right, get_decay(get_parent(x->right)),
		get_pos(x->right));
	set_parent(y->left, get_decay(get_parent(y->left)),
		get_pos(y->left));
	if (!rcu_rbtree_is_nil(y_right)) {
		set_parent(y_right->left, get_decay(get_parent(y_right->left)),
			get_pos(y_right->left));
		set_parent(y_right->right, get_decay(get_parent(y_right->right)),
			get_pos(y_right->right));
	}

	/* Sanity checks */
	assert(y == rbtree->root || get_parent(y)->right == y || get_parent(y)->left == y);
	assert(x == rbtree->root || get_parent(x)->right == x || get_parent(x)->left == x);
	assert(rcu_rbtree_is_nil(x->left) || get_parent(x->left) == x);
	assert(rcu_rbtree_is_nil(x->right) || get_parent(x->right) == x);
	assert(rcu_rbtree_is_nil(y->left) || get_parent(y->left) == y);
	assert(rcu_rbtree_is_nil(y->right) || get_parent(y->right) == y);
	assert(!is_decay(rbtree->root));
	assert(!is_decay(x));
	assert(!is_decay(y));
	assert(!is_decay(x->left));
	assert(!is_decay(x->right));
	assert(!is_decay(y->left));
	assert(!is_decay(y->right));
}

#else

/* non-rcu version */
static
void right_rotate(struct rcu_rbtree *rbtree,
		  struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y;

	lock_test_mutex();
	y = x->left;
	x->left = y->right;
	if (!rcu_rbtree_is_nil(y->right))
		set_parent(y->right, x, IS_LEFT);
	set_parent(y, get_parent(x), get_pos(x));
	if (rcu_rbtree_is_nil(get_parent(x)))
		rbtree->root = y;
	else if (x == get_parent(x)->right) {
		get_parent(x)->right = y;
	} else {
		get_parent(x)->left = y;
	}
	y->right = x;
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
		if (get_parent(z) == get_parent(get_parent(z))->left) {
			y = get_parent(get_parent(z))->right;
			if (y->color == COLOR_RED) {
				get_parent(z)->color = COLOR_BLACK;
				y->color = COLOR_BLACK;
				get_parent(get_parent(z))->color = COLOR_RED;
				z = get_parent(get_parent(z));
			} else {
				if (z == get_parent(z)->right) {
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
			y = get_parent(get_parent(z))->left;
			if (y->color == COLOR_RED) {
				get_parent(z)->color = COLOR_BLACK;
				y->color = COLOR_BLACK;
				get_parent(get_parent(z))->color = COLOR_RED;
				z = get_parent(get_parent(z));
			} else {
				if (z == get_parent(z)->left) {
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

	dbg_printf("insert %p\n", z->key);
	assert(!is_decay(rbtree->root));

	y = make_nil(rbtree);
	if (!rbtree->root)
		rbtree->root = make_nil(rbtree);
	x = rbtree->root;
	while (!rcu_rbtree_is_nil(x)) {
		y = x;
		if (rbtree->comp(z->key, x->key) < 0)
			x = x->left;
		else
			x = x->right;
	}

	z->left = make_nil(rbtree);
	z->right = make_nil(rbtree);
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

	if (rcu_rbtree_is_nil(y))
		_CMM_STORE_SHARED(rbtree->root, z);
	else if (rbtree->comp(z->key, y->key) < 0)
		_CMM_STORE_SHARED(y->left, z);
	else
		_CMM_STORE_SHARED(y->right, z);
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
			struct rcu_rbtree_node *v)
{
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
		if (get_pos(u) == IS_LEFT)
			_CMM_STORE_SHARED(get_parent(u)->left, v);
		else
			_CMM_STORE_SHARED(get_parent(u)->right, v);
	}

	/* Point children to new copy (parent only used by updates/next/prev) */
	if (!rcu_rbtree_is_nil(v)) {
		set_parent(v->right, get_decay(get_parent(v->right)),
			get_pos(v->right));
		set_parent(v->left, get_decay(get_parent(v->left)),
			get_pos(v->left));
	}
	assert(!is_decay(rbtree->root));
}

#else

/* Non-RCU version */
static
void rcu_rbtree_transplant(struct rcu_rbtree *rbtree,
			   struct rcu_rbtree_node *u,
			   struct rcu_rbtree_node *v)
{
	dbg_printf("transplant %p\n", v->key);

	lock_test_mutex();
	if (rcu_rbtree_is_nil(get_parent(u)))
		rbtree->root = v;
	else if (u == get_parent(u)->left)
		get_parent(u)->left = v;
	else
		get_parent(u)->right = v;
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
		assert(!is_decay(get_parent(x)->left));
		if (x == get_parent(x)->left) {
			struct rcu_rbtree_node *w;

			w = get_parent(x)->right;

			if (w->color == COLOR_RED) {
				w->color = COLOR_BLACK;
				get_parent(x)->color = COLOR_RED;
				left_rotate(rbtree, get_parent(x));
				x = get_decay(x);
				assert(!is_decay(rbtree->root));
				w = get_parent(x)->right;
			}
			if (w->left->color == COLOR_BLACK
			    && w->right->color == COLOR_BLACK) {
				w->color = COLOR_RED;
				x = get_parent(x);
				assert(!is_decay(rbtree->root));
				assert(!is_decay(x));
			} else {
				if (w->right->color == COLOR_BLACK) {
					w->left->color = COLOR_BLACK;
					w->color = COLOR_RED;
					right_rotate(rbtree, w);
					assert(!is_decay(rbtree->root));
					x = get_decay(x);
					w = get_parent(x)->right;
				}
				w->color = get_parent(x)->color;
				get_parent(x)->color = COLOR_BLACK;
				w->right->color = COLOR_BLACK;
				left_rotate(rbtree, get_parent(x));
				assert(!is_decay(rbtree->root));
				x = rbtree->root;
			}
		} else {
			struct rcu_rbtree_node *w;

			w = get_parent(x)->left;

			if (w->color == COLOR_RED) {
				w->color = COLOR_BLACK;
				get_parent(x)->color = COLOR_RED;
				right_rotate(rbtree, get_parent(x));
				assert(!is_decay(rbtree->root));
				x = get_decay(x);
				w = get_parent(x)->left;
			}
			if (w->right->color == COLOR_BLACK
			    && w->left->color == COLOR_BLACK) {
				w->color = COLOR_RED;
				x = get_parent(x);
				assert(!is_decay(rbtree->root));
				assert(!is_decay(x));
			} else {
				if (w->left->color == COLOR_BLACK) {
					w->right->color = COLOR_BLACK;
					w->color = COLOR_RED;
					left_rotate(rbtree, w);
					assert(!is_decay(rbtree->root));
					x = get_decay(x);
					w = get_parent(x)->left;
				}
				w->color = get_parent(x)->color;
				get_parent(x)->color = COLOR_BLACK;
				w->left->color = COLOR_BLACK;
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

	dbg_printf("remove nonil %p\n", z->key);
	show_tree(rbtree);

	assert(!is_decay(z));
	assert(!is_decay(y));
	assert(!is_decay(y->right));
	assert(!is_decay(get_parent(y)));
	x = y->right;
	assert(!is_decay(x));
	if (get_parent(y) == z) {
		y = dup_decay_node(rbtree, y);
		set_parent(x, y, get_pos(x));	/* parent for nil */
		y->left = z->left;
		rcu_rbtree_transplant(rbtree, z, y);
	} else {
		struct rcu_rbtree_node *oy_right, *z_right;

		/*
		 * Need to make sure y is always visible by readers.
		 */
		y = rcu_rbtree_min_dup_decay(rbtree, z->right, &z_right);
		assert(!is_decay(y));
		assert(!is_decay(z));
		oy_right = y->right;
		y->right = z_right;
		set_parent(y->right, y, IS_RIGHT);
		assert(!is_decay(z->left));
		y->left = z->left;
		assert(!is_decay(oy_right));
		rcu_rbtree_transplant(rbtree, y, oy_right);
		rcu_rbtree_transplant(rbtree, z, y);
		/* Update children */
		(void) rcu_rbtree_min_update_decay(rbtree, y->right);
	}
	y = get_decay(y);
	assert(!is_decay(z));
	assert(!is_decay(z->left));
	y->color = z->color;
	set_parent(y->left, y, IS_LEFT);
	set_parent(y->right, get_decay(get_parent(y->right)), IS_RIGHT);
	assert(!is_decay(y->left));
	assert(!is_decay(y->right));
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

	if (rcu_rbtree_is_nil(z->left)) {
		rcu_rbtree_transplant(rbtree, z, z->right);
		assert(!is_decay(z));
		x = get_decay(z->right);
		show_tree(rbtree);
	} else if (rcu_rbtree_is_nil(z->right)) {
		rcu_rbtree_transplant(rbtree, z, z->left);
		assert(!is_decay(z));
		x = get_decay(z->left);
		show_tree(rbtree);
	} else {
		y = rcu_rbtree_min(rbtree, z->right);
		assert(!is_decay(y));
		y_original_color = y->color;
		x = y->right;
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
