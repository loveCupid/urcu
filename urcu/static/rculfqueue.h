#ifndef _URCU_RCULFQUEUE_STATIC_H
#define _URCU_RCULFQUEUE_STATIC_H

/*
 * rculfqueue-static.h
 *
 * Userspace RCU library - Lock-Free RCU Queue
 *
 * Copyright 2010-2011 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright 2011 - Lai Jiangshan <laijs@cn.fujitsu.com>
 *
 * TO BE INCLUDED ONLY IN LGPL-COMPATIBLE CODE. See rculfqueue.h for linking
 * dynamically with the userspace rcu library.
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

#include <urcu-call-rcu.h>
#include <urcu/uatomic.h>
#include <assert.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

enum node_type {
	NODE_NODE = 0,
	NODE_HEAD = 1,
	NODE_NULL = 2, /* transitional */
};

#define NODE_TYPE_BITS 2
#define NODE_TYPE_MASK ((1UL << NODE_TYPE_BITS) - 1)

/*
 * Lock-free RCU queue.
 *
 * Node addresses must be allocated on multiples of 4 bytes, because the
 * two bottom bits are used internally.  "Special" HEAD and NULL node
 * references use a sequence counter (rather than an address).  The
 * sequence count is incremented as elements are enqueued.  Enqueue and
 * dequeue operations hold a RCU read lock to deal with uatomic_cmpxchg
 * ABA problem on standard node addresses. The sequence count of HEAD
 * and NULL nodes deals with ABA problem with these nodes.
 *
 * Keeping a sequence count throughout the list allows dealing with
 * dequeue-the-last/enqueue-the-first operations without need for adding
 * any dummy node in the queue.
 *
 * This queue is not circular.  The head node is located prior to the
 * oldest node, tail points to the newest node.
 *
 * Keeping a separate head and tail helps with large queues: enqueue and
 * dequeue can proceed concurrently without wrestling for exclusive
 * access to the same variables.
 */

static inline
enum node_type queue_node_type(unsigned long node)
{
	return node & NODE_TYPE_MASK;
}

static inline
unsigned long queue_node_seq(unsigned long node)
{
	assert(queue_node_type(node) == NODE_HEAD
		|| queue_node_type(node) == NODE_NULL);
	return node >> NODE_TYPE_BITS;
}

static inline
struct cds_lfq_node_rcu *queue_node_node(unsigned long node)
{
	assert(queue_node_type(node) == NODE_NODE);
	return (void *) (node & ~NODE_TYPE_MASK);
}

static inline
unsigned long queue_make_node(struct cds_lfq_node_rcu *node)
{
	return ((unsigned long) node) | NODE_NODE;
}

static inline
unsigned long queue_make_head(unsigned long seq)
{
	return (seq << NODE_TYPE_BITS) | NODE_HEAD;
}

static inline
unsigned long queue_make_null(unsigned long seq)
{
	return (seq << NODE_TYPE_BITS) | NODE_NULL;
}


static inline
void _cds_lfq_node_init_rcu(struct cds_lfq_node_rcu *node)
{
	/* Kept here for object debugging. */
}

static inline
void _cds_lfq_init_rcu(struct cds_lfq_queue_rcu *q)
{
	q->head.next = queue_make_head(0);
	q->tail = queue_make_head(0);
}

static inline
int _cds_lfq_is_empty(struct cds_lfq_queue_rcu *q)
{
	unsigned long head, next;
	struct cds_lfq_node_rcu *phead;

	head = rcu_dereference(q->head.next);
	if (queue_node_type(head) == NODE_HEAD) {
		/* F0 or T0b */
		return 1;
	}

	phead = queue_node_node(head);
	next = rcu_dereference(phead->next);

	if (queue_node_type(next) == NODE_HEAD) { /* T1, F1 */
		/* only one node */
		return 0;
	} else if (queue_node_type(next) == NODE_NODE) {
		/* have >=2 nodes, Tn{n>=2} Fn{n>=2} */
		return 0;
	} else {
		/* T0a */
		assert(queue_node_type(next) == NODE_NULL);
		return 1;
	}
}

