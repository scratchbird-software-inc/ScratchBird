#!/usr/bin/env perl
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

use strict;
use warnings;
use Digest::SHA qw(sha256_hex);
use File::Basename qw(basename dirname);
use File::Path qw(make_path);
use FindBin;
use JSON::PP;
use Time::HiRes qw(time);

use lib "$FindBin::Bin/../lib";

my %PAGE_SIZE_BYTES = (
    '4k' => 4096,
    '8k' => 8192,
    '16k' => 16384,
    '32k' => 32768,
    '64k' => 65536,
    '128k' => 131072,
);
my %PAGE_SIZES = map { $_ => 1 } keys %PAGE_SIZE_BYTES;
my %ROUTES = map { $_ => 1 } qw(embedded ipc_local listener-parser manager-listener-parser);
my %PARSER_MODES = map { $_ => 1 } qw(server-parser standalone-parser driver-sblr-uuid);
my %SSLMODES = map { $_ => 1 } qw(allow disable prefer require verify-ca verify-full);
my %BOOLEAN_ARGS = map { $_ => 1 } qw(--stop-on-error --create-database --standard-english-fallback);
my %SUPPORTED_ARGS = map { $_ => 1 } qw(
    --database
    --host
    --port
    --user
    --password
    --role
    --sslmode
    --sslrootcert
    --sslcert
    --sslkey
    --ipc-path
    --route
    --parser-mode
    --page-size
    --namespace
    --input
    --output
    --error
    --diagnostics
    --metrics
    --transcript
    --summary
    --stop-on-error
    --expected-refusals
    --statement-timeout-ms
    --fetch-size
    --concurrency-worker
    --create-database
    --create-emulation-mode
    --run-id
    --language-resource-pack
    --language-resource-identity
    --language-resource-hash
    --language-profile
    --syntax-profile
    --topology-profile
    --standard-english-fallback
);

exit main(@ARGV);

sub main {
    my (@argv) = @_;
    my $args = eval { parse_args(@argv) };
    if (!$args) {
        print STDERR ($@ || "argument parsing failed");
        return 1;
    }
    my $code = eval { run_tool($args) };
    if ($@) {
        print STDERR $@;
        return 1;
    }
    return $code;
}

