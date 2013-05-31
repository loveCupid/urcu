/*
 * test_urcu_ja.c
 *
 * Userspace RCU library - test program
 *
 * Copyright 2009-2012 - Mathieu Desnoyers <mathieu.desnoyers@polymtl.ca>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE
#include "test_urcu_ja.h"
#include <inttypes.h>
#include <stdint.h>

DEFINE_URCU_TLS(unsigned int, rand_lookup);
DEFINE_URCU_TLS(unsigned long, nr_add);
DEFINE_URCU_TLS(unsigned long, nr_addexist);
DEFINE_URCU_TLS(unsigned long, nr_del);
DEFINE_URCU_TLS(unsigned long, nr_delnoent);
DEFINE_URCU_TLS(unsigned long, lookup_fail);
DEFINE_URCU_TLS(unsigned long, lookup_ok);

struct cds_ja *test_ja;

volatile int test_go, test_stop;

unsigned long wdelay;

unsigned long duration;

/* read-side C.S. duration, in loops */
unsigned long rduration;

unsigned long init_populate;
int add_only;

unsigned long init_pool_offset, lookup_pool_offset, write_pool_offset;
unsigned long init_pool_size = DEFAULT_RAND_POOL,
	lookup_pool_size = DEFAULT_RAND_POOL,
	write_pool_size = DEFAULT_RAND_POOL;
int validate_lookup;
int sanity_test;
unsigned int key_bits = 32;

int count_pipe[2];

int verbose_mode;

unsigned int cpu_affinities[NR_CPUS];
unsigned int next_aff = 0;
int use_affinity = 0;

pthread_mutex_t affinity_mutex = PTHREAD_MUTEX_INITIALIZER;

DEFINE_URCU_TLS(unsigned long long, nr_writes);
DEFINE_URCU_TLS(unsigned long long, nr_reads);

unsigned int nr_readers;
unsigned int nr_writers;

static unsigned int add_ratio = 50;
static uint64_t key_mul = 1ULL;

static int add_unique, add_replace;

static pthread_mutex_t rcu_copy_mutex = PTHREAD_MUTEX_INITIALIZER;

void set_affinity(void)
{
	cpu_set_t mask;
	int cpu;
	int ret;

	if (!use_affinity)
		return;

#if HAVE_SCHED_SETAFFINITY
	ret = pthread_mutex_lock(&affinity_mutex);
	if (ret) {
		perror("Error in pthread mutex lock");
		exit(-1);
	}
	cpu = cpu_affinities[next_aff++];
	ret = pthread_mutex_unlock(&affinity_mutex);
	if (ret) {
		perror("Error in pthread mutex unlock");
		exit(-1);
	}
	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
#if SCHED_SETAFFINITY_ARGS == 2
	sched_setaffinity(0, &mask);
#else
        sched_setaffinity(0, sizeof(mask), &mask);
#endif
#endif /* HAVE_SCHED_SETAFFINITY */
}

void rcu_copy_mutex_lock(void)
{
	int ret;
	ret = pthread_mutex_lock(&rcu_copy_mutex);
	if (ret) {
		perror("Error in pthread mutex lock");
		exit(-1);
	}
}

void rcu_copy_mutex_unlock(void)
{
	int ret;

	ret = pthread_mutex_unlock(&rcu_copy_mutex);
	if (ret) {
		perror("Error in pthread mutex unlock");
		exit(-1);
	}
}

void free_node_cb(struct rcu_head *head)
{
	struct ja_test_node *node =
		caa_container_of(head, struct ja_test_node, node.head);
	poison_free(node);
}

#if 0
static
void test_delete_all_nodes(struct cds_lfht *ht)
{
	struct cds_lfht_iter iter;
	struct lfht_test_node *node;
	unsigned long count = 0;

	cds_lfht_for_each_entry(ht, &iter, node, node) {
		int ret;

		ret = cds_lfht_del(test_ht, cds_lfht_iter_get_node(&iter));
		assert(!ret);
		call_rcu(&node->head, free_node_cb);
		count++;
	}
	printf("deleted %lu nodes.\n", count);
}
#endif

