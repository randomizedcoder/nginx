#!/usr/bin/perl
use strict;
use warnings;
use Test::More;

my $binary = $ENV{FINE_HEADERS_BINARY} // die "Set FINE_HEADERS_BINARY\n";
my $pair = 'max-age=0, max-age-ms=200';
my @cases = (
    ['origin response delay', 1, $pair, '', '', '', '185 0 0 15'],
    ['shield residence and RTT', 2, $pair, '', '0', '175', '10 0 0 190'],
    ['seconds rollover', 2, 'max-age=1,max-age-ms=1250', '', '1', '1175', '60 0 0 1190'],
    ['split lines', 1, 'max-age-ms=200', 'max-age=0', '', '', '185 0 0 15'],
    ['zero lifetime', 1, 'max-age=0,max-age-ms=0', '', '', '', '-15 0 0 15'],
    ['unpaired', 1, 'max-age-ms=200', '', '', '', '0 0 0'],
    ['duplicate', 1, $pair, 'max-age-ms=200', '', '', '0 0 0'],
    ['negative', 1, 'max-age=0,max-age-ms=-1', '', '', '', '0 0 0'],
    ['oversized', 1, 'max-age=0,max-age-ms=99999999999999999', '', '', '', '0 0 0'],
    ['quoted comma', 1, 'max-age=0,x="a,max-age-ms=200"', '', '', '', '0 0 0'],
    ['coarse only on trusted path', 2, $pair, '', '0', '', '0 0 0'],
    ['inconsistent age', 2, $pair, '', '2', '175', '0 0 0'],
    ['coarse shared precedence', 1, $pair, 's-maxage=0', '', '', '0 0 0'],
    ['stale window', 1, $pair, 'stale-while-revalidate=0, stale-while-revalidate-ms=200',
     '', '', '185 200 0 15'],
    ['private', 1, $pair, 'private', '', '', '0 0 0'],
    ['error window', 1, $pair, 'stale-if-error=0, stale-if-error-ms=250',
     '', '', '185 0 250 15'],
    ['error window alone makes the entry fine', 1, 'max-age=1',
     'stale-if-error=0, stale-if-error-ms=250', '', '', '985 0 250 15'],
    ['zero error window', 1, $pair, 'stale-if-error=5, stale-if-error-ms=0',
     '', '', '185 0 0 15'],
    ['invalid error companion uses seconds', 1, $pair,
     'stale-if-error=2, stale-if-error-ms=1.5', '', '', '185 0 2000 15'],
    ['duplicate error companion uses seconds', 1, $pair,
     'stale-if-error=2, stale-if-error-ms=5, stale-if-error-ms=5', '', '',
     '185 0 2000 15'],
    ['oversized error companion uses seconds', 1, $pair,
     'stale-if-error=2, stale-if-error-ms=9999999999999999', '', '',
     '185 0 2000 15'],
    ['unpaired error companion ignored', 1, $pair, 'stale-if-error-ms=250',
     '', '', '185 0 0 15'],
    ['unpaired error companion alone is coarse', 1, 'max-age=1',
     'stale-if-error-ms=250', '', '', '0 0 0'],
    ['must-revalidate forbids error window', 1, $pair,
     'must-revalidate, stale-if-error=0, stale-if-error-ms=250', '', '',
     '185 0 0 15'],
    ['must-revalidate with only an error pair is coarse', 1, 'max-age=1',
     'must-revalidate, stale-if-error=0, stale-if-error-ms=250', '', '',
     '0 0 0'],
    ['shared lifetime forbids error window', 1,
     's-maxage=0, s-maxage-ms=200', 'stale-if-error=0, stale-if-error-ms=250',
     '', '', '185 0 0 15'],
    ['both stale windows', 1, $pair,
     'stale-while-revalidate=0, stale-while-revalidate-ms=100, '
     . 'stale-if-error=1, stale-if-error-ms=1500', '', '', '185 100 1500 15'],
    # Mismatched and malformed pairs.  The two values are independent, so a
    # mismatch is valid; a malformed companion yields to its base.
    ['companion below base', 1, 'max-age=60, max-age-ms=200', '', '', '',
     '185 0 0 15'],
    ['companion above base', 1, 'max-age=1, max-age-ms=2000', '', '', '',
     '1985 0 0 15'],
    ['companion equal to base', 1, 'max-age=1, max-age-ms=1000', '', '', '',
     '985 0 0 15'],
    ['base zero and companion of a day', 1, 'max-age=0, max-age-ms=86400000',
     '', '', '', '86399985 0 0 15'],
    ['fifteen digits', 1, 'max-age=0, max-age-ms=999999999999999', '', '', '',
     '999999999999984 0 0 15'],
    ['sixteen digits', 1, 'max-age=0, max-age-ms=1000000000000000', '', '',
     '', '0 0 0'],
    ['leading zeros', 1, 'max-age=0, max-age-ms=0200', '', '', '',
     '185 0 0 15'],
    ['sixteen digits with leading zero', 1,
     'max-age=0, max-age-ms=0000000000000200', '', '', '', '0 0 0'],
    ['plus sign', 1, 'max-age=0, max-age-ms=+200', '', '', '', '0 0 0'],
    ['unit suffix', 1, 'max-age=0, max-age-ms=200ms', '', '', '', '0 0 0'],
    ['fraction', 1, 'max-age=0, max-age-ms=0.2', '', '', '', '0 0 0'],
    ['empty argument', 1, 'max-age=0, max-age-ms=', '', '', '', '0 0 0'],
    ['no argument', 1, 'max-age=0, max-age-ms', '', '', '', '0 0 0'],
    ['quoted argument', 1, 'max-age=0, max-age-ms="200"', '', '', '',
     '0 0 0'],
    ['mixed case names', 1, 'Max-Age=0, MAX-AGE-MS=200', '', '', '',
     '185 0 0 15'],
    ['longer directive name', 1, 'max-age=0, max-age-msx=200', '', '', '',
     '0 0 0'],
    ['prefixed directive name', 1, 'max-age=0, x-max-age-ms=200', '', '', '',
     '0 0 0'],
    ['shared companion on a private base', 1, 'max-age=0, s-maxage-ms=200',
     '', '', '', '0 0 0'],
    ['window companion below its base', 1, $pair,
     'stale-while-revalidate=60, stale-while-revalidate-ms=200', '', '',
     '185 200 0 15'],
    ['age beyond the lifetime', 2, $pair, '', '0', '900', '-715 0 0 915'],
);
for my $case (@cases) {
    # name, mode, Cache-Control lines, Age, Age-Ms, 'deadline update error age-ms'
    my ($name, $mode, $first, $second, $age, $fine, $expected) = @$case;
    open my $pipe, '-|', $binary, $mode, $first, $second, $age, $fine
        or die "Run header test: $!";
    my $result = <$pipe> // '';
    chomp $result;
    close $pipe or die "Header test exited $?";
    is($result, $expected, $name);
}
for my $test (
    ['304 inherits lifetime', '', '185 0 0 15'],
    ['304 replaces lifetime', 'max-age=0, max-age-ms=1250', '1235 0 0 15'],
    ['304 switches to whole seconds', 'max-age=5', '0 0 0'],
    ['304 disables reuse', 'no-store', '0 0 0'],
) {
    my ($name, $update, $expected) = @$test;
    open my $pipe, '-|', $binary, 1, $pair, '', '9', '9000', $update
        or die "Run validation test: $!";
    chomp(my $result = <$pipe>);
    close $pipe or die "Validation test exited $?";
    is($result, $expected, $name);
}
done_testing();
