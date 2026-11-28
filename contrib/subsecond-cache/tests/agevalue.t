#!/usr/bin/perl
use strict;
use warnings;
use Test::More;
use File::Temp qw(tempdir);

# Problem 18: proxy_cache_fine_age_header takes "on", "off" or a variable,
# so that Age-Ms can be sent to chosen recipients only.
#
# Each row is one configuration checked with "nginx -t".  expected is "ok",
# or the text that the error has to contain.  FINE_NGINX_BINARY is the nginx
# to check.

my $binary = $ENV{FINE_NGINX_BINARY} // die "Set FINE_NGINX_BINARY\n";
my $invalid = 'it must be "on", "off" or a variable';

my @cases = (
    { description => 'on', tags => ['18:positive'],
      http => '', location => 'proxy_cache_fine_age_header on;',
      expected => 'ok' },
    { description => 'off', tags => ['18:positive'],
      http => '', location => 'proxy_cache_fine_age_header off;',
      expected => 'ok' },
    { description => 'a variable', tags => ['18:positive'],
      http => '', location => 'proxy_cache_fine_age_header $http_x_want;',
      expected => 'ok' },
    { description => 'a word that is neither on nor off',
      tags => ['18:negative'],
      http => '', location => 'proxy_cache_fine_age_header yes;',
      expected => $invalid },
    { description => 'a number', tags => ['18:negative'],
      http => '', location => 'proxy_cache_fine_age_header 1;',
      expected => $invalid },
    { description => 'two arguments', tags => ['18:boundary'],
      http => '', location => 'proxy_cache_fine_age_header on $http_x_want;',
      expected => 'invalid number of arguments' },
    { description => 'no argument', tags => ['18:boundary'],
      http => '', location => 'proxy_cache_fine_age_header;',
      expected => 'invalid number of arguments' },
    { description => 'given twice in one location', tags => ['18:corner'],
      http => '',
      location => 'proxy_cache_fine_age_header on; '
                  . 'proxy_cache_fine_age_header $http_x_want;',
      expected => 'is duplicate' },
    { description => 'a variable at http level, off in the location',
      tags => ['18:corner'],
      http => 'proxy_cache_fine_age_header $http_x_want;',
      location => 'proxy_cache_fine_age_header off;',
      expected => 'ok' },
    { description => 'text with a variable in it', tags => ['18:corner'],
      http => '', location => 'proxy_cache_fine_age_header a$http_x_want;',
      expected => 'ok' },
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

    if ($case->{expected} eq 'ok') {
        is($? >> 8, 0, $case->{description}) or diag($output);

    } else {
        ok($? != 0 && index($output, $case->{expected}) >= 0,
            $case->{description})
            or diag($output);
    }
}
done_testing();
