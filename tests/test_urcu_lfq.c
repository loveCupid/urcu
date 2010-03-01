/*
 * test_urcu.c
 *
 * Userspace RCU library - example RCU-based lock-free queue
 *
 * Copyright February 2010 - Paolo Bonzini <pbonzinI@redhat.com>
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
#include "../config.h"
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <sys/syscall.h>
#include <sched.h>
#include <errno.h>

#include <urcu/arch.h>

/* hardcoded number of CPUs */
#define NR_CPUS 16384

#if defined(_syscall0)
_syscall0(pid_t, gettid)
#elif defined(__NR_gettid)
static inline pid_t gettid(void)
{
	return syscall(__NR_gettid);
}
#else
#warning "use pid as tid"
static inline pid_t gettid(void)
{
	return getpid();
}
#endif

#ifndef DYNAMIC_LINK_TEST
#define _LGPL_SOURCE
#endif
#include <urcu.h>


static volatile int test_go, test_stop;

static unsigned long rduration;

static unsigned long duration;

/* read-side C.S. duration, in loops */
static unsigned long wdelay;

static inline void loop_sleep(unsigned long l)
{
	while(l-- != 0)
		cpu_relax();
}

static int verbose_mode;

#define printf_verbose(fmt, args...)		\
	do {					\
		if (verbose_mode)		\
			printf(fmt, args);	\
	} while (0)

static unsigned int cpu_affinities[NR_CPUS];
static unsigned int next_aff = 0;
static int use_affinity = 0;

pthread_mutex_t affinity_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifndef HAVE_CPU_SET_T
typedef unsigned long cpu_set_t;
# define CPU_ZERO(cpuset) do { *(cpuset) = 0; } while(0)
# define CPU_SET(cpu, cpuset) do { *(cpuset) |= (1UL << (cpu)); } while(0)
#endif

static void set_affinity(void)
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

/*
 * returns 0 if test should end.
 */
static int test_duration_dequeue(void)
{
	return !test_stop;
}

static int test_duration_enqueue(void)
{
	return !test_stop;
}

static unsigned long long __thread nr_dequeues;
static unsigned long long __thread nr_enqueues;

static unsigned long nr_successful_dequeues;
static unsigned int nr_enqueuers;
static unsigned int nr_dequeuers;


#define ARRAY_POISON	0xDEADBEEF
#define PAGE_SIZE	4096
#define PAGE_MASK	(PAGE_SIZE - 1)
#define NODES_PER_PAGE	(PAGE_SIZE - 16) / sizeof (struct node)

/* Lock-free queue, using the RCU to avoid the ABA problem and (more
   interestingly) to efficiently handle freeing memory.

   We have to protect both the enqueuer and dequeuer's compare-and-
   exchange operation from running across a free and a subsequent
   reallocation of the same memory.  So, we protect the free with
   synchronize_rcu; this is enough because all the allocations take
   place before the compare-and-exchange ops.

   Besides adding rcu_read_{,un}lock, the enqueue/dequeue are a standard
   implementation of a lock-free-queue.  The first node in the queue is
   always dummy: dequeuing returns the data from HEAD->NEXT, advances
   HEAD to HEAD->NEXT (which will now serve as dummy node), and frees the
   old HEAD.  Since RCU avoids the ABA problem, it doesn't use double-word
   compare-and-exchange operations.  Node allocation and deallocation are
   a "black box" and synchronize_rcu is hidden within node deallocation.

   So, the tricky part is finding a good allocation strategy for nodes.
   The allocator for nodes is shared by multiple threads, and since
   malloc/free are not lock-free a layer above them is obviously
   necessary: otherwise the whole exercise is useless.  In addition,
   to avoid penalizing dequeues, the allocator should avoid frequent
   synchronization (because synchronize_rcu is expensive).

   The scheme that is used here uses a page as the allocation
   unit for nodes.  A page is freed when no more nodes are in use.
   Nodes from a page are never reused.

   The nodes are allocated from Q->CURRENT.  Since whoever finds a full
   page has to busy wait, we use a trick to limit the duration of busy
   waiting.  A free page Q->FREE is always kept ready, so that any thread
   that allocates the last node in a page, or finds a full page can try
   to update Q->CURRENT.  Whoever loses the race has to busy wait, OTOH
   whoever wins the race has to allocate the new Q->FREE.  In other words,  
   if the following sequence happens

     Thread 1                    Thread 2                 other threads
     -----------------------------------------------------------------------
     Get last node from page
	                         q->current = q->free;
				                          fill up q->current
     q->current = q->free fails

   then thread 1 does not have anymore the duty of allocating q->current;
   thread 2 will do that.  If a thread finds a full current page and
   Q->CURRENT == Q->FREE, this means that another thread is going to
   allocate Q->FREE soon, and it busy waits.  After the allocation
   finishes, everything proceeds normally: some thread will take care
   of setting Q->CURRENT and allocating a new Q->FREE.
 
   One common scheme for allocation is to use a free list (implemented
   as a lock-free stack), but this free list is potentially unbounded.
   Instead, with the above scheme the number of live pages at any time
   is equal to the number of enqueuing threads.  */