sub run_tool {
    my ($args) = @_;
    validate_args($args);
    my $run_root = dirname(required($args, '--summary'));
    my $paths = artifact_paths($args, $run_root);
    initialize_artifacts(values %{$paths});

    my %timings;
    my %api_hits = (
        'DBI->connect' => 0,
        'prepare' => 0,
        'execute' => 0,
        'fetchrow_arrayref' => 0,
        'errstr' => 0,
        'table_info' => 0,
        'column_info' => 0,
        'commit' => 0,
        'rollback' => 0,
    );
    my @testcases;
    my @failures;
    my @digests;
    my @security_refusals;
	    my @statements;
	    my $metadata_written = 0;
	    my $started = now_ns();
	    my $dbh;
	    my $route_env = route_environment($args, undef, 'fail', 'not_probed');
	    write_json($paths->{route_environment}, $route_env);

    eval {
        require_host_packages();
        @statements = split_statements(read_input(required($args, '--input')));
        my $expected_refusals = load_expected_refusals(value_or_default($args, '--expected-refusals', ''));
        my $route = required($args, '--route');
        ensure_route_supported($route, $args);
        my $dsn = build_dsn($args);
        my $connect_started = now_ns();
        $dbh = DBI->connect($dsn, required($args, '--user'), required($args, '--password'), {
            RaiseError => 0,
            PrintError => 0,
            AutoCommit => 1,
        });
        $api_hits{'DBI->connect'}++;
        if (!$dbh) {
            $api_hits{errstr}++;
            die(DBI->errstr || 'DBI connect failed without an error string');
        }
        add_timing(\%timings, 'connection', $connect_started);
        append_jsonl($paths->{transcript}, {
            event => 'connect',
            driver => 'perl',
            route => $route,
            parser_mode => required($args, '--parser-mode'),
            page_size => required($args, '--page-size'),
            language_profile => value_or_default($args, '--language-profile', 'en-US'),
            language_resource_identity => value_or_default($args, '--language-resource-identity', 'sbsql.common_resource_pack.v1'),
            language_resource_hash => value_or_default($args, '--language-resource-hash', 'sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc'),
            syntax_profile => value_or_default($args, '--syntax-profile', 'sbsql.v3'),
            topology_profile => value_or_default($args, '--topology-profile', 'topology.sbsql.canonical.v1'),
	        });
	        append_jsonl($paths->{wire}, { event => 'server_admission_required', driver_or_parser_finality => 'forbidden' });
	        $route_env = probe_route_environment($dbh, $args, \%api_hits);
	        write_json($paths->{route_environment}, $route_env);
	        if ($route ne 'embedded' && ($route_env->{page_size_verification_status} || '') ne 'pass') {
	            push @failures, {
	                statement_id => 'route_page_size',
	                message => 'route page-size verification failed',
	                expected_page_size_bytes => $route_env->{expected_page_size_bytes},
	                actual_page_size_bytes => $route_env->{actual_page_size_bytes},
	            };
	        }

	        if (flag_enabled($args, '--create-database', 0)) {
            die "0A000: ScratchBird Perl DBI attach/create requires a live ScratchBird creation surface; refusing delegated database creation";
        }
        for my $index (0 .. $#statements) {
            my $sql = $statements[$index];
            my $statement_id = basename(required($args, '--input')) . ':' . ($index + 1);
            my $expected_outcome = $expected_refusals->{$statement_id} ? 'refusal' : 'success';
            my $group = classify_statement($sql);
            my $statement_started = now_ns();
            my $outcome = 'success';
            my $row_count = -1;
            my $result_digest;
            my $sqlstate;
            my $diagnostic;
            eval {
                if ($group eq 'transaction') {
                    run_transaction($dbh, $sql, \%api_hits);
                    $row_count = 0;
                    $result_digest = sha256_text('transaction');
                } else {
                    my $sth = $dbh->prepare($sql);
                    $api_hits{prepare}++;
                    if (!$sth) {
                        $api_hits{errstr}++;
                        die($dbh->errstr || 'DBI prepare failed without an error string');
                    }
                    my $ok = $sth->execute;
                    $api_hits{execute}++;
                    if (!$ok) {
                        $api_hits{errstr}++;
                        die($sth->errstr || 'DBI execute failed without an error string');
                    }
                    my @rows;
                    while (my $row = $sth->fetchrow_arrayref) {
                        $api_hits{fetchrow_arrayref}++;
                        push @rows, [@{$row}];
                    }
                    $row_count = scalar @rows;
                    $result_digest = sha256_text(json_text(\@rows));
                    push @digests, {
                        statement_id => $statement_id,
                        row_count => $row_count,
                        result_digest => $result_digest,
                    };
                    append_text($paths->{output}, json_text({ statement_id => $statement_id, rows => \@rows }) . "\n");
                }
                if ($expected_outcome eq 'refusal') {
                    $outcome = 'unexpected_success';
                    push @failures, { statement_id => $statement_id, message => 'statement succeeded but was expected to refuse' };
                }
            };
            if ($@) {
                $outcome = 'refusal';
                $diagnostic = clean_error($@);
                $sqlstate = sqlstate_from_error($diagnostic);
                append_jsonl($paths->{diagnostics}, { statement_id => $statement_id, sqlstate => $sqlstate, message => $diagnostic });
                append_text($paths->{error}, "$statement_id: $diagnostic\n");
                if ($expected_outcome eq 'success') {
                    push @failures, { statement_id => $statement_id, message => $diagnostic };
                    if (flag_enabled($args, '--stop-on-error', 1)) {
                        add_timing(\%timings, $group, $statement_started);
                        my $event = event_record($args, $index + 1, $statement_id, $sql, $group, $expected_outcome, $outcome, $sqlstate, $diagnostic, $row_count, $result_digest, now_ns() - $statement_started);
                        append_jsonl($paths->{events}, $event);
                        push @testcases, $event;
                        last;
                    }
                } else {
                    push @security_refusals, { statement_id => $statement_id, sqlstate => $sqlstate, diagnostic_code => $diagnostic };
                }
            }
            my $elapsed = now_ns() - $statement_started;
            add_timing(\%timings, $group, $statement_started);
            my $event = event_record($args, $index + 1, $statement_id, $sql, $group, $expected_outcome, $outcome, $sqlstate, $diagnostic, $row_count, $result_digest, $elapsed);
            append_jsonl($paths->{events}, $event);
            push @testcases, $event;
        }

        my $metadata_started = now_ns();
        eval {
            my $tables = $dbh->table_info(undef, undef, undef, undef);
            $api_hits{table_info}++;
            my $columns = $dbh->column_info(undef, undef, undef, undef);
            $api_hits{column_info}++;
            write_json($paths->{metadata}, {
                tables_statement => ($tables ? $tables->{Statement} : undef),
                columns_statement => ($columns ? $columns->{Statement} : undef),
            });
            $metadata_written = 1;
        };
        if ($@) {
            write_json($paths->{metadata}, { status => 'error', message => clean_error($@), sqlstate => sqlstate_from_error($@) });
            $metadata_written = 1;
        }
        add_timing(\%timings, 'metadata', $metadata_started);
    };
    if ($@) {
        my $message = clean_error($@);
        push @failures, { statement_id => 'run', message => $message };
        append_jsonl($paths->{diagnostics}, { statement_id => 'run', sqlstate => sqlstate_from_error($message), message => $message });
        append_text($paths->{stderr}, "$message\n");
    }
    if ($dbh) {
        eval { $dbh->disconnect };
    }
    if (!$metadata_written) {
        write_json($paths->{metadata}, { status => 'not_run' });
    }

    my $elapsed = now_ns() - $started;
    $timings{overall} = $elapsed;
    my $process_metrics = current_process_metrics();
    my $summary = {
        run_id => value_or_default($args, '--run-id', 'manual'),
        driver_name => 'perl',
        route => required($args, '--route'),
        parser_mode => required($args, '--parser-mode'),
        page_size => required($args, '--page-size'),
        namespace => required($args, '--namespace'),
        sslmode => effective_sslmode_for_route(required($args, '--route'), value_or_default($args, '--sslmode', 'require')),
        transport_mode => resolve_transport_mode(required($args, '--route'), effective_sslmode_for_route(required($args, '--route'), value_or_default($args, '--sslmode', 'require'))),
        transport_endpoint_kind => endpoint_kind_for_route(required($args, '--route')),
        driver_transport_implementation => transport_implementation_for_route(required($args, '--route')),
        cpp_library_boundary => 'none',
        language_resource_pack => value_or_default($args, '--language-resource-pack', 'project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack'),
        language_resource_identity => value_or_default($args, '--language-resource-identity', 'sbsql.common_resource_pack.v1'),
        language_resource_hash => value_or_default($args, '--language-resource-hash', 'sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc'),
        language_resource_authority => 'shared_server_parser_resource_pack',
        language_profile => value_or_default($args, '--language-profile', 'en-US'),
        syntax_profile => value_or_default($args, '--syntax-profile', 'sbsql.v3'),
        topology_profile => value_or_default($args, '--topology-profile', 'topology.sbsql.canonical.v1'),
        standard_english_fallback => flag_enabled($args, '--standard-english-fallback', 1) ? JSON::PP::true : JSON::PP::false,
        status => @failures ? 'fail' : 'pass',
        statement_count => scalar @statements,
        failure_count => scalar @failures,
        elapsed_ns => $elapsed,
	        process_metrics => $process_metrics,
	        route_environment => $route_env,
	        server_revalidation_required => JSON::PP::true,
        driver_or_parser_finality => 'forbidden',
        mga_authority => 'engine',
        artifacts => {
            'command-events.jsonl' => $paths->{events},
            'summary.json' => $paths->{summary},
            'diagnostics.jsonl' => $paths->{diagnostics},
            'wire-transcript.jsonl' => $paths->{wire},
            'timing-groups.json' => $paths->{timing},
            'result-digests.json' => $paths->{digests},
            'metadata-snapshots.json' => $paths->{metadata},
            'route-environment.json' => $paths->{route_environment},
            'process-metrics.jsonl' => $paths->{process},
            'security-refusals.json' => $paths->{refusals},
            'native-api-coverage.json' => $paths->{api},
            'code-example-review.json' => $paths->{review},
            'junit.xml' => $paths->{junit},
            'stdout.log' => $paths->{stdout},
            'stderr.log' => $paths->{stderr},
        },
    };
    write_json($paths->{summary}, $summary);
    write_json($paths->{metrics}, { role => 'client', rss_kb => $process_metrics->{client}->{last_rss_kb}, vsize_kb => $process_metrics->{client}->{last_vsize_kb} });
    write_json($paths->{timing}, \%timings);
    write_json($paths->{digests}, \@digests);
    append_jsonl($paths->{process}, { role => 'client', rss_kb => $process_metrics->{client}->{last_rss_kb}, vsize_kb => $process_metrics->{client}->{last_vsize_kb} });
    write_json($paths->{refusals}, \@security_refusals);
    write_json($paths->{api}, \%api_hits);
    write_json($paths->{review}, {
        driver => 'perl',
        public_api_only => JSON::PP::true,
        shells_out_to_other_driver => JSON::PP::false,
        source_is_canonical_example => JSON::PP::true,
        sections => [qw(connection prepare execute fetchrow_arrayref metadata diagnostics transaction)],
    });
    write_text($paths->{junit}, junit_xml('SBIsqlPerl', 'scratchbird.perl', \@testcases, \@failures));
    append_text($paths->{stdout}, 'SBIsqlPerl status=' . $summary->{status} . "\n");
    return @failures ? 1 : 0;
}

