// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:scratchbird/scratchbird.dart';

const pageSizes = {'4k', '8k', '16k', '32k', '64k', '128k'};
const routes = {
  'embedded',
  'ipc_local',
  'listener-parser',
  'manager-listener-parser',
};
const parserModes = {'server-parser', 'standalone-parser', 'driver-sblr-uuid'};
const sslModes = {
  'allow',
  'disable',
  'prefer',
  'require',
  'verify-ca',
  'verify-full',
};
const pageSizeBytes = {
  '4k': 4096,
  '8k': 8192,
  '16k': 16384,
  '32k': 32768,
  '64k': 65536,
  '128k': 131072,
};
const supportedArgs = {
  '--database',
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
};

Future<void> main(List<String> raw) async {
  try {
    final args = parseArgs(raw);
    final code = await runTool(args);
    exit(code);
  } catch (error) {
    stderr.writeln(error);
    exit(1);
  }
}

Future<int> runTool(Map<String, String> args) async {
  validate(args);
  final runRoot = File(required(args, '--summary')).parent.path;
  await Directory(runRoot).create(recursive: true);
  final paths = <String, String>{
    'events': '$runRoot/command-events.jsonl',
    'wire': '$runRoot/wire-transcript.jsonl',
    'timing': '$runRoot/timing-groups.json',
    'digests': '$runRoot/result-digests.json',
    'metadata': '$runRoot/metadata-snapshots.json',
    'route_environment': '$runRoot/route-environment.json',
    'process': '$runRoot/process-metrics.jsonl',
    'refusals': '$runRoot/security-refusals.json',
    'api': '$runRoot/native-api-coverage.json',
    'review': '$runRoot/code-example-review.json',
    'junit': '$runRoot/junit.xml',
    'stdout': '$runRoot/stdout.log',
    'stderr': '$runRoot/stderr.log',
  };
  for (final path in [
    required(args, '--output'),
    required(args, '--error'),
    required(args, '--diagnostics'),
    required(args, '--metrics'),
    required(args, '--transcript'),
    required(args, '--summary'),
    ...paths.values,
  ]) {
    await writeText(path, '');
  }

  final timings = <String, int>{};
  final apiHits = <String, int>{
    'ScratchBirdConnection': 0,
    'ScratchBirdClient': 0,
    'connect': 0,
    'query': 0,
    'queryMetadata': 0,
    'attachCreate': 0,
    'begin': 0,
    'commit': 0,
    'rollback': 0,
    'close': 0,
    'copy_in': 0,
  };
  final testcases = <Map<String, Object?>>[];
  final failures = <Map<String, Object?>>[];
  final digests = <Map<String, Object?>>[];
  final securityRefusals = <Map<String, Object?>>[];
  final started = monotonicNs();
  final expectedRefusals = await loadExpectedRefusals(
    args['--expected-refusals'],
  );
  final route = required(args, '--route');
  final sslmode = effectiveSslMode(route, args['--sslmode'] ?? 'require');
  Map<String, Object?> routeEnvironment = buildRouteEnvironment(
    args,
    route: route,
    sslmode: sslmode,
    actualPageSize: null,
    status: 'fail',
    reason: 'not_probed',
  );
  await writeText(
    paths['route_environment']!,
    '${jsonEncode(routeEnvironment)}\n',
  );
  ScratchBirdClient? client;

  try {
    final config = ScratchBirdConfig(
      host: required(args, '--host'),
      port: int.parse(required(args, '--port')),
      ipcPath: route == 'ipc_local' ? required(args, '--ipc-path') : null,
      database: required(args, '--database'),
      user: required(args, '--user'),
      password: required(args, '--password'),
      role: args['--role'],
      sslmode: sslmode,
      sslrootcert: args['--sslrootcert'],
      sslcert: args['--sslcert'],
      sslkey: args['--sslkey'],
      frontDoorMode:
          route == 'manager-listener-parser' ? 'manager_proxy' : 'direct',
      applicationName: 'SBIsqlDart',
      metadataExpandSchemaParents: true,
      fetchSize: int.parse(args['--fetch-size'] ?? '1000'),
    );
    final connectStarted = monotonicNs();
    client = await ScratchBirdClient.connect(config);
    apiHits['ScratchBirdConnection'] = apiHits['ScratchBirdConnection']! + 1;
    apiHits['ScratchBirdClient'] = apiHits['ScratchBirdClient']! + 1;
    apiHits['connect'] = apiHits['connect']! + 1;
    addTiming(timings, 'connection', connectStarted);
    await appendJsonl(required(args, '--transcript'), {
      'event': 'connect',
      'driver': 'dart',
      'route': route,
      'parser_mode': required(args, '--parser-mode'),
      'page_size': required(args, '--page-size'),
    });
    await appendJsonl(paths['wire']!, {
      'event': 'server_admission_required',
      'driver_or_parser_finality': 'forbidden',
    });

    routeEnvironment =
        await probeRouteEnvironment(client, args, route, sslmode);
    await writeText(
      paths['route_environment']!,
      '${jsonEncode(routeEnvironment)}\n',
    );
    if (route != 'embedded' &&
        routeEnvironment['page_size_verification_status'] != 'pass') {
      throw StateError(
        'route page-size verification failed: expected '
        '${routeEnvironment['expected_page_size_bytes']} actual '
        '${routeEnvironment['actual_page_size_bytes']}',
      );
    }

    if (flagEnabled(args, '--create-database')) {
      final createStarted = monotonicNs();
      await client.attachCreate(
        args['--create-emulation-mode'] ?? 'sbsql',
        required(args, '--database'),
      );
      apiHits['attachCreate'] = apiHits['attachCreate']! + 1;
      addTiming(timings, 'database_create', createStarted);
    }
    if (required(args, '--parser-mode') != 'server-parser') {
      throw StateError(
        '${required(args, '--parser-mode')} is not accepted by the Dart native tool lane; it fails closed',
      );
    }

    final inputPath = required(args, '--input');
    final inputText = await readInput(inputPath);
    final statements = splitInputStatements(inputPath, inputText);
    for (final statement in statements) {
      final sql = statement.sql;
      final statementId = '${statement.scriptName}:${statement.statementIndex}';
      final expectedOutcome =
          expectedRefusals.contains(statementId) ? 'refusal' : 'success';
      final group = classifyStatement(sql);
      final statementStarted = monotonicNs();
      var outcome = 'success';
      var rowCount = -1;
      String? resultDigest;
      String? sqlstate;
      String? diagnostic;
      try {
        if (group == 'transaction') {
          await runTransaction(client, sql, apiHits);
          rowCount = 0;
          resultDigest = sha256Text('transaction');
        } else if (group == 'copy' && isCopyStdinStatement(sql)) {
          final payload = copyPayloadForStatement(sql);
          if (payload.isEmpty) {
            throw StateError(
              'COPY FROM STDIN requires SB_COPY_INPUT rows in the script',
            );
          }
          final rowsCopied = await client.copyIn(
            executableSqlWithoutCopyMarkers(sql),
            utf8.encode(payload),
          );
          apiHits['copy_in'] = apiHits['copy_in']! + 1;
          rowCount = rowsCopied;
          final rows = [
            ['copy_in', rowsCopied]
          ];
          resultDigest = sha256Text(jsonEncode(rows));
          await appendText(
            required(args, '--output'),
            '${jsonEncode({'statement_id': statementId, 'rows': rows})}\n',
          );
        } else {
          final result = await client.query(sql);
          apiHits['query'] = apiHits['query']! + 1;
          rowCount = result.rows.length;
          final rows = jsonSafeRows(result.rows);
          resultDigest = sha256Text(jsonEncode(rows));
          await appendText(
            required(args, '--output'),
            '${jsonEncode({'statement_id': statementId, 'rows': rows})}\n',
          );
        }
        digests.add({
          'statement_id': statementId,
          'row_count': rowCount,
          'result_digest': resultDigest,
        });
        if (expectedOutcome == 'refusal') {
          outcome = 'unexpected_success';
          failures.add({
            'statement_id': statementId,
            'message': 'statement succeeded but was expected to refuse',
          });
        }
      } catch (error) {
        outcome = 'refusal';
        sqlstate = 'HY000';
        diagnostic = error.toString();
        await appendJsonl(required(args, '--diagnostics'), {
          'statement_id': statementId,
          'sqlstate': sqlstate,
          'message': diagnostic,
        });
        await appendText(
          required(args, '--error'),
          '$statementId: $diagnostic\n',
        );
        if (expectedOutcome == 'success') {
          failures.add({'statement_id': statementId, 'message': diagnostic});
        } else {
          securityRefusals.add({
            'statement_id': statementId,
            'sqlstate': sqlstate,
            'diagnostic_code': diagnostic,
          });
        }
        if (expectedOutcome == 'success' &&
            flagEnabled(args, '--stop-on-error')) {
          addTiming(timings, group, statementStarted);
          break;
        }
      }
      final elapsed = monotonicNs() - statementStarted;
      addTiming(timings, group, statementStarted);
      final event = <String, Object?>{
        'run_id': args['--run-id'] ?? 'manual',
        'driver_name': 'dart',
        'driver_version': 'unknown',
        'route': required(args, '--route'),
        'parser_mode': required(args, '--parser-mode'),
        'page_size': required(args, '--page-size'),
        'namespace': required(args, '--namespace'),
        'script': required(args, '--input'),
        'statement_index': statement.statementIndex,
        'statement_id': statementId,
        'command_group': group,
        'sql_hash': sha256Text(sql),
        'expected_outcome': expectedOutcome,
        'actual_outcome': outcome,
        'sqlstate': sqlstate,
        'diagnostic_code': diagnostic,
        'canonical_message_vector': <Object?>[],
        'row_count': rowCount,
        'result_digest': resultDigest,
        'elapsed_ns': elapsed,
        'server_revalidation_state': 'required',
        'language_profile': args['--language-profile'] ?? 'en-US',
        'language_resource_pack': args['--language-resource-pack'] ??
            'project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack',
        'language_resource_identity': args['--language-resource-identity'] ??
            'sbsql.common_resource_pack.v1',
        'language_resource_hash': args['--language-resource-hash'] ??
            'sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc',
        'syntax_profile': args['--syntax-profile'] ?? 'sbsql.v3',
        'topology_profile':
            args['--topology-profile'] ?? 'topology.sbsql.canonical.v1',
        'standard_english_fallback': flagEnabled(
          args,
          '--standard-english-fallback',
          true,
        ),
        'transaction_id_observed': null,
        'mga_authority': 'engine',
        'native_api_surface': 'dart',
        'code_example_section': 'query_fetch',
      };
      await appendJsonl(paths['events']!, event);
      testcases.add(event);
    }

    final metadataStarted = monotonicNs();
    final metadata = await client.queryMetadata('tables');
    apiHits['queryMetadata'] = apiHits['queryMetadata']! + 1;
    await writeText(
      paths['metadata']!,
      '${jsonEncode({
            'tables_digest': sha256Text(jsonEncode(metadata.rows)),
            'row_count': metadata.rows.length
          })}\n',
    );
    addTiming(timings, 'metadata', metadataStarted);
  } catch (error) {
    failures.add({'statement_id': 'run', 'message': error.toString()});
    await appendText(paths['stderr']!, '$error\n');
  } finally {
    if (client != null) {
      await client.close();
      apiHits['close'] = apiHits['close']! + 1;
    }
  }

  final elapsed = monotonicNs() - started;
  timings['overall'] = elapsed;
  final transportMode = resolveTransportMode(
    route,
    sslmode,
  );
  final processMetrics = currentProcessMetrics();
  final summary = {
    'run_id': args['--run-id'] ?? 'manual',
    'driver_name': 'dart',
    'route': route,
    'parser_mode': required(args, '--parser-mode'),
    'page_size': required(args, '--page-size'),
    'namespace': required(args, '--namespace'),
    'sslmode': sslmode,
    'transport_mode': transportMode,
    'transport_endpoint_kind': endpointKindForRoute(route),
    'driver_transport_implementation': transportImplementationForRoute(
      route,
    ),
    'cpp_library_boundary': 'none',
    'language_resource_pack': args['--language-resource-pack'] ??
        'project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack',
    'language_resource_identity':
        args['--language-resource-identity'] ?? 'sbsql.common_resource_pack.v1',
    'language_resource_hash': args['--language-resource-hash'] ??
        'sha256:752c7a9823bdad00b48ab318c8b2d5d6d53b2739ecfe43f565952fd510f4e3dc',
    'language_resource_authority': 'shared_server_parser_resource_pack',
    'language_profile': args['--language-profile'] ?? 'en-US',
    'syntax_profile': args['--syntax-profile'] ?? 'sbsql.v3',
    'topology_profile':
        args['--topology-profile'] ?? 'topology.sbsql.canonical.v1',
    'standard_english_fallback': flagEnabled(
      args,
      '--standard-english-fallback',
      true,
    ),
    'actual_page_size_bytes': routeEnvironment['actual_page_size_bytes'],
    'status': failures.isEmpty ? 'pass' : 'fail',
    'failure_count': failures.length,
    'elapsed_ns': elapsed,
    'process_metrics': processMetrics,
    'server_revalidation_required': true,
    'driver_or_parser_finality': 'forbidden',
    'mga_authority': 'engine',
  };
  await writeText(required(args, '--summary'), '${jsonEncode(summary)}\n');
  await writeText(required(args, '--metrics'), '${jsonEncode(timings)}\n');
  await writeText(paths['timing']!, '${jsonEncode(timings)}\n');
  await writeText(paths['digests']!, '${jsonEncode(digests)}\n');
  await appendJsonl(paths['process']!, {
    'role': 'client',
    'rss_kb': processMetrics['client']!['last_rss_kb'],
    'vsize_kb': processMetrics['client']!['last_vsize_kb'],
  });
  await writeText(paths['refusals']!, '${jsonEncode(securityRefusals)}\n');
  await writeText(paths['api']!, '${jsonEncode(apiHits)}\n');
  await writeText(
    paths['review']!,
    '${jsonEncode({
          'driver': 'dart',
          'public_api_only': true,
          'shells_out_to_other_driver': false,
          'source_is_canonical_example': true,
          'sections': [
            'connection',
            'query',
            'fetch',
            'metadata',
            'diagnostics',
            'transaction'
          ],
        })}\n',
  );
  await writeText(
    paths['junit']!,
    junitXml('SBIsqlDart', 'scratchbird.dart', testcases, failures),
  );
  await appendText(
    paths['stdout']!,
    'SBIsqlDart status=${summary['status']}\n',
  );
  return failures.isEmpty ? 0 : 1;
}

