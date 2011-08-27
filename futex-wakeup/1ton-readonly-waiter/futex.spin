/*
 * futex.spin: Promela code to validate 1 waker to n waiters futex
 * wakeup algorithm, where waiters have read-only access to the futex.
 *
 * In this model, the waker thread unconditionally wakes all waiters if
 * they need to be awakened. We guarantee that all waiters will never
 * wait forever if they need to be awakened, even if the waker is
 * inactive after requiring the wakeup. When "active" is set (e.g. a
 * daemon is available to service waiter requests), the waiter should
 * progress.
 *
 * Algorithm verified :
 *
 * active = 0;   (waker daemon is active)
 * futex = 0;
 * futex_wake = 0;
 *
 *                          1 waker (2 loops)
 *
 *                          futex = 0;
 *                          active = 1;   (e.g. listen())
 *                          futex_wake = 1;
 *                          active = 0;   (e.g. close())
 *                          futex = -1;
 *
 * n waiters (read-only)
 *
 * while (1) {
 *   if (active == 0) {
 *     if (futex == -1) {
 *       futex_wake = (futex == -1 ? 0 : 1);  (atomic)
 *       while (futex_wake == 0) { };
 *     }
 *   }
 * progress:
 * }
 *
 * if active = 1, then !_np
 *
 * By testing progress, i.e. [] <> ((!np_) || (!isactive)), we
 * check that waiters we can never block forever if the waker is active.
 *
 * The waker performs only 2 loops (and NOT an infinite number of loops)
 * because we really want to see what happens when the waker stops
 * running.
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

int _active = 0;
int futex = 0;
int futex_wake = 0;

active proctype waker()
{
	/* loop 1 */
	futex = 0;
	_active = 1;
	futex_wake = 1;
	_active = 0;
	futex = -1;

	/* loop 2 */
#ifndef INJ_MISORDER_WAKE
	futex = 0;
	_active = 1;
	futex_wake = 1;
#else
	futex_wake = 1;
	futex = 0;
	_active = 1;
#endif

#ifdef INJ_QUEUE_NO_WAKE
	_active = 0;
	futex = -1;

	/* loop 3 */
	futex = 0;
	_active = 1;
#endif
}

/*
 * The INJ_MISORDER error-injection test case succeeds, which means
 * order of active vs futex value read does not matter. It is
 * understandable because every time the active value is enabled by the
 * waker, a wake is performed.
 *
 * However, the order in which wakeup sets the futex value vs sending
 * the wakeup DOES matter, as shows the INJ_MISORDER_WAKE
 * error-injection.
 */
active [2] proctype waiter()
{
	do
	:: 1 ->
		if
#ifndef INJ_MISORDER
		:: (_active == 0) ->
#else
		:: (futex == -1) ->
#endif
			if
#ifndef INJ_MISORDER
			:: (futex == -1) ->
#else
			:: (_active == 0) ->
#endif
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
		:: else ->
			skip;
		fi;
progress:
		skip;
	od;
}
