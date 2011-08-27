/*
 * futex.spin: Promela code to validate n wakers to 1 waiter selective
 * futex wakeup algorithm.
 *
 * In this model, waker threads are told whether they are still being
 * waited on, and skip the futex wakeup if not.
 *
 * Algorithm verified :
 *
 * queue = 0;
 * waiting = 0;
 * gp_futex = 0;
 * gp = 1;
 *
 *                          Waker
 *                          while (1) {
 *                            this.queue = gp;
 *                            if (this.waiting == 1) {
 *                              this.waiting = 0;
 *                              if (gp_futex == -1) {
 *                                gp_futex = 0;
 *                                futex_wake = 1;
 *                              }
 *                            }
 *                          }
 *
 * Waiter
 * in_registry = 1;
 * while (1) {
 *   gp_futex = -1;
 *   waiting |= (queue != gp);
 *   in_registry &= (queue != gp);
 *   if (all in_registry == 0) {
 * progress:
 *     gp_futex = 0;
 *     gp = !gp;
 *     restart;
 *   } else {
 *     futex_wake = (gp_futex == -1 ? 0 : 1);
 *     while (futex_wake == 0) { }
 *   }
 *   queue = 0;
 * }
 *
 * By testing progress, i.e. [] <> !np_, we check that an infinite sequence
 * of update_counter_and_wait (and consequently of synchronize_rcu) will
 * not block.
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (c) 2009-2011 Mathieu Desnoyers
 */

#define get_pid()       (_pid)

int queue[2] = 0;
int waiting[2] = 0;
int futex_wake = 0;
int gp_futex = 0;
int gp = 1;
int in_registry[2] = 0;

active [2] proctype waker()
{
	assert(get_pid() < 2);

	do
	:: 1 ->
		queue[get_pid()] = gp;
	
		if
		:: (waiting[get_pid()] == 1) ->
			waiting[get_pid()] = 0;
			if
			:: (gp_futex == -1) ->
				gp_futex = 0;
#ifndef INJ_QUEUE_NO_WAKE
				futex_wake = 1;
#endif
			:: else ->
				skip;
			fi;
		fi;
	od;
}


active proctype waiter()
{
restart:
	in_registry[0] = 1;
	in_registry[1] = 1;
	do
	:: 1 ->
#if (!defined(INJ_LATE_DEC) && !defined(INJ_INVERT_WAITING_VS_GP_FUTEX))
		gp_futex = -1;
#endif
		if
		:: (in_registry[0] == 1 && queue[0] != gp) ->
			waiting[0] = 1;
                :: else ->
		        skip;
		fi;
		if
		:: (in_registry[1] == 1 && queue[1] != gp) ->
			waiting[1] = 1;
                :: else ->
		        skip;
		fi;

		if
		:: (in_registry[0] == 1 && queue[0] == gp) ->
			in_registry[0] = 0;
		:: else ->
			skip;
		fi;
		if
		:: (in_registry[1] == 1 && queue[1] == gp) ->
			in_registry[1] = 0;
		:: else ->
			skip;
		fi;
#ifdef INJ_INVERT_WAITING_VS_GP_FUTEX
		gp_futex = -1;
#endif

		if
		:: (in_registry[0] == 0 && in_registry[1] == 0) ->
progress:
#ifndef INJ_LATE_DEC
			gp_futex = 0;
#endif
			gp = !gp;
			goto restart;
		:: else ->
#ifdef INJ_LATE_DEC
			gp_futex = -1;
#endif
			futex_wake = gp_futex + 1;
			do
			:: 1 ->
				if
				:: (futex_wake == 0) ->
					skip;
				:: else ->
					break;
				fi;
			od;
		fi;
	od;
}