Future<void> runTransaction(
  ScratchBirdClient client,
  String sql,
  Map<String, int> apiHits,
) async {
  final executable = executableSqlWithoutCopyMarkers(sql)
      .split(RegExp(r'\r\n|\r|\n'))
      .map((line) => line.trim())
      .where((line) => line.isNotEmpty && !line.startsWith('--'))
      .join(' ');
  final tokens = executable.split(RegExp(r'\s+'));
  if (tokens.isEmpty || tokens.first.isEmpty) return;
  final first = tokens.first.toLowerCase();
  if (first == 'commit') {
    await client.commit();
    apiHits['commit'] = apiHits['commit']! + 1;
  } else if (first == 'rollback' &&
      tokens.length >= 4 &&
      tokens[1].toLowerCase() == 'to' &&
      tokens[2].toLowerCase() == 'savepoint') {
    await client.rollbackToSavepoint(normalizeControlName(tokens[3]));
  } else if (first == 'rollback') {
    await client.rollback();
    apiHits['rollback'] = apiHits['rollback']! + 1;
  } else if (first == 'savepoint' && tokens.length >= 2) {
    await client.savepoint(normalizeControlName(tokens[1]));
  } else if (first == 'release' &&
      tokens.length >= 3 &&
      tokens[1].toLowerCase() == 'savepoint') {
    await client.releaseSavepoint(normalizeControlName(tokens[2]));
  } else if (first == 'release' && tokens.length >= 2) {
    await client.releaseSavepoint(normalizeControlName(tokens[1]));
  } else {
    await client.begin();
    apiHits['begin'] = apiHits['begin']! + 1;
  }
}