/*
 * The queue should be emptied before calling destroy.
 *
 * Return 0 on success, -EPERM if queue is not empty.
 */
static inline
int _cds_lfq_destroy_rcu(struct cds_lfq_queue_rcu *q)
{
	if (!_cds_lfq_is_empty(q))
		return -EPERM;
	/* Kept here for object debugging. */
	return 0;
}

static void __queue_post_dequeue_the_last(struct cds_lfq_queue_rcu *q,
		unsigned long old_head, unsigned long new_head);

/*
 * Lock-free queue uses rcu_read_lock() to proctect the life-time
 * of the nodes and to prevent ABA-problem.
 *
 * cds_lfq_enqueue_rcu() and cds_lfq_dequeue_rcu() need to be called
 * under rcu read lock critical section.  The node returned by
 * queue_dequeue() must not be modified/re-used/freed until a
 * grace-period passed.
 */

static inline
unsigned long __queue_enqueue(struct cds_lfq_queue_rcu *q,
		unsigned long tail, unsigned long next,
		struct cds_lfq_node_rcu *ptail, struct cds_lfq_node_rcu *pnode)
{
	unsigned long newnext;

	/* increase the seq for every enqueued node */
	pnode->next = queue_make_head(queue_node_seq(next) + 1);

	/* Fn(seq) -> T(n+1)(seq+1) */
	newnext = uatomic_cmpxchg(&ptail->next, next, queue_make_node(pnode));

	if (newnext != next)
		return newnext;

	/* success, move tail(or done by other), T(n+1) -> F(n+1) */
	uatomic_cmpxchg(&q->tail, tail, queue_make_node(pnode));
	return next;
}

/*
 * Needs to be called with RCU read-side lock held.
 */
static inline
void _cds_lfq_enqueue_rcu(struct cds_lfq_queue_rcu *q,
			  struct cds_lfq_node_rcu *pnode)
{
	unsigned long tail, next;
	struct cds_lfq_node_rcu *ptail;

	for (;;) {
		tail = rcu_dereference(q->tail);
		if (queue_node_type(tail) == NODE_HEAD) { /* F0 */
			ptail = &q->head;
			next = tail;
			/*
			 * We cannot use "next = rcu_dereference(ptail->next);"
			 * here, because it is control dependency, not data
			 * dependency. But since F0 is the most likely state
			 * when 0 node, so we use 'next = tail'.
			 */
		} else { /* Fn, Tn */
			ptail = queue_node_node(tail);
			next = rcu_dereference(ptail->next);
		}

		if (queue_node_type(next) == NODE_HEAD) { /* Fn */
			unsigned long newnext;

			/* Fn{n>=0} -> F(n+1) */
			newnext = __queue_enqueue(q, tail, next, ptail, pnode);
			if (newnext == next) {
				return;
			}
			next = newnext;
		}

		if (queue_node_type(next) == NODE_NODE) { /* Tn */
			/* help moving tail, Tn{n>=1} -> Fn */
			uatomic_cmpxchg(&q->tail, tail, next);
		} else if (queue_node_type(next) == NODE_NULL) {
			/* help finishing dequeuing the last, T0a or T0b -> F0 */
			__queue_post_dequeue_the_last(q, tail,
					queue_make_head(queue_node_seq(next)));
		}
	}
}

static inline
void __queue_post_dequeue_the_last(struct cds_lfq_queue_rcu *q,
		unsigned long old_head, unsigned long new_head)
{
	/* step2: T0a -> T0b */
	uatomic_cmpxchg(&q->head.next, old_head, new_head);

	/* step3: T0b -> F0 */
	uatomic_cmpxchg(&q->tail, old_head, new_head);
}

static inline
int __queue_dequeue_the_last(struct cds_lfq_queue_rcu *q,
		unsigned long head, unsigned long next,
		struct cds_lfq_node_rcu *plast)
{
	unsigned long origin_tail = rcu_dereference(q->tail);

