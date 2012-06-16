/*
 * rcuja/testpop.c
 *
 * Userspace RCU library - RCU Judy Array population size test
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

/*
 * This program generates random populations, and shows the worse-case
 * unbalance, as well as the distribution of unbalance encountered.
 * Remember that the unbalance is the delta between the lowest and
 * largest population. Therefore, to get the delta between the subclass
 * size and the actual number of items, we need to divide the unbalance
 * by the number of subclasses (by hand).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
#include <limits.h>

static int sel_pool_len = 50;	/* default */
static int nr_distrib = 2;	/* default */
//#define SEL_POOL_LEN	100
//#define NR_POOLS	10000000ULL

static uint8_t pool[256];
static uint8_t nr_one[8];
static uint8_t nr_2d_11[8][8];
static uint8_t nr_2d_10[8][8];
static int global_max_minunbalance = 0;

static unsigned int unbalance_distrib[256];

static
uint8_t random_char(void)
{
	return (uint8_t) random();
}

static
void print_pool(void)
{
	int i;

	printf("pool: ");
	for (i = 0; i < sel_pool_len; i++) {
		printf("%d ", (int) pool[i]);
	}
	printf("\n");
}

static
void gen_pool(void)
{
	uint8_t src_pool[256];
	int i;
	int nr_left = 256;

	memset(pool, 0, sizeof(pool));
	for (i = 0; i < 256; i++)
		src_pool[i] = (uint8_t) i;
	for (i = 0; i < sel_pool_len; i++) {
		int sel;

		sel = random_char() % nr_left;
		pool[i] = src_pool[sel];
		src_pool[sel] = src_pool[nr_left - 1];
		nr_left--;
	}
}

static
void count_pool(void)
{
	int i;

	memset(nr_one, 0, sizeof(nr_one));
	memset(nr_2d_11, 0, sizeof(nr_2d_11));
	memset(nr_2d_10, 0, sizeof(nr_2d_10));
	for (i = 0; i < sel_pool_len; i++) {
		if (nr_distrib == 2) {
			int j;

			for (j = 0; j < 8; j++) {
				if (pool[i] & (1U << j))
					nr_one[j]++;
			}
		}

		if (nr_distrib == 4) {
			int j, k;

			for (j = 0; j < 8; j++) {
				for (k = 0; k < j; k++) {
					if ((pool[i] & (1U << j)) && (pool[i] & (1U << k))) {
						nr_2d_11[j][k]++;
					}
					if ((pool[i] & (1U << j)) && !(pool[i] & (1U << k))) {
						nr_2d_10[j][k]++;
					}
				}
			}
		}
	}
}

static
void print_count(void)
{
	int i;

	printf("pool distribution:\n");

	if (nr_distrib == 2) {
		printf("  0      1\n");
		printf("----------\n");
		for (i = 0; i < 8; i++) {
			printf("%3d    %3d\n",
				sel_pool_len - nr_one[i], nr_one[i]);
		}
	}

	if (nr_distrib == 4) {
		/* TODO */
	}
	printf("\n");
}

static
void stat_count(void)
{
	int minunbalance = INT_MAX;

	if (nr_distrib == 2) {
		int i;

		for (i = 0; i < 8; i++) {
			int diff;

			diff = (int) nr_one[i] * 2 - sel_pool_len;
			if (diff < 0)
				diff = -diff;
			if (diff < minunbalance) {
				minunbalance = diff;
			}
		}
	}

	if (nr_distrib == 4) {
		int j, k;

		for (j = 0; j < 8; j++) {
			for (k = 0; k < j; k++) {
				int diff[2];

				diff[0] = (int) nr_2d_11[j][k] * 4 - sel_pool_len;
				if (diff[0] < 0)
					diff[0] = -diff[0];

				diff[1] = (int) nr_2d_10[j][k] * 4 - sel_pool_len;
				if (diff[1] < 0)
					diff[1] = -diff[1];
				/* Get max linear array size */
				if (diff[1] > diff[0])
					diff[0] = diff[1];
				if (diff[0] < minunbalance) {
					minunbalance = diff[0];
				}
			}
		}
	}

	if (minunbalance > global_max_minunbalance) {
		global_max_minunbalance = minunbalance;
	}
	unbalance_distrib[minunbalance]++;
}

static
void print_distrib(void)
{
	int i;
	unsigned long long tot = 0;

	for (i = 0; i < 256; i++) {
		tot += unbalance_distrib[i];
	}
	if (tot == 0)
		return;
	printf("Distribution:\n");
	for (i = 0; i < 256; i++) {
		printf("(%u, %u, %llu%%) ",
			i, unbalance_distrib[i],
			100 * (unsigned long long) unbalance_distrib[i] / tot);
	}
	printf("\n");
}

static
void print_stat(uint64_t i)
{
	printf("after %llu pools, global_max_minunbalance: %d\n",
		(unsigned long long) i, global_max_minunbalance);
	print_distrib();
}

int main(int argc, char **argv)
{
	uint64_t i = 0;

	srandom(time(NULL));

	if (argc > 1) {
		sel_pool_len = atoi(argv[1]);
		if (sel_pool_len > 256 || sel_pool_len < 1) {
			printf("Wrong pool len\n");
			return -1;
		}
	}
	printf("pool len: %d\n", sel_pool_len);

	if (argc > 2) {
		nr_distrib = atoi(argv[2]);
		if (nr_distrib > 256 || nr_distrib < 1) {
			printf("Wrong number of distributions\n");
			return -1;
		}
	}
	printf("pool distributions: %d\n", nr_distrib);

	if (nr_distrib != 2 && nr_distrib != 4) {
		printf("Wrong number of distributions. Only 2 and 4 supported.\n");
		return -1;
	}

	//for (i = 0; i < NR_POOLS; i++) {
	while (1) {
		gen_pool();
		count_pool();
		//print_pool();
		//print_count();
		stat_count();
		if (!(i % 100000ULL))
			print_stat(i);
		i++;
	}
	print_stat(i);
	print_distrib();

	return 0;
}