Map<String, Object?> buildRouteEnvironment(
  Map<String, String> args, {
  required String route,
  required String sslmode,
  required int? actualPageSize,
  required String status,
  String? reason,
}) {
  final pageSize = required(args, '--page-size');
  final record = <String, Object?>{
    'run_id': args['--run-id'] ?? 'manual',
    'driver': 'dart',
    'route': route,
    'sslmode': sslmode,
    'parser_mode': required(args, '--parser-mode'),
    'concurrency_mode': 'single',
    'namespace': required(args, '--namespace'),
    'page_size': pageSize,
    'expected_page_size_bytes': pageSizeBytes[pageSize],
    'actual_page_size_bytes': actualPageSize,
    'page_size_verification_source': 'SHOW DATABASE',
    'page_size_verification_status': status,
    'transport_mode': resolveTransportMode(route, sslmode),
    'transport_endpoint_kind': endpointKindForRoute(route),
    'driver_transport_implementation': transportImplementationForRoute(route),
  };
  if (reason != null && reason.isNotEmpty) {
    record['failure_reason'] = reason;
  }
  return record;
}

Future<Map<String, Object?>> probeRouteEnvironment(
  ScratchBirdClient client,
  Map<String, String> args,
  String route,
  String sslmode,
) async {
  try {
    final result = await client.query('SHOW DATABASE');
    final actual = pageSizeFromShowDatabase(result);
    if (actual == null) {
      return buildRouteEnvironment(
        args,
        route: route,
        sslmode: sslmode,
        actualPageSize: null,
        status: 'fail',
        reason: 'show_database_missing_page_size_bytes',
      );
    }
    final expected = pageSizeBytes[required(args, '--page-size')];
    final status = actual == expected ? 'pass' : 'fail';
    return buildRouteEnvironment(
      args,
      route: route,
      sslmode: sslmode,
      actualPageSize: actual,
      status: status,
      reason: status == 'pass' ? null : 'actual_page_size_mismatch',
    );
  } catch (error) {
    return buildRouteEnvironment(
      args,
      route: route,
      sslmode: sslmode,
      actualPageSize: null,
      status: 'fail',
      reason: error.toString(),
    );
  }
}

