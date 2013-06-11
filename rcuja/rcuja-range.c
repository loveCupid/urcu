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
 * Discussion: concurrent deletion of contiguous allocated ranges.
 *
 * Ensuring that merge of contiguous free ranges is always performed, we
 * need to ensure locking of concurrent removal of contiguous allocated
 * ranges one with respect to another. This is done by locking the
 * ranges prior to and after the range to remove, even if that range is
 * allocated. This serializes removal of contiguous ranges. The only
 * cases for which there is no range to lock is when removing an
 * allocated range starting at 0, and/or ending at the end of the key
 * space.
 */

/*
 * Discussion: concurrent lookup vs add
 *
 * When executed concurrently with node add, the inequality
 * lookup can see no node for the looked-up range, because a range can
 * be shrinked. This can happen if, for instance, we lookup key 2
 * between addition of a "free" range for values [1,2], and removal of
 * the old "free" range for values [0,2]. We would then fail to observe
 * any range for key 2. Given that the lookup is performed during a
 * range transition, we can safely return that there is no allocated
 * node in the range.
 */

/*
 * Discussion: concurrent lookup vs del
 *
 * There is no special case for lookups performed concurrently with node
 * del, because node del either replaces the node with the exact same
 * start key (see duplicates guarantees), or replaces it with a larger
 * range containing the prior range. Therefore, we are sure that
 * inequality lookups will see the larger range before the old range is
 * deleted, in whichever direction the lookup is performed.
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

//#define RANGE_DEBUG

#undef dbg_printf

#ifdef RANGE_DEBUG
#define dbg_printf(fmt, args...)				\
	fprintf(stderr, "[debug rcuja-range %lu %s()@%s:%u] " fmt, \
		(unsigned long) gettid(), __func__,		\
		__FILE__, __LINE__, ## args)
#else
#define dbg_printf(fmt, args...)				\
do {								\
	/* do nothing but check printf format */		\
	if (0)							\
		fprintf(stderr, "[debug rcuja-range %lu %s()@%s:%u] " fmt, \
			(unsigned long) gettid(), __func__,	\
			__FILE__, __LINE__, ## args);		\
} while (0)
#endif

enum cds_ja_range_type {
	CDS_JA_RANGE_ALLOCATED,
	CDS_JA_RANGE_FREE,
	CDS_JA_RANGE_REMOVED,
};

/*
 * Range goes from start (inclusive) to end (inclusive).
 * Range start is used as node key in the Judy array.
 */
struct cds_ja_range {
	uint64_t end;
	struct cds_ja_node ja_node;
	pthread_mutex_t lock;
	void *priv;
	enum cds_ja_range_type type;

	/* not required on lookup fast-path */
	uint64_t start;
	struct rcu_head head;
};

struct cds_ja_range *cds_ja_range_lookup(struct cds_ja *ja, uint64_t key)
{
	struct cds_ja_node *node, *last_node;
	struct cds_ja_range *range;

	dbg_printf("key: %" PRIu64 "\n", key);
	node = cds_ja_lookup_below_equal(ja, key, NULL);
	if (!node)
		return NULL;
	/*
	 * Get the last of duplicate chain. Adding a node to Judy array
	 * duplicates inserts them at the end of the chain.
	 */
	cds_ja_for_each_duplicate_rcu(node)
		last_node = node;
	range = caa_container_of(last_node, struct cds_ja_range, ja_node);

	/* Check if range is currently hidden by concurrent add */
	if (range->end < key)
		return NULL;

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
		void *priv,
		enum cds_ja_range_type type)
{
	struct cds_ja_range *range;

