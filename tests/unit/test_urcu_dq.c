/*
 * test_urcu_dq.c
 *
 * Userspace RCU library - double-ended queue unit test
 *
 * Copyright 2013 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
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

#include <urcu.h>
#include <urcu/rcudq.h>
#include <urcu/compiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define err_printf(fmt, args...)	\
	fprintf(stderr, "[error in %s@%d:%s()] " fmt, __FILE__, __LINE__, __func__, ## args)

struct myobj {
	int a;
	int b;
	struct cds_rcudq_head node;
	struct rcu_head rcu_head;
};

static CDS_RCUDQ_HEAD(dq);

static
struct myobj *create_obj(int a, int b)
{
	struct myobj *obj;

	obj = malloc(sizeof(*obj));
	if (!obj)
		return NULL;
	obj->a = a;
	obj->b = b;
	CDS_INIT_RCUDQ_HEAD(&obj->node);
	return obj;
}

static
void poison_head(struct cds_rcudq_head *head)
{
	memset(head, 42, sizeof(*head));
}

static
void print_obj(struct myobj *obj)
{
	printf("(%d, %d) ", obj->a, obj->b);
}

static
void free_obj(struct rcu_head *rcu_head)
{
	struct myobj *obj = caa_container_of(rcu_head, struct myobj, rcu_head);

	free(obj);
}

int main(int argc, char **argv)
{
	struct myobj *obj, *tmp;
	struct cds_rcudq_head *pos, *p;
	unsigned int i, j;

	rcu_register_thread();

	if (!cds_rcudq_empty(&dq)) {
		err_printf("Queue is not empty as expected\n");
		goto error;
	}

	poison_head(&dq);
	CDS_INIT_RCUDQ_HEAD(&dq);
	if (!cds_rcudq_empty(&dq)) {
		err_printf("Queue is not empty as expected\n");
		goto error;
	}

	/* Single updater */
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			obj = create_obj(i, j);
			if (!obj)
				goto error;
			/* Add to tail */
			cds_rcudq_add_tail(&obj->node, &dq);
		}
	}
	for (i = 42; i < 46; i++) {
		obj = create_obj(i, i);
		if (!obj)
			goto error;
		/* Add to head */
		cds_rcudq_add(&obj->node, &dq);
	}

	printf("cds_rcudq_first_entry()\n");
	obj = cds_rcudq_first_entry(&dq, struct myobj, node);
	print_obj(obj);
	printf("\n");

	printf("cds_rcudq_for_each()\n");
	cds_rcudq_for_each(pos, &dq) {
		obj = cds_rcudq_entry(pos, struct myobj, node);
		print_obj(obj);
	}
	printf("\n");

	printf("cds_rcudq_for_each_safe()\n");
	cds_rcudq_for_each_safe(pos, p, &dq) {
		obj = cds_rcudq_entry(pos, struct myobj, node);
		print_obj(obj);
		if (obj->a == 42) {
			printf("(removing) ");
			cds_rcudq_del(&obj->node);
			call_rcu(&obj->rcu_head, free_obj);
		}
	}
	printf("\n");

	printf("cds_rcudq_for_each_entry()\n");
	cds_rcudq_for_each_entry(obj, &dq, node) {
		print_obj(obj);
	}
	printf("\n");

	printf("cds_rcudq_for_each_entry_safe()\n");
	cds_rcudq_for_each_entry_safe(obj, tmp, &dq, node) {
		print_obj(obj);
		if (obj->a == 43) {
			printf("(removing) ");
			cds_rcudq_del(&obj->node);
			call_rcu(&obj->rcu_head, free_obj);
		}
	}
	printf("\n");

	printf("cds_rcudq_for_each_reverse()\n");
	cds_rcudq_for_each_reverse(pos, &dq) {
		obj = cds_rcudq_entry(pos, struct myobj, node);
		print_obj(obj);
	}
	printf("\n");

	printf("cds_rcudq_for_each_reverse_safe()\n");
	cds_rcudq_for_each_reverse_safe(pos, p, &dq) {
		obj = cds_rcudq_entry(pos, struct myobj, node);
		print_obj(obj);
		if (obj->a == 44) {
			printf("(removing) ");
			cds_rcudq_del(&obj->node);
			call_rcu(&obj->rcu_head, free_obj);
		}
	}
	printf("\n");

	printf("cds_rcudq_for_each_entry_reverse()\n");
	cds_rcudq_for_each_entry_reverse(obj, &dq, node) {
		print_obj(obj);
	}
	printf("\n");

	printf("cds_rcudq_for_each_entry_reverse_safe()\n");
	cds_rcudq_for_each_entry_reverse_safe(obj, tmp, &dq, node) {
		print_obj(obj);
		if (obj->a == 45) {
			printf("(removing) ");
			cds_rcudq_del(&obj->node);
			call_rcu(&obj->rcu_head, free_obj);
		}
	}
	printf("\n");


	rcu_read_lock();

	printf("cds_rcudq_for_each_rcu()\n");
	cds_rcudq_for_each_rcu(pos, &dq) {
		obj = cds_rcudq_entry(pos, struct myobj, node);
		print_obj(obj);
	}
	printf("\n");

	printf("cds_rcudq_for_each_entry_rcu()\n");
	cds_rcudq_for_each_entry_rcu(obj, &dq, node) {
		print_obj(obj);
	}
	printf("\n");

	printf("cds_rcudq_for_each_reverse_rcu()\n");
	cds_rcudq_for_each_reverse_rcu(pos, &dq) {
		obj = cds_rcudq_entry(pos, struct myobj, node);
		print_obj(obj);
	}
	printf("\n");

	printf("cds_rcudq_for_each_entry_reverse_rcu()\n");
	cds_rcudq_for_each_entry_reverse_rcu(obj, &dq, node) {
		print_obj(obj);
	}
	printf("\n");

	rcu_read_unlock();

	cds_rcudq_for_each_entry_safe(obj, tmp, &dq, node) {
		cds_rcudq_del(&obj->node);
		call_rcu(&obj->rcu_head, free_obj);
	}

	if (!cds_rcudq_empty(&dq)) {
		err_printf("Queue is not empty as expected\n");
		goto error;
	}

	/* Free memory (in flight call_rcu callback execution) before exiting */
	rcu_barrier();

	rcu_unregister_thread();

	exit(EXIT_SUCCESS);

error:
	exit(EXIT_FAILURE);
}