int? pageSizeFromShowDatabase(ScratchBirdResult result) {
  var pageIndex = result.columns
      .indexWhere((column) => column.name.toLowerCase() == 'page_size_bytes');
  if (pageIndex < 0 && result.columns.length >= 3) {
    pageIndex = 2;
  }
  if (pageIndex < 0 &&
      result.rows.isNotEmpty &&
      result.rows.first.length >= 3) {
    pageIndex = 2;
  }
  if (pageIndex >= 0 && result.rows.isNotEmpty) {
    final row = result.rows.first;
    if (pageIndex < row.length) {
      final value = intValue(row[pageIndex]);
      if (value != null) return value;
    }
  }
  for (final row in result.rows) {
    for (var index = 0; index < row.length; index++) {
      final text = '${row[index]}'.trim();
      final lower = text.toLowerCase();
      if (lower == 'page_size_bytes' && index + 1 < row.length) {
        final value = intValue(row[index + 1]);
        if (value != null) return value;
      }
      final match =
          RegExp(r'page_size_bytes\s*[:=]\s*(\d+)', caseSensitive: false)
              .firstMatch(text);
      if (match != null) {
        return int.parse(match.group(1)!);
      }
    }
  }
  return null;
}

int? intValue(dynamic value) {
  if (value == null) return null;
  if (value is int) return value;
  if (value is double && value.isFinite) return value.toInt();
  return int.tryParse('$value'.trim());
}

