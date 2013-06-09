/*
 * rcuja/rcuja-range.c
 *
 * Userspace RCU library - RCU Judy Array Range Support
 *
 * Copyright 2012-2013 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
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

#define _LGPL_SOURCE
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <urcu/rcuja.h>
#include <urcu/compiler.h>
#include <urcu/arch.h>
#include <urcu-pointer.h>
#include <urcu/uatomic.h>
#include <urcu/rcuja-range.h>
#include <urcu-flavor.h>

#include "rcuja-internal.h"

/*
 * Discussion about order of lookup/lock vs allocated node deletion.
 *
 * - If node deletion returns before call to
 *   cds_ja_range_lookup(), the node will not be found by lookup.
 * - If node deletion is called after cds_ja_range_lock() returns a
 *   non-NULL range, the deletion will wait until the lock is released
 *   before it takes place.
 * - If node deletion call/return overlaps with the call to
 *   cds_ja_range_lookup() and return from cds_ja_range_lock(), the node
 *   may or may not be found by each of cds_ja_range_lookup() and
 *   cds_ja_range_lock().
 */

/*
 * Discussion about order of lookup/lock vs allocated node add. Assuming
 * no concurrent delete.
 *
 * - If node add returns before call to
 *   cds_ja_range_lookup(), the node will be found by lookup.
 * - If node add is called after cds_ja_range_lookup returns, the node
 *   will not be found by lookup.
 * - If node add call/return overlaps with the call to and return from
 *   cds_ja_range_lookup(), the node may or may not be found.
 * - If node add call/return overlaps with call to cds_ja_range_lookup()
 *   and return from cds_ja_range_lock(), in the specific case where
 *   cds_ja_range_lookup() _does_ succeed, then cds_ja_range_lock() will
 *   succeed (still assuming no concurrent deletion).
 */

/*
 * Discussion of the type state transitions.
 *
 * State transitions of "type" always go from either:
 *
 * CDS_JA_RANGE_FREE -> CDS_JA_RANGE_REMOVED
 * or
 * CDS_JA_RANGE_ALLOCATED -> CDS_JA_RANGE_REMOVED
 *
 * A range type never changes otherwise.
 */


struct cds_ja_range *cds_ja_range_lookup(struct cds_ja *ja, uint64_t key)
{
	struct cds_ja_node *node, *last_node;
	struct cds_ja_range *range;

	node = cds_ja_lookup_below_equal(ja, key, NULL);
	assert(node);
	/*
	 * Get the last of duplicate chain. Adding a node to Judy array
	 * duplicates inserts them at the end of the chain.
	 */
	cds_ja_for_each_duplicate_rcu(node)
		last_node = node;
	range = caa_container_of(last_node, struct cds_ja_range, ja_node);
	/*
	 * If last node in the duplicates is removed or free, we can
	 * consider that either a removal or add operation is in
	 * progress, or removal is the last completed operation to
	 * update this range. We can therefore consider that this area
	 * is not allocated.
	 */
	if (range->type != CDS_JA_RANGE_ALLOCATED)
		return NULL;
	/*
	 * We found an allocated range. We can return it for use with
	 * RCU read-side protection for existence. However, we have no
	 * mutual exclusion against removal at this point.
	 */
	return range;
}

/*
 * Provide mutual exclusion against removal.
 */
struct cds_ja_range *cds_ja_range_lock(struct cds_ja_range *range)
{
	pthread_mutex_lock(&range->lock);

	if (range->type == CDS_JA_RANGE_REMOVED)
		goto removed;
	return range;

removed:
	pthread_mutex_unlock(&range->lock);
	return NULL;
}

void cds_ja_range_unlock(struct cds_ja_range *range)
{
	pthread_mutex_unlock(&range->lock);
}

static
struct cds_ja_range *range_create(
		uint64_t start,		/* inclusive */
		uint64_t end,		/* inclusive */
		enum cds_ja_range_type type)
{
	struct cds_ja_range *range;

