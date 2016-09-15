#!/usr/bin/perl

use warnings;
use strict;
$ENV{"DALE_TEST_ARGS"} ||= "";
my $test_dir = $ENV{"DALE_TEST_DIR"} || ".";
$ENV{PATH} .= ":.";

use Data::Dumper;
use Test::More tests => 3;

my @res = `dalec $ENV{"DALE_TEST_ARGS"} $test_dir/t/src/locale.dt -o locale `;
is_deeply(\@res, [], 'No compilation errors');
@res = `./locale`;
is($?, 0, 'Program executed successfully');

chomp for @res;

is_deeply(\@res,
      [ '.' ],
          'Got correct results (if wrong, . may not be dec. point here)');

`rm locale`;

1;