void show_usage(int argc, char **argv)
{
	printf("Usage : %s nr_readers nr_writers duration (s)\n", argv[0]);
#ifdef DEBUG_YIELD
	printf("        [-r] [-w] (yield reader and/or writer)\n");
#endif
	printf("        [-d delay] (writer period (us))\n");
	printf("        [-c duration] (reader C.S. duration (in loops))\n");
	printf("        [-v] (verbose output)\n");
	printf("        [-a cpu#] [-a cpu#]... (affinity)\n");
	printf("        [-u] Add unique keys.\n");
	printf("        [-s] Replace existing keys.\n");
printf("        [not -u nor -s] Add entries (supports redundant keys).\n");
	printf("        [-r ratio] Add ratio (in %% of add+removal).\n");
	printf("        [-k] Populate init nodes.\n");
	printf("        [-R offset] Lookup pool offset.\n");
	printf("        [-S offset] Write pool offset.\n");
	printf("        [-T offset] Init pool offset.\n");
	printf("        [-M size] Lookup pool size.\n");
	printf("        [-N size] Write pool size.\n");
	printf("        [-O size] Init pool size.\n");
	printf("        [-V] Validate lookups of init values (use with filled init pool, same lookup range, with different write range).\n");
	printf("        [-t] Do sanity test.\n");
	printf("        [-B] Key bits for multithread test (default: 32).\n");
	printf("        [-m factor] Key multiplication factor.\n");
	printf("\n\n");
}