	range = calloc(sizeof(*range), 1);
	if (!range)
		return NULL;
	range->start = start;
	range->end = end;
	range->type = type;
	pthread_mutex_init(&range->lock, NULL);
	return range;
}

static
void free_range_cb(struct rcu_head *head)
{
	struct cds_ja_range *range =
		caa_container_of(head, struct cds_ja_range, head);
	free(range);
}

static
void free_range(struct cds_ja_range *range)
{
	free(range);
}

static
void rcu_free_range(struct cds_ja *ja, struct cds_ja_range *range)
{
	cds_lfht_rcu_flavor(ja->ht)->update_call_rcu(&range->head,
			free_range_cb);
}

struct cds_ja_range *cds_ja_range_add(struct cds_ja *ja,
		uint64_t start,		/* inclusive */
		uint64_t end)		/* inclusive */
{
	struct cds_ja_node *old_node, *old_node_end;
	struct cds_ja_range *old_range, *old_range_end, *new_range, *ranges[3];
	unsigned int nr_ranges, i;
	int ret;

retry:
	/*
	 * Find if requested range is entirely contained within a single
	 * free range.
	 */
	old_node = cds_ja_lookup_below_equal(ja, start, NULL);
	assert(old_node);

	old_range = caa_container_of(old_node, struct cds_ja_range, ja_node);
	switch (CMM_LOAD_SHARED(old_range->type)) {
	case CDS_JA_RANGE_ALLOCATED:
		return NULL;
	case CDS_JA_RANGE_FREE:
		break;
	case CDS_JA_RANGE_REMOVED:
		goto retry;
	}

	old_node_end = cds_ja_lookup_below_equal(ja, end, NULL);
	assert(old_node_end);
	old_range_end = caa_container_of(old_node_end,
			struct cds_ja_range, ja_node);
	if (old_range_end != old_range) {
		switch (CMM_LOAD_SHARED(old_range->type)) {
		case CDS_JA_RANGE_ALLOCATED:
		case CDS_JA_RANGE_FREE:		/* fall-through */
			/*
			 * TODO: think about free range merge.
			 */
			return NULL;
		case CDS_JA_RANGE_REMOVED:
			goto retry;
		}
	}

	pthread_mutex_lock(&old_range->lock);

	if (old_range->type == CDS_JA_RANGE_REMOVED) {
		pthread_mutex_unlock(&old_range->lock);
		goto retry;
	}

	/* Create replacement ranges: at most 2 free and 1 allocated */
	if (start == old_range->start) {
		if (end == old_range->end) {
			/* 1 range */
			ranges[0] = new_range = range_create(start, end,
				CDS_JA_RANGE_ALLOCATED);
			nr_ranges = 1;
		} else {
			/* 2 ranges */
			ranges[0] = new_range = range_create(start, end,
				CDS_JA_RANGE_ALLOCATED);
			ranges[1] = range_create(end + 1, old_range->end,
				CDS_JA_RANGE_FREE);
			nr_ranges = 2;
		}
	} else {
		if (end == old_range->end) {
			/* 2 ranges */
			ranges[0] = range_create(old_range->start, start - 1,
				CDS_JA_RANGE_FREE);
			ranges[1] = new_range = range_create(start, end,
				CDS_JA_RANGE_ALLOCATED);
			nr_ranges = 2;
		} else {
			/* 3 ranges */
			ranges[0] = range_create(old_range->start, start - 1,
				CDS_JA_RANGE_FREE);
			ranges[1] = new_range = range_create(start, end,
				CDS_JA_RANGE_ALLOCATED);
			ranges[2] = range_create(end + 1, old_range->end,
				CDS_JA_RANGE_FREE);
			nr_ranges = 3;
		}
	}

	/* Add replacement ranges to Judy array */
	for (i = 0; i < nr_ranges; i++) {
		ret = cds_ja_add(ja, ranges[i]->start, &ranges[i]->ja_node);
		assert(!ret);
	}

	/*
	 * We add replacement ranges _before_ removing old ranges, so
	 * concurrent traversals will always see one or the other. This
	 * is OK because we temporarily have a duplicate key, and Judy
	 * arrays provide key existence guarantee for lookups performed
	 * concurrently with add followed by del of duplicate keys.
	 */

	/* Remove old free range */
	ret = cds_ja_del(ja, old_range->start, &old_range->ja_node);
	assert(!ret);
	old_range->type = CDS_JA_RANGE_REMOVED;
	pthread_mutex_unlock(&old_range->lock);

	rcu_free_range(ja, old_range);

	return new_range;
}