String normalizeControlName(String token) =>
    token.replaceAll(RegExp(r'[;]$'), '').trim();

List<List<Object?>> jsonSafeRows(List<List<dynamic>> rows) => [
      for (final row in rows) [for (final value in row) jsonSafeValue(value)]
    ];

Object? jsonSafeValue(dynamic value) {
  if (value == null || value is bool || value is String || value is int) {
    return value;
  }
  if (value is double) {
    if (value.isNaN) return 'NaN';
    if (value == double.infinity) return 'Infinity';
    if (value == double.negativeInfinity) return '-Infinity';
    return value;
  }
  if (value is BigInt) return value.toString();
  if (value is DateTime) return value.toUtc().toIso8601String();
  if (value is Iterable) {
    return [for (final item in value) jsonSafeValue(item)];
  }
  if (value is Map) {
    return {
      for (final entry in value.entries)
        '${entry.key}': jsonSafeValue(entry.value)
    };
  }
  return value.toString();
}

Map<String, String> parseArgs(List<String> raw) {
  final args = <String, String>{};
  for (var i = 0; i < raw.length; i++) {
    final key = raw[i];
    if (!key.startsWith('--')) {
      throw ArgumentError('unexpected positional argument: $key');
    }
    if (!supportedArgs.contains(key)) {
      throw ArgumentError('unsupported argument: $key');
    }
    if (key == '--stop-on-error' ||
        key == '--create-database' ||
        key == '--standard-english-fallback') {
      if (i + 1 < raw.length && !raw[i + 1].startsWith('--')) {
        args[key] = parseBoolValue(key, raw[++i]).toString();
      } else {
        args[key] = 'true';
      }
      continue;
    }
    if (i + 1 >= raw.length || raw[i + 1].startsWith('--')) {
      throw ArgumentError('missing value for $key');
    }
    args[key] = raw[++i];
  }
  return args;
}

