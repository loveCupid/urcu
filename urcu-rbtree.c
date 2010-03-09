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

#include <urcu-rbtree.h>
#include <urcu-pointer.h>

/*
 * TODO
 * Deal with memory allocation errors.
 * Can be ensured by reserving a pool of memory entries before doing the
 * insertion, which will have to be function of number of
 * transplantations/rotations required for the operation.
 */

/* Sentinel (bottom nodes). Don't care about p, left, right and key values */
struct rcu_rbtree_node rcu_rbtree_nil = {
	.color = COLOR_BLACK,
};

/*
 * Iterative rbtree search.
 */
struct rcu_rbtree_node* rcu_rbtree_search(struct rcu_rbtree_node *x,
					  void *k, rcu_rbtree_comp comp)
{
	x = rcu_dereference(x);

	while (x != &rcu_rbtree_nil && k != x->key) {
		if (k < x->key)
			x = rcu_dereference(x->left);
		else
			x = rcu_dereference(x->right);
	}
	return x;
}

struct rcu_rbtree_node *rcu_rbtree_min(struct rcu_rbtree_node *x,
				       rcu_rbtree_comp comp)
{
	struct rcu_rbtree_node *xl;

	x = rcu_dereference(x);

	while ((xl = rcu_dereference(x->left)) != &rcu_rbtree_nil)
		x = xl;
	return x;
}

struct rcu_rbtree_node *rcu_rbtree_max(struct rcu_rbtree_node *x,
				       rcu_rbtree_comp comp)
{
	struct rcu_rbtree_node *xr;

	x = rcu_dereference(x);

	while ((xr = rcu_dereference(x->right)) != &rcu_rbtree_nil)
		x = xr;
	return x;
}

/*
 * next and prev need to have mutex held to ensure that parent pointer is
 * coherent.
 */
struct rcu_rbtree_node *rcu_rbtree_next(struct rcu_rbtree_node *x,
					rcu_rbtree_comp comp)
{
	struct rcu_rbtree_node *xr, *y, *yredir, *yr, *yrredir;

	x = rcu_dereference(x);

	if ((xr = rcu_dereference(x->right)) != &rcu_rbtree_nil)
		return rcu_rbtree_min(xr, comp);
	y = rcu_dereference(x->p);
	for (;;) {
		int found = 0;
		if (y == &rcu_rbtree_nil)
			break;
		yr = rcu_dereference(y->right);
		/* Find out if x is one of the parent right children versions */
		if (x == yr) {
			goto found;
		} else {
			while ((yrredir = rcu_dereference(yr->redir)) != NULL) {
				yr = yrredir;
				if (x == yr)
					goto found;
			}
			break;
		}
found:
		x = y;
		y = rcu_dereference(y->p);
	}
	return y;
}

