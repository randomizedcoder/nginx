#!/bin/sh
# Run from a configured, built nginx source tree on Darwin.
set -eu
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
tests=contrib/subsecond-cache/tests
cat > "$dir/tests.mk" <<'MAKE'
include objs/Makefile
fine-tests:
	$(CC) $(CFLAGS) $(ALL_INCS) $(TESTS)/deadlines.c -o $(TMP)/deadlines
	$(CC) $(CFLAGS) $(ALL_INCS) -Dmain=ngx_test_unused_main -c src/core/nginx.c -o $(TMP)/nginx.o
	$(CC) $(CFLAGS) $(ALL_INCS) $(TESTS)/headers.c $(TMP)/nginx.o $$(find objs -name '*.o' ! -path objs/src/core/nginx.o) -Wl,-dead_strip $(TEST_LIBS) -o $(TMP)/headers
MAKE
make -f "$dir/tests.mk" fine-tests TMP="$dir" TESTS="$tests"
"$dir/deadlines"
FINE_HEADERS_BINARY="$dir/headers" prove -v "$tests/headers.t"
