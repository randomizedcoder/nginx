#!/bin/sh
# Show that the tests catch each mistake listed in the draft's
# "Implementation Pitfalls".
#
# The suites must pass on the tree as it is.  Then each patch in mutants/
# puts one mistake back into a scratch copy, and at least one test row has
# to fail.  The working tree is never patched.
#
# Run from the nginx source root:
#
#   nix develop ./contrib/subsecond-cache#build -c \
#       sh contrib/subsecond-cache/tests/mutants.sh
#
# NGINX_TESTS_DIR  nginx-tests checkout with proxy_cache_fine*.t
#                  (default ../nginx-tests)
# MUTANTS_WORK     directory for the scratch copies (default: mktemp -d)
# MUTANTS_JOBS     parallel compile jobs (default 8)
set -eu

tests=contrib/subsecond-cache/tests
if [ ! -f "$tests/mutants.sh" ]; then
    echo "Run from the nginx source root" >&2
    exit 2
fi

root=$(pwd)
harness=${NGINX_TESTS_DIR:-$root/../nginx-tests}
jobs=${MUTANTS_JOBS:-8}
base=${MUTANTS_WORK:-$(mktemp -d)}
work=$base/run.$$
mkdir -p "$work"
echo "Scratch copies and logs: $work"

integration=1
if [ ! -f "$harness/proxy_cache_fine_exposure.t" ]; then
    integration=0
    echo "No integration tests at $harness; unit suites only" >&2
fi

build() {    # tree log
    ( cd "$1" && make -j"$jobs" ) > "$2" 2>&1
}

# Unit suites, then the integration tests against the tree's binary.
suites() {    # tree log
    ( cd "$1" && sh "$tests/native.sh" ) > "$2.unit" 2>&1 || true
    : > "$2.integration"
    if [ "$integration" = 1 ]; then
        ( cd "$harness" && TEST_NGINX_BINARY="$1/objs/nginx" \
            prove -v proxy_cache_fine*.t ) > "$2.integration" 2>&1 || true
    fi
}

# The configuration test, against a build without a monotonic clock.
without_monotonic() {    # tree log
    ( cd "$1" && FINE_NO_MONOTONIC=1 FINE_NGINX_BINARY=objs/nginx \
        prove -v "$tests/monotonic.t" ) > "$2.unit" 2>&1 || true
    : > "$2.integration"
}

# Descriptions of the rows that failed.
failed_rows() {    # log
    grep -h -E '^not ok |: .* failed$' "$1.unit" "$1.integration" \
        | sed -E 's/^not ok [0-9]+ - //' || true
}

passed_count() {    # log
    cat "$1.unit" "$1.integration" | grep -c -E '^ok ' || true
}

status=0

# 1. The tree as it is.

echo
echo "== Fixed tree =="
mkdir "$work/fixed"
tar -C "$root" --exclude=.git --exclude=./objs -cf - . | tar -C "$work/fixed" -xf -
( cd "$work/fixed" && ./auto/configure ) > "$work/fixed.configure" 2>&1
if ! build "$work/fixed" "$work/fixed.build"; then
    echo "The fixed tree does not build; see $work/fixed.build" >&2
    exit 1
fi
suites "$work/fixed" "$work/fixed"
grep -h -E '^(deadlines|clock): ' "$work/fixed.unit" || true
grep -h -E '^(Files=|Result:)' "$work/fixed.unit" "$work/fixed.integration" || true
rows=$(failed_rows "$work/fixed")
if [ -n "$rows" ] || ! grep -q '^deadlines: ' "$work/fixed.unit" \
    || ! grep -q '^clock: ' "$work/fixed.unit"
then
    echo "FAILED on the fixed tree:" >&2
    echo "$rows" >&2
    exit 1
fi
echo "fixed tree: $(passed_count "$work/fixed") TAP rows pass, none fail"

# 2. The same source, configured as if there were no monotonic clock.

