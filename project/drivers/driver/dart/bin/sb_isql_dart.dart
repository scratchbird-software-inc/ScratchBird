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

Future<void> main(List<String> raw) async {
  try {
    final args = parseArgs(raw);
    final code = await runTool(args);
    exitCode = code;
  } catch (error) {
    stderr.writeln(error);
    exitCode = 1;
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
    'begin': 0,
    'commit': 0,
    'rollback': 0,
    'close': 0,
  };
  final testcases = <Map<String, Object?>>[];
  final failures = <Map<String, Object?>>[];
  final digests = <Map<String, Object?>>[];
  final securityRefusals = <Map<String, Object?>>[];
  final started = monotonicNs();
  ScratchBirdClient? client;

  try {
    final config = ScratchBirdConfig(
      host: required(args, '--host'),
      port: int.parse(required(args, '--port')),
      database: required(args, '--database'),
      user: required(args, '--user'),
      password: required(args, '--password'),
      role: args['--role'],
      sslmode: args['--sslmode'] ?? 'require',
      sslrootcert: args['--sslrootcert'],
      sslcert: args['--sslcert'],
      sslkey: args['--sslkey'],
      frontDoorMode: required(args, '--route') == 'manager-listener-parser'
          ? 'manager_proxy'
          : 'direct',
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
      'route': required(args, '--route'),
      'parser_mode': required(args, '--parser-mode'),
      'page_size': required(args, '--page-size'),
    });
    await appendJsonl(paths['wire']!, {
      'event': 'server_admission_required',
      'driver_or_parser_finality': 'forbidden',
    });

    if (args.containsKey('--create-database')) {
      throw StateError(
        '--create-database is not implemented in the Dart native tool yet',
      );
    }
    if (required(args, '--parser-mode') != 'server-parser') {
      throw StateError(
        '${required(args, '--parser-mode')} is not yet implemented by the Dart native tool; it fails closed',
      );
    }

    final statements = splitStatements(
      await readInput(required(args, '--input')),
    );
    for (var index = 0; index < statements.length; index++) {
      final sql = statements[index];
      final statementId =
          '${File(required(args, '--input')).uri.pathSegments.last}:${index + 1}';
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
        } else {
          final result = await client.query(sql);
          apiHits['query'] = apiHits['query']! + 1;
          rowCount = result.rows.length;
          resultDigest = sha256Text(jsonEncode(result.rows));
          await appendText(
            required(args, '--output'),
            '${jsonEncode({'statement_id': statementId, 'rows': result.rows})}\n',
          );
        }
        digests.add({
          'statement_id': statementId,
          'row_count': rowCount,
          'result_digest': resultDigest,
        });
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
        failures.add({'statement_id': statementId, 'message': diagnostic});
        if (args.containsKey('--stop-on-error')) {
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
        'statement_index': index + 1,
        'statement_id': statementId,
        'command_group': group,
        'sql_hash': sha256Text(sql),
        'expected_outcome': 'success',
        'actual_outcome': outcome,
        'sqlstate': sqlstate,
        'diagnostic_code': diagnostic,
        'canonical_message_vector': <Object?>[],
        'row_count': rowCount,
        'result_digest': resultDigest,
        'elapsed_ns': elapsed,
        'server_revalidation_state': 'required',
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
      '${jsonEncode({'tables_digest': sha256Text(jsonEncode(metadata.rows)), 'row_count': metadata.rows.length})}\n',
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
  final sslmode = args['--sslmode'] ?? 'require';
  final summary = {
    'run_id': args['--run-id'] ?? 'manual',
    'driver_name': 'dart',
    'route': required(args, '--route'),
    'parser_mode': required(args, '--parser-mode'),
    'page_size': required(args, '--page-size'),
    'namespace': required(args, '--namespace'),
    'sslmode': sslmode,
    'transport_mode': sslmode == 'disable' ? 'tls_disabled' : 'tls_required',
    'status': failures.isEmpty ? 'pass' : 'fail',
    'failure_count': failures.length,
    'elapsed_ns': elapsed,
    'server_revalidation_required': true,
    'driver_or_parser_finality': 'forbidden',
    'mga_authority': 'engine',
  };
  await writeText(required(args, '--summary'), '${jsonEncode(summary)}\n');
  await writeText(required(args, '--metrics'), '${jsonEncode(timings)}\n');
  await writeText(paths['timing']!, '${jsonEncode(timings)}\n');
  await writeText(paths['digests']!, '${jsonEncode(digests)}\n');
  await writeText(paths['refusals']!, '${jsonEncode(securityRefusals)}\n');
  await writeText(paths['api']!, '${jsonEncode(apiHits)}\n');
  await writeText(
    paths['review']!,
    '${jsonEncode({
      'driver': 'dart',
      'public_api_only': true,
      'shells_out_to_other_driver': false,
      'source_is_canonical_example': true,
      'sections': ['connection', 'query', 'fetch', 'metadata', 'diagnostics', 'transaction'],
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
  final first = sql.trim().split(RegExp(r'\s+')).first.toLowerCase();
  if (first == 'commit') {
    await client.commit();
    apiHits['commit'] = apiHits['commit']! + 1;
  } else if (first == 'rollback') {
    await client.rollback();
    apiHits['rollback'] = apiHits['rollback']! + 1;
  } else {
    await client.begin();
    apiHits['begin'] = apiHits['begin']! + 1;
  }
}

Map<String, String> parseArgs(List<String> raw) {
  final args = <String, String>{};
  for (var i = 0; i < raw.length; i++) {
    final key = raw[i];
    if (!key.startsWith('--')) {
      throw ArgumentError('unexpected positional argument: $key');
    }
    if (key == '--stop-on-error' || key == '--create-database') {
      args[key] = 'true';
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
  if (!parserModes.contains(required(args, '--parser-mode')))
    throw ArgumentError(
      'unsupported parser mode: ${required(args, '--parser-mode')}',
    );
}

List<String> splitStatements(String script) => script
    .split(';')
    .map((part) => part.trim())
    .where((part) => part.isNotEmpty)
    .toList();

String classifyStatement(String sql) {
  final trimmed = sql.trim().toLowerCase();
  final first = trimmed.split(RegExp(r'\s+')).first;
  if (const {'create', 'alter', 'drop'}.contains(first)) return 'ddl';
  if (const {'insert', 'update', 'delete', 'merge', 'upsert'}.contains(first))
    return 'dml';
  if (const {
    'commit',
    'rollback',
    'savepoint',
    'begin',
    'start',
  }.contains(first))
    return 'transaction';
  if (const {'grant', 'revoke'}.contains(first)) return 'security_refusal';
  if (trimmed.contains('sys.')) return 'metadata';
  return 'query';
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