void validate(Map<String, String> args) {
  if (!pageSizes.contains(required(args, '--page-size')))
    throw ArgumentError(
      'unsupported page size: ${required(args, '--page-size')}',
    );
  if (!routes.contains(required(args, '--route')))
    throw ArgumentError('unsupported route: ${required(args, '--route')}');
  if (required(args, '--route') == 'ipc_local' &&
      (args['--ipc-path'] == null || args['--ipc-path']!.trim().isEmpty)) {
    throw ArgumentError('ipc_local route requires --ipc-path');
  }
  if (!parserModes.contains(required(args, '--parser-mode')))
    throw ArgumentError(
      'unsupported parser mode: ${required(args, '--parser-mode')}',
    );
  final sslmode = args['--sslmode'] ?? 'require';
  if (!sslModes.contains(sslmode)) {
    throw ArgumentError('unsupported sslmode: $sslmode');
  }
}

String resolveTransportMode(String route, String sslmode) {
  if (route == 'embedded') return 'embedded_no_network_transport';
  if (route == 'ipc_local') return 'local_ipc_no_tls';
  return sslmode == 'disable' ? 'tls_disabled' : 'tls_required';
}

String effectiveSslMode(String route, String sslmode) =>
    route == 'ipc_local' ? 'disable' : sslmode;

String endpointKindForRoute(String route) {
  if (route == 'embedded') return 'none';
  if (route == 'ipc_local') return 'unix_domain_socket';
  return 'tcp';
}

String transportImplementationForRoute(String route) {
  if (route == 'embedded') return 'unsupported_no_cpp_library_boundary';
  if (route == 'ipc_local') return 'native_dart_unix_socket';
  return 'native_dart_tcp';
}

String classifyStatement(String sql) {
  final trimmed = executableSqlWithoutCopyMarkers(sql)
      .split(RegExp(r'\r\n|\r|\n'))
      .map((line) => line.trim())
      .where((line) => line.isNotEmpty && !line.startsWith('--'))
      .join(' ')
      .toLowerCase();
  final first = trimmed.split(RegExp(r'\s+')).first;
  if (first == 'copy') return 'copy';
  if (const {'create', 'alter', 'drop'}.contains(first)) return 'ddl';
  if (const {'insert', 'update', 'delete', 'merge', 'upsert'}.contains(first))
    return 'dml';
  if (const {
    'commit',
    'rollback',
    'savepoint',
    'begin',
    'start',
    'release',
  }.contains(first)) return 'transaction';
  if (const {'grant', 'revoke'}.contains(first)) return 'security_refusal';
  if (trimmed.contains('sys.')) return 'metadata';
  return 'query';
}

class ToolStatement {
  final String scriptName;
  final int statementIndex;
  final String sql;

  const ToolStatement(this.scriptName, this.statementIndex, this.sql);
}

List<ToolStatement> splitInputStatements(String inputPath, String inputText) {
  final chain = splitChainStatements(inputText);
  if (chain.isNotEmpty) {
    return [
      for (final statement in chain)
        ToolStatement(
          statement.scriptName,
          statement.statementIndex,
          statement.sql,
        ),
    ];
  }
  final fileName = File(inputPath).uri.pathSegments.last;
  final split = splitStatements(inputText);
  return [
    for (var i = 0; i < split.length; i++)
      ToolStatement(fileName, i + 1, split[i]),
  ];
}

String executableSqlWithoutCopyMarkers(String sql) => sql
    .split(RegExp(r'\r\n|\r|\n'))
    .where((line) => !line.trimLeft().startsWith('-- SB_COPY_INPUT '))
    .join('\n')
    .trim();

String copyPayloadForStatement(String sql) {
  final rows = <String>[];
  for (final line in sql.split(RegExp(r'\r\n|\r|\n'))) {
    final stripped = line.trimLeft();
    if (stripped.startsWith('-- SB_COPY_INPUT ')) {
      rows.add(stripped.substring('-- SB_COPY_INPUT '.length));
    }
  }
  return rows.isEmpty ? '' : '${rows.join('\n')}\n';
}