sub require_host_packages {
    eval {
        require DBI;
        DBI->import;
        require DBD::ScratchBird;
        1;
    } or die "missing Perl module DBI required by sb_isql_perl.pl; install DBI and load project/drivers/driver/perl/lib so the tool can use DBI directly: $@";
}

sub build_dsn {
    my ($args) = @_;
    my $route = required($args, '--route');
    my @parts = (
        'database=' . required($args, '--database'),
        'host=' . value_or_default($args, '--host', '127.0.0.1'),
        'port=' . value_or_default($args, '--port', '3092'),
        'user=' . required($args, '--user'),
        'password=' . required($args, '--password'),
        'role=' . value_or_default($args, '--role', ''),
        'sslmode=' . effective_sslmode_for_route($route, value_or_default($args, '--sslmode', 'require')),
        'sslrootcert=' . value_or_default($args, '--sslrootcert', ''),
        'sslcert=' . value_or_default($args, '--sslcert', ''),
        'sslkey=' . value_or_default($args, '--sslkey', ''),
        'transport=' . transport_config_for_route($route),
        'ipc_path=' . value_or_default($args, '--ipc-path', ''),
        'parser_mode=' . required($args, '--parser-mode'),
        'front_door_mode=' . ($route eq 'manager-listener-parser' ? 'manager_proxy' : 'direct'),
    );
    return 'dbi:ScratchBird:' . join(';', @parts);
}

