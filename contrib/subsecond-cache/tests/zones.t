#!/usr/bin/perl
use strict;
use warnings;
use Test::More;
use File::Temp qw(tempdir);

# Problem 13: a cache zone holds one file version for each key.  A location
# with proxy_cache_fine_time and one without, sharing a zone, replace each
# other's files, so "nginx -t" warns.
#
# Each row is a configuration with two proxied locations.  expected is
# "warn" or "quiet".  FINE_NGINX_BINARY is the nginx to check.

my $binary = $ENV{FINE_NGINX_BINARY} // die "Set FINE_NGINX_BINARY\n";
my $warning = 'is used with and without "proxy_cache_fine_time"';

my $pass = 'proxy_pass http://127.0.0.1:18090;';
my $fine = 'proxy_cache_fine_time origin;';

my @cases = (
    { description => 'both locations fine', tags => ['13:positive'],
      http => 'proxy_cache one;',
      a => "$pass $fine", b => "$pass $fine",
      expected => 'quiet' },
    { description => 'neither location fine', tags => ['13:positive'],
      http => 'proxy_cache one;',
      a => $pass, b => $pass,
      expected => 'quiet' },
    { description => 'fine and whole-second locations on one zone',
      tags => ['13:negative'],
      http => 'proxy_cache one;',
      a => "$pass $fine", b => $pass,
      expected => 'warn' },
    { description => 'whole-second location first', tags => ['13:negative'],
      http => 'proxy_cache one;',
      a => $pass, b => "$pass $fine",
      expected => 'warn' },
    { description => 'origin and trusted are both fine',
      tags => ['13:boundary'],
      http => 'proxy_cache one;',
      a => "$pass $fine", b => "$pass proxy_cache_fine_time trusted;",
      expected => 'quiet' },
    { description => 'fine time explicitly off counts as whole seconds',
      tags => ['13:boundary'],
      http => "proxy_cache one; $fine",
      a => $pass, b => "$pass proxy_cache_fine_time off;",
      expected => 'warn' },
    { description => 'fine and whole-second locations on separate zones',
      tags => ['13:corner'],
      http => '',
      a => "proxy_cache one; $pass $fine", b => "proxy_cache two; $pass",
      expected => 'quiet' },
    { description => 'a location that does not proxy is not counted',
      tags => ['13:corner'],
      http => 'proxy_cache one;',
      a => "$pass $fine", b => 'return 204;',
      expected => 'quiet' },
    { description => 'fine time inherited by both locations',
      tags => ['13:corner'],
      http => "proxy_cache one; $fine",
      a => $pass, b => $pass,
      expected => 'quiet' },
    { description => 'a zone named by a variable is not checked',
      tags => ['13:corner'],
      http => '',
      a => "proxy_cache one; $pass $fine", b => "proxy_cache \$arg_z; $pass",
      expected => 'quiet' },
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
    proxy_cache_path $dir/one keys_zone=one:1m;
    proxy_cache_path $dir/two keys_zone=two:1m;
    $case->{http}
    server {
        listen 127.0.0.1:18089;
        location /a/ {
            $case->{a}
        }
        location /b/ {
            $case->{b}
        }
    }
}
CONF
    close $conf;

    my $output = `"$binary" -t -e stderr -p "$dir" -c "$dir/nginx.conf" 2>&1`;
    my $warned = index($output, $warning) >= 0 ? 'warn' : 'quiet';

    ok($? == 0 && $warned eq $case->{expected}, $case->{description})
        or diag($output);
}
done_testing();
