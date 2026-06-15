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

use ScratchBird\PDO\ScratchBirdPDO;

require_driver_sources(dirname(__DIR__));

main($argv);

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
        'commit' => 0,
        'rollback' => 0,
    ];
    $testcases = [];
    $failures = [];
    $digests = [];
    $securityRefusals = [];
    $started = hrtime(true);
    $pdo = null;

    try {
        $dsn = sprintf(
            'scratchbird:host=%s;port=%s;database=%s;sslmode=%s;front_door_mode=%s;metadata_expand_schema_parents=true',
            required($args, '--host'),
            required($args, '--port'),
            required($args, '--database'),
            value_or_default($args, '--sslmode', 'require'),
            required($args, '--route') === 'manager-listener-parser' ? 'manager_proxy' : 'direct'
        );
        $connectStarted = hrtime(true);
        $pdo = new ScratchBirdPDO($dsn, required($args, '--user'), required($args, '--password'), [
            'role' => value_or_default($args, '--role', ''),
        ]);
        $apiHits['PDO']++;
        add_timing($timings, 'connection', $connectStarted);
        append_jsonl(required($args, '--transcript'), [
            'event' => 'connect',
            'driver' => 'php',
            'route' => required($args, '--route'),
            'parser_mode' => required($args, '--parser-mode'),
            'page_size' => required($args, '--page-size'),
        ]);
        append_jsonl($paths['wire'], [
            'event' => 'server_admission_required',
            'driver_or_parser_finality' => 'forbidden',
        ]);

        if (flag_enabled($args, '--create-database')) {
            throw new RuntimeException('--create-database is not implemented in the PHP native tool yet');
        }
        if (required($args, '--parser-mode') !== 'server-parser') {
            throw new RuntimeException(required($args, '--parser-mode') . ' is not yet implemented by the PHP native tool; it fails closed');
        }

        $statements = split_statements(read_input(required($args, '--input')));
        foreach ($statements as $index => $sql) {
            $statementId = basename(required($args, '--input')) . ':' . ($index + 1);
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
                $failures[] = ['statement_id' => $statementId, 'message' => $diagnostic];
                if (flag_enabled($args, '--stop-on-error')) {
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
                'expected_outcome' => 'success',
                'actual_outcome' => $outcome,
                'sqlstate' => $sqlstate,
                'diagnostic_code' => $diagnostic,
                'canonical_message_vector' => [],
                'row_count' => $rowCount,
                'result_digest' => $resultDigest,
                'elapsed_ns' => $elapsed,
                'server_revalidation_state' => 'required',
                'transaction_id_observed' => null,
                'mga_authority' => 'engine',
                'native_api_surface' => 'php_pdo_style',
                'code_example_section' => 'prepare_execute_fetch',
            ];
            append_jsonl($paths['events'], $event);
            $testcases[] = $event;
        }

        $metadataStarted = hrtime(true);
        $metadataStatement = $pdo->queryMetadata('tables');
        $apiHits['queryMetadata']++;
        $metadataRows = $metadataStatement->fetchAll(PDO::FETCH_ASSOC);
        $apiHits['fetch']++;
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
    $summary = [
        'run_id' => value_or_default($args, '--run-id', 'manual'),
        'driver_name' => 'php',
        'route' => required($args, '--route'),
        'parser_mode' => required($args, '--parser-mode'),
        'page_size' => required($args, '--page-size'),
        'namespace' => required($args, '--namespace'),
        'status' => empty($failures) ? 'pass' : 'fail',
        'failure_count' => count($failures),
        'elapsed_ns' => $elapsed,
        'server_revalidation_required' => true,
        'driver_or_parser_finality' => 'forbidden',
        'mga_authority' => 'engine',
    ];
    write_text(required($args, '--summary'), json_encode($summary, JSON_THROW_ON_ERROR) . PHP_EOL);
    write_text(required($args, '--metrics'), json_encode($timings, JSON_THROW_ON_ERROR) . PHP_EOL);
    write_text($paths['timing'], json_encode($timings, JSON_THROW_ON_ERROR) . PHP_EOL);
    write_text($paths['digests'], json_encode($digests, JSON_THROW_ON_ERROR) . PHP_EOL);
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
        if ($key === '--stop-on-error' || $key === '--create-database') {
            $args[$key] = true;
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

function flag_enabled(array $args, string $key): bool
{
    return array_key_exists($key, $args) && $args[$key] === true;
}

function split_statements(string $script): array
{
    $statements = [];
    $current = '';
    $single = false;
    $double = false;
    foreach (str_split($script) as $ch) {
        if ($ch === "'" && !$double) {
            $single = !$single;
        } elseif ($ch === '"' && !$single) {
            $double = !$double;
        }
        if ($ch === ';' && !$single && !$double) {
            $trimmed = trim($current);
            if ($trimmed !== '') {
                $statements[] = $trimmed;
            }
            $current = '';
            continue;
        }
        $current .= $ch;
    }
    $trimmed = trim($current);
    if ($trimmed !== '') {
        $statements[] = $trimmed;
    }
    return $statements;
}

function classify_statement(string $sql): string
{
    $trimmed = strtolower(trim($sql));
    $first = strtok($trimmed, " \t\r\n") ?: '';
    if (in_array($first, ['create', 'alter', 'drop'], true)) {
        return 'ddl';
    }
    if (in_array($first, ['insert', 'update', 'delete', 'merge', 'upsert'], true)) {
        return 'dml';
    }
    if (in_array($first, ['commit', 'rollback', 'savepoint', 'begin', 'start'], true)) {
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

function run_transaction(ScratchBirdPDO $pdo, string $sql, array &$apiHits): void
{
    $first = strtok(strtolower(trim($sql)), " \t\r\n") ?: '';
    if ($first === 'commit') {
        $pdo->commit();
        $apiHits['commit']++;
    } elseif ($first === 'rollback') {
        $pdo->rollBack();
        $apiHits['rollback']++;
    } else {
        $pdo->beginTransaction();
    }
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