	range = calloc(sizeof(*range), 1);
	if (!range)
		return NULL;
	range->start = start;
	range->end = end;
	range->priv = priv;
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

int cds_ja_range_add(struct cds_ja *ja,
		uint64_t start,		/* inclusive */
		uint64_t end,		/* inclusive */
		void *priv)
{
	struct cds_ja_node *old_node;
	struct cds_ja_range *old_range, *new_range, *ranges[3];
	unsigned int nr_ranges, i;
	int ret;

	if (start > end || end == UINT64_MAX)
		return -EINVAL;

retry:
	dbg_printf("start: %" PRIu64 ", end: %" PRIu64 ", priv %p\n",
			start, end, priv);
	/*
	 * Find if requested range is entirely contained within a single
	 * free range.
	 */
	old_node = cds_ja_lookup_below_equal(ja, start, NULL);
	/* Range hidden by concurrent add */
	if (!old_node)
		goto retry;

	old_range = caa_container_of(old_node, struct cds_ja_range, ja_node);

	/* Range hidden by concurrent add */
	if (old_range->end < start)
		goto retry;

	/* We now know that old_range overlaps with our range */
	switch (CMM_LOAD_SHARED(old_range->type)) {
	case CDS_JA_RANGE_ALLOCATED:
		return -EEXIST;
	case CDS_JA_RANGE_FREE:
		break;
	case CDS_JA_RANGE_REMOVED:
		goto retry;
	}

	/* We do not fit entirely within the range */
	if (old_range->end < end)
		return -EEXIST;

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
				priv, CDS_JA_RANGE_ALLOCATED);
			nr_ranges = 1;
		} else {
			/* 2 ranges */
			assert(old_range->end > end);
			ranges[0] = new_range = range_create(start, end,
				priv, CDS_JA_RANGE_ALLOCATED);
			ranges[1] = range_create(end + 1, old_range->end,
				NULL, CDS_JA_RANGE_FREE);
			nr_ranges = 2;
		}
	} else {
		if (end == old_range->end) {
			/* 2 ranges */
			assert(old_range->start < start);
			ranges[0] = range_create(old_range->start, start - 1,
				NULL, CDS_JA_RANGE_FREE);
			ranges[1] = new_range = range_create(start, end,
				priv, CDS_JA_RANGE_ALLOCATED);
			nr_ranges = 2;
		} else {
			/* 3 ranges */
			assert(old_range->start < start);
			assert(old_range->end > end);
			ranges[0] = range_create(old_range->start, start - 1,
				NULL, CDS_JA_RANGE_FREE);
			ranges[1] = new_range = range_create(start, end,
				priv, CDS_JA_RANGE_ALLOCATED);
			ranges[2] = range_create(end + 1, old_range->end,
				NULL, CDS_JA_RANGE_FREE);
			nr_ranges = 3;
		}
	}

	/* Add replacement ranges to Judy array */
	for (i = 0; i < nr_ranges; i++) {
		dbg_printf("ADD RANGE: %" PRIu64 "-%" PRIu64 " %s.\n",
			ranges[i]->start, ranges[i]->end,
			ranges[i]->type == CDS_JA_RANGE_ALLOCATED ?
				"allocated" : "free");
		pthread_mutex_lock(&ranges[i]->lock);
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

	dbg_printf("REM RANGE: %" PRIu64 "-%" PRIu64 " %s.\n",
		old_range->start, old_range->end,
		old_range->type == CDS_JA_RANGE_ALLOCATED ?
			"allocated" : "free");
	/* Remove old free range */
	ret = cds_ja_del(ja, old_range->start, &old_range->ja_node);
	assert(!ret);
	old_range->type = CDS_JA_RANGE_REMOVED;
	pthread_mutex_unlock(&old_range->lock);
	for (i = 0; i < nr_ranges; i++)
		pthread_mutex_unlock(&ranges[i]->lock);

	rcu_free_range(ja, old_range);

	dbg_printf("<SUCCEED>\n");

	return 0;
}