sub parse_args {
    my (@raw) = @_;
    my %args;
    my $i = 0;
    while ($i <= $#raw) {
        my $key = $raw[$i];
        die "unexpected positional argument: $key\n" unless $key =~ /^--/;
        die "unsupported argument: $key\n" unless $SUPPORTED_ARGS{$key};
        if ($BOOLEAN_ARGS{$key}) {
            if ($i + 1 <= $#raw && $raw[$i + 1] !~ /^--/) {
                $args{$key} = parse_bool_value($key, $raw[$i + 1]);
                $i += 2;
            } else {
                $args{$key} = 1;
                $i++;
            }
        } else {
            die "missing value for $key\n" if $i + 1 > $#raw || $raw[$i + 1] =~ /^--/;
            $args{$key} = $raw[$i + 1];
            $i += 2;
        }
    }
    return \%args;
}

sub validate_args {
    my ($args) = @_;
    die 'unsupported page size: ' . required($args, '--page-size') . "\n" unless $PAGE_SIZES{required($args, '--page-size')};
    die 'unsupported route: ' . required($args, '--route') . "\n" unless $ROUTES{required($args, '--route')};
    die 'unsupported parser mode: ' . required($args, '--parser-mode') . "\n" unless $PARSER_MODES{required($args, '--parser-mode')};
    die 'unsupported sslmode: ' . value_or_default($args, '--sslmode', 'require') . "\n" unless $SSLMODES{value_or_default($args, '--sslmode', 'require')};
}

sub required {
    my ($args, $key) = @_;
    die "missing required argument $key\n" unless defined $args->{$key} && length "$args->{$key}";
    return "$args->{$key}";
}

sub value_or_default {
    my ($args, $key, $default) = @_;
    return defined $args->{$key} ? "$args->{$key}" : $default;
}

sub flag_enabled {
    my ($args, $key, $default) = @_;
    return exists $args->{$key} ? ($args->{$key} ? 1 : 0) : $default;
}