struct node {
	void *data;
	void *next;
};

struct node_page {
	int in;
	int out;
	char padding[16 - sizeof(int) * 2];
	struct node nodes[NODES_PER_PAGE];
};


struct queue {
	struct node_page *current, *free;
	struct node *head, *tail;
};

static struct node_page *new_node_page()
{
	struct node_page *p = valloc (PAGE_SIZE);
	p->in = p->out = 0;
	return p;
}

static void free_node_page(struct node_page *p)
{
	/* Help making sure that accessing a dangling pointer is
	   adequately punished.  */
	p->in = ARRAY_POISON;
	free (p);
}

static struct node *new_node(struct queue *q)
{
	struct node *n;
	struct node_page *p;
	int i;

	do {
		p = q->current;
		i = p->in;
		if (i >= NODES_PER_PAGE - 1 &&
		    q->free != p &&
		    uatomic_cmpxchg(&q->current, p, q->free) == p)
			q->free = new_node_page();

	} while (i == NODES_PER_PAGE || uatomic_cmpxchg(&p->in, i, i+1) != i);

	assert (i >= 0 && i < NODES_PER_PAGE);
	n = &p->nodes[i];
	n->next = NULL;
	return n;
}

void free_node(struct node *n)
{
	struct node_page *p = (struct node_page *) ((intptr_t) n & ~PAGE_MASK);

	if (uatomic_add_return(&p->out, 1) == NODES_PER_PAGE) {
		synchronize_rcu();
		free_node_page(p);
	}
}

void init_queue(struct queue *q)
{
	q->current = new_node_page();
	q->free = new_node_page();
	q->head = q->tail = new_node(q);
}

void enqueue(struct queue *q, void *value)
{
	struct node *n = new_node(q);
	n->data = value;
	rcu_read_lock();
	for (;;) {
		struct node *tail = rcu_dereference(q->tail);
		struct node *next = rcu_dereference(tail->next);
		if (tail != q->tail) {
			/* A change occurred while reading the values.  */
			continue;
		}

		if (next) {
			/* Help moving tail further.  */
			uatomic_cmpxchg(&q->tail, tail, next);
			continue;
		}

		if (uatomic_cmpxchg(&tail->next, NULL, n) == NULL) {
			/* Move tail (another operation might beat us to it,
			   that's fine).  */
			uatomic_cmpxchg(&q->tail, tail, n);
			rcu_read_unlock();
			return;
		}
	}
}

void *dequeue(struct queue *q, bool *not_empty)
{
	bool dummy;
	if (!not_empty)
		not_empty = &dummy;

	rcu_read_lock();
	*not_empty = false;
	for (;;) {
		void *data;
		struct node *head = rcu_dereference(q->head);
		struct node *tail = rcu_dereference(q->tail);
		struct node *next = rcu_dereference(head->next);

		if (head != q->head) {
			/* A change occurred while reading the values.  */
			continue;
		}

		if (head == tail) {
			/* If all three are consistent, the queue is empty.  */
			if (!next)
				return NULL;

			/* Help moving tail further.  */
			uatomic_cmpxchg(&q->tail, tail, next);
			continue;
		}

		data = next->data;
		if (uatomic_cmpxchg(&q->head, head, next) == head) {
			/* Next remains as a dummy node, head is freed.  */
			rcu_read_unlock();
			*not_empty = true;
			free_node (head);
			return data;
		}
	}
}


static struct queue q;

void *thr_enqueuer(void *_count)
{
	unsigned long long *count = _count;

	printf_verbose("thread_begin %s, thread id : %lx, tid %lu\n",
			"enqueuer", pthread_self(), (unsigned long)gettid());

	set_affinity();

	rcu_register_thread();

	while (!test_go)
	{
	}
	smp_mb();

	for (;;) {
		enqueue (&q, NULL);

		if (unlikely(wdelay))
			loop_sleep(wdelay);
		nr_enqueues++;
		if (unlikely(!test_duration_enqueue()))
			break;
	}

	rcu_unregister_thread();

	*count = nr_enqueues;
	printf_verbose("thread_end %s, thread id : %lx, tid %lu - count %d\n",
		       "enqueuer", pthread_self(), (unsigned long)gettid(), nr_enqueues);
	return ((void*)1);

}