int cds_ja_range_del(struct cds_ja *ja, struct cds_ja_range *range)
{
	struct cds_ja_node *prev_node, *next_node;
	struct cds_ja_range *new_range;
	struct cds_ja_range *merge_ranges[3], *lock_ranges[3];
	unsigned int nr_merge, nr_lock, i;
	uint64_t start, end;
	int ret;

retry:
	dbg_printf("start: %" PRIu64 ", end %" PRIu64 ", priv: %p\n",
			range->start, range->end, range->priv);

	nr_merge = 0;
	nr_lock = 0;

	/*
	 * Range has been concurrently updated.
	 */
	if (range->type != CDS_JA_RANGE_ALLOCATED)
		return -ENOENT;

	if (range->start > 0) {
		struct cds_ja_range *prev_range;

		prev_node = cds_ja_lookup_below_equal(ja, range->start - 1,
			NULL);
		if (!prev_node)
			goto retry;

		prev_range = caa_container_of(prev_node,
			struct cds_ja_range, ja_node);
		/* Prev range temporarily hidden due to concurrent add. */
		if (prev_range->end != range->start - 1)
			goto retry;

		lock_ranges[nr_lock++] = prev_range;
		if (prev_range->type != CDS_JA_RANGE_ALLOCATED)
			merge_ranges[nr_merge++] = prev_range;
	}

	lock_ranges[nr_lock++] = range;
	merge_ranges[nr_merge++] = range;

	if (range->end < UINT64_MAX - 1) {
		struct cds_ja_range *next_range;

		next_node = cds_ja_lookup_below_equal(ja, range->end + 1,
			NULL);
		/* Next range temporarily hidden due to concurrent add. */
		if (!next_node)
			goto retry;

		next_range = caa_container_of(next_node,
			struct cds_ja_range, ja_node);
		if (next_range->start != range->end + 1)
			goto retry;

		lock_ranges[nr_lock++] = next_range;
		if (next_range->type != CDS_JA_RANGE_ALLOCATED)
			merge_ranges[nr_merge++] = next_range;
	}

	/* Acquire locks in increasing key order for range merge */
	for (i = 0; i < nr_lock; i++)
		pthread_mutex_lock(&lock_ranges[i]->lock);
	if (range->type != CDS_JA_RANGE_ALLOCATED) {
		ret = -ENOENT;
		goto unlock_error;
	}
	/* Ensure they are valid */
	for (i = 0; i < nr_lock; i++) {
		if (lock_ranges[i]->type == CDS_JA_RANGE_REMOVED)
			goto unlock_retry;
	}

	/* Create new free range */
	start = merge_ranges[0]->start;
	end = merge_ranges[nr_merge - 1]->end;
	new_range = range_create(start, end, NULL, CDS_JA_RANGE_FREE);
	pthread_mutex_lock(&new_range->lock);

	dbg_printf("ADD RANGE: %" PRIu64 "-%" PRIu64 " %s.\n",
		new_range->start, new_range->end,
		new_range->type == CDS_JA_RANGE_ALLOCATED ?
			"allocated" : "free");

	ret = cds_ja_add(ja, start, &new_range->ja_node);
	assert(!ret);

	/* Remove old ranges */
	for (i = 0; i < nr_merge; i++) {

		dbg_printf("REM RANGE: %" PRIu64 "-%" PRIu64 " %s.\n",
			merge_ranges[i]->start, merge_ranges[i]->end,
			merge_ranges[i]->type == CDS_JA_RANGE_ALLOCATED ?
				"allocated" : "free");
		ret = cds_ja_del(ja, merge_ranges[i]->start,
				&merge_ranges[i]->ja_node);
		assert(!ret);
		merge_ranges[i]->type = CDS_JA_RANGE_REMOVED;
	}
	for (i = 0; i < nr_lock; i++)
		pthread_mutex_unlock(&lock_ranges[i]->lock);
	pthread_mutex_unlock(&new_range->lock);
	/* Free old merged ranges */
	for (i = 0; i < nr_merge; i++)
		rcu_free_range(ja, merge_ranges[i]);

	dbg_printf("<SUCCEED>\n");

	return 0;

	/* retry paths */
unlock_retry:
	for (i = 0; i < nr_lock; i++)
		pthread_mutex_unlock(&lock_ranges[i]->lock);
	goto retry;
	/* error paths */
unlock_error:
	for (i = 0; i < nr_lock; i++)
		pthread_mutex_unlock(&lock_ranges[i]->lock);
	return ret;
}

