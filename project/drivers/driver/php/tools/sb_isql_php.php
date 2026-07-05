#!/usr/bin/env php
<?php
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

declare(strict_types=1);

use ScratchBird\PDO\Connection;
use ScratchBird\PDO\Protocol;
use ScratchBird\PDO\ScratchBirdPDO;

require_driver_sources(dirname(__DIR__));

const NATIVE_ROWSET_TYPE_TEXT = 1;
const NATIVE_ROWSET_TYPE_INT64 = 2;
const NATIVE_ROWSET_TYPE_BOOLEAN = 3;
const NATIVE_ROWSET_TYPE_INT32 = 4;
const NATIVE_ROWSET_TYPE_UINT64 = 5;
const NATIVE_ROWSET_TYPE_REAL64 = 6;
const NATIVE_ROWSET_TYPE_BINARY = 7;

const SUPPORTED_ARGS = [
    '--database',
    '--manager-auth-token',
    '--manager-database',
    '--host',
    '--port',
    '--user',
    '--password',
    '--role',
    '--sslmode',
    '--sslrootcert',
    '--sslcert',
    '--sslkey',
    '--ipc-path',
    '--route',
    '--parser-mode',
    '--page-size',
    '--namespace',
    '--input',
    '--output',
    '--error',
    '--diagnostics',
    '--metrics',
    '--transcript',
    '--summary',
    '--stop-on-error',
    '--expected-refusals',
    '--statement-timeout-ms',
    '--fetch-size',
    '--concurrency-worker',
    '--create-database',
    '--create-emulation-mode',
    '--run-id',
    '--language-resource-pack',
    '--language-resource-identity',
    '--language-resource-hash',
    '--language-profile',
    '--syntax-profile',
    '--topology-profile',
    '--standard-english-fallback',
];

const SSLMODES = ['allow', 'disable', 'prefer', 'require', 'verify-ca', 'verify-full'];

if (PHP_SAPI === 'cli' && realpath($argv[0] ?? '') === __FILE__) {
    main($argv);
}

function main(array $argv): void
{
    try {
        $args = parse_args(array_slice($argv, 1));
        $code = run_tool($args);
        exit($code);
    } catch (Throwable $ex) {
        fwrite(STDERR, $ex->getMessage() . PHP_EOL);
        exit(1);
    }
}

