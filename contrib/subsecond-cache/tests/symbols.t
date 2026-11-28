#!/usr/bin/perl
use strict;
use warnings;
use Test::More;

# Problem 14: the unit tests fix the clock through a variable.  It belongs
# to the test build only, so the nginx binary must not define it.
#
# Each row looks for one symbol in one binary with nm.  expected is
# "present" or "absent".  FINE_NGINX_BINARY is the nginx to check and
# FINE_HEADERS_BINARY the unit test program built beside it.

my $nginx = $ENV{FINE_NGINX_BINARY} // die "Set FINE_NGINX_BINARY\n";
my $unit = $ENV{FINE_HEADERS_BINARY} // die "Set FINE_HEADERS_BINARY\n";

my @cases = (
    { description => 'unit test program has the fixed clock',
      tags => ['14:positive'],
      binary => $unit, symbol => 'ngx_http_cache_fine_fixed_clock',
      expected => 'present' },
    { description => 'nginx does not have the fixed clock',
      tags => ['14:negative'],
      binary => $nginx, symbol => 'ngx_http_cache_fine_fixed_clock',
      expected => 'absent' },
    { description => 'nginx has the clock function itself',
      tags => ['14:boundary'],
      binary => $nginx, symbol => 'ngx_http_cache_now_ms',
      expected => 'present' },
    { description => 'nginx has no symbol that merely starts the same',
      tags => ['14:corner'],
      binary => $nginx, symbol => 'ngx_http_cache_fine_fixed',
      expected => 'absent' },
);

for my $case (@cases) {
    my @names = map { (split ' ')[-1] } `nm "$case->{binary}" 2>/dev/null`;
    die "nm found no symbols in $case->{binary}\n" unless @names;

    my $found = grep { index($_, $case->{symbol}) == 0 } @names;

    is($found ? 'present' : 'absent', $case->{expected},
        $case->{description});
}

done_testing();
