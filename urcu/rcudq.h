#ifndef _CDS_RCUDQ_H
#define _CDS_RCUDQ_H

/*
 * Copyright (C) 2002 Free Software Foundation, Inc.
 * (originally part of the GNU C Library)
 * Contributed by Ulrich Drepper <drepper@redhat.com>, 2002.
 *
 * Copyright (C) 2009 Pierre-Marc Fournier
 * Copyright (C) 2010-2013 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
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

#include <urcu/arch.h>
#include <urcu/compiler.h>
#include <urcu-pointer.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RCU double-ended queue (DQ).
 *
 * Allow consistent forward and backward traversal of the DQ. For
 * instance, given traversals occurring concurrently with a
 * cds_rcudq_add() operation, if a node is seen by a forward RCU
 * traversal, it will be seen by a following backward RCU traversal. The
 * reverse is also true: if seen by backward RCU traversal, it will be
 * seen by a following forward traversal.
 *
 * For node deletion, if forward and backward traversals execute
 * concurrently with cds_rcudq_del(), if the node is not seen by a
 * forward traversal, any following backward traversal is guaranteed not
 * to see it. Likewise for backward traversal followed by forward
 * traversal.
 *
 * Updates are RCU-aware. RCU-protected traversals end with _rcu
 * suffix.
 */

#define CDS_RCUDQ_FLAG_SKIP	(1U << 0)	/* Traversal should skip node */

/* Basic type for the DQ. */
struct cds_rcudq_head {
	struct cds_rcudq_head *next, *prev;
	unsigned int flags;
};

#define CDS_RCUDQ_HEAD_INIT(name) \
	{ .prev = &(name), .next = &(name), .flags = 0 }

/* Define a variable with the head and tail of the DQ. */
#define CDS_RCUDQ_HEAD(name) \
	struct cds_rcudq_head name = CDS_RCUDQ_HEAD_INIT(name)

/* Initialize a new DQ head. */
#define CDS_INIT_RCUDQ_HEAD(ptr) \
	do { \
		(ptr)->next = (ptr)->prev = (ptr); \
		(ptr)->flags = 0; \
	} while (0)

/* Add new element at the head of the DQ. */
static inline
void cds_rcudq_add(struct cds_rcudq_head *newp, struct cds_rcudq_head *head)
{
	newp->next = head->next;
	newp->prev = head;
	newp->flags = CDS_RCUDQ_FLAG_SKIP;
	cmm_smp_wmb();	/* Initialize newp before adding to dq */
	_CMM_STORE_SHARED(head->next->prev, newp);
	_CMM_STORE_SHARED(head->next, newp);
	cmm_smp_wmb();	/* Order adding to dq before showing node */
	CMM_STORE_SHARED(newp->flags, 0);	/* Show node */
}

/* Add new element at the tail of the DQ. */
static inline
void cds_rcudq_add_tail(struct cds_rcudq_head *newp,
		struct cds_rcudq_head *head)
{
	newp->next = head;
	newp->prev = head->prev;
	newp->flags = CDS_RCUDQ_FLAG_SKIP;
	cmm_smp_wmb();	/* Initialize newp before adding to dq */
	_CMM_STORE_SHARED(head->prev->next, newp);
	_CMM_STORE_SHARED(head->prev, newp);
	cmm_smp_wmb();	/* Order adding to dq before showing node */
	CMM_STORE_SHARED(newp->flags, 0);	/* Show node */
}

/* Remove element from list. */
static inline
void cds_rcudq_del(struct cds_rcudq_head *elem)
{
	_CMM_STORE_SHARED(elem->flags, CDS_RCUDQ_FLAG_SKIP);	/* Hide node */
	cmm_smp_wmb();	/* Order hiding node before removing from dq */
	_CMM_STORE_SHARED(elem->next->prev, elem->prev);
	CMM_STORE_SHARED(elem->prev->next, elem->next);
}

static inline
int cds_rcudq_empty(struct cds_rcudq_head *head)
{
	return head == CMM_LOAD_SHARED(head->next);
}

/* Get typed element from list at a given position. */
#define cds_rcudq_entry(ptr, type, member)	\
	caa_container_of(ptr, type, member)

/*
 * Traversals NOT RCU-protected. Need mutual exclusion against updates.
 */

/* Get first entry from a list. */
#define cds_rcudq_first_entry(ptr, type, member) \
	cds_rcudq_entry((ptr)->next, type, member)

/* Iterate forward over the elements of the list. */
#define cds_rcudq_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

/*
 * Iterate forward over the elements list. The list elements can be
 * removed from the list while doing this.
 */
#define cds_rcudq_for_each_safe(pos, p, head) \
	for (pos = (head)->next, p = pos->next; \
		pos != (head); \
		pos = p, p = pos->next)