echo
echo "== Fixed tree, built without a monotonic clock =="
mkdir "$work/nomono"
tar -C "$work/fixed" -cf - . | tar -C "$work/nomono" -xf -
sed -i 's/^#define NGX_HAVE_CLOCK_MONOTONIC .*/#define NGX_HAVE_CLOCK_MONOTONIC  0/' \
    "$work/nomono/objs/ngx_auto_config.h"
if ! build "$work/nomono" "$work/nomono.build"; then
    echo "The build without a monotonic clock failed; see $work/nomono.build" >&2
    exit 1
fi
without_monotonic "$work/nomono" "$work/nomono"
grep -h -E '^(Files=|Result:)' "$work/nomono.unit" || true
rows=$(failed_rows "$work/nomono")
if [ -n "$rows" ] || ! grep -q '^Result: PASS' "$work/nomono.unit"; then
    echo "FAILED without a monotonic clock:" >&2
    echo "$rows" >&2
    exit 1
fi

# 3. One mistake at a time.

mkdir "$work/mutant"
tar -C "$work/fixed" -cf - . | tar -C "$work/mutant" -xf -
table=$work/table
: > "$table"

for patch in "$root/$tests"/mutants/*.patch; do
    name=$(basename "$patch" .patch)
    pitfall=${name%%-*}
    pitfall=${pitfall#0}
    log=$work/$name
    echo
    echo "== Mutant $name: $(head -1 "$patch") =="

    tree=$work/mutant
    patch -s -p1 -d "$tree" < "$patch"
    if build "$tree" "$log.build"; then
        suites "$tree" "$log"
        result=
    else
        result="BUILD FAILED"
    fi
    patch -s -R -p1 -d "$tree" < "$patch"

    if [ -z "$result" ] && [ "$pitfall" = 4 ]; then
        # Also where the mistake matters: a build with no monotonic clock.
        patch -s -p1 -d "$work/nomono" < "$patch"
        if build "$work/nomono" "$log.nomono.build"; then
            without_monotonic "$work/nomono" "$log.nomono"
            cat "$log.nomono.unit" >> "$log.unit"
        else
            result="BUILD FAILED"
        fi
        patch -s -R -p1 -d "$work/nomono" < "$patch"
    fi

    rows=$(failed_rows "$log")
    count=$(printf '%s\n' "$rows" | grep -c . || true)
    unit=$(grep -h -c -E '^not ok |: .* failed$' "$log.unit" || true)

    if [ -z "$result" ]; then
        if [ "$count" -gt 0 ]; then
            result=CAUGHT
        else
            result="NOT CAUGHT"
        fi
    fi
    [ "$result" = CAUGHT ] || status=1

    printf '%s\n' "$rows" | sed 's/^/    failed: /'
    printf '%s | %s | %s (%s unit, %s integration) | %s\n' \
        "$pitfall" "$name" "$count" "$unit" "$((count - unit))" "$result" \
        >> "$table"
done

# 4. Rows per pitfall and kind, from the tags in the test tables.

echo
echo "== Tagged rows per pitfall =="
echo "pitfall | positive | negative | boundary | corner"
files="$root/$tests/headers.t $root/$tests/deadlines.c $root/$tests/clock.c $root/$tests/monotonic.t"
if [ "$integration" = 1 ]; then
    files="$files $harness/proxy_cache_fine_exposure.t $harness/proxy_cache_fine_resolution.t"
fi
# shellcheck disable=SC2086
grep -o -h -E '[1-9]:(positive|negative|boundary|corner)' $files \
    | awk -F: '{ n[$1 " " $2]++ }
        END { for (p = 1; p <= 9; p++)
                printf "%d | %d | %d | %d | %d\n", p, n[p " positive"],
                    n[p " negative"], n[p " boundary"], n[p " corner"] }'

echo
echo "== Result =="
echo "pitfall | mutant | rows that failed | result"
cat "$table"

if [ "$status" != 0 ]; then
    echo "At least one mistake was not caught" >&2
fi
exit $status