void *thr_dequeuer(void *_count)
{
	unsigned long long *count = _count;

	printf_verbose("thread_begin %s, thread id : %lx, tid %lu\n",
			"dequeuer", pthread_self(), (unsigned long)gettid());

	set_affinity();

	rcu_register_thread();

	while (!test_go)
	{
	}
	smp_mb();

	for (;;) {
		bool not_empty;
		dequeue (&q, &not_empty);
		if (not_empty)
			uatomic_inc (&nr_successful_dequeues);

		nr_dequeues++;
		if (unlikely(!test_duration_dequeue()))
			break;
		if (unlikely(rduration))
			loop_sleep(rduration);
	}

	rcu_unregister_thread();

	printf_verbose("thread_end %s, thread id : %lx, tid %lu - count %d\n",
			"dequeuer", pthread_self(), (unsigned long)gettid(), nr_dequeues);
	*count = nr_dequeues;
	return ((void*)2);
}

void test_end(struct queue *q)
{
	bool not_empty;
	do
		dequeue (q, &not_empty);
	while (!not_empty);
	if (q->current != q->free)
		free_node_page(q->free);
	free_node_page(q->current);
}

void show_usage(int argc, char **argv)
{
	printf("Usage : %s nr_dequeuers nr_enqueuers duration (s)", argv[0]);
	printf(" [-d delay] (enqueuer period (in loops))");
	printf(" [-c duration] (dequeuer period (in loops))");
	printf(" [-v] (verbose output)");
	printf(" [-a cpu#] [-a cpu#]... (affinity)");
	printf("\n");
}

int main(int argc, char **argv)
{
	int err;
	pthread_t *tid_enqueuer, *tid_dequeuer;
	void *tret;
	unsigned long long *count_enqueuer, *count_dequeuer;
	unsigned long long tot_enqueues = 0, tot_dequeues = 0;
	int i, a;

	if (argc < 4) {
		show_usage(argc, argv);
		return -1;
	}

	err = sscanf(argv[1], "%u", &nr_dequeuers);
	if (err != 1) {
		show_usage(argc, argv);
		return -1;
	}

	err = sscanf(argv[2], "%u", &nr_enqueuers);
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
		}
	}

	printf_verbose("running test for %lu seconds, %u enqueuers, %u dequeuers.\n",
		duration, nr_enqueuers, nr_dequeuers);
	printf_verbose("Writer delay : %lu loops.\n", rduration);
	printf_verbose("Reader duration : %lu loops.\n", wdelay);
	printf_verbose("thread %-6s, thread id : %lx, tid %lu\n",
			"main", pthread_self(), (unsigned long)gettid());

	tid_enqueuer = malloc(sizeof(*tid_enqueuer) * nr_enqueuers);
	tid_dequeuer = malloc(sizeof(*tid_dequeuer) * nr_dequeuers);
	count_enqueuer = malloc(sizeof(*count_enqueuer) * nr_enqueuers);
	count_dequeuer = malloc(sizeof(*count_dequeuer) * nr_dequeuers);
	init_queue (&q);

	next_aff = 0;

	for (i = 0; i < nr_enqueuers; i++) {
		err = pthread_create(&tid_enqueuer[i], NULL, thr_enqueuer,
				     &count_enqueuer[i]);
		if (err != 0)
			exit(1);
	}
	for (i = 0; i < nr_dequeuers; i++) {
		err = pthread_create(&tid_dequeuer[i], NULL, thr_dequeuer,
				     &count_dequeuer[i]);
		if (err != 0)
			exit(1);
	}

	smp_mb();

	test_go = 1;

	for (i = 0; i < duration; i++) {
		sleep(1);
		if (verbose_mode)
			write (1, ".", 1);
	}

	test_stop = 1;

	for (i = 0; i < nr_enqueuers; i++) {
		err = pthread_join(tid_enqueuer[i], &tret);
		if (err != 0)
			exit(1);
		tot_enqueues += count_enqueuer[i];
	}
	for (i = 0; i < nr_dequeuers; i++) {
		err = pthread_join(tid_dequeuer[i], &tret);
		if (err != 0)
			exit(1);
		tot_dequeues += count_dequeuer[i];
	}
	
	printf_verbose("total number of enqueues : %llu, dequeues %llu\n", tot_enqueues,
	       tot_dequeues);
	printf("SUMMARY %-25s testdur %4lu nr_enqueuers %3u wdelay %6lu "
		"nr_dequeuers %3u "
		"rdur %6lu nr_enqueues %12llu nr_dequeues %12llu successful %12lu nr_ops %12llu\n",
		argv[0], duration, nr_enqueuers, wdelay,
		nr_dequeuers, rduration, tot_enqueues, tot_dequeues,
		nr_successful_dequeues, tot_enqueues + tot_dequeues);

	test_end(&q);
	free(tid_enqueuer);
	free(tid_dequeuer);
	free(count_enqueuer);
	free(count_dequeuer);
	return 0;
}
