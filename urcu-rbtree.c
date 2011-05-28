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

#include <urcu/rcurbtree.h>
#include <urcu-pointer.h>
#include <urcu-defer.h>

#define DEBUG

#ifdef DEBUG
#define dbg_printf(args...)	printf(args)
#else
#define dbg_printf(args...)
#endif

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
		printf("{ 0x%lX p:%lX r:%lX l:%lX %s %s %s} ",
			(unsigned long)node->key,
			node->p->key,
			node->right->key,
			node->left->key,
			node->color ? "red" : "black",
			node->pos ? "right" : "left",
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
		if (rbtree->comp(k, x->key) < 0)
			x = rcu_dereference(x->left);
		else
			x = rcu_dereference(x->right);
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
 * next and prev need to have mutex held to ensure that parent pointer is
 * coherent.
 */
struct rcu_rbtree_node *rcu_rbtree_next(struct rcu_rbtree *rbtree,
					struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xr, *y, *yr;

	x = rcu_dereference(x);

	if (!rcu_rbtree_is_nil(xr = rcu_dereference(x->right)))
		return rcu_rbtree_min(rbtree, xr);
	y = rcu_dereference(x->p);
	while (!rcu_rbtree_is_nil(y) && x->pos == IS_RIGHT) {
		x = y;
		y = rcu_dereference(y->p);
	}
	return y;
}

struct rcu_rbtree_node *rcu_rbtree_prev(struct rcu_rbtree *rbtree,
					struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *xl, *y, *yl;

	x = rcu_dereference(x);

	if (!rcu_rbtree_is_nil(xl = rcu_dereference(x->left)))
		return rcu_rbtree_min(rbtree, xl);
	y = rcu_dereference(x->p);
	while (!rcu_rbtree_is_nil(y) && x->pos == IS_LEFT) {
		x = y;
		y = rcu_dereference(y->p);
	}
	return y;
}

/*
 * We have to ensure these assumptions are correct for prev/next
 * traversal:
 *
 * with x being a right child, the assumption that:
 *   x->p->right == x
 * or if x is a left child, the assumption that:
 *   x->p->left == x
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
/* Returns the new x. Previous x->right references are changed to yc.
 * Previous y->left->right is changed to bc. */

static
struct rcu_rbtree_node *left_rotate(struct rcu_rbtree *rbtree,
			struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y;
	struct rcu_rbtree_node *t;

	t = x->p;
	y = x->right;
	x->right = y->left;
	if (!rcu_rbtree_is_nil(y->left)) {
		y->left->p = x;
		y->left->pos = IS_RIGHT;
	}
	y->p = x->p;
	if (rcu_rbtree_is_nil(x->p))
		rbtree->root = y;
	else if (x == x->p->left) {
		x->p->left = y;
		y->pos = IS_LEFT;
	} else {
		x->p->right = y;
		y->pos = IS_RIGHT;
	}
	y->left = x;
	x->pos = IS_LEFT;
	x->p = y;
	return t;
}

static
struct rcu_rbtree_node *right_rotate(struct rcu_rbtree *rbtree,
			struct rcu_rbtree_node *x)
{
	struct rcu_rbtree_node *y;
	struct rcu_rbtree_node *t;

	t = x->p;
	y = x->left;
	x->left = y->right;
	if (!rcu_rbtree_is_nil(y->right)) {
		y->right->p = x;
		y->right->pos = IS_LEFT;
	}
	y->p = x->p;
	if (rcu_rbtree_is_nil(x->p))
		rbtree->root = y;
	else if (x == x->p->right) {
		x->p->right = y;
		y->pos = IS_RIGHT;
	} else {
		x->p->left = y;
		y->pos = IS_LEFT;
	}
	y->right = x;
	x->pos = IS_RIGHT;
	x->p = y;
	return t;
}

static void rcu_rbtree_insert_fixup(struct rcu_rbtree *rbtree,
				    struct rcu_rbtree_node *z)
{
	struct rcu_rbtree_node *y;

	dbg_printf("insert fixup %p\n", z->key);

	while (z->p->color == COLOR_RED) {
		if (z->p == z->p->p->left) {
			y = z->p->p->right;
			if (y->color == COLOR_RED) {
				z->p->color = COLOR_BLACK;
				y->color = COLOR_BLACK;
				z->p->p->color = COLOR_RED;
				z = z->p->p;
			} else {
				if (z == z->p->right) {
					z = z->p;
					left_rotate(rbtree, z);
				}
				z->p->color = COLOR_BLACK;
				z->p->p->color = COLOR_RED;
				right_rotate(rbtree, z->p->p);
			}
		} else {
			y = z->p->p->left;
			if (y->color == COLOR_RED) {
				z->p->color = COLOR_BLACK;
				y->color = COLOR_BLACK;
				z->p->p->color = COLOR_RED;
				z = z->p->p;
			} else {
				if (z == z->p->left) {
					z = z->p;
					right_rotate(rbtree, z);
				}
				z->p->color = COLOR_BLACK;
				z->p->p->color = COLOR_RED;
				left_rotate(rbtree, z->p->p);
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

	z->p = y;

	z->left = make_nil(rbtree);
	z->right = make_nil(rbtree);
	z->color = COLOR_RED;
	z->nil = 0;

	if (rcu_rbtree_is_nil(y))
		z->pos = IS_RIGHT;	/* arbitrary for root node */
	else if (rbtree->comp(z->key, y->key) < 0)
		z->pos = IS_LEFT;
	else
		z->pos = IS_RIGHT;

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
 * Returns new copy of v.
 */
static struct rcu_rbtree_node *
rcu_rbtree_transplant(struct rcu_rbtree *rbtree,
		      struct rcu_rbtree_node *u,
		      struct rcu_rbtree_node *v)
{
	struct rcu_rbtree_node *vc;

	dbg_printf("transplant %p\n", v->key);

	if (rcu_rbtree_is_nil(u->p))
		rbtree->root = v;
	else if (u == u->p->left) {
		u->p->left = v;
		v->pos = IS_LEFT;
	} else {
		u->p->right = v;
		v->pos = IS_RIGHT;
	}
	v->p = u->p;
	return v;
}

static void rcu_rbtree_remove_fixup(struct rcu_rbtree *rbtree,
				    struct rcu_rbtree_node *x)
{
	dbg_printf("remove fixup %p\n", x->key);

	while (x != rbtree->root && x->color == COLOR_BLACK) {
		if (x == x->p->left) {
			struct rcu_rbtree_node *w, *t;

			w = x->p->right;

			if (w->color == COLOR_RED) {
				w->color == COLOR_BLACK;
				x->p->color = COLOR_RED;
				t = left_rotate(rbtree, x->p);
				assert(x->p->left == t->left);
				/* x is a left node, not copied by rotation */
				w = x->p->right;
			}
			if (w->left->color == COLOR_BLACK
			    && w->right->color == COLOR_BLACK) {
				w->color = COLOR_RED;
				x = x->p;
			} else {
				if (w->right->color == COLOR_BLACK) {
					w->left->color = COLOR_BLACK;
					w->color = COLOR_RED;
					right_rotate(rbtree, w);
					w = x->p->right;
				}
				w->color = x->p->color;
				x->p->color = COLOR_BLACK;
				w->right->color = COLOR_BLACK;
				left_rotate(rbtree, x->p);
				x = rbtree->root;
			}
		} else {
			struct rcu_rbtree_node *w, *t;

			w = x->p->left;

			if (w->color == COLOR_RED) {
				w->color == COLOR_BLACK;
				x->p->color = COLOR_RED;
				t = right_rotate(rbtree, x->p);
				assert(x->p->right == t->right);
				/* x is a right node, not copied by rotation */
				w = x->p->left;
			}
			if (w->right->color == COLOR_BLACK
			    && w->left->color == COLOR_BLACK) {
				w->color = COLOR_RED;
				x = x->p;
			} else {
				if (w->left->color == COLOR_BLACK) {
					w->right->color = COLOR_BLACK;
					w->color = COLOR_RED;
					left_rotate(rbtree, w);
					w = x->p->left;
				}
				w->color = x->p->color;
				x->p->color = COLOR_BLACK;
				w->left->color = COLOR_BLACK;
				right_rotate(rbtree, x->p);
				x = rbtree->root;
			}
		}
	}
	x->color = COLOR_BLACK;
}

/*
 * Returns the new copy of y->right.
 * Delete z. All non-copied children left/right positions are unchanged. */
static struct rcu_rbtree_node *
rcu_rbtree_remove_nonil(struct rcu_rbtree *rbtree,
			struct rcu_rbtree_node *z,
			struct rcu_rbtree_node *y)
{
	struct rcu_rbtree_node *x, *xc, *yc;

	dbg_printf("remove nonil %p\n", z->key);
	show_tree(rbtree);

	x = y->right;
	if (y->p == z)
		x->p = y;
	else {
		rcu_rbtree_transplant(rbtree, y, y->right);
		y->right = z->right;
		y->right->p = y;
	}
	rcu_rbtree_transplant(rbtree, z, y);
	y->left = z->left;
	y->left->p = y;
	y->color = z->color;
	return x;
}

int rcu_rbtree_remove(struct rcu_rbtree *rbtree,
		      struct rcu_rbtree_node *z)
{
	struct rcu_rbtree_node *x, *y;
	unsigned int y_original_color;

	dbg_printf("remove %p\n", z->key);
	show_tree(rbtree);

	y = z;
	y_original_color = y->color;

	if (rcu_rbtree_is_nil(z->left)) {
		x = rcu_rbtree_transplant(rbtree, z, z->right);
		show_tree(rbtree);
	} else if (rcu_rbtree_is_nil(z->right)) {
		x = rcu_rbtree_transplant(rbtree, z, z->left);
		show_tree(rbtree);
	} else {
		y = rcu_rbtree_min(rbtree, z->right);
		y_original_color = y->color;
		x = rcu_rbtree_remove_nonil(rbtree, z, y);
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