struct cds_ja *_cds_ja_range_new(unsigned int key_bits,
		const struct rcu_flavor_struct *flavor)
{
	struct cds_ja_range *range;
	struct cds_ja *ja;
	int ret;

	ja = _cds_ja_new(key_bits, flavor);
	if (!ja)
		return NULL;
	range = range_create(0, UINT64_MAX - 1, NULL, CDS_JA_RANGE_FREE);
	if (!range)
		goto free_ja;
	cds_lfht_rcu_flavor(ja->ht)->read_lock();
	ret = cds_ja_add(ja, 0, &range->ja_node);
	cds_lfht_rcu_flavor(ja->ht)->read_unlock();
	if (ret)
		goto free_range;
	return ja;

free_range:
	free_range(range);
free_ja:
	ret = cds_ja_destroy(ja);
	assert(!ret);
	return NULL;
}

int cds_ja_range_validate(struct cds_ja *ja)
{
	uint64_t iter_key, start, end, last_end = UINT64_MAX;
	struct cds_ja_node *ja_node, *last_node;
	int ret = 0;

	cds_lfht_rcu_flavor(ja->ht)->read_lock();
	cds_ja_for_each_key_rcu(ja, iter_key, ja_node) {
		struct cds_ja_range *range;
		struct cds_ja_node *first_node;

		first_node = ja_node;
		cds_ja_for_each_duplicate_rcu(ja_node)
			last_node = ja_node;
		if (last_node != first_node) {
			struct cds_ja_range *first_range = caa_container_of(first_node,
				struct cds_ja_range, ja_node);
			struct cds_ja_range *last_range = caa_container_of(last_node,
				struct cds_ja_range, ja_node);
			fprintf(stderr, "found duplicate node: first %" PRIu64 "-%" PRIu64 " last %" PRIu64 "-%" PRIu64 "\n",
				first_range->start, first_range->end, last_range->start, last_range->end);
				ret |= -1;
		}
		range = caa_container_of(last_node,
			struct cds_ja_range, ja_node);
		start = range->start;
		end = range->end;
		if (last_end != UINT64_MAX) {
			if (start != last_end + 1) {
				fprintf(stderr, "ja range discrepancy: last end: %" PRIu64 ", start: %" PRIu64 "\n",
					last_end, start);
				ret |= -1;
			}
		}
		last_end = end;
	}
	if (last_end != UINT64_MAX - 1) {
		fprintf(stderr, "ja range error: end of last range is: %" PRIu64 "\n",
			last_end);
		ret |= 1;
	}
	cds_lfht_rcu_flavor(ja->ht)->read_unlock();
	return ret;
}

int cds_ja_range_destroy(struct cds_ja *ja,
		void (*free_priv)(void *ptr))
{
	uint64_t key;
	struct cds_ja_node *ja_node;
	int ret = 0;

	cds_lfht_rcu_flavor(ja->ht)->read_lock();
	cds_ja_for_each_key_rcu(ja, key, ja_node) {
		struct cds_ja_node *tmp_node;

		cds_ja_for_each_duplicate_safe_rcu(ja_node, tmp_node) {
			struct cds_ja_range *range;

			range = caa_container_of(ja_node,
					struct cds_ja_range, ja_node);
			ret = cds_ja_del(ja, key, &range->ja_node);
			if (ret)
				goto error;
			if (free_priv)
				free_priv(range->priv);
			/* Alone using Judy array, OK to free now */
			free_range(range);
		}
	}
	cds_lfht_rcu_flavor(ja->ht)->read_unlock();
	return cds_ja_destroy(ja);

error:
	cds_lfht_rcu_flavor(ja->ht)->read_unlock();
	return ret;
}
