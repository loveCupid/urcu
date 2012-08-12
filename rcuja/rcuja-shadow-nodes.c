/*
 * rcuja/rcuja-hashtable.c
 *
 * Userspace RCU library - RCU Judy Array Shadow Node Hash Table
 *
 * Copyright 2012 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
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

/*
 * The hash table used by judy array updates only for the shadow node
 * mapping rely on standard urcu_mb flavor. It does not put any
 * requirement on the RCU flavor used by applications using the judy
 * array.
 */
#include <urcu.h>

#include <urcu/rcuja.h>
#include <urcu/compiler.h>
#include <urcu/arch.h>
#include <assert.h>
#include <urcu-pointer.h>
#include <stdlib.h>
#include <time.h>

#include "rcuja-internal.h"
#include "bitfield.h"

static unsigned long hash_seed;

/*
 * Hash function
 * Source: http://burtleburtle.net/bob/c/lookup3.c
 * Originally Public Domain
 */

#define rot(x, k) (((x) << (k)) | ((x) >> (32 - (k))))

#define mix(a, b, c) \
do { \
	a -= c; a ^= rot(c,  4); c += b; \
	b -= a; b ^= rot(a,  6); a += c; \
	c -= b; c ^= rot(b,  8); b += a; \
	a -= c; a ^= rot(c, 16); c += b; \
	b -= a; b ^= rot(a, 19); a += c; \
	c -= b; c ^= rot(b,  4); b += a; \
} while (0)

#define final(a, b, c) \
{ \
	c ^= b; c -= rot(b, 14); \
	a ^= c; a -= rot(c, 11); \
	b ^= a; b -= rot(a, 25); \
	c ^= b; c -= rot(b, 16); \
	a ^= c; a -= rot(c,  4);\
	b ^= a; b -= rot(a, 14); \
	c ^= b; c -= rot(b, 24); \
}

static inline __attribute__((unused))
uint32_t hash_u32(
	const uint32_t *k,	/* the key, an array of uint32_t values */
	size_t length,		/* the length of the key, in uint32_ts */
	uint32_t initval)	/* the previous hash, or an arbitrary value */
{
	uint32_t a, b, c;

	/* Set up the internal state */
	a = b = c = 0xdeadbeef + (((uint32_t) length) << 2) + initval;

	/*----------------------------------------- handle most of the key */
	while (length > 3) {
		a += k[0];
		b += k[1];
		c += k[2];
		mix(a, b, c);
		length -= 3;
		k += 3;
	}

	/*----------------------------------- handle the last 3 uint32_t's */
	switch (length) {	/* all the case statements fall through */
	case 3: c += k[2];
	case 2: b += k[1];
	case 1: a += k[0];
		final(a, b, c);
	case 0:			/* case 0: nothing left to add */
		break;
	}
	/*---------------------------------------------- report the result */
	return c;
}

static inline
void hashword2(
	const uint32_t *k,	/* the key, an array of uint32_t values */
	size_t length,		/* the length of the key, in uint32_ts */
	uint32_t *pc,		/* IN: seed OUT: primary hash value */
	uint32_t *pb)		/* IN: more seed OUT: secondary hash value */
{
	uint32_t a, b, c;

	/* Set up the internal state */
	a = b = c = 0xdeadbeef + ((uint32_t) (length << 2)) + *pc;
	c += *pb;

	/*----------------------------------------- handle most of the key */
	while (length > 3) {
		a += k[0];
		b += k[1];
		c += k[2];
		mix(a, b, c);
		length -= 3;
		k += 3;
	}

	/*----------------------------------- handle the last 3 uint32_t's */
	switch (length) {	/* all the case statements fall through */
	case 3: c += k[2];
	case 2: b += k[1];
	case 1: a += k[0];
		final(a, b, c);
	case 0:			/* case 0: nothing left to add */
		break;
	}
	/*---------------------------------------------- report the result */
	*pc = c;
	*pb = b;
}

#if (CAA_BITS_PER_LONG == 32)
static
unsigned long hash_pointer(const void *_key, unsigned long seed)
{
	unsigned int key = (unsigned int) _key;

	return hash_u32(&key, 1, seed);
}
#else
static
unsigned long hash_pointer(const void *_key, unsigned long seed)
{
	union {
		uint64_t v64;
		uint32_t v32[2];
	} v;
	union {
		uint64_t v64;
		uint32_t v32[2];
	} key;

	v.v64 = (uint64_t) seed;
	key.v64 = (uint64_t) _key;
	hashword2(key.v32, 2, &v.v32[0], &v.v32[1]);
	return v.v64;
}
#endif