sub parse_bool_value {
    my ($key, $value) = @_;
    my $normalized = lc "$value";
    return 1 if $normalized =~ /^(1|true|yes|on)$/;
    return 0 if $normalized =~ /^(0|false|no|off)$/;
    die "$key expects a boolean value, got: $value\n";
}

sub artifact_paths {
    my ($args, $run_root) = @_;
    return {
        output => required($args, '--output'),
        error => required($args, '--error'),
        diagnostics => required($args, '--diagnostics'),
        metrics => required($args, '--metrics'),
        transcript => required($args, '--transcript'),
        summary => required($args, '--summary'),
        events => "$run_root/command-events.jsonl",
        wire => "$run_root/wire-transcript.jsonl",
        timing => "$run_root/timing-groups.json",
        digests => "$run_root/result-digests.json",
        metadata => "$run_root/metadata-snapshots.json",
        route_environment => "$run_root/route-environment.json",
        process => "$run_root/process-metrics.jsonl",
        refusals => "$run_root/security-refusals.json",
        api => "$run_root/native-api-coverage.json",
        review => "$run_root/code-example-review.json",
        junit => "$run_root/junit.xml",
        stdout => "$run_root/stdout.log",
        stderr => "$run_root/stderr.log",
    };
}

sub initialize_artifacts {
    my %seen;
    for my $path (@_) {
        next if $seen{$path}++;
        write_text($path, '');
    }
}

sub ensure_route_supported {
    my ($route, $args) = @_;
    die "embedded transport is unsupported by the Perl driver; no ScratchBird C++ library boundary is exposed\n" if $route eq 'embedded';
    die "ipc_path is required for local IPC transport\n" if $route eq 'ipc_local' && !length value_or_default($args, '--ipc-path', '');
}

sub effective_sslmode_for_route {
    my ($route, $sslmode) = @_;
    return $route eq 'ipc_local' ? 'disable' : $sslmode;
}

sub transport_config_for_route {
    my ($route) = @_;
    return 'ipc' if $route eq 'ipc_local';
    return 'embedded' if $route eq 'embedded';
    return 'inet';
}

sub resolve_transport_mode {
    my ($route, $sslmode) = @_;
    return 'embedded_no_network_transport' if $route eq 'embedded';
    return 'local_ipc_no_tls' if $route eq 'ipc_local';
    return $sslmode eq 'disable' ? 'tls_disabled' : 'tls_required';
}

sub endpoint_kind_for_route {
    my ($route) = @_;
    return 'unix_domain_socket' if $route eq 'ipc_local';
    return 'embedded_bridge' if $route eq 'embedded';
    return 'tcp';
}

sub transport_implementation_for_route {
    my ($route) = @_;
    return 'native_perl_unix_socket' if $route eq 'ipc_local';
    return 'unsupported_no_cpp_library_boundary' if $route eq 'embedded';
    return 'native_perl_tcp';
}

sub route_environment {
    my ($args, $actual, $status, $reason) = @_;
    my $route = required($args, '--route');
    my $sslmode = effective_sslmode_for_route($route, value_or_default($args, '--sslmode', 'require'));
    my $record = {
        driver => 'perl',
        route => required($args, '--route'),
        run_id => value_or_default($args, '--run-id', 'manual'),
        sslmode => $sslmode,
        parser_mode => required($args, '--parser-mode'),
        concurrency_mode => value_or_default($args, '--concurrency-worker', 'single'),
        namespace => required($args, '--namespace'),
        page_size => required($args, '--page-size'),
        expected_page_size_bytes => $PAGE_SIZE_BYTES{required($args, '--page-size')},
        actual_page_size_bytes => $actual,
        page_size_verification_source => 'SHOW DATABASE',
        page_size_verification_status => $status,
        transport_mode => resolve_transport_mode($route, $sslmode),
        transport_endpoint_kind => endpoint_kind_for_route($route),
        driver_transport_implementation => transport_implementation_for_route($route),
    };
    $record->{failure_reason} = $reason if defined $reason && length $reason;
    return $record;
}