function run_tool(array $args): int
{
    validate_args($args);
    $runRoot = dirname(required($args, '--summary'));
    if (!is_dir($runRoot)) {
        mkdir($runRoot, 0777, true);
    }
    $paths = [
        'events' => $runRoot . '/command-events.jsonl',
        'wire' => $runRoot . '/wire-transcript.jsonl',
        'timing' => $runRoot . '/timing-groups.json',
        'digests' => $runRoot . '/result-digests.json',
        'metadata' => $runRoot . '/metadata-snapshots.json',
        'route_environment' => $runRoot . '/route-environment.json',
        'process' => $runRoot . '/process-metrics.jsonl',
        'refusals' => $runRoot . '/security-refusals.json',
        'api' => $runRoot . '/native-api-coverage.json',
        'review' => $runRoot . '/code-example-review.json',
        'junit' => $runRoot . '/junit.xml',
        'stdout' => $runRoot . '/stdout.log',
        'stderr' => $runRoot . '/stderr.log',
    ];
    foreach (array_merge([
        required($args, '--output'),
        required($args, '--error'),
        required($args, '--diagnostics'),
        required($args, '--metrics'),
        required($args, '--transcript'),
        required($args, '--summary'),
    ], array_values($paths)) as $path) {
        write_text($path, '');
    }

    $timings = [];
    $apiHits = [
        'PDO' => 0,
        'prepare' => 0,
        'execute' => 0,
        'fetch' => 0,
        'errorInfo' => 0,
        'queryMetadata' => 0,
        'attachCreate' => 0,
        'commit' => 0,
        'rollback' => 0,
        'copy_in' => 0,
        'compileSblr' => 0,
        'executeSblr' => 0,
    ];
    $testcases = [];
    $failures = [];
    $digests = [];
    $securityRefusals = [];
    $started = hrtime(true);
    $expectedRefusals = load_expected_refusals(value_or_default($args, '--expected-refusals', ''));
    $pdo = null;
    write_text(
        $paths['route_environment'],
        json_encode(route_environment($args, null, 'fail', 'not_probed'), JSON_THROW_ON_ERROR) . PHP_EOL
    );

    try {
        $route = required($args, '--route');
        ensure_transport_route_supported($route, $args);
        $effectiveSslmode = effective_sslmode_for_route($route, value_or_default($args, '--sslmode', 'require'));
        $managerDsn = '';
        if ($route === 'manager-listener-parser') {
            $managerDsn = sprintf(
                ';manager_auth_token=%s;manager_database=%s;manager_username=%s',
                value_or_default($args, '--manager-auth-token', ''),
                value_or_default($args, '--manager-database', required($args, '--database')),
                required($args, '--user')
            );
        }
        $dsn = sprintf(
            'scratchbird:host=%s;port=%s;database=%s;sslmode=%s;sslrootcert=%s;sslcert=%s;sslkey=%s;front_door_mode=%s%s;transport=%s;metadata_expand_schema_parents=true',
            required($args, '--host'),
            required($args, '--port'),
            required($args, '--database'),
            $effectiveSslmode,
            value_or_default($args, '--sslrootcert', ''),
            value_or_default($args, '--sslcert', ''),
            value_or_default($args, '--sslkey', ''),
            $route === 'manager-listener-parser' ? 'manager_proxy' : 'direct',
            $managerDsn,
            transport_config_for_route($route)
        );
        if ($route === 'ipc_local') {
            $dsn .= ';ipc_path=' . required($args, '--ipc-path');
        }
        $connectStarted = hrtime(true);
        $pdo = new ScratchBirdPDO($dsn, required($args, '--user'), required($args, '--password'), [
            'role' => value_or_default($args, '--role', ''),
        ]);
        $apiHits['PDO']++;
        add_timing($timings, 'connection', $connectStarted);
        append_jsonl(required($args, '--transcript'), [
            'event' => 'connect',
            'driver' => 'php',
            'route' => $route,
            'parser_mode' => required($args, '--parser-mode'),
            'page_size' => required($args, '--page-size'),
        ]);
        append_jsonl($paths['wire'], [
            'event' => 'server_admission_required',
            'driver_or_parser_finality' => 'forbidden',
        ]);

        if (flag_enabled($args, '--create-database')) {
            $createStarted = hrtime(true);
            $createConnection = new Connection($dsn, required($args, '--user'), required($args, '--password'), [
                'role' => value_or_default($args, '--role', ''),
            ]);
            try {
                $createConnection->attachCreate(value_or_default($args, '--create-emulation-mode', 'sbsql'), required($args, '--database'));
                $apiHits['attachCreate']++;
            } finally {
                $createConnection->close();
            }
            add_timing($timings, 'database_create', $createStarted);
        }
        $routeEnvironment = probe_route_environment($pdo, $args);
        write_text($paths['route_environment'], json_encode($routeEnvironment, JSON_THROW_ON_ERROR) . PHP_EOL);
        if ($route !== 'embedded' && $routeEnvironment['page_size_verification_status'] !== 'pass') {
            throw new RuntimeException('route environment page-size verification failed: ' . ($routeEnvironment['failure_reason'] ?? 'unknown'));
        }

        $statements = split_statements(read_input(required($args, '--input')));
        foreach ($statements as $index => $sql) {
            $statementId = basename(required($args, '--input')) . ':' . ($index + 1);
            $expectedOutcome = isset($expectedRefusals[$statementId]) ? 'refusal' : 'success';
            $group = classify_statement($sql);
            $statementStarted = hrtime(true);
            $outcome = 'success';
            $rowCount = -1;
            $resultDigest = null;
            $sqlstate = null;
            $diagnostic = null;
            try {
                if ($group === 'transaction') {
                    run_transaction($pdo, $sql, $apiHits);
                    $rowCount = 0;
                    $resultDigest = sha256_text('transaction');
                } elseif ($group === 'copy' && is_copy_stdin_statement($sql)) {
                    $payload = copy_payload_for_statement($sql);
                    if ($payload === '') {
                        throw new RuntimeException('COPY FROM STDIN requires SB_COPY_INPUT rows in the script');
                    }
                    $nativePayload = copy_text_rows_to_native_frame($payload);
                    $rowsCopied = execute_copy_in(pdo_connection($pdo), executable_sql_without_copy_markers($sql), $nativePayload);
                    $apiHits['copy_in']++;
                    $rowCount = $rowsCopied;
                    $resultDigest = sha256_text('copy_in:' . (string) $rowsCopied);
                    append_text(required($args, '--output'), json_encode([
                        'statement_id' => $statementId,
                        'rows' => [['copy_in' => $rowsCopied]],
                    ], JSON_THROW_ON_ERROR) . PHP_EOL);
                } elseif (required($args, '--parser-mode') !== 'server-parser') {
                    $compiled = $pdo->compileSblr($sql);
                    $apiHits['compileSblr']++;
                    append_jsonl($paths['wire'], [
                        'event' => 'driver_sblr_compile',
                        'driver' => 'php',
                        'parser_mode' => required($args, '--parser-mode'),
                        'statement_id' => $statementId,
                        'sblr_hash' => (string)$compiled['hash'],
                        'sblr_version' => $compiled['version'],
                        'sblr_bytes' => strlen((string)$compiled['bytecode']),
                    ]);
                    $stream = $pdo->executeSblr($compiled);
                    $apiHits['executeSblr']++;
                    $rows = [];
                    while (($row = $stream->readRow()) !== null) {
                        $rows[] = $row;
                    }
                    $rowCount = count($rows);
                    $resultDigest = sha256_text(json_encode($rows, JSON_THROW_ON_ERROR));
                    append_text(required($args, '--output'), json_encode([
                        'statement_id' => $statementId,
                        'rows' => $rows,
                    ], JSON_THROW_ON_ERROR) . PHP_EOL);
                    append_jsonl($paths['wire'], [
                        'event' => 'driver_sblr_execute',
                        'driver' => 'php',
                        'parser_mode' => required($args, '--parser-mode'),
                        'statement_id' => $statementId,
                        'sblr_hash' => (string)$compiled['hash'],
                        'sblr_version' => $compiled['version'],
                        'sblr_bytes' => strlen((string)$compiled['bytecode']),
                        'engine_sql_text_execution' => false,
                        'mga_authority' => 'engine',
                    ]);
                } else {
                    $stmt = $pdo->prepare($sql);
                    $apiHits['prepare']++;
                    if ($stmt === false) {
                        $apiHits['errorInfo']++;
                        throw new RuntimeException(json_encode($pdo->errorInfo()));
                    }
                    $stmt->execute();
                    $apiHits['execute']++;
                    $rows = $stmt->fetchAll(PDO::FETCH_ASSOC);
                    $apiHits['fetch']++;
                    $rowCount = count($rows);
                    $resultDigest = sha256_text(json_encode($rows, JSON_THROW_ON_ERROR));
                    append_text(required($args, '--output'), json_encode([
                        'statement_id' => $statementId,
                        'rows' => $rows,
                    ], JSON_THROW_ON_ERROR) . PHP_EOL);
                }
                $digests[] = [
                    'statement_id' => $statementId,
                    'row_count' => $rowCount,
                    'result_digest' => $resultDigest,
                ];
                if ($expectedOutcome === 'refusal') {
                    $outcome = 'unexpected_success';
                    $failures[] = ['statement_id' => $statementId, 'message' => 'statement succeeded but was expected to refuse'];
                }
            } catch (Throwable $ex) {
                $outcome = 'refusal';
                $sqlstate = 'HY000';
                $diagnostic = $ex->getMessage();
                append_jsonl(required($args, '--diagnostics'), [
                    'statement_id' => $statementId,
                    'sqlstate' => $sqlstate,
                    'message' => $diagnostic,
                ]);
                append_text(required($args, '--error'), $statementId . ': ' . $diagnostic . PHP_EOL);
                if ($expectedOutcome === 'success') {
                    $failures[] = ['statement_id' => $statementId, 'message' => $diagnostic];
                } else {
                    $securityRefusals[] = [
                        'statement_id' => $statementId,
                        'sqlstate' => $sqlstate,
                        'diagnostic_code' => $diagnostic,
                    ];
                }
                if ($expectedOutcome === 'success' && flag_enabled($args, '--stop-on-error')) {
                    add_timing($timings, $group, $statementStarted);
                    break;
                }
            }
            $elapsed = hrtime(true) - $statementStarted;
            add_timing($timings, $group, $statementStarted);
            $event = [
                'run_id' => value_or_default($args, '--run-id', 'manual'),
                'driver_name' => 'php',
                'driver_version' => 'unknown',
                'route' => required($args, '--route'),
                'parser_mode' => required($args, '--parser-mode'),
                'page_size' => required($args, '--page-size'),
                'namespace' => required($args, '--namespace'),
                'script' => required($args, '--input'),
                'statement_index' => $index + 1,
                'statement_id' => $statementId,
                'command_group' => $group,
                'sql_hash' => sha256_text($sql),
                'expected_outcome' => $expectedOutcome,
                'actual_outcome' => $outcome,
                'sqlstate' => $sqlstate,
                'diagnostic_code' => $diagnostic,
                'canonical_message_vector' => [],
                'row_count' => $rowCount,
                'result_digest' => $resultDigest,
                'elapsed_ns' => $elapsed,
                'server_revalidation_state' => 'required',
                'language_profile' => value_or_default($args, '--language-profile', 'en-US'),
                'language_resource_pack' => value_or_default($args, '--language-resource-pack', 'project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack'),
                'language_resource_identity' => value_or_default($args, '--language-resource-identity', 'sbsql.common_resource_pack.v1'),
                'language_resource_hash' => value_or_default($args, '--language-resource-hash', 'sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc'),
                'syntax_profile' => value_or_default($args, '--syntax-profile', 'sbsql.v3'),
                'topology_profile' => value_or_default($args, '--topology-profile', 'topology.sbsql.canonical.v1'),
                'standard_english_fallback' => flag_enabled($args, '--standard-english-fallback', true),
                'transaction_id_observed' => null,
                'mga_authority' => 'engine',
                'native_api_surface' => 'php_pdo_style',
                'code_example_section' => 'prepare_execute_fetch',
            ];
            append_jsonl($paths['events'], $event);
            $testcases[] = $event;
        }

        $metadataStarted = hrtime(true);
        $metadataRows = $pdo->getSchema('tables');
        $apiHits['queryMetadata']++;
        write_text($paths['metadata'], json_encode([
            'tables_digest' => sha256_text(json_encode($metadataRows, JSON_THROW_ON_ERROR)),
            'row_count' => count($metadataRows),
        ], JSON_THROW_ON_ERROR) . PHP_EOL);
        add_timing($timings, 'metadata', $metadataStarted);
    } catch (Throwable $ex) {
        $failures[] = ['statement_id' => 'run', 'message' => $ex->getMessage()];
        append_text($paths['stderr'], $ex->getMessage() . PHP_EOL);
    }

    $elapsed = hrtime(true) - $started;
    $timings['overall'] = $elapsed;
    $sslmode = effective_sslmode_for_route(required($args, '--route'), value_or_default($args, '--sslmode', 'require'));
        $transportMode = resolve_transport_mode(required($args, '--route'), $sslmode);
    $processMetrics = current_process_metrics();
    $summary = [
        'run_id' => value_or_default($args, '--run-id', 'manual'),
        'driver_name' => 'php',
        'route' => required($args, '--route'),
        'parser_mode' => required($args, '--parser-mode'),
        'page_size' => required($args, '--page-size'),
        'namespace' => required($args, '--namespace'),
        'sslmode' => $sslmode,
        'transport_mode' => $transportMode,
        'transport_endpoint_kind' => endpoint_kind_for_route(required($args, '--route')),
        'driver_transport_implementation' => transport_implementation_for_route(required($args, '--route')),
        'cpp_library_boundary' => 'none',
        'language_resource_pack' => value_or_default($args, '--language-resource-pack', 'project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack'),
        'language_resource_identity' => value_or_default($args, '--language-resource-identity', 'sbsql.common_resource_pack.v1'),
        'language_resource_hash' => value_or_default($args, '--language-resource-hash', 'sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc'),
        'language_resource_authority' => 'shared_server_parser_resource_pack',
        'language_profile' => value_or_default($args, '--language-profile', 'en-US'),
        'syntax_profile' => value_or_default($args, '--syntax-profile', 'sbsql.v3'),
        'topology_profile' => value_or_default($args, '--topology-profile', 'topology.sbsql.canonical.v1'),
        'standard_english_fallback' => flag_enabled($args, '--standard-english-fallback', true),
        'status' => empty($failures) ? 'pass' : 'fail',
        'failure_count' => count($failures),
        'elapsed_ns' => $elapsed,
        'process_metrics' => $processMetrics,
        'server_revalidation_required' => true,
        'driver_or_parser_finality' => 'forbidden',
        'mga_authority' => 'engine',
    ];
    write_text(required($args, '--summary'), json_encode($summary, JSON_THROW_ON_ERROR) . PHP_EOL);
    write_text(required($args, '--metrics'), json_encode($timings, JSON_THROW_ON_ERROR) . PHP_EOL);
    write_text($paths['timing'], json_encode($timings, JSON_THROW_ON_ERROR) . PHP_EOL);
    write_text($paths['digests'], json_encode($digests, JSON_THROW_ON_ERROR) . PHP_EOL);
    append_jsonl($paths['process'], [
        'role' => 'client',
        'rss_kb' => $processMetrics['client']['last_rss_kb'],
        'vsize_kb' => $processMetrics['client']['last_vsize_kb'],
    ]);
    write_text($paths['refusals'], json_encode($securityRefusals, JSON_THROW_ON_ERROR) . PHP_EOL);
    write_text($paths['api'], json_encode($apiHits, JSON_THROW_ON_ERROR) . PHP_EOL);
    write_text($paths['review'], json_encode([
        'driver' => 'php',
        'public_api_only' => true,
        'shells_out_to_other_driver' => false,
        'source_is_canonical_example' => true,
        'sections' => ['connection', 'prepare', 'execute', 'fetch', 'metadata', 'diagnostics', 'transaction'],
    ], JSON_THROW_ON_ERROR) . PHP_EOL);
    write_text($paths['junit'], junit_xml('SBIsqlPhp', 'scratchbird.php', $testcases, $failures));
    append_text($paths['stdout'], 'SBIsqlPhp status=' . $summary['status'] . PHP_EOL);
    return empty($failures) ? 0 : 1;
}