bool isCopyStdinStatement(String sql) {
  final executable = executableSqlWithoutCopyMarkers(sql)
      .split(RegExp(r'\r\n|\r|\n'))
      .map((line) => line.trim().toLowerCase())
      .where((line) => line.isNotEmpty && !line.startsWith('--'))
      .join(' ');
  return executable.startsWith('copy ') && executable.contains(' from stdin');
}

String required(Map<String, String> args, String key) {
  final value = args[key];
  if (value == null || value.isEmpty)
    throw ArgumentError('missing required argument $key');
  return value;
}

Future<String> readInput(String path) async => path == '-'
    ? stdin.transform(utf8.decoder).join()
    : File(path).readAsString();

Future<Set<String>> loadExpectedRefusals(String? path) async {
  if (path == null || path.isEmpty) return <String>{};
  final file = File(path);
  if (!await file.exists()) {
    throw ArgumentError('expected refusal file not found: $path');
  }
  final decoded = jsonDecode(await file.readAsString());
  if (decoded is Map<String, dynamic>) {
    final ids = <String>{};
    final statementIds = decoded['statement_ids'];
    if (statementIds is List) ids.addAll(statementIds.map((value) => '$value'));
    final expected = decoded['expected_refusals'];
    if (expected is List) ids.addAll(expected.map((value) => '$value'));
    final diagnostics = decoded['expected_diagnostics'];
    if (diagnostics is Map)
      ids.addAll(diagnostics.keys.map((value) => '$value'));
    return ids;
  }
  if (decoded is List) return decoded.map((value) => '$value').toSet();
  throw ArgumentError('expected refusals must be a JSON object or array');
}

bool parseBoolValue(String key, String value) {
  final normalized = value.toLowerCase();
  if (normalized == 'true') return true;
  if (normalized == 'false') return false;
  throw ArgumentError('$key expects true or false, got: $value');
}

bool flagEnabled(
  Map<String, String> args,
  String key, [
  bool fallback = false,
]) =>
    (args[key] ?? fallback.toString()).toLowerCase() == 'true';

Map<String, Map<String, int>> currentProcessMetrics() {
  final rssKb = (ProcessInfo.currentRss / 1024).ceil();
  final value = rssKb < 1 ? 1 : rssKb;
  return {
    'client': {
      'last_rss_kb': value,
      'last_vsize_kb': value,
      'max_rss_kb': value,
      'max_vsize_kb': value,
    },
  };
}

int monotonicNs() => DateTime.now().microsecondsSinceEpoch * 1000;
void addTiming(Map<String, int> timings, String group, int started) =>
    timings[group] = (timings[group] ?? 0) + (monotonicNs() - started);
String sha256Text(String text) => 'sha256:${sha256.convert(utf8.encode(text))}';

Future<void> writeText(String path, String text) async {
  await File(path).parent.create(recursive: true);
  await File(path).writeAsString(text);
}

Future<void> appendText(String path, String text) async {
  await File(path).parent.create(recursive: true);
  await File(path).writeAsString(text, mode: FileMode.append);
}

Future<void> appendJsonl(String path, Map<String, Object?> record) =>
    appendText(path, '${jsonEncode(record)}\n');

String junitXml(
  String suite,
  String klass,
  List<Map<String, Object?>> testcases,
  List<Map<String, Object?>> failures,
) {
  final out = StringBuffer()
    ..writeln('<?xml version="1.0" encoding="UTF-8"?>')
    ..writeln(
      '<testsuite name="${escapeXml(suite)}" tests="${testcases.isEmpty ? 1 : testcases.length}" failures="${failures.length}">',
    );
  if (testcases.isEmpty)
    out.writeln(
      '  <testcase classname="${escapeXml(klass)}" name="run"></testcase>',
    );
  for (final testcase in testcases) {
    out.writeln(
      '  <testcase classname="${escapeXml(klass)}" name="${escapeXml('${testcase['statement_id']}')}"></testcase>',
    );
  }
  for (final failure in failures) {
    out.writeln(
      '  <testcase classname="${escapeXml(klass)}" name="${escapeXml('${failure['statement_id']}')}"><failure message="${escapeXml('${failure['message']}')}" /></testcase>',
    );
  }
  out.writeln('</testsuite>');
  return out.toString();
}

String escapeXml(String text) => text
    .replaceAll('&', '&amp;')
    .replaceAll('"', '&quot;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;');