	/*
	 * T1 -> F1 if T1, we cannot dequeue the last node when T1.
	 *
	 * pseudocode is:
	 *   tail = rcu_dereference(q->tail); (*)
	 *   if (tail == queue_make_head(seq - 1))
	 *     uatomic_cmpxchg(&q->tail, tail, head);
	 * But we only expect (*) gets tail's value is:
	 *     head                             (F1)(likely got)
	 *     queue_make_head(seq - 1)         (T1)
	 * not newer nor older value, so the pseudocode is not acceptable.
	 */
	if (origin_tail != head) {
		unsigned long tail;

		/* Don't believe the orderless-read tail! */
		origin_tail = queue_make_head(queue_node_seq(next) - 1);

		/* help moving tail, T1 -> F1 */
		tail = uatomic_cmpxchg(&q->tail, origin_tail, head);

		if (tail != origin_tail && tail != head)
			return 0;
	}

	/* step1: F1 -> T0a */
	if (uatomic_cmpxchg(&plast->next, next, queue_make_null(queue_node_seq(next))) != next)
		return 0;

	__queue_post_dequeue_the_last(q, head, next);
	return 1;
}

static inline
int __queue_dequeue(struct cds_lfq_queue_rcu *q,
		unsigned long head, unsigned long next)
{
	struct cds_lfq_node_rcu *pnext = queue_node_node(next);
	unsigned long nextnext = rcu_dereference(pnext->next);

	/*
	 * T2 -> F2 if T2, we cannot dequeue the first node when T2.
	 *
	 * pseudocode is:
	 *   tail = rcu_dereference(q->tail); (*)
	 *   if (tail == head)
	 *     uatomic_cmpxchg(&q->tail, head, next);
	 * But we only expect (*) gets tail's value is:
	 *     node in the queue
	 * not older value, the older value cause us save a uatomic_cmpxchg() wrongly,
	 * so the pseudocode is not acceptable.
	 *
	 * using uatomic_cmpxchg always is OK, but it adds a uatomic_cmpxchg overhead always:
	 *   uatomic_cmpxchg(&q->tail, head, next);
	 */
	if (queue_node_type(nextnext) == NODE_HEAD) { /* 2 nodes */
		unsigned long tail = rcu_dereference(q->tail);

		/*
		 * tail == next: now is F2, don't need help moving tail
		 * tail != next: it is unlikely when 2 nodes.
		 * Don't believe the orderless-read tail!
		 */
		if (tail != next)
			uatomic_cmpxchg(&q->tail, head, next); /* help for T2 -> F2 */
	}

	/* Fn{n>=2} -> F(n-1), Tn{n>=3} -> T(n-1) */
	if (uatomic_cmpxchg(&q->head.next, head, next) != head)
		return 0;

	return 1;
}

/*
 * Needs to be called with rcu read-side lock held.
 * Wait for a grace period before freeing/reusing the returned node.
 * If NULL is returned, the queue is empty.
 */
static inline
struct cds_lfq_node_rcu *_cds_lfq_dequeue_rcu(struct cds_lfq_queue_rcu *q)
{
	unsigned long head, next;
	struct cds_lfq_node_rcu *phead;

	for (;;) {
		head = rcu_dereference(q->head.next);
		if (queue_node_type(head) == NODE_HEAD) {
			/* F0 or T0b */
			return NULL;
		}

		phead = queue_node_node(head);
		next = rcu_dereference(phead->next);

		if (queue_node_type(next) == NODE_HEAD) { /* T1, F1 */
			/* dequeue when only one node */
			if (__queue_dequeue_the_last(q, head, next, phead))
				goto done;
		} else if (queue_node_type(next) == NODE_NODE) {
			/* dequeue when have >=2 nodes, Tn{n>=2} Fn{n>=2} */
			if (__queue_dequeue(q, head, next))
				goto done;
		} else {
			/* T0a */
			assert(queue_node_type(next) == NODE_NULL);
			return NULL;
		}
	}
done:
	return phead;
}

#ifdef __cplusplus
}
#endif

#endif /* _URCU_RCULFQUEUE_STATIC_H */