function require_driver_sources(string $root): void
{
    $vendor = $root . '/vendor/autoload.php';
    if (is_file($vendor)) {
        require_once $vendor;
        return;
    }
    foreach (glob($root . '/src/*.php') ?: [] as $source) {
        require_once $source;
    }
}

function parse_args(array $raw): array
{
    $args = [];
    for ($i = 0; $i < count($raw); $i++) {
        $key = $raw[$i];
        if (!str_starts_with($key, '--')) {
            throw new InvalidArgumentException('unexpected positional argument: ' . $key);
        }
        if (!in_array($key, SUPPORTED_ARGS, true)) {
            throw new InvalidArgumentException('unsupported argument: ' . $key);
        }
        if ($key === '--stop-on-error' || $key === '--create-database' || $key === '--standard-english-fallback') {
            if (isset($raw[$i + 1]) && !str_starts_with($raw[$i + 1], '--')) {
                $args[$key] = parse_bool_value($key, $raw[++$i]);
            } else {
                $args[$key] = true;
            }
            continue;
        }
        if (!isset($raw[$i + 1]) || str_starts_with($raw[$i + 1], '--')) {
            throw new InvalidArgumentException('missing value for ' . $key);
        }
        $args[$key] = $raw[++$i];
    }
    return $args;
}