struct rcu_rbtree_node *rcu_rbtree_prev(struct rcu_rbtree_node *x,
					rcu_rbtree_comp comp)
{
	struct rcu_rbtree_node *xl, *y, *yredir, *yl, *ylredir;

	x = rcu_dereference(x);

	if ((xl = rcu_dereference(x->left)) != &rcu_rbtree_nil)
		return rcu_rbtree_min(xl, comp);
	y = rcu_dereference(x->p);
	for (;;) {
		int found = 0;
		if (y == &rcu_rbtree_nil)
			break;
		yl = rcu_dereference(y->left);
		/* Find out if x is one of the parent left children versions */
		if (x == yl) {
			goto found;
		} else {
			while ((ylredir = rcu_dereference(yl->redir)) != NULL) {
				yl = ylredir;
				if (x == yl)
					goto found;
			}
			break;
		}
found:
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
/* Should be eventually followed by a smp_wmc() */
/* Returns the new x. Previous x->right references are changed to yc. */
static struct rcu_rbtree_node *left_rotate(struct rcu_rbtree_node **root,
					   struct rcu_rbtree_node *x,
					   rcu_rbtree_alloc rballoc,
					   rcu_rbtree_free rbfree)
{
	struct rcu_rbtree_node *xc, *y, *yc;

	y = x->right;

	if (x != &rcu_rbtree_nil) {
		xc = rballoc();
		*xc = *x;
	}

	if (y != &rcu_rbtree_nil) {
		yc = rballoc();
		*yc = *y;
	}

	/* Modify children and parents in the node copies */
	if (x != &rcu_rbtree_nil) {
		xc->right = y->left;
		xc->p = yc;
	} else
		xc = &rcu_rbtree_nil;

	if (y != &rcu_rbtree_nil) {
		yc->left = xc;
		yc->p = x->p;
	} else
		yc = &rcu_rbtree_nil;

	/*
	 * Order stores to node copies (children/parents) before stores that
	 * will make the copies visible to the rest of the tree.
	 */
	smp_wmb();

	/*
	 * redirect old nodes to new.
	 */
	_STORE_SHARED(x->redir, xc);
	_STORE_SHARED(y->redir, yc);

	/*
	 * Ensure that redirections are visible before updating external
	 * pointers.
	 */
	smp_wmb();

	/* Make parents point to the copies */
	if (x->p == &rcu_rbtree_nil)
		_STORE_SHARED(*root, yc);
	else if (x == x->p->left)
		_STORE_SHARED(x->p->left, yc);
	else
		_STORE_SHARED(x->p->right, yc);

	/* Assign children parents to copies */
	if (x != &rcu_rbtree_nil) {
		_STORE_SHARED(xc->right->p, xc);
		_STORE_SHARED(xc->left->p, xc);
		defer_rcu(rbfree, x);
	}
	if (y != &rcu_rbtree_nil) {
		_STORE_SHARED(yc->right->p, yc);
		defer_rcu(rbfree, y);
		/* yc->left is xc, its parent is already set in node copy */
	}
	return xc;
}

#if 0 /* orig */
static void left_rotate(struct rcu_rbtree_node **root,
			struct rcu_rbtree_node *x,
			rcu_rbtree_alloc rballoc)
{
	struct rcu_rbtree_node *y;

	y = x->right;
	x->right = y->left;
	if (y->left != &rcu_rbtree_nil)
		y->left->p = x;
	y->p = x->p;
	if (x->p == &rcu_rbtree_nil)
		*root = y;
	else if (x == x->p->left)
		x->p->left = y;
	else
		x->p->right = y;
	y->left = x;
	x->p = y;
}
#endif //0

/* RCU: copy x and y, atomically point to new versions. GC old. */
/* Should be eventually followed by a smp_wmc() */
/* Returns the new x. Previous x->left references are changed to yc. */
static struct rcu_rbtree_node *right_rotate(struct rcu_rbtree_node **root,
					    struct rcu_rbtree_node *x,
					    rcu_rbtree_alloc rballoc,
					    rcu_rbtree_free rbfree)
{
	struct rcu_rbtree_node *xc, *y, *yc;

	y = x->left;

	if (x != &rcu_rbtree_nil) {
		xc = rballoc();
		*xc = *x;
	}

	if (y != &rcu_rbtree_nil) {
		yc = rballoc();
		*yc = *y;
	}

	/* Modify children and parents in the node copies */
	if (x != &rcu_rbtree_nil) {
		xc->left = y->right;
		xc->p = yc;
	} else
		xc = &rcu_rbtree_nil;

	if (y != &rcu_rbtree_nil) {
		yc->right = xc;
		yc->p = x->p;
	} else
		yc = &rcu_rbtree_nil;

	/*
	 * Order stores to node copies (children/parents) before stores that
	 * will make the copies visible to the rest of the tree.
	 */
	smp_wmb();

	/*
	 * redirect old nodes to new.
	 */
	_STORE_SHARED(x->redir, xc);
	_STORE_SHARED(y->redir, yc);

	/*
	 * Ensure that redirections are visible before updating external
	 * pointers.
	 */
	smp_wmb();

	/* Make parents point to the copies */
	if (x->p == &rcu_rbtree_nil)
		_STORE_SHARED(*root, yc);
	else if (x == x->p->right)
		_STORE_SHARED(x->p->right, yc);
	else
		_STORE_SHARED(x->p->left, yc);

	/* Assign children parents to copies */
	if (x != &rcu_rbtree_nil) {
		_STORE_SHARED(xc->left->p, xc);
		_STORE_SHARED(xc->right->p, xc);
		defer_rcu(rbfree, x);
	}
	if (y != &rcu_rbtree_nil) {
		_STORE_SHARED(yc->left->p, yc);
		defer_rcu(rbfree, y);
		/* yc->right is xc, its parent is already set in node copy */
	}
	return xc;
}

#if 0 //orig
static void right_rotate(struct rcu_rbtree_node **root,
			 struct rcu_rbtree_node *x,
			 rcu_rbtree_alloc rballoc)
{
	struct rcu_rbtree_node *y;

	y = x->left;
	x->left = y->right;
	if (y->right != &rcu_rbtree_nil)
		y->right->p = x;
	y->p = x->p;
	if (x->p == &rcu_rbtree_nil)
		*root = y;
	else if (x == x->p->right)
		x->p->right = y;
	else
		x->p->left = y;
	y->right = x;
	x->p = y;
}
#endif //0

static void rcu_rbtree_insert_fixup(struct rcu_rbtree_node **root,
				    struct rcu_rbtree_node *z,
				    rcu_rbtree_alloc rballoc,
				    rcu_rbtree_free rbfree)
{
	struct rcu_rbtree_node *y;

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
					z = left_rotate(root, z,
							rballoc, rbfree);
				}
				z->p->color = COLOR_BLACK;
				z->p->p->color = COLOR_RED;
				right_rotate(root, z->p->p, rballoc, rbfree);
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
					z = right_rotate(root, z,
							 rballoc, rbfree);
				}
				z->p->color = COLOR_BLACK;
				z->p->p->color = COLOR_RED;
				left_rotate(root, z->p->p, rballoc, rbfree);
			}
		}
	}
	(*root)->color = COLOR_BLACK;
}