static
int match_pointer(struct cds_lfht_node *node, const void *key)
{
	struct rcu_ja_shadow_node *shadow =
		caa_container_of(node, struct rcu_ja_shadow_node, ht_node);

	return (key == shadow->node);
}

__attribute__((visibility("protected")))
struct rcu_ja_shadow_node *rcuja_shadow_lookup_lock(struct cds_lfht *ht,
		struct rcu_ja_node *node)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *lookup_node;
	struct rcu_ja_shadow_node *shadow_node;
	int ret;

	rcu_read_lock();
	cds_lfht_lookup(ht, hash_pointer(node, hash_seed),
			match_pointer, node, &iter);

	lookup_node = cds_lfht_iter_get_node(&iter);
	if (!lookup_node) {
		shadow_node = NULL;
		goto rcu_unlock;
	}
	shadow_node = caa_container_of(lookup_node,
			struct rcu_ja_shadow_node, ht_node);
	ret = pthread_mutex_lock(&shadow_node->lock);
	assert(!ret);
	if (cds_lfht_is_node_deleted(lookup_node)) {
		ret = pthread_mutex_unlock(&shadow_node->lock);
		assert(!ret);
		shadow_node = NULL;
	}
rcu_unlock:
	rcu_read_unlock();
	return shadow_node;
}

void rcuja_shadow_unlock(struct rcu_ja_shadow_node *shadow_node)
{
	int ret;

	ret = pthread_mutex_unlock(&shadow_node->lock);
	assert(!ret);
}

__attribute__((visibility("protected")))
int rcuja_shadow_set(struct cds_lfht *ht,
		struct rcu_ja_node *node)
{
	struct rcu_ja_shadow_node *shadow_node;
	struct cds_lfht_node *ret_node;

	shadow_node = calloc(sizeof(*shadow_node), 1);
	if (!shadow_node)
		return -ENOMEM;

	shadow_node->node = node;
	pthread_mutex_init(&shadow_node->lock, NULL);

	rcu_read_lock();
	ret_node = cds_lfht_add_unique(ht,
			hash_pointer(node, hash_seed),
			match_pointer,
			node,
			&shadow_node->ht_node);
	rcu_read_unlock();

	if (ret_node != &shadow_node->ht_node) {
		free(shadow_node);
		return -EEXIST;
	}
	return 0;
}

static
void free_shadow_node(struct rcu_head *head)
{
	struct rcu_ja_shadow_node *shadow_node =
		caa_container_of(head, struct rcu_ja_shadow_node, head);
	free(shadow_node);
}

__attribute__((visibility("protected")))
int rcuja_shadow_clear(struct cds_lfht *ht,
		struct rcu_ja_node *node)
{
	struct cds_lfht_iter iter;
	struct cds_lfht_node *lookup_node;
	struct rcu_ja_shadow_node *shadow_node;
	int ret, lockret;

	rcu_read_lock();
	cds_lfht_lookup(ht, hash_pointer(node, hash_seed),
			match_pointer, node, &iter);
	lookup_node = cds_lfht_iter_get_node(&iter);
	if (!lookup_node) {
		ret = -ENOENT;
		goto rcu_unlock;
	}
	shadow_node = caa_container_of(lookup_node,
			struct rcu_ja_shadow_node, ht_node);
	lockret = pthread_mutex_lock(&shadow_node->lock);
	assert(!lockret);

	/*
	 * Holding the mutex across deletion, and by also re-checking if
	 * the node is deleted with mutex held at lookup ensure that we
	 * don't let RCU JA use a node being removed.
	 */
	ret = cds_lfht_del(ht, lookup_node);
	if (!ret) {
		call_rcu(&shadow_node->head, free_shadow_node);
	}
	lockret = pthread_mutex_unlock(&shadow_node->lock);
	assert(!lockret);
rcu_unlock:
	rcu_read_unlock();

	return ret;
}

__attribute__((visibility("protected")))
struct cds_lfht *rcuja_create_ht(void)
{
	return cds_lfht_new(1, 1, 0,
		CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING,
		NULL);
}

__attribute__((visibility("protected")))
void rcuja_delete_ht(struct cds_lfht *ht)
{
	int ret;

	ret = cds_lfht_destroy(ht, NULL);
	assert(!ret);
}

__attribute__((constructor))
void rcuja_ht_init(void)
{
	hash_seed = (unsigned long) time(NULL);
}