sub probe_route_environment {
    my ($dbh, $args, $api_hits) = @_;
    my $actual;
    my $failure;
    eval {
        my $sth = $dbh->prepare('SHOW DATABASE');
        $api_hits->{prepare}++;
        die($dbh->errstr || 'DBI prepare failed without an error string') unless $sth;
        my $ok = $sth->execute;
        $api_hits->{execute}++;
        die($sth->errstr || 'DBI execute failed without an error string') unless $ok;
        my @names = map { lc($_ // '') } @{$sth->{NAME} || []};
        my $page_index = -1;
        for my $index (0 .. $#names) {
            if ($names[$index] eq 'page_size_bytes') {
                $page_index = $index;
                last;
            }
        }
        while (my $row = $sth->fetchrow_arrayref) {
            $api_hits->{fetchrow_arrayref}++;
            if ($page_index >= 0 && defined $row->[$page_index]) {
                $actual = int($row->[$page_index]);
                last;
            }
        }
        die 'SHOW DATABASE did not expose page_size_bytes' unless defined $actual;
        1;
    } or do {
        $failure = clean_error($@ || 'route page-size probe failed');
    };
    my $expected = $PAGE_SIZE_BYTES{required($args, '--page-size')};
    my $status = defined $actual && $actual == $expected ? 'pass' : 'fail';
    my $reason = $status eq 'pass' ? undef : ($failure || 'actual_page_size_mismatch');
    return route_environment($args, $actual, $status, $reason);
}

sub run_transaction {
    my ($dbh, $sql, $api_hits) = @_;
    my $body = significant_sql($sql);
    my $first = first_token($body);
    if ($first eq 'commit') {
        my $ok = $dbh->commit;
        $api_hits->{commit}++;
        die($dbh->errstr || 'DBI commit failed') unless $ok;
    } elsif ($first eq 'rollback') {
        if ($body =~ /^rollback\s+to\s+(?:savepoint\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*$/i) {
            DBD::ScratchBird::Util::txn_control($dbh, 'rollback_to_savepoint', $1);
        } else {
            my $ok = $dbh->rollback;
            $api_hits->{rollback}++;
            die($dbh->errstr || 'DBI rollback failed') unless $ok;
        }
    } elsif ($first eq 'savepoint') {
        my $name = savepoint_name_from_sql($body, 'savepoint');
        DBD::ScratchBird::Util::txn_control($dbh, 'savepoint', $name);
    } elsif ($first eq 'release') {
        my $name = savepoint_name_from_sql($body, 'release');
        DBD::ScratchBird::Util::txn_control($dbh, 'release_savepoint', $name);
    } else {
        my $ok = $dbh->begin_work;
        die($dbh->errstr || 'DBI begin failed') unless $ok;
    }
}

sub savepoint_name_from_sql {
    my ($sql, $kind) = @_;
    if ($kind eq 'savepoint' && $sql =~ /^savepoint\s+([A-Za-z_][A-Za-z0-9_]*)\s*$/i) {
        return $1;
    }
    if ($kind eq 'release' && $sql =~ /^release\s+(?:savepoint\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*$/i) {
        return $1;
    }
    die "3B000: invalid $kind savepoint syntax";
}

sub event_record {
    my ($args, $index, $statement_id, $sql, $group, $expected_outcome, $outcome, $sqlstate, $diagnostic, $row_count, $result_digest, $elapsed) = @_;
    return {
        run_id => value_or_default($args, '--run-id', 'manual'),
        driver_name => 'perl',
        driver_version => '0.01',
        route => required($args, '--route'),
        parser_mode => required($args, '--parser-mode'),
        page_size => required($args, '--page-size'),
        namespace => required($args, '--namespace'),
        script => required($args, '--input'),
        statement_index => $index,
        statement_id => $statement_id,
        command_group => $group,
        sql_hash => sha256_text($sql),
        expected_outcome => $expected_outcome,
        actual_outcome => $outcome,
        sqlstate => $sqlstate,
        diagnostic_code => $diagnostic,
        canonical_message_vector => [],
        row_count => $row_count,
        result_digest => $result_digest,
        elapsed_ns => $elapsed,
        server_revalidation_state => 'required',
        language_profile => value_or_default($args, '--language-profile', 'en-US'),
        language_resource_pack => value_or_default($args, '--language-resource-pack', 'project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack'),
        language_resource_identity => value_or_default($args, '--language-resource-identity', 'sbsql.common_resource_pack.v1'),
        language_resource_hash => value_or_default($args, '--language-resource-hash', 'sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc'),
        syntax_profile => value_or_default($args, '--syntax-profile', 'sbsql.v3'),
        topology_profile => value_or_default($args, '--topology-profile', 'topology.sbsql.canonical.v1'),
        standard_english_fallback => flag_enabled($args, '--standard-english-fallback', 1) ? JSON::PP::true : JSON::PP::false,
        transaction_id_observed => undef,
        mga_authority => 'engine',
        native_api_surface => 'perl_dbi',
        code_example_section => 'prepare_execute_fetchrow_arrayref',
    };
}

sub load_expected_refusals {
    my ($path) = @_;
    return {} unless defined $path && length $path;
    die "expected refusal file not found: $path\n" unless -f $path;
    my $doc = decode_json_text(read_input($path));
    my %ids;
    add_expected_refusal_ids(\%ids, $doc);
    if (ref($doc) ne 'HASH' && ref($doc) ne 'ARRAY') {
        die "expected refusals must be a JSON object or array\n";
    }
    return \%ids;
}

sub add_expected_refusal_ids {
    my ($ids, $value) = @_;
    return unless defined $value;
    if (!ref($value)) {
        $ids->{$value} = 1 if length "$value";
        return;
    }
    if (ref($value) eq 'ARRAY') {
        add_expected_refusal_ids($ids, $_) for @{$value};
        return;
    }
    if (ref($value) ne 'HASH') {
        return;
    }
    for my $key (qw(statement_id statementId id)) {
        $ids->{$value->{$key}} = 1
            if defined $value->{$key} && length "$value->{$key}";
    }
    for my $key (qw(statement_ids statementIds expected_refusals expectedRefusals expected_diagnostics expectedDiagnostics)) {
        next unless exists $value->{$key};
        if (ref($value->{$key}) eq 'HASH') {
            $ids->{$_} = 1 for keys %{$value->{$key}};
        } else {
            add_expected_refusal_ids($ids, $value->{$key});
        }
    }
}

sub read_input {
    my ($path) = @_;
    if ($path eq '-') {
        local $/;
        return <STDIN>;
    }
    open my $fh, '<', $path or die "cannot read $path: $!\n";
    local $/;
    my $text = <$fh>;
    close $fh;
    return $text;
}

sub split_statements {
    my ($script) = @_;
    my @statements;
    my $term = ';';
    my $current = '';
    my $single = 0;
    my $double = 0;
    my $i = 0;
    while ($i < length $script) {
        my $ch = substr($script, $i, 1);
        if (!$single && !$double && $ch eq '-' && $i + 1 < length($script) && substr($script, $i + 1, 1) eq '-') {
            my $eol = index($script, "\n", $i);
            $eol = length($script) if $eol < 0;
            $current .= substr($script, $i, $eol - $i);
            $i = $eol;
            next;
        }
        if ($ch eq "'" && !$double) {
            $single = !$single;
        } elsif ($ch eq '"' && !$single) {
            $double = !$double;
        }
        if (!$single && !$double && length($term) && substr($script, $i, length($term)) eq $term) {
            my $matched_len = length($term);
            my $new_term = flush_statement(\@statements, $current);
            $term = $new_term if defined $new_term;
            $current = '';
            $i += $matched_len;
            next;
        }
        $current .= $ch;
        $i++;
    }
    flush_statement(\@statements, $current);
    return @statements;
}

sub flush_statement {
    my ($statements, $chunk) = @_;
    my $stripped = trim($chunk);
    return unless length $stripped;
    my @meaningful = grep { length($_) && $_ !~ /^--/ } map { trim($_) } split /\n/, $stripped;
    if (@meaningful == 1 && $meaningful[0] =~ /^set\s+term\s+(.+)$/i) {
        return trim($1);
    }
    push @{$statements}, $stripped;
    return;
}

sub classify_statement {
    my ($sql) = @_;
    my $first = first_token($sql);
    return 'ddl' if $first =~ /^(create|alter|drop)$/;
    return 'dml' if $first =~ /^(insert|update|delete|merge|upsert)$/;
    return 'transaction' if $first =~ /^(commit|rollback|savepoint|release|begin|start)$/;
    return 'security_refusal' if $first =~ /^(grant|revoke)$/;
    return 'metadata' if lc($sql) =~ /sys\./;
    return 'query';
}

sub first_token {
    my ($sql) = @_;
    $sql = trim(lc(significant_sql($sql)));
    my ($first) = split /\s+/, $sql, 2;
    return $first // '';
}

sub significant_sql {
    my ($sql) = @_;
    my @lines = split /\n/, ($sql // '');
    my @meaningful;
    for my $line (@lines) {
        $line =~ s/--.*\z//;
        $line = trim($line);
        push @meaningful, $line if length $line;
    }
    return trim(join(' ', @meaningful));
}

sub trim {
    my ($text) = @_;
    $text //= '';
    $text =~ s/\A\s+//;
    $text =~ s/\s+\z//;
    return $text;
}

sub current_process_metrics {
    my ($rss_kb, $vsize_kb) = (1, 1);
    if (open my $fh, '<', "/proc/$$/status") {
        while (my $line = <$fh>) {
            $rss_kb = $1 if $line =~ /^VmRSS:\s+(\d+)/;
            $vsize_kb = $1 if $line =~ /^VmSize:\s+(\d+)/;
        }
        close $fh;
    }
    $rss_kb = 1 if $rss_kb < 1;
    $vsize_kb = $rss_kb if $vsize_kb < 1;
    return {
        client => {
            last_rss_kb => int($rss_kb),
            last_vsize_kb => int($vsize_kb),
            max_rss_kb => int($rss_kb),
            max_vsize_kb => int($vsize_kb),
        },
    };
}

sub now_ns {
    return int(time() * 1_000_000_000);
}

sub add_timing {
    my ($timings, $group, $started) = @_;
    $timings->{$group} = ($timings->{$group} // 0) + (now_ns() - $started);
}

sub sha256_text {
    my ($text) = @_;
    return 'sha256:' . sha256_hex($text // '');
}

sub sqlstate_from_error {
    my ($message) = @_;
    return $1 if defined $message && $message =~ /\b([0-9A-Z]{5}):/;
    return 'HY000';
}

sub clean_error {
    my ($message) = @_;
    $message //= '';
    chomp $message;
    $message =~ s/\s+at\s+\S+\s+line\s+\d+\.?\z//;
    return $message;
}

sub decode_json_text {
    my ($text) = @_;
    return JSON::PP->new->utf8->decode($text);
}

sub json_text {
    my ($value) = @_;
    return JSON::PP->new->utf8->canonical->allow_nonref->encode($value);
}

sub write_json {
    my ($path, $value) = @_;
    write_text($path, json_text($value) . "\n");
}

sub append_jsonl {
    my ($path, $value) = @_;
    append_text($path, json_text($value) . "\n");
}

sub write_text {
    my ($path, $text) = @_;
    my $dir = dirname($path);
    make_path($dir) if defined $dir && length $dir && !-d $dir;
    open my $fh, '>', $path or die "cannot write $path: $!\n";
    print {$fh} $text;
    close $fh;
}

sub append_text {
    my ($path, $text) = @_;
    my $dir = dirname($path);
    make_path($dir) if defined $dir && length $dir && !-d $dir;
    open my $fh, '>>', $path or die "cannot append $path: $!\n";
    print {$fh} $text;
    close $fh;
}

sub junit_xml {
    my ($suite, $class, $testcases, $failures) = @_;
    my $tests = @$testcases > 0 ? scalar @$testcases : 1;
    my $xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    $xml .= '<testsuite name="' . xml_escape($suite) . '" tests="' . $tests . '" failures="' . scalar(@$failures) . "\">\n";
    if (!@$testcases) {
        $xml .= '  <testcase classname="' . xml_escape($class) . "\" name=\"run\"></testcase>\n";
    }
    for my $testcase (@$testcases) {
        $xml .= '  <testcase classname="' . xml_escape($class) . '" name="' . xml_escape($testcase->{statement_id}) . "\"></testcase>\n";
    }
    for my $failure (@$failures) {
        $xml .= '  <testcase classname="' . xml_escape($class) . '" name="' . xml_escape($failure->{statement_id}) . '"><failure message="' . xml_escape($failure->{message}) . "\" /></testcase>\n";
    }
    $xml .= "</testsuite>\n";
    return $xml;
}

sub xml_escape {
    my ($text) = @_;
    $text //= '';
    $text =~ s/&/&amp;/g;
    $text =~ s/"/&quot;/g;
    $text =~ s/</&lt;/g;
    $text =~ s/>/&gt;/g;
    return $text;
}
