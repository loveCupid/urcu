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

# sanity test
${TESTPROG} 0 $((4*${THREAD_MUL})) ${TIME_UNITS} -t ${EXTRA_PARAMS} || exit 1

# rw test, single key, add and del randomly, 4 threads
# key range: init, lookup, and update: 0 to 0
${TESTPROG} 0 $((4*${THREAD_MUL})) ${TIME_UNITS} -M 1 -N 1 -O 1 ${EXTRA_PARAMS} || exit 1

# rw test, single key, add and del randomly, 2 lookup threads, 2 update threads
# key range: init, lookup, and update: 0 to 0
${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -M 1 -N 1 -O 1 ${EXTRA_PARAMS} || exit 1

# add with duplicates

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 8 -M 10 -N 10 -O 10 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 8 -M 100 -N 100 -O 100 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 8 -M 255 -N 255 -O 255 ${EXTRA_PARAMS} || exit 1

#expected fail (TODO)
#${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 8 -M 256 -N 256 -O 256 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 16 -M 10 -N 10 -O 10 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 16 -M 1000 -N 1000 -O 1000 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 16 -M 65535 -N 65535 -O 65535 ${EXTRA_PARAMS} || exit 1


${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 24 -M 10 -N 10 -O 10 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 24 -M 65535 -N 65535 -O 65535 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 24 -M 16777215 -N 16777215 -O 16777215 ${EXTRA_PARAMS} || exit 1


${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 32 -M 10 -N 10 -O 10 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 32 -M 1000 -N 1000 -O 1000 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 32 -M 1000000 -N 1000000 -O 1000000 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 32 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -B 64 ${EXTRA_PARAMS} || exit 1

# add unique

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 8 -M 10 -N 10 -O 10 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 8 -M 100 -N 100 -O 100 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 8 -M 255 -N 255 -O 255 ${EXTRA_PARAMS} || exit 1

#expected fail (TODO)
#${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 8 -M 256 -N 256 -O 256 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 16 -M 10 -N 10 -O 10 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 16 -M 1000 -N 1000 -O 1000 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 16 -M 65535 -N 65535 -O 65535 ${EXTRA_PARAMS} || exit 1


${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 24 -M 10 -N 10 -O 10 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 24 -M 65535 -N 65535 -O 65535 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 24 -M 16777215 -N 16777215 -O 16777215 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 32 -M 10 -N 10 -O 10 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 32 -M 1000 -N 1000 -O 1000 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 32 -M 1000000 -N 1000000 -O 1000000 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 32 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 64 ${EXTRA_PARAMS} || exit 1


# removal (0% add)

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 32 -k 10000 -M 10000 -N 10000 -O 10000 -r 0 ${EXTRA_PARAMS} || exit 1

# vary add ratio

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 32 -k 10000 -M 10000 -N 10000 -O 10000 -r 5 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -u -B 32 -k 10000 -M 10000 -N 10000 -O 10000 -r 95 ${EXTRA_PARAMS} || exit 1


# validate lookup of init values

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -V -u -B 32 -k 100 -S 100 -M 100 -N 100 -O 100 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -V -u -B 32 -k 10000 -S 10000 -M 10000 -N 10000 -O 10000 ${EXTRA_PARAMS} || exit 1

# vary key multiplication factor

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -V -u -B 32 -m 17 -k 100 -S 100 -M 100 -N 100 -O 100 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -V -u -B 32 -m 17 -k 10000 -S 10000 -M 10000 -N 10000 -O 10000 ${EXTRA_PARAMS} || exit 1


${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -V -u -B 64 -m 1717 -k 100 -S 100 -M 100 -N 100 -O 100 ${EXTRA_PARAMS} || exit 1

${TESTPROG} $((2*${THREAD_MUL})) $((2*${THREAD_MUL})) ${TIME_UNITS} -V -u -B 64 -m 1717 -k 10000 -S 10000 -M 10000 -N 10000 -O 10000 ${EXTRA_PARAMS} || exit 1