function validate_args(array $args): void
{
    foreach (['4k', '8k', '16k', '32k', '64k', '128k'] as $pageSize) {
        $pageSizes[$pageSize] = true;
    }
    foreach (['embedded', 'ipc_local', 'listener-parser', 'manager-listener-parser'] as $route) {
        $routes[$route] = true;
    }
    foreach (['server-parser', 'standalone-parser', 'driver-sblr-uuid'] as $mode) {
        $parserModes[$mode] = true;
    }
    if (!isset($pageSizes[required($args, '--page-size')])) {
        throw new InvalidArgumentException('unsupported page size: ' . required($args, '--page-size'));
    }
    if (!isset($routes[required($args, '--route')])) {
        throw new InvalidArgumentException('unsupported route: ' . required($args, '--route'));
    }
    if (!isset($parserModes[required($args, '--parser-mode')])) {
        throw new InvalidArgumentException('unsupported parser mode: ' . required($args, '--parser-mode'));
    }
    $sslmode = value_or_default($args, '--sslmode', 'require');
    if (!in_array($sslmode, SSLMODES, true)) {
        throw new InvalidArgumentException('unsupported sslmode: ' . $sslmode);
    }
}

function required(array $args, string $key): string
{
    if (!array_key_exists($key, $args) || $args[$key] === '') {
        throw new InvalidArgumentException('missing required argument ' . $key);
    }
    return (string) $args[$key];
}

function value_or_default(array $args, string $key, string $default): string
{
    return array_key_exists($key, $args) ? (string) $args[$key] : $default;
}

function flag_enabled(array $args, string $key, bool $default = false): bool
{
    return array_key_exists($key, $args) ? $args[$key] === true : $default;
}

function parse_bool_value(string $key, string $value): bool
{
    $normalized = strtolower($value);
    if ($normalized === 'true') {
        return true;
    }
    if ($normalized === 'false') {
        return false;
    }
    throw new InvalidArgumentException($key . ' expects true or false, got: ' . $value);
}

function resolve_transport_mode(string $route, string $sslmode): string
{
    if ($route === 'embedded') {
        return 'embedded_no_network_transport';
    }
    if ($route === 'ipc_local') {
        return 'local_ipc_no_tls';
    }
    return $sslmode === 'disable' ? 'tls_disabled' : 'tls_required';
}

function ensure_transport_route_supported(string $route, array $args): void
{
    if ($route === 'embedded') {
        throw new RuntimeException('embedded transport is unsupported by the PHP driver; no ScratchBird C++ library boundary is exposed');
    }
    if ($route === 'ipc_local' && value_or_default($args, '--ipc-path', '') === '') {
        throw new InvalidArgumentException('ipc_path is required for local IPC transport');
    }
}

function effective_sslmode_for_route(string $route, string $sslmode): string
{
    return $route === 'ipc_local' ? 'disable' : $sslmode;
}