#define cds_rcudq_for_each_entry(pos, head, member) \
	for (pos = cds_rcudq_entry((head)->next, __typeof__(*pos), member); \
		&pos->member != (head); \
		pos = cds_rcudq_entry(pos->member.next, __typeof__(*pos), member))

#define cds_rcudq_for_each_entry_safe(pos, p, head, member) \
	for (pos = cds_rcudq_entry((head)->next, __typeof__(*pos), member), \
			p = cds_rcudq_entry(pos->member.next, __typeof__(*pos), member); \
		&pos->member != (head); \
		pos = p, p = cds_rcudq_entry(pos->member.next, __typeof__(*pos), member))

/* Iterate backward over the elements of the list. */
#define cds_rcudq_for_each_reverse(pos, head) \
	for (pos = (head)->prev; pos != (head); pos = pos->prev)

/*
 * Iterate backward over the elements list. The list elements can be
 * removed from the list while doing this.
 */
#define cds_rcudq_for_each_reverse_safe(pos, p, head) \
	for (pos = (head)->prev, p = pos->prev; \
		pos != (head); \
		pos = p, p = pos->prev)

#define cds_rcudq_for_each_entry_reverse(pos, head, member) \
	for (pos = cds_rcudq_entry((head)->prev, __typeof__(*pos), member); \
		&pos->member != (head); \
		pos = cds_rcudq_entry(pos->member.prev, __typeof__(*pos), member))

#define cds_rcudq_for_each_entry_reverse_safe(pos, p, head, member) \
	for (pos = cds_rcudq_entry((head)->prev, __typeof__(*pos), member), \
			p = cds_rcudq_entry(pos->member.prev, __typeof__(*pos), member); \
		&pos->member != (head); \
		pos = p, p = cds_rcudq_entry(pos->member.prev, __typeof__(*pos), member))


/*
 * RCU-protected traversals.
 *
 * Iteration through all elements of the list must be done while rcu_read_lock()
 * is held. cds_rcudq_get_next/cds_rcudq_get_prev are helpers to get the
 * next and previous DQ nodes in RCU traversal.
 */

static inline struct cds_rcudq_head *cds_rcudq_get_next(struct cds_rcudq_head *pos,
		struct cds_rcudq_head *head)
{
	unsigned int flags;

	do {
		pos = rcu_dereference(pos->next);
		/* Implicit read barrier ordering load of next pointer before flags */
		flags = CMM_LOAD_SHARED(pos->flags);
		assert(!(flags & CDS_RCUDQ_FLAG_SKIP && pos == head));
	} while (flags & CDS_RCUDQ_FLAG_SKIP);
	return pos;
}

static inline struct cds_rcudq_head *cds_rcudq_get_prev(struct cds_rcudq_head *pos,
		struct cds_rcudq_head *head)
{
	unsigned int flags;

	do {
		pos = rcu_dereference(pos->prev);
		/* Implicit read barrier ordering load of prev pointer before flags */
		flags = CMM_LOAD_SHARED(pos->flags);
		assert(!(flags & CDS_RCUDQ_FLAG_SKIP && pos == head));
	} while (flags & CDS_RCUDQ_FLAG_SKIP);
	return pos;
}

/* Iterate forward over the elements of the list. RCU-protected. */
#define cds_rcudq_for_each_rcu(pos, head) \
	for (pos = cds_rcudq_get_next(head, head); pos != (head); \
		pos = cds_rcudq_get_next(pos, head))

/* Iterate forward through elements of the list. RCU-protected. */
#define cds_rcudq_for_each_entry_rcu(pos, head, member) \
	for (pos = cds_rcudq_entry(cds_rcudq_get_next(head, head), __typeof__(*pos), member); \
		&pos->member != (head); \
		pos = cds_rcudq_entry(cds_rcudq_get_next(&pos->member, head), __typeof__(*pos), member))

/* Iterate backward over the elements of the list. RCU-protected. */
#define cds_rcudq_for_each_reverse_rcu(pos, head) \
	for (pos = cds_rcudq_get_prev(head, head); pos != (head); \
		pos = cds_rcudq_get_prev(pos, head))

/* Iterate forward through elements of the list. RCU-protected. */
#define cds_rcudq_for_each_entry_reverse_rcu(pos, head, member) \
	for (pos = cds_rcudq_entry(cds_rcudq_get_prev(head, head), __typeof__(*pos), member); \
		&pos->member != (head); \
		pos = cds_rcudq_entry(cds_rcudq_get_prev(&pos->member, head), __typeof__(*pos), member))

#ifdef __cplusplus
}
#endif

#endif	/* _CDS_RCUDQ_H */
