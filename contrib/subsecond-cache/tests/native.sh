#!/bin/sh
# Run from a configured, built nginx source tree.
set -eu
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
tests=contrib/subsecond-cache/tests
cat > "$dir/tests.mk" <<'MAKE'
include objs/Makefile
fine-tests:
	$(CC) $(CFLAGS) $(ALL_INCS) $(TESTS)/deadlines.c -o $(TMP)/deadlines
	$(CC) $(CFLAGS) $(ALL_INCS) -Dmain=ngx_test_unused_main -c src/core/nginx.c -o $(TMP)/nginx.o
	$(CC) $(CFLAGS) $(ALL_INCS) $(TESTS)/headers.c $(TMP)/nginx.o $$(find objs -name '*.o' ! -path objs/src/core/nginx.o) $(DEAD_STRIP) $(TEST_LIBS) -o $(TMP)/headers
MAKE
case $(uname) in Darwin) strip=-Wl,-dead_strip ;; *) strip=-Wl,--gc-sections ;; esac
libs=${TEST_LIBS:-$(grep -m1 -- '-lz' objs/Makefile | tr -d '\\\t')}
make -f "$dir/tests.mk" fine-tests TMP="$dir" TESTS="$tests" DEAD_STRIP="$strip" \
    TEST_LIBS="$libs"
"$dir/deadlines"
FINE_HEADERS_BINARY="$dir/headers" prove -v "$tests/headers.t"