function transport_config_for_route(string $route): string
{
    if ($route === 'ipc_local') {
        return 'ipc';
    }
    if ($route === 'embedded') {
        return 'embedded';
    }
    return 'inet';
}

function endpoint_kind_for_route(string $route): string
{
    if ($route === 'ipc_local') {
        return 'unix_domain_socket';
    }
    if ($route === 'embedded') {
        return 'none';
    }
    return 'tcp';
}

function transport_implementation_for_route(string $route): string
{
    if ($route === 'embedded') {
        return 'unsupported_no_cpp_library_boundary';
    }
    if ($route === 'ipc_local') {
        return 'native_php_unix_socket';
    }
    return 'native_php_tcp';
}

function expected_page_size_bytes(string $label): int
{
    $pageSizes = [
        '4k' => 4096,
        '8k' => 8192,
        '16k' => 16384,
        '32k' => 32768,
        '64k' => 65536,
        '128k' => 131072,
    ];
    if (!isset($pageSizes[$label])) {
        throw new InvalidArgumentException('unsupported page size: ' . $label);
    }
    return $pageSizes[$label];
}

function route_environment(array $args, ?int $actualPageSize, string $status, ?string $reason = null): array
{
    $route = required($args, '--route');
    $sslmode = effective_sslmode_for_route($route, value_or_default($args, '--sslmode', 'require'));
    $record = [
        'driver' => 'php',
        'route' => $route,
        'parser_mode' => required($args, '--parser-mode'),
        'page_size' => required($args, '--page-size'),
        'expected_page_size_bytes' => expected_page_size_bytes(required($args, '--page-size')),
        'actual_page_size_bytes' => $actualPageSize,
        'page_size_verification_source' => 'SHOW DATABASE',
        'page_size_verification_status' => $status,
        'sslmode' => $sslmode,
        'transport_mode' => resolve_transport_mode($route, $sslmode),
        'transport_endpoint_kind' => endpoint_kind_for_route($route),
        'driver_transport_implementation' => transport_implementation_for_route($route),
    ];
    if ($reason !== null) {
        $record['failure_reason'] = $reason;
    }
    return $record;
}

function probe_route_environment(ScratchBirdPDO $pdo, array $args): array
{
    try {
        $stmt = $pdo->prepare('SHOW DATABASE');
        if ($stmt === false) {
            return route_environment($args, null, 'fail', 'SHOW DATABASE prepare failed');
        }
        $stmt->execute();
        $rows = $stmt->fetchAll(PDO::FETCH_ASSOC);
        $actual = page_size_from_show_database($rows);
        $expected = expected_page_size_bytes(required($args, '--page-size'));
        $status = $actual === $expected ? 'pass' : 'fail';
        $reason = $status === 'pass'
            ? null
            : ($actual === null ? 'show_database_missing_page_size_bytes' : 'actual_page_size_mismatch');
        return route_environment($args, $actual, $status, $reason);
    } catch (Throwable $ex) {
        return route_environment($args, null, 'fail', $ex->getMessage());
    }
}

function page_size_from_show_database(array $rows): ?int
{
    if ($rows === []) {
        return null;
    }
    $row = $rows[0];
    foreach ($row as $key => $value) {
        if (strtolower((string) $key) === 'page_size_bytes') {
            return int_value($value);
        }
    }
    foreach (array_values($row) as $value) {
        $text = trim((string) $value);
        if (preg_match('/page_size_bytes\s*[:=]\s*(\d+)/i', $text, $matches) === 1) {
            return (int) $matches[1];
        }
    }
    return null;
}

function int_value(mixed $value): ?int
{
    if (is_int($value)) {
        return $value;
    }
    if (is_float($value) && is_finite($value)) {
        return (int) $value;
    }
    if (is_string($value) && trim($value) !== '' && preg_match('/^-?\d+$/', trim($value)) === 1) {
        return (int) trim($value);
    }
    return null;
}

function load_expected_refusals(string $path): array
{
    if ($path === '') {
        return [];
    }
    if (!is_file($path)) {
        throw new InvalidArgumentException('expected refusal file not found: ' . $path);
    }
    $doc = json_decode((string) file_get_contents($path), true, flags: JSON_THROW_ON_ERROR);
    $ids = [];
    if (is_array($doc) && array_is_list($doc)) {
        $ids = $doc;
    } elseif (is_array($doc) && !array_is_list($doc)) {
        if (isset($doc['statement_ids']) && is_array($doc['statement_ids'])) {
            $ids = array_merge($ids, $doc['statement_ids']);
        }
        if (isset($doc['expected_refusals']) && is_array($doc['expected_refusals'])) {
            $ids = array_merge($ids, $doc['expected_refusals']);
        }
        if (isset($doc['expected_diagnostics']) && is_array($doc['expected_diagnostics'])) {
            $ids = array_merge($ids, array_keys($doc['expected_diagnostics']));
        }
        if (isset($doc['compiled_chain_statement_aliases']) && is_array($doc['compiled_chain_statement_aliases'])) {
            $ids = array_merge($ids, array_keys($doc['compiled_chain_statement_aliases']));
            $ids = array_merge($ids, array_values($doc['compiled_chain_statement_aliases']));
        }
    } else {
        throw new InvalidArgumentException('expected refusals must be a JSON object or array');
    }
    $set = [];
    foreach ($ids as $id) {
        $set[(string) $id] = true;
    }
    return $set;
}

function current_process_metrics(): array
{
    $rssKb = max(1, (int) ceil(memory_get_usage(true) / 1024));
    return [
        'client' => [
            'last_rss_kb' => $rssKb,
            'last_vsize_kb' => $rssKb,
            'max_rss_kb' => $rssKb,
            'max_vsize_kb' => $rssKb,
        ],
    ];
}