/*
 * rcu_rbtree_insert - Insert a node in the RCU rbtree
 *
 * Returns 0 on success, or < 0 on error.
 */
int rcu_rbtree_insert(struct rcu_rbtree_node **root,
		      struct rcu_rbtree_node *z,
		      rcu_rbtree_comp comp,
		      rcu_rbtree_alloc rballoc,
		      rcu_rbtree_free rbfree)
{
	struct rcu_rbtree_node *x, *y;

	y = &rcu_rbtree_nil;
	x = *root;

	while (x != &rcu_rbtree_nil) {
		y = x;
		if (comp(z->key, x->key) < 0)
			x = x->left;
		else
			x = x->right;
	}

	z->p = y;
	z->left = &rcu_rbtree_nil;
	z->right = &rcu_rbtree_nil;
	z->color = COLOR_RED;
	z->redir = NULL;

	/*
	 * Order stores to z (children/parents) before stores that will make it
	 * visible to the rest of the tree.
	 */
	smp_wmb();

	if (y == &rcu_rbtree_nil)
		_STORE_SHARED(*root, z);
	else if (comp(z->key, y->key) < 0)
		_STORE_SHARED(y->left, z);
	else
		_STORE_SHARED(y->right, z);
	rcu_rbtree_insert_fixup(root, z, rballoc, rbfree);
	/*
	 * Make sure to commit all _STORE_SHARED() for non-coherent caches.
	 */
	smp_wmc();

	return 0;
}

/*
 * Transplant v into u position.
 * Returns new copy of v.
 */
static struct rcu_rbtree_node *
rcu_rbtree_transplant(struct rcu_rbtree_node **root,
		      struct rcu_rbtree_node *u,
		      struct rcu_rbtree_node *v,
		      rcu_rbtree_alloc rballoc,
		      rcu_rbtree_free rbfree)
{
	struct rcu_rbtree_node *vc;

	if (v != &rcu_rbtree_nil) {
		vc = rballoc();
		*vc = *v;

		/* Change vc parent pointer */
		vc->p = u->p;

		/*
		 * Order stores to node copies (children/parents) before stores
		 * that will make the copies visible to the rest of the tree.
		 */
		smp_wmb();

		/*
		 * redirect old node to new.
		 */
		_STORE_SHARED(v->redir, vc);

		/*
		 * Ensure that redirections are visible before updating external
		 * pointers.
		 */
		smp_wmb();
	} else {
		vc = &rcu_rbtree_nil;
	}

	/* Assign upper-level pointer to vc, replacing u. */
	if (u->p == &rcu_rbtree_nil)
		_STORE_SHARED(*root, vc);
	else if (u == u->p->left)
		_STORE_SHARED(u->p->left, vc);
	else
		_STORE_SHARED(u->p->right, vc);

	if (v != &rcu_rbtree_nil) {
		/*
		 * The children pointers in vc are the same as v. We can
		 * therefore reparent v's children to vc safely.
		 */
		_STORE_SHARED(vc->right->p, vc);
		_STORE_SHARED(vc->left->p, vc);

		defer_rcu(rbfree, v);
	}
	return vc;
}

