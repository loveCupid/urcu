#!/bin/sh

# TODO: missing tests:
# - send kill signals during tests to change the behavior between
#   add/remove/random
# - validate that "nr_leaked" is always 0 in SUMMARY for all tests

# 30 seconds per test
TIME_UNITS=30

TESTPROG=./test_urcu_ja

#thread multiplier
THREAD_MUL=1

EXTRA_PARAMS=-v

# ** test update coherency with single-value table

# rw test, single key, add and del randomly, 4 threads
# key range: init, lookup, and update: 0 to 0
${TESTPROG} 0 $((4*${THREAD_MUL})) ${TIME_UNITS} -M 1 -N 1 -O 1 ${EXTRA_PARAMS} || exit 1

# rw test, single key, add and del randomly, 2 lookup threads, 2 update threads
# key range: init, lookup, and update: 0 to 0
${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -M 1 -N 1 -O 1 ${EXTRA_PARAMS} || exit 1