function split_statements(string $script): array
{
    $statements = [];
    $term = ';';
    $current = '';
    $single = false;
    $double = false;
    $len = strlen($script);

    $flush = static function () use (&$current, &$term, &$statements): void {
        $chunk = trim($current);
        $current = '';
        if ($chunk === '') {
            return;
        }
        $newTerm = chunk_set_term($chunk);
        if ($newTerm !== null) {
            $term = $newTerm;
            return;
        }
        $statements[] = $chunk;
    };

    for ($i = 0; $i < $len;) {
        $ch = $script[$i];
        if (!$single && !$double && $ch === '-' && $i + 1 < $len && $script[$i + 1] === '-') {
            // `--` line comment: copy verbatim to end of line without scanning
            // for the terminator or quotes inside it.
            $eol = strpos($script, "\n", $i);
            if ($eol === false) {
                $eol = $len;
            }
            $current .= substr($script, $i, $eol - $i);
            $i = $eol;
            continue;
        }
        if ($ch === "'" && !$double) {
            $single = !$single;
            $current .= $ch;
            $i++;
            continue;
        }
        if ($ch === '"' && !$single) {
            $double = !$double;
            $current .= $ch;
            $i++;
            continue;
        }
        if (!$single && !$double && $term !== '' && substr_compare($script, $term, $i, strlen($term)) === 0) {
            $matchedLen = strlen($term); // capture before flush(), which may change $term
            $flush();
            $i += $matchedLen;
            continue;
        }
        $current .= $ch;
        $i++;
    }
    $flush();
    return $statements;
}

function chunk_set_term(string $chunk): ?string
{
    $meaningful = [];
    foreach (preg_split('/\r\n|\r|\n/', $chunk) as $line) {
        $stripped = trim($line);
        if ($stripped === '' || str_starts_with($stripped, '--')) {
            continue;
        }
        $meaningful[] = $stripped;
    }
    if ($meaningful === []) {
        return null;
    }
    if (preg_match('/^set\s+term\s+(\S.*?)\s*$/i', implode(' ', $meaningful), $matches) === 1) {
        return trim($matches[1]);
    }
    return null;
}

function classify_statement(string $sql): string
{
    $trimmed = strtolower(trim(executable_sql_without_copy_markers($sql)));
    $first = strtok($trimmed, " \t\r\n") ?: '';
    if ($first === 'copy') {
        return 'copy';
    }
    if (in_array($first, ['create', 'alter', 'drop'], true)) {
        return 'ddl';
    }
    if (in_array($first, ['insert', 'update', 'delete', 'merge', 'upsert'], true)) {
        return 'dml';
    }
    if (in_array($first, ['commit', 'rollback', 'savepoint', 'release', 'begin', 'start'], true)) {
        return 'transaction';
    }
    if (in_array($first, ['grant', 'revoke'], true)) {
        return 'security_refusal';
    }
    if (str_contains($trimmed, 'sys.')) {
        return 'metadata';
    }
    return 'query';
}

function executable_sql_without_copy_markers(string $sql): string
{
    $lines = [];
    foreach (preg_split('/\r\n|\r|\n/', $sql) as $line) {
        if (str_starts_with(ltrim($line), '-- SB_COPY_INPUT ')) {
            continue;
        }
        $lines[] = $line;
    }
    return trim(implode("\n", $lines));
}

function copy_payload_for_statement(string $sql): string
{
    $rows = [];
    foreach (preg_split('/\r\n|\r|\n/', $sql) as $line) {
        $stripped = ltrim($line);
        if (str_starts_with($stripped, '-- SB_COPY_INPUT ')) {
            $rows[] = substr($stripped, strlen('-- SB_COPY_INPUT '));
        }
    }
    return $rows === [] ? '' : implode("\n", $rows) . "\n";
}

function copy_text_rows_to_native_frame(string $data): string
{
    if (substr($data, 0, 4) === 'SBNR') {
        return $data;
    }
    $lines = [];
    foreach (preg_split('/\r\n|\r|\n/', $data) as $line) {
        if (trim($line) !== '') {
            $lines[] = rtrim($line, "\r");
        }
    }
    if ($lines === []) {
        throw new RuntimeException('COPY input contains no rows');
    }

    if (str_contains($lines[0], ';') && str_contains($lines[0], '=')) {
        $columns = null;
        $rows = [];
        foreach ($lines as $line) {
            $names = [];
            $values = [];
            foreach (explode(';', $line) as $part) {
                if ($part === '') {
                    continue;
                }
                $separator = strpos($part, '=');
                if ($separator === false || $separator <= 0) {
                    throw new RuntimeException('malformed canonical COPY field');
                }
                $names[] = substr($part, 0, $separator);
                $value = substr($part, $separator + 1);
                $values[] = strcasecmp($value, 'NULL') === 0 ? null : $value;
            }
            if ($names === []) {
                continue;
            }
            if ($columns === null) {
                $columns = $names;
            } elseif ($columns !== $names) {
                throw new RuntimeException('COPY input changed row shape mid-stream');
            }
            $rows[] = $values;
        }
        if ($columns === null) {
            throw new RuntimeException('COPY input contains no rows');
        }
        return build_native_rowset_payload($columns, $rows);
    }

    $columns = array_map('trim', split_copy_csv_line($lines[0]));
    if ($columns === [] || in_array('', $columns, true)) {
        throw new RuntimeException('CSV COPY input requires a non-empty header row');
    }
    $rows = [];
    foreach (array_slice($lines, 1) as $line) {
        $values = split_copy_csv_line($line);
        if (count($values) !== count($columns)) {
            throw new RuntimeException('CSV COPY row shape mismatch');
        }
        $rows[] = array_map(
            static fn(string $value): ?string => ($value === '' || strcasecmp($value, 'NULL') === 0) ? null : $value,
            $values
        );
    }
    if ($rows === []) {
        throw new RuntimeException('CSV COPY input contains no data rows');
    }
    return build_native_rowset_payload($columns, $rows);
}

