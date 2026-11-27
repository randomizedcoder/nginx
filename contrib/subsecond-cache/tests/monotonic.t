#!/usr/bin/perl
use strict;
use warnings;
use Test::More;
use File::Temp qw(tempdir);

# Pitfall 4: proxy_cache_fine_time is a configuration error in a build
# without a monotonic clock, because a step of the wall clock could not be
# detected there.
#
# Each row is one configuration checked with "nginx -t".  expected has the
# outcome for a build with a monotonic clock and for one without: "ok", or
# the text that the error has to contain.
#
# FINE_NGINX_BINARY is the nginx to check.  FINE_NO_MONOTONIC=1 says it was
# built with NGX_HAVE_CLOCK_MONOTONIC off, which mutants.sh arranges.

my $binary = $ENV{FINE_NGINX_BINARY} // die "Set FINE_NGINX_BINARY\n";
my $build = $ENV{FINE_NO_MONOTONIC} ? 'without' : 'with';
my $error = 'requires a monotonic clock';

my @cases = (
    { description => 'fine time for an origin', tags => ['4:negative'],
      http => '', location => 'proxy_cache_fine_time origin;',
      expected => { with => 'ok', without => $error } },
    { description => 'fine time for a trusted cache', tags => ['4:negative'],
      http => '', location => 'proxy_cache_fine_time trusted;',
      expected => { with => 'ok', without => $error } },
    { description => 'fine time explicitly off', tags => ['4:positive'],
      http => '', location => 'proxy_cache_fine_time off;',
      expected => { with => 'ok', without => 'ok' } },
    { description => 'no fine directive', tags => ['4:positive'],
      http => '', location => '',
      expected => { with => 'ok', without => 'ok' } },
    { description => 'age header on, fine time not set',
      tags => ['4:boundary'],
      http => '', location => 'proxy_cache_fine_age_header on;',
      expected => { with => 'ok', without => 'ok' } },
    { description => 'request directives on, fine time not set',
      tags => ['4:boundary'],
      http => '', location => 'proxy_cache_fine_request on;',
      expected => { with => 'ok', without => 'ok' } },
    { description => 'fine time inherited from the http block',
      tags => ['4:corner'],
      http => 'proxy_cache_fine_time origin;', location => '',
      expected => { with => 'ok', without => $error } },
    { description => 'fine time at http level, off in the location',
      tags => ['4:corner'],
      http => 'proxy_cache_fine_time origin;',
      location => 'proxy_cache_fine_time off;',
      expected => { with => 'ok', without => $error } },
);

for my $case (@cases) {
    my $dir = tempdir(CLEANUP => 1);
    mkdir "$dir/logs";
    open my $conf, '>', "$dir/nginx.conf" or die "Write configuration: $!";
    print $conf <<"CONF";
pid $dir/nginx.pid;
error_log stderr;
events {}
http {
    access_log off;
    proxy_cache_path $dir/cache keys_zone=fine:1m;
    $case->{http}
    server {
        listen 127.0.0.1:18089;
        location / {
            proxy_pass http://127.0.0.1:18090;
            proxy_cache fine;
            $case->{location}
        }
    }
}
CONF
    close $conf;

    my $output = `"$binary" -t -e stderr -p "$dir" -c "$dir/nginx.conf" 2>&1`;
    my $expected = $case->{expected}{$build};
    my $name = "$case->{description}, $build a monotonic clock";

    if ($expected eq 'ok') {
        is($? >> 8, 0, $name) or diag($output);

    } else {
        ok($? != 0 && index($output, $expected) >= 0, $name)
            or diag($output);
    }
}
done_testing();
