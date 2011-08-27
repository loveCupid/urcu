/*
 * futex.spin: Promela code to validate n wakers to 1 waiter futex
 * wakeup algorithm.
 *
 * In this model, waker threads unconditionally wake the waiter if it
 * needs to be awakened. We guarantee that the waiter will never wait
 * forever if it needs to be awakened, even if the waker is inactive
 * after requiring the wakeup.
 *
 * Algorithm verified :
 *
 * queue = 0;
 * futex = 0;
 * futex_wake = 0;
 *
 *                          n wakers (2 loops)
 *
 *                          queue = 1;
 *                          if (futex == -1) {
 *                            futex = 0;
 *                            futex_wake = 1;
 *                          }
 *
 * 1 waiter
 *
 * while (1) {
 * progress:
 *   futex = -1;
 *   if (queue == 1) {
 *     futex = 0;
 *   } else {
 *     if (futex == -1) {
 *       futex_wake = (futex == -1 ? 0 : 1);  (atomic)
 *       while (futex_wake == 0) { };
 *   }
 *   queue = 0;
 * }
 *
 * if queue = 1, then !_np
 *
 * By testing progress, i.e. [] <> ((!np_) || (!queue_has_entry)), we
 * check that we can never block forever if there is an entry in the
 * queue.
 *
 * The waker performs only 2 loops (and NOT an infinite number of loops)
 * because we really want to see what happens when the waker stops
 * enqueuing.
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
 * Copyright (c) 2009 Mathieu Desnoyers
 */

#define get_pid()       (_pid)

int queue[2] = 0;
int futex = 0;
int futex_wake = 0;

active [2] proctype waker()
{
	assert(get_pid() < 2);

	/* loop 1 */
	queue[get_pid()] = 1;

	if
	:: (futex == -1) ->
		futex = 0;
		futex_wake = 1;
	:: else ->
		skip;
	fi;

	/* loop 2 */
	queue[get_pid()] = 1;

	if
	:: (futex == -1) ->
		futex = 0;
		futex_wake = 1;
	:: else ->
		skip;
	fi;

#ifdef INJ_QUEUE_NO_WAKE
	/* loop 3 */
	queue[get_pid()] = 1;
#endif
}


active proctype waiter()
{
	do
	:: 1 ->
#ifndef INJ_LATE_DEC
		futex = -1;
#endif

		if
		:: (queue[0] == 1 || queue[1] == 1) ->
#ifndef INJ_LATE_DEC
			futex = 0;
#endif
			skip;
		:: else ->
#ifdef INJ_LATE_DEC
			futex = -1;
#endif
			if
			:: (futex == -1) ->
				atomic {
					if
					:: (futex == -1) ->
						futex_wake = 0;
					:: else ->
						futex_wake = 1;
					fi;
				}
				/* block */
				do
				:: 1 ->
					if
					:: (futex_wake == 0) ->
						skip;
					:: else ->
						break;
					fi;
				od;
			:: else ->
				skip;
			fi;
		fi;
progress:	/* Progress on dequeue */
		queue[0] = 0;
		queue[1] = 0;
	od;

}