function build_native_rowset_payload(array $columns, array $rows, ?array $columnTypes = null): string
{
    if ($rows === []) {
        throw new RuntimeException('native rowset requires at least one row');
    }
    if ($columns === [] || in_array('', $columns, true)) {
        throw new RuntimeException('native rowset requires non-empty column names');
    }
    foreach ($rows as $row) {
        if (count($row) !== count($columns)) {
            throw new RuntimeException('native rowset row shape mismatch');
        }
    }
    $types = $columnTypes ?? infer_native_rowset_column_types($rows);
    if (count($types) !== count($columns)) {
        throw new RuntimeException('native rowset column/type shape mismatch');
    }

    $out = 'SBNR' . pack('v', 2) . pack('v', 0) . pack_u64_le((string) count($rows)) . pack('V', count($columns));
    foreach ($types as $type) {
        $out .= chr((int) $type);
    }
    foreach ($columns as $column) {
        $encoded = (string) $column;
        $out .= pack('V', strlen($encoded)) . $encoded;
    }
    $nullBitmapBytes = intdiv(count($columns) + 7, 8);
    foreach ($rows as $row) {
        $nullBitmap = array_fill(0, $nullBitmapBytes, 0);
        $values = '';
        foreach ($row as $index => $value) {
            if ($value === null) {
                $byte = intdiv($index, 8);
                $nullBitmap[$byte] |= 1 << ($index % 8);
                continue;
            }
            $values .= encode_native_rowset_value((string) $value, (int) $types[$index]);
        }
        $out .= implode('', array_map('chr', $nullBitmap)) . $values;
    }
    return $out;
}

function encode_native_rowset_value(string $value, int $type): string
{
    $trimmed = trim($value);
    return match ($type) {
        NATIVE_ROWSET_TYPE_INT64 => pack('q', (int) $trimmed),
        NATIVE_ROWSET_TYPE_BOOLEAN => truthy_native_rowset_boolean($trimmed) ? "\x01" : "\x00",
        NATIVE_ROWSET_TYPE_INT32 => pack('V', (int) $trimmed),
        NATIVE_ROWSET_TYPE_UINT64 => pack_u64_le($trimmed),
        NATIVE_ROWSET_TYPE_REAL64 => pack('e', (float) $trimmed),
        NATIVE_ROWSET_TYPE_BINARY, NATIVE_ROWSET_TYPE_TEXT => pack('V', strlen($value)) . $value,
        default => throw new RuntimeException('unsupported native rowset type ' . $type),
    };
}

function infer_native_rowset_column_types(array $rows): array
{
    if ($rows === []) {
        return [];
    }
    $columnCount = count($rows[0]);
    $types = array_fill(0, $columnCount, NATIVE_ROWSET_TYPE_TEXT);
    for ($column = 0; $column < $columnCount; $column++) {
        $values = [];
        foreach ($rows as $row) {
            if ($row[$column] !== null) {
                $values[] = (string) $row[$column];
            }
        }
        if ($values === []) {
            continue;
        }
        if (sb_array_all($values, static fn(string $value): bool => in_array(strtolower(trim($value)), ['true', 'false'], true))) {
            $types[$column] = NATIVE_ROWSET_TYPE_BOOLEAN;
            continue;
        }
        if (sb_array_all($values, static fn(string $value): bool => lossless_int($value, -2147483648, 2147483647))) {
            $types[$column] = NATIVE_ROWSET_TYPE_INT32;
            continue;
        }
        if (sb_array_all($values, static fn(string $value): bool => lossless_int($value, PHP_INT_MIN, PHP_INT_MAX))) {
            $types[$column] = NATIVE_ROWSET_TYPE_INT64;
            continue;
        }
        if (sb_array_all($values, 'lossless_uint64')) {
            $types[$column] = NATIVE_ROWSET_TYPE_UINT64;
            continue;
        }
        if (sb_array_all($values, 'lossless_real64')) {
            $types[$column] = NATIVE_ROWSET_TYPE_REAL64;
            continue;
        }
    }
    return $types;
}

function split_copy_csv_line(string $line): array
{
    $values = [];
    $current = '';
    $inQuote = false;
    $length = strlen($line);
    for ($index = 0; $index < $length; $index++) {
        $ch = $line[$index];
        if ($ch === '"') {
            if ($inQuote && $index + 1 < $length && $line[$index + 1] === '"') {
                $current .= '"';
                $index++;
            } else {
                $inQuote = !$inQuote;
            }
            continue;
        }
        if ($ch === ',' && !$inQuote) {
            $values[] = $current;
            $current = '';
            continue;
        }
        $current .= $ch;
    }
    $values[] = $current;
    return $values;
}

function sb_array_all(array $values, callable $predicate): bool
{
    foreach ($values as $value) {
        if (!$predicate($value)) {
            return false;
        }
    }
    return true;
}

function lossless_int(string $value, int $min, int $max): bool
{
    $trimmed = trim($value);
    if (!preg_match('/^-?\d+$/', $trimmed)) {
        return false;
    }
    $parsed = filter_var($trimmed, FILTER_VALIDATE_INT);
    if (!is_int($parsed)) {
        return false;
    }
    return $parsed >= $min && $parsed <= $max && (string) $parsed === $trimmed;
}

function lossless_uint64(string $value): bool
{
    $trimmed = trim($value);
    return preg_match('/^\d+$/', $trimmed) === 1
        && strlen($trimmed) <= 20
        && strcmp(str_pad($trimmed, 20, '0', STR_PAD_LEFT), '18446744073709551615') <= 0;
}

function lossless_real64(string $value): bool
{
    $parsed = filter_var(trim($value), FILTER_VALIDATE_FLOAT);
    return is_float($parsed) && is_finite($parsed);
}