int cds_ja_range_del(struct cds_ja *ja, struct cds_ja_range *range)
{
	struct cds_ja_node *prev_node, *next_node;
	struct cds_ja_range *prev_range, *next_range, *new_range;
	struct cds_ja_range *merge_ranges[3];
	unsigned int nr_merge, i;
	uint64_t start, end;
	int ret;

retry:
	nr_merge = 0;
	prev_node = cds_ja_lookup_below_equal(ja, range->start - 1, NULL);
	prev_range = caa_container_of(prev_node, struct cds_ja_range, ja_node);
	if (prev_node && prev_range->type != CDS_JA_RANGE_ALLOCATED)
		merge_ranges[nr_merge++] = prev_range;

	merge_ranges[nr_merge++] = range;

	next_node = cds_ja_lookup_above_equal(ja, range->end + 1, NULL);
	next_range = caa_container_of(next_node, struct cds_ja_range, ja_node);
	if (next_node && next_range->type != CDS_JA_RANGE_ALLOCATED)
		merge_ranges[nr_merge++] = next_range;

	/* Acquire locks in increasing key order for range merge */
	for (i = 0; i < nr_merge; i++)
		pthread_mutex_lock(&merge_ranges[i]->lock);
	/* Ensure they are valid */
	for (i = 0; i < nr_merge; i++) {
		if (merge_ranges[i]->type == CDS_JA_RANGE_REMOVED)
			goto unlock_retry;
	}

	/* Create new free range */
	start = merge_ranges[0]->start;
	end = merge_ranges[nr_merge - 1]->end;
	new_range = range_create(start, end, CDS_JA_RANGE_FREE);
	ret = cds_ja_add(ja, start, &new_range->ja_node);
	assert(!ret);

	/* Remove old ranges */
	for (i = 0; i < nr_merge; i++) {
		ret = cds_ja_del(ja, merge_ranges[i]->start,
				&merge_ranges[i]->ja_node);
		assert(!ret);
		merge_ranges[i]->type = CDS_JA_RANGE_REMOVED;
		pthread_mutex_unlock(&merge_ranges[i]->lock);
		rcu_free_range(ja, merge_ranges[i]);
	}

	return 0;

	/* retry paths */
unlock_retry:
	for (i = 0; i < nr_merge; i++)
		pthread_mutex_unlock(&merge_ranges[i]->lock);
	goto retry;
}

int cds_ja_range_init(struct cds_ja *ja)
{
	struct cds_ja_node *node;
	struct cds_ja_range *range;
	int ret;

	/*
	 * Sanity check: should be empty.
	 */
	node = cds_ja_lookup_above_equal(ja, 0, NULL);
	if (node)
		return -EINVAL;
	range = range_create(0, UINT64_MAX, CDS_JA_RANGE_FREE);
	if (!range)
		return -EINVAL;
	ret = cds_ja_add(ja, 0, &range->ja_node);
	return ret;
}

int cds_ja_range_fini(struct cds_ja *ja)
{
	uint64_t key;
	struct cds_ja_node *ja_node;
	int ret = 0;

	cds_ja_for_each_key_rcu(ja, key, ja_node) {
		struct cds_ja_node *tmp_node;

		cds_ja_for_each_duplicate_safe_rcu(ja_node, tmp_node) {
			struct cds_ja_range *range;

			range = caa_container_of(ja_node,
					struct cds_ja_range, ja_node);
			ret = cds_ja_del(ja, key, &range->ja_node);
			if (ret) {
				goto end;
			}
			/* Alone using Judy array, OK to free now */
			free_range(range);
		}
	}
end:
	return ret;
}