static void rcu_rbtree_remove_fixup(struct rcu_rbtree_node **root,
				    struct rcu_rbtree_node *x,
				    rcu_rbtree_alloc rballoc,
				    rcu_rbtree_free rbfree)
{
	while (x != *root && x->color == COLOR_BLACK) {
		if (x == x->p->left) {
			struct rcu_rbtree_node *w, *t;

			w = x->p->right;
			if (w->color == COLOR_RED) {
				w->color == COLOR_BLACK;
				x->p->color = COLOR_RED;
				t = left_rotate(root, x->p, rballoc, rbfree);
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
					right_rotate(root, w, rballoc, rbfree);
					w = x->p->right;
				}
				w->color = x->p->color;
				x->p->color = COLOR_BLACK;
				w->right->color = COLOR_BLACK;
				left_rotate(root, x->p, rballoc, rbfree);
				x = *root;
			}
		} else {
			struct rcu_rbtree_node *w;

			w = x->p->left;
			if (w->color == COLOR_RED) {
				w->color == COLOR_BLACK;
				x->p->color = COLOR_RED;
				right_rotate(root, x->p, rballoc, rbfree);
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
					left_rotate(root, w, rballoc, rbfree);
					w = x->p->left;
				}
				w->color = x->p->color;
				x->p->color = COLOR_BLACK;
				w->left->color = COLOR_BLACK;
				right_rotate(root, x->p, rballoc, rbfree);
				x = *root;
			}
		}
	}
	x->color = COLOR_BLACK;
}

/* Returns the new copy of y->right */
static struct rcu_rbtree_node *
rcu_rbtree_remove_nonil(struct rcu_rbtree_node **root,
			struct rcu_rbtree_node *z,
			struct rcu_rbtree_node *y,
			rcu_rbtree_comp comp,
			rcu_rbtree_alloc rballoc,
			rcu_rbtree_free rbfree)
{
	struct rcu_rbtree_node *x, *xc, *yc;

	x = y->right;

	if (x != &rcu_rbtree_nil) {
		xc = rballoc();
		*xc = *x;
	} else
		xc = &rcu_rbtree_nil;

	yc = rballoc();
	*yc = *y;

	/* Update parent and children pointers within copies */
	if (y->p == z)
		xc->p = yc;
	else {
		/* Transplant y->right (xc) into y, within copy */
		xc->p = y->p;
		/* But also change the right pointer of yc */
		yc->right = z->right;
	}
	/* Transplant y into z, within copy */
	yc->p = z->p;

	yc->left = z->left;
	yc->color = z->color;

	/*
	 * Order stores to node copies (children/parents) before stores that
	 * will make the copies visible to the rest of the tree.
	 */
	smp_wmb();

	/*
	 * redirect old nodes to new.
	 */
	if (x != &rcu_rbtree_nil)
		_STORE_SHARED(x->redir, xc);
	_STORE_SHARED(y->redir, yc);

	/*
	 * Ensure that redirections are visible before updating external
	 * pointers.
	 */
	smp_wmb();

	/* Update external pointers */

	if (y->p != z) {
		/* Transplant y->right into y, external parent links */

		/* Assign upper-level pointer to xc, replacing y. */
		if (y->p == &rcu_rbtree_nil)
			_STORE_SHARED(*root, xc);
		else if (y == y->p->left)
			_STORE_SHARED(y->p->left, xc);
		else
			_STORE_SHARED(y->p->right, xc);
	}

	/* Transplant y into z, update external parent links */
	if (z->p == &rcu_rbtree_nil)
		_STORE_SHARED(*root, yc);
	else if (z == z->p->left)
		_STORE_SHARED(z->p->left, yc);
	else
		_STORE_SHARED(z->p->right, yc);

	if (x != &rcu_rbtree_nil) {
		/* Reparent xc's children to xc. */
		_STORE_SHARED(xc->right->p, xc);
		_STORE_SHARED(xc->left->p, xc);
		defer_rcu(rbfree, x);
	}

	/* Reparent yc's children to yc */
	_STORE_SHARED(yc->right->p, yc);
	_STORE_SHARED(yc->left->p, yc);
	defer_rcu(rbfree, y);

	return xc;
}

int rcu_rbtree_remove(struct rcu_rbtree_node **root,
		      struct rcu_rbtree_node *z,
		      rcu_rbtree_comp comp,
		      rcu_rbtree_alloc rballoc,
		      rcu_rbtree_free rbfree)
{
	struct rcu_rbtree_node *x, *y;
	unsigned int y_original_color;

	y = z;
	y_original_color = y->color;

	if (z->left == &rcu_rbtree_nil) {
		x = rcu_rbtree_transplant(root, z, z->right, rballoc, rbfree);
	} else if (z->right == &rcu_rbtree_nil) {
		x = rcu_rbtree_transplant(root, z, z->left, rballoc, rbfree);
	} else {
		y = rcu_rbtree_min(z->right, comp);
		y_original_color = y->color;
		x = rcu_rbtree_remove_nonil(root, z, y, comp, rballoc, rbfree);
	}
	if (y_original_color == COLOR_BLACK)
		rcu_rbtree_remove_fixup(root, x, rballoc, rbfree);
	/*
	 * Commit all _STORE_SHARED().
	 */
	smp_wmc();

	return 0;
}