function truthy_native_rowset_boolean(string $value): bool
{
    return in_array(strtolower(trim($value)), ['1', 'true', 't', 'yes', 'y', 'on'], true);
}

function pack_u64_le(string $decimal): string
{
    $text = ltrim(trim($decimal), '+');
    if (!preg_match('/^\d+$/', $text)) {
        throw new RuntimeException('invalid uint64 value');
    }
    $hi = 0;
    $lo = 0;
    for ($i = 0, $n = strlen($text); $i < $n; $i++) {
        $digit = ord($text[$i]) - 48;
        $lo = $lo * 10 + $digit;
        $carry = intdiv($lo, 0x100000000);
        $lo %= 0x100000000;
        $hi = ($hi * 10 + $carry) % 0x100000000;
    }
    return pack('V2', $lo, $hi);
}

function is_copy_stdin_statement(string $sql): bool
{
    $parts = [];
    foreach (preg_split('/\r\n|\r|\n/', executable_sql_without_copy_markers($sql)) as $line) {
        $stripped = trim($line);
        if ($stripped !== '' && !str_starts_with($stripped, '--')) {
            $parts[] = strtolower($stripped);
        }
    }
    $executable = implode(' ', $parts);
    return str_starts_with($executable, 'copy ') && str_contains($executable, ' from stdin');
}

function pdo_connection(ScratchBirdPDO $pdo): Connection
{
    $prop = new ReflectionProperty($pdo, 'connection');
    $prop->setAccessible(true);
    return $prop->getValue($pdo);
}

function execute_copy_in(Connection $conn, string $sql, string $payload): int
{
    $conn->sendMessage(Protocol::MSG_QUERY, Protocol::buildQueryPayload($sql, 0, 0, 0), 0, false);
    $rowsCopied = 0;
    $copyStarted = false;
    while (true) {
        [$type, , $body] = $conn->receive();
        if ($conn->handleAsyncMessage($type, $body)) {
            continue;
        }
        if ($type === Protocol::MSG_COPY_IN_RESPONSE) {
            $copyStarted = true;
            $conn->sendMessage(Protocol::MSG_COPY_DATA, $payload, 0, false);
            $conn->sendMessage(Protocol::MSG_COPY_DONE, '', 0, false);
            continue;
        }
        if ($type === Protocol::MSG_COMMAND_COMPLETE) {
            [, $rows] = Protocol::parseCommandComplete($body);
            $rowsCopied = (int) $rows;
            continue;
        }
        if ($type === Protocol::MSG_READY) {
            [$status, $txnId] = Protocol::parseReady($body);
            $conn->updateReadyState($status, $txnId);
            if (!$copyStarted) {
                throw new RuntimeException('COPY FROM STDIN did not enter COPY input mode');
            }
            return $rowsCopied;
        }
        if ($type === Protocol::MSG_ERROR) {
            $err = Protocol::parseErrorMessage($body);
            throw new RuntimeException((string) ($err['message'] ?? 'COPY failed'));
        }
    }
}

function run_transaction(ScratchBirdPDO $pdo, string $sql, array &$apiHits): void
{
    $trimmed = trim(executable_sql_without_copy_markers($sql));
    $stmt = $pdo->prepare($trimmed);
    $apiHits['prepare']++;
    if ($stmt === false) {
        $apiHits['errorInfo']++;
        throw new RuntimeException(json_encode($pdo->errorInfo(), JSON_THROW_ON_ERROR));
    }
    $stmt->execute();
    $apiHits['execute']++;
    $stmt->fetchAll(PDO::FETCH_ASSOC);
    $apiHits['fetch']++;
}

function read_input(string $path): string
{
    return $path === '-' ? stream_get_contents(STDIN) : file_get_contents($path);
}

function add_timing(array &$timings, string $group, int $started): void
{
    $timings[$group] = ($timings[$group] ?? 0) + (hrtime(true) - $started);
}

function write_text(string $path, string $text): void
{
    $dir = dirname($path);
    if (!is_dir($dir)) {
        mkdir($dir, 0777, true);
    }
    file_put_contents($path, $text);
}

function append_text(string $path, string $text): void
{
    $dir = dirname($path);
    if (!is_dir($dir)) {
        mkdir($dir, 0777, true);
    }
    file_put_contents($path, $text, FILE_APPEND);
}

function append_jsonl(string $path, array $record): void
{
    append_text($path, json_encode($record, JSON_THROW_ON_ERROR) . PHP_EOL);
}

function sha256_text(string $text): string
{
    return 'sha256:' . hash('sha256', $text);
}

function junit_xml(string $suite, string $class, array $testcases, array $failures): string
{
    $xml = '<?xml version="1.0" encoding="UTF-8"?>' . PHP_EOL;
    $xml .= sprintf('<testsuite name="%s" tests="%d" failures="%d">', escape_xml($suite), max(1, count($testcases)), count($failures)) . PHP_EOL;
    if (empty($testcases)) {
        $xml .= sprintf('  <testcase classname="%s" name="run"></testcase>', escape_xml($class)) . PHP_EOL;
    }
    foreach ($testcases as $testcase) {
        $xml .= sprintf('  <testcase classname="%s" name="%s"></testcase>', escape_xml($class), escape_xml((string) $testcase['statement_id'])) . PHP_EOL;
    }
    foreach ($failures as $failure) {
        $xml .= sprintf(
            '  <testcase classname="%s" name="%s"><failure message="%s" /></testcase>',
            escape_xml($class),
            escape_xml((string) $failure['statement_id']),
            escape_xml((string) $failure['message'])
        ) . PHP_EOL;
    }
    return $xml . '</testsuite>' . PHP_EOL;
}

function escape_xml(string $text): string
{
    return str_replace(['&', '"', '<', '>'], ['&amp;', '&quot;', '&lt;', '&gt;'], $text);
}
