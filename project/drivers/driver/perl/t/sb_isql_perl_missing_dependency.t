# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

use strict;
use warnings;
use File::Temp qw(tempdir);
use FindBin;
use Test::More;

my $driver_root = "$FindBin::Bin/..";
my $tool = "$driver_root/tools/sb_isql_perl.pl";
my $dir = tempdir(CLEANUP => 1);
my $input = "$dir/input.sbsql";
open my $in, '>', $input or die "cannot write input: $!";
print {$in} "SELECT 1;\n";
close $in;

my $run_root = "$dir/run";
mkdir $run_root or die "cannot create run root: $!";
my @command = (
    $^X,
    $tool,
    '--database', 'contract.sbdb',
    '--host', '127.0.0.1',
    '--port', '3092',
    '--user', 'sysdba',
    '--password', 'masterkey',
    '--role', '',
    '--sslmode', 'require',
    '--sslrootcert', '',
    '--sslcert', '',
    '--sslkey', '',
    '--ipc-path', '',
    '--route', 'listener-parser',
    '--parser-mode', 'server-parser',
    '--page-size', '8k',
    '--namespace', 'users.public.examples.perl.test',
    '--input', $input,
    '--output', "$run_root/stdout.log",
    '--error', "$run_root/stderr.log",
    '--diagnostics', "$run_root/diagnostics.jsonl",
    '--metrics', "$run_root/process-metrics.jsonl",
    '--transcript', "$run_root/wire-transcript.jsonl",
    '--summary', "$run_root/summary.json",
);

system @command;
ok($? != 0, 'native tool fails closed without live implementation');

for my $name (qw(
    summary.json
    diagnostics.jsonl
    wire-transcript.jsonl
    command-events.jsonl
    timing-groups.json
    result-digests.json
    metadata-snapshots.json
    route-environment.json
    process-metrics.jsonl
    security-refusals.json
    native-api-coverage.json
    code-example-review.json
    junit.xml
    stdout.log
    stderr.log
)) {
    ok(-f "$run_root/$name", "$name exists");
}

open my $summary_fh, '<', "$run_root/summary.json" or die "cannot read summary: $!";
my $summary = do { local $/; <$summary_fh> };
close $summary_fh;
open my $diag_fh, '<', "$run_root/diagnostics.jsonl" or die "cannot read diagnostics: $!";
my $diagnostics = do { local $/; <$diag_fh> };
close $diag_fh;

like($summary, qr/"driver_name":"perl"/, 'summary names perl driver');
like($summary, qr/"status":"fail"/, 'summary reports failure');
like(
    $diagnostics,
    qr/missing Perl module DBI|IO::Socket::SSL|connect failed|SBWP\/SBLR executor binding/,
    'diagnostics identify the fail-closed boundary',
);

done_testing();