static
int test_8bit_key(void)
{
	int ret;
	uint64_t key;

	/* Test with 8-bit key */
	test_ja = cds_ja_new(8);
	if (!test_ja) {
		printf("Error allocating judy array.\n");
		return -1;
	}

	/* Add keys */
	printf("Test #1: add keys (8-bit).\n");
	for (key = 0; key < 200; key++) {
		struct ja_test_node *node =
			calloc(sizeof(*node), 1);

		ja_test_node_init(node, key);
		rcu_read_lock();
		ret = cds_ja_add(test_ja, key, &node->node);
		rcu_read_unlock();
		if (ret) {
			fprintf(stderr, "Error (%d) adding node %" PRIu64 "\n",
				ret, key);
			assert(0);
		}
	}
	printf("OK\n");

	printf("Test #2: successful key lookup (8-bit).\n");
	for (key = 0; key < 200; key++) {
		struct cds_hlist_head head;

		rcu_read_lock();
		head = cds_ja_lookup(test_ja, key);
		if (cds_hlist_empty(&head)) {
			fprintf(stderr, "Error lookup node %" PRIu64 "\n", key);
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");
	printf("Test #3: unsuccessful key lookup (8-bit).\n");
	for (key = 200; key < 240; key++) {
		struct cds_hlist_head head;

		rcu_read_lock();
		head = cds_ja_lookup(test_ja, key);
		if (!cds_hlist_empty(&head)) {
			fprintf(stderr,
				"Error unexpected lookup node %" PRIu64 "\n",
				key);
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");
	printf("Test #4: remove keys (8-bit).\n");
	for (key = 0; key < 200; key++) {
		struct cds_hlist_head head;
		struct ja_test_node *node;

		rcu_read_lock();
		head = cds_ja_lookup(test_ja, key);
		node = cds_hlist_first_entry_rcu(&head, struct ja_test_node, node.list);
		if (!node) {
			fprintf(stderr, "Error lookup node %" PRIu64 "\n", key);
			assert(0);
		}
		ret = cds_ja_del(test_ja, key, &node->node);
		if (ret) {
			fprintf(stderr, "Error (%d) removing node %" PRIu64 "\n", ret, key);
			assert(0);
		}
		call_rcu(&node->node.head, free_node_cb);
		head = cds_ja_lookup(test_ja, key);
		if (!cds_hlist_empty(&head)) {
			fprintf(stderr, "Error lookup %" PRIu64 ": %p (after delete) failed. Node is not expected.\n", key, head.next);
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");

	ret = cds_ja_destroy(test_ja, free_node_cb);
	if (ret) {
		fprintf(stderr, "Error destroying judy array\n");
		return -1;
	}
	return 0;
}

static
int test_16bit_key(void)
{
	int ret;
	uint64_t key;

	/* Test with 16-bit key */
	test_ja = cds_ja_new(16);
	if (!test_ja) {
		printf("Error allocating judy array.\n");
		return -1;
	}

	/* Add keys */
	printf("Test #1: add keys (16-bit).\n");
	for (key = 0; key < 10000; key++) {
	//for (key = 0; key < 65536; key+=256) {
		struct ja_test_node *node =
			calloc(sizeof(*node), 1);

		ja_test_node_init(node, key);
		rcu_read_lock();
		ret = cds_ja_add(test_ja, key, &node->node);
		rcu_read_unlock();
		if (ret) {
			fprintf(stderr, "Error (%d) adding node %" PRIu64 "\n",
				ret, key);
			assert(0);
		}
	}
	printf("OK\n");

	printf("Test #2: successful key lookup (16-bit).\n");
	for (key = 0; key < 10000; key++) {
	//for (key = 0; key < 65536; key+=256) {
		struct cds_hlist_head head;

		rcu_read_lock();
		head = cds_ja_lookup(test_ja, key);
		if (cds_hlist_empty(&head)) {
			fprintf(stderr, "Error lookup node %" PRIu64 "\n", key);
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");
	printf("Test #3: unsuccessful key lookup (16-bit).\n");
	for (key = 11000; key <= 11002; key++) {
		struct cds_hlist_head head;

		rcu_read_lock();
		head = cds_ja_lookup(test_ja, key);
		if (!cds_hlist_empty(&head)) {
			fprintf(stderr,
				"Error unexpected lookup node %" PRIu64 "\n",
				key);
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");
	printf("Test #4: remove keys (16-bit).\n");
	for (key = 0; key < 10000; key++) {
	//for (key = 0; key < 65536; key+=256) {
		struct cds_hlist_head head;
		struct ja_test_node *node;

		rcu_read_lock();
		head = cds_ja_lookup(test_ja, key);
		node = cds_hlist_first_entry_rcu(&head, struct ja_test_node, node.list);
		if (!node) {
			fprintf(stderr, "Error lookup node %" PRIu64 "\n", key);
			assert(0);
		}
		ret = cds_ja_del(test_ja, key, &node->node);
		if (ret) {
			fprintf(stderr, "Error (%d) removing node %" PRIu64 "\n", ret, key);
			assert(0);
		}
		call_rcu(&node->node.head, free_node_cb);
		head = cds_ja_lookup(test_ja, key);
		if (!cds_hlist_empty(&head)) {
			fprintf(stderr, "Error lookup %" PRIu64 ": %p (after delete) failed. Node is not expected.\n", key, head.next);
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");

	ret = cds_ja_destroy(test_ja, free_node_cb);
	if (ret) {
		fprintf(stderr, "Error destroying judy array\n");
		return -1;
	}
	return 0;
}

/*
 * nr_dup is number of nodes per key.
 */
static
int test_sparse_key(unsigned int bits, int nr_dup)
{
	uint64_t key, max_key;
	int zerocount, i, ret;

	if (bits == 64)
		max_key = UINT64_MAX;
	else
		max_key = (1ULL << bits) - 1;

	printf("Sparse key test begins for %u-bit keys\n", bits);
	/* Test with 16-bit key */
	test_ja = cds_ja_new(bits);
	if (!test_ja) {
		printf("Error allocating judy array.\n");
		return -1;
	}

	/* Add keys */
	printf("Test #1: add keys (%u-bit).\n", bits);
	for (i = 0; i < nr_dup; i++) {
		zerocount = 0;
		for (key = 0; key <= max_key && (key != 0 || zerocount < 1); key += 1ULL << (bits - 8)) {
			struct ja_test_node *node =
				calloc(sizeof(*node), 1);

			ja_test_node_init(node, key);
			rcu_read_lock();
			ret = cds_ja_add(test_ja, key, &node->node);
			rcu_read_unlock();
			if (ret) {
				fprintf(stderr, "Error (%d) adding node %" PRIu64 "\n",
					ret, key);
				assert(0);
			}
			if (key == 0)
				zerocount++;
		}
	}
	printf("OK\n");

	printf("Test #2: successful key lookup (%u-bit).\n", bits);
	zerocount = 0;
	for (key = 0; key <= max_key && (key != 0 || zerocount < 1); key += 1ULL << (bits - 8)) {
		struct cds_hlist_head head;
		struct ja_test_node *node;
		struct cds_hlist_node *pos;
		int count = 0;

		rcu_read_lock();
		head = cds_ja_lookup(test_ja, key);
		if (cds_hlist_empty(&head)) {
			fprintf(stderr, "Error lookup node %" PRIu64 "\n", key);
			assert(0);
		}
		cds_hlist_for_each_entry_rcu(node, pos, &head, node.list) {
			count++;
		}
		if (count != nr_dup) {
			fprintf(stderr, "Unexpected number of match for key %" PRIu64 ", expected %d, got %d.\n", key, nr_dup, count);
		}
		rcu_read_unlock();
		if (key == 0)
			zerocount++;
	}
	printf("OK\n");
	if (bits > 8) {
		printf("Test #3: unsuccessful key lookup (%u-bit).\n", bits);
		zerocount = 0;
		for (key = 0; key <= max_key && (key != 0 || zerocount < 1); key += 1ULL << (bits - 8)) {
			struct cds_hlist_head head;

			rcu_read_lock();
			head = cds_ja_lookup(test_ja, key + 42);
			if (!cds_hlist_empty(&head)) {
				fprintf(stderr,
					"Error unexpected lookup node %" PRIu64 "\n",
					key + 42);
				assert(0);
			}
			rcu_read_unlock();
			if (key == 0)
				zerocount++;
		}
		printf("OK\n");
	}
	printf("Test #4: remove keys (16-bit).\n");
	zerocount = 0;
	for (key = 0; key <= max_key && (key != 0 || zerocount < 1); key += 1ULL << (bits - 8)) {
		struct cds_hlist_head head;
		struct ja_test_node *node;
		struct cds_hlist_node *pos;
		int count = 0;

		rcu_read_lock();
		head = cds_ja_lookup(test_ja, key);


		cds_hlist_for_each_entry_rcu(node, pos, &head, node.list) {
			struct cds_hlist_head testhead;

			count++;
			if (!node) {
				fprintf(stderr, "Error lookup node %" PRIu64 "\n", key);
				assert(0);
			}
			ret = cds_ja_del(test_ja, key, &node->node);
			if (ret) {
				fprintf(stderr, "Error (%d) removing node %" PRIu64 "\n", ret, key);
				assert(0);
			}
			call_rcu(&node->node.head, free_node_cb);
			testhead = cds_ja_lookup(test_ja, key);
			if (count < nr_dup && cds_hlist_empty(&testhead)) {
				fprintf(stderr, "Error: no node found after deletion of some nodes of a key\n");
				assert(0);
			}
		}
		head = cds_ja_lookup(test_ja, key);
		if (!cds_hlist_empty(&head)) {
			fprintf(stderr, "Error lookup %" PRIu64 ": %p (after delete) failed. Node is not expected.\n", key, head.next);
			assert(0);
		}
		rcu_read_unlock();
		if (key == 0)
			zerocount++;
	}
	printf("OK\n");

	ret = cds_ja_destroy(test_ja, free_node_cb);
	if (ret) {
		fprintf(stderr, "Error destroying judy array\n");
		return -1;
	}
	printf("Test ends\n");

	return 0;
}

static
int do_sanity_test(void)
{
	int i, j, ret;

	printf("Sanity test start.\n");

	for (i = 0; i < 3; i++) {
		ret = test_8bit_key();
		if (ret) {
			return ret;
		}
		rcu_quiescent_state();
	}
	ret = test_16bit_key();
	if (ret) {
		return ret;
	}
	rcu_quiescent_state();

	/* key bits */
	for (i = 8; i <= 64; i *= 2) {
		/* nr of nodes per key */
		for (j = 1; j < 4; j++) {
			ret = test_sparse_key(i, j);
			if (ret) {
				return ret;
			}
			rcu_quiescent_state();
		}
	}
	printf("Sanity test end.\n");

	return 0;
}

enum urcu_ja_addremove {
	AR_RANDOM = 0,
	AR_ADD = 1,
	AR_REMOVE = -1,
};	/* 1: add, -1 remove, 0: random */

static enum urcu_ja_addremove addremove; /* 1: add, -1 remove, 0: random */

static
void test_ja_rw_sigusr1_handler(int signo)
{
	switch (addremove) {
	case AR_ADD:
		printf("Add/Remove: random.\n");
		addremove = AR_RANDOM;
		break;
	case AR_RANDOM:
		printf("Add/Remove: remove only.\n");
		addremove = AR_REMOVE;
		break;
	case AR_REMOVE:
		printf("Add/Remove: add only.\n");
		addremove = AR_ADD;
		break;
	}
}

static
void *test_ja_rw_thr_reader(void *_count)
{
	unsigned long long *count = _count;
	struct cds_hlist_head head;
	uint64_t key;

	printf_verbose("thread_begin %s, thread id : %lx, tid %lu\n",
			"reader", pthread_self(), (unsigned long) gettid());

	set_affinity();

	rcu_register_thread();

	while (!test_go)
	{
	}
	cmm_smp_mb();

	for (;;) {
		rcu_read_lock();

		/* note: only looking up ulong keys */
		key = ((unsigned long) rand_r(&URCU_TLS(rand_lookup)) % lookup_pool_size) + lookup_pool_offset;
		key *= key_mul;
		head = cds_ja_lookup(test_ja, key);
		if (cds_hlist_empty(&head)) {
			if (validate_lookup) {
				printf("[ERROR] Lookup cannot find initial node.\n");
				exit(-1);
			}
			URCU_TLS(lookup_fail)++;
		} else {
			URCU_TLS(lookup_ok)++;
		}
		rcu_debug_yield_read();
		if (caa_unlikely(rduration))
			loop_sleep(rduration);
		rcu_read_unlock();
		URCU_TLS(nr_reads)++;
		if (caa_unlikely(!test_duration_read()))
			break;
		if (caa_unlikely((URCU_TLS(nr_reads) & ((1 << 10) - 1)) == 0))
			rcu_quiescent_state();
	}

	rcu_unregister_thread();

	*count = URCU_TLS(nr_reads);
	printf_verbose("thread_end %s, thread id : %lx, tid %lu\n",
			"reader", pthread_self(), (unsigned long) gettid());
	printf_verbose("readid : %lx, lookupfail %lu, lookupok %lu\n",
			pthread_self(), URCU_TLS(lookup_fail),
			URCU_TLS(lookup_ok));
	return ((void*)1);
}

static
int is_add(void)
{
	return ((unsigned int) rand_r(&URCU_TLS(rand_lookup)) % 100) < add_ratio;
}

static
void *test_ja_rw_thr_writer(void *_count)
{
	struct wr_count *count = _count;
	struct cds_hlist_head head;
	uint64_t key;
	int ret;

	printf_verbose("thread_begin %s, thread id : %lx, tid %lu\n",
			"writer", pthread_self(), (unsigned long) gettid());

	set_affinity();

	rcu_register_thread();

	while (!test_go)
	{
	}
	cmm_smp_mb();

	for (;;) {
		if ((addremove == AR_ADD)
				|| (addremove == AR_RANDOM && is_add())) {
			struct ja_test_node *node = malloc(sizeof(*node));
			struct cds_ja_node *ret_node;

			/* note: only inserting ulong keys */
			key = ((unsigned long) rand_r(&URCU_TLS(rand_lookup)) % write_pool_size) + write_pool_offset;
			key *= key_mul;
			ja_test_node_init(node, key);
			rcu_read_lock();
			if (add_unique) {
				ret_node = cds_ja_add_unique(test_ja, key, &node->node);
				if (ret_node != &node->node) {
					free(node);
					URCU_TLS(nr_addexist)++;
				} else {
					URCU_TLS(nr_add)++;
				}
			} else if (add_replace) {
				assert(0);	/* not implemented yet. */
			} else {
				ret = cds_ja_add(test_ja, key, &node->node);
				if (ret) {
					fprintf(stderr, "Error in cds_ja_add: %d\n", ret);
					free(node);
				} else {
					URCU_TLS(nr_add)++;
				}
			}
			rcu_read_unlock();
		} else {
			struct ja_test_node *node;

			/* May delete */
			/* note: only deleting ulong keys */
			key = ((unsigned long) rand_r(&URCU_TLS(rand_lookup)) % write_pool_size) + write_pool_offset;
			key *= key_mul;

			rcu_read_lock();

			head = cds_ja_lookup(test_ja, key);
			node = cds_hlist_first_entry_rcu(&head, struct ja_test_node, node.list);
			if (node) {
				ret = cds_ja_del(test_ja, key, &node->node);
				if (!ret) {
					call_rcu(&node->node.head, free_node_cb);
					URCU_TLS(nr_del)++;
				} else {
					URCU_TLS(nr_delnoent)++;
				}
			} else {
				URCU_TLS(nr_delnoent)++;
			}
			rcu_read_unlock();
		}

		URCU_TLS(nr_writes)++;
		if (caa_unlikely(!test_duration_write()))
			break;
		if (caa_unlikely(wdelay))
			loop_sleep(wdelay);
		if (caa_unlikely((URCU_TLS(nr_writes) & ((1 << 10) - 1)) == 0))
			rcu_quiescent_state();
	}

	rcu_unregister_thread();

	printf_verbose("thread_end %s, thread id : %lx, tid %lu\n",
			"writer", pthread_self(), (unsigned long) gettid());
	printf_verbose("info id %lx: nr_add %lu, nr_addexist %lu, nr_del %lu, "
			"nr_delnoent %lu\n", pthread_self(), URCU_TLS(nr_add),
			URCU_TLS(nr_addexist), URCU_TLS(nr_del),
			URCU_TLS(nr_delnoent));
	count->update_ops = URCU_TLS(nr_writes);
	count->add = URCU_TLS(nr_add);
	count->add_exist = URCU_TLS(nr_addexist);
	count->remove = URCU_TLS(nr_del);
	return ((void*)2);
}

static
int do_mt_populate_ja(void)
{
	struct cds_hlist_head head;
	uint64_t iter;
	int ret;

	if (!init_populate)
		return 0;

	printf("Starting rw test\n");

	for (iter = init_pool_offset; iter < init_pool_offset + init_pool_size; iter++) {
		struct ja_test_node *node = malloc(sizeof(*node));
		uint64_t key;

		/* note: only inserting ulong keys */
		key = (unsigned long) iter;
		key *= key_mul;
		ja_test_node_init(node, key);
		rcu_read_lock();
		ret = cds_ja_add(test_ja, key, &node->node);
		URCU_TLS(nr_add)++;
		URCU_TLS(nr_writes)++;
		rcu_read_unlock();
		if (ret) {
			fprintf(stderr, "Error (%d) adding node %" PRIu64 "\n",
				ret, key);
			assert(0);
		}
	}
	return 0;
}

static
int do_mt_test(void)
{
	pthread_t *tid_reader, *tid_writer;
	void *tret;
	int ret, i, err;
	unsigned long long *count_reader;
	struct wr_count *count_writer;
	unsigned long long tot_reads = 0, tot_writes = 0,
		tot_add = 0, tot_add_exist = 0, tot_remove = 0;
	unsigned int remain;

	tid_reader = malloc(sizeof(*tid_reader) * nr_readers);
	tid_writer = malloc(sizeof(*tid_writer) * nr_writers);
	count_reader = malloc(sizeof(*count_reader) * nr_readers);
	count_writer = malloc(sizeof(*count_writer) * nr_writers);

	printf("Allocating Judy Array for %u-bit keys\n", key_bits);
	test_ja = cds_ja_new(key_bits);
	if (!test_ja) {
		printf("Error allocating judy array.\n");
		ret = -1;
		goto end;
	}

	do_mt_populate_ja();

	next_aff = 0;

	for (i = 0; i < nr_readers; i++) {
		err = pthread_create(&tid_reader[i],
				     NULL, test_ja_rw_thr_reader,
				     &count_reader[i]);
		if (err != 0)
			exit(1);
	}
	for (i = 0; i < nr_writers; i++) {
		err = pthread_create(&tid_writer[i],
				     NULL, test_ja_rw_thr_writer,
				     &count_writer[i]);
		if (err != 0)
			exit(1);
	}

	cmm_smp_mb();

	test_go = 1;

	rcu_thread_offline_qsbr();

	remain = duration;
	do {
		remain = sleep(remain);
	} while (remain > 0);

	test_stop = 1;

	for (i = 0; i < nr_readers; i++) {
		err = pthread_join(tid_reader[i], &tret);
		if (err != 0)
			exit(1);
		tot_reads += count_reader[i];
	}
	for (i = 0; i < nr_writers; i++) {
		err = pthread_join(tid_writer[i], &tret);
		if (err != 0)
			exit(1);
		tot_writes += count_writer[i].update_ops;
		tot_add += count_writer[i].add;
		tot_add_exist += count_writer[i].add_exist;
		tot_remove += count_writer[i].remove;
	}
	rcu_thread_online_qsbr();

	ret = cds_ja_destroy(test_ja, free_node_cb);
	if (ret) {
		fprintf(stderr, "Error destroying judy array\n");
		goto end;
	}

	free(tid_reader);
	free(tid_writer);
	free(count_reader);
	free(count_writer);
	ret = 0;
end:
	return ret;
}

int main(int argc, char **argv)
{
	int i, j, a, ret, err;
	uint64_t key;
	struct sigaction act;

	if (argc < 4) {
		show_usage(argc, argv);
		return -1;
	}

	err = sscanf(argv[1], "%u", &nr_readers);
	if (err != 1) {
		show_usage(argc, argv);
		return -1;
	}

	err = sscanf(argv[2], "%u", &nr_writers);
	if (err != 1) {
		show_usage(argc, argv);
		return -1;
	}
	
	err = sscanf(argv[3], "%lu", &duration);
	if (err != 1) {
		show_usage(argc, argv);
		return -1;
	}

	for (i = 4; i < argc; i++) {
		if (argv[i][0] != '-')
			continue;
		switch (argv[i][1]) {
#ifdef DEBUG_YIELD
		case 'r':
			yield_active |= YIELD_READ;
			break;
		case 'w':
			yield_active |= YIELD_WRITE;
			break;
#endif
		case 'a':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			a = atoi(argv[++i]);
			cpu_affinities[next_aff++] = a;
			use_affinity = 1;
			printf_verbose("Adding CPU %d affinity\n", a);
			break;
		case 'c':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			rduration = atol(argv[++i]);
			break;
		case 'd':
			if (argc < i + 2) {
				show_usage(argc, argv);
				return -1;
			}
			wdelay = atol(argv[++i]);
			break;
		case 'v':
			verbose_mode = 1;
			break;
		case 'r':
			add_ratio = atoi(argv[++i]);
			break;
		case 'k':
			init_populate = 1;
			break;
		case 'R':
			lookup_pool_offset = atol(argv[++i]);
			break;
		case 'S':
			write_pool_offset = atol(argv[++i]);
			break;
		case 'T':
			init_pool_offset = atol(argv[++i]);
			break;
		case 'M':
			lookup_pool_size = atol(argv[++i]);
			break;
		case 'N':
			write_pool_size = atol(argv[++i]);
			break;
		case 'O':
			init_pool_size = atol(argv[++i]);
			break;
		case 'V':
			validate_lookup = 1;
			break;
		case 't':
			sanity_test = 1;
			break;
		case 'B':
			key_bits = atol(argv[++i]);
			break;
		case 'm':
			key_mul = atoll(argv[++i]);
			break;
		case 'u':
			add_unique = 1;
			break;
		case 's':
			add_replace = 1;
			break;
		}
	}

	printf_verbose("running test for %lu seconds, %u readers, %u writers.\n",
		duration, nr_readers, nr_writers);
	printf_verbose("Writer delay : %lu loops.\n", wdelay);
	printf_verbose("Reader duration : %lu loops.\n", rduration);
	printf_verbose("Add ratio: %u%%.\n", add_ratio);
	printf_verbose("Mode:%s%s.\n",
		" add/remove",
		add_unique ? " uniquify" : ( add_replace ? " replace" : " insert"));
	printf_verbose("Key multiplication factor: %" PRIu64 ".\n", key_mul);
	printf_verbose("Init pool size offset %lu size %lu.\n",
		init_pool_offset, init_pool_size);
	printf_verbose("Lookup pool size offset %lu size %lu.\n",
		lookup_pool_offset, lookup_pool_size);
	printf_verbose("Update pool size offset %lu size %lu.\n",
		write_pool_offset, write_pool_size);
	if (validate_lookup)
		printf_verbose("Validating lookups.\n");
	printf_verbose("thread %-6s, thread id : %lx, tid %lu\n",
			"main", pthread_self(), (unsigned long)gettid());

	memset(&act, 0, sizeof(act));
	ret = sigemptyset(&act.sa_mask);
	if (ret == -1) {
		perror("sigemptyset");
		return -1;
	}
	act.sa_handler = test_ja_rw_sigusr1_handler;
	act.sa_flags = SA_RESTART;
	ret = sigaction(SIGUSR1, &act, NULL);
	if (ret == -1) {
		perror("sigaction");
		return -1;
	}

	err = create_all_cpu_call_rcu_data(0);
	if (err) {
		printf("Per-CPU call_rcu() worker threads unavailable. Using default global worker thread.\n");
	}

	rcu_register_thread();

	if (sanity_test) {
		ret = do_sanity_test();
	} else {
		ret = do_mt_test();
	}

	rcu_unregister_thread();
	free_all_cpu_call_rcu_data();

	if (ret) {
		printf("Test ended with error: %d\n", ret);
	}
	return ret;

#if 0
	/*
	 * Hash Population needs to be seen as a RCU reader
	 * thread from the point of view of resize.
	 */
	rcu_register_thread();
      	ret = (get_populate_hash_cb())();
	assert(!ret);

	rcu_thread_offline();

	/* teardown counter thread */
	act.sa_handler = SIG_IGN;
	act.sa_flags = SA_RESTART;
	ret = sigaction(SIGUSR2, &act, NULL);
	if (ret == -1) {
		perror("sigaction");
		return -1;
	}
	{
		char msg[1] = { 0x42 };
		ssize_t ret;

		do {
			ret = write(count_pipe[1], msg, 1);	/* wakeup thread */
		} while (ret == -1L && errno == EINTR);
	}

	fflush(stdout);
	rcu_thread_online();
	rcu_read_lock();
	printf("Counting nodes... ");
	cds_lfht_count_nodes(test_ht, &approx_before, &count, &approx_after);
	printf("done.\n");
	test_delete_all_nodes(test_ht);
	rcu_read_unlock();
	rcu_thread_offline();
	if (count) {
		printf("Approximation before node accounting: %ld nodes.\n",
			approx_before);
		printf("Nodes deleted from hash table before destroy: "
			"%lu nodes.\n",
			count);
		printf("Approximation after node accounting: %ld nodes.\n",
			approx_after);
	}
	ret = cds_lfht_destroy(test_ht, NULL);
	if (ret)
		printf_verbose("final delete aborted\n");
	else
		printf_verbose("final delete success\n");
	printf_verbose("total number of reads : %llu, writes %llu\n", tot_reads,
	       tot_writes);
	printf("SUMMARY %-25s testdur %4lu nr_readers %3u rdur %6lu "
		"nr_writers %3u "
		"wdelay %6lu nr_reads %12llu nr_writes %12llu nr_ops %12llu "
		"nr_add %12llu nr_add_fail %12llu nr_remove %12llu nr_leaked %12lld\n",
		argv[0], duration, nr_readers, rduration,
		nr_writers, wdelay, tot_reads, tot_writes,
		tot_reads + tot_writes, tot_add, tot_add_exist, tot_remove,
		(long long) tot_add + init_populate - tot_remove - count);
	rcu_unregister_thread();
#endif
	return 0;
}
