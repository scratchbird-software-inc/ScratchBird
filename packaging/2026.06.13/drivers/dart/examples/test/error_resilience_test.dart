// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:async';
import 'dart:convert';
import 'dart:mirrors';
import 'dart:typed_data';

import 'package:scratchbird/scratchbird.dart';
import 'package:scratchbird/src/protocol.dart';
import 'package:test/test.dart';

MessageHeader _header({
  int type = MessageType.query,
  int flags = 0,
  int length = 0,
  int sequence = 1,
  int txnId = 0,
}) {
  return MessageHeader(
    type: type,
    flags: flags,
    length: length,
    sequence: sequence,
    attachmentId: Uint8List(16),
    txnId: txnId,
  );
}

Uint8List _errorPayload(Map<String, String> fields) {
  final out = BytesBuilder();
  for (final entry in fields.entries) {
    out.add([entry.key.codeUnitAt(0)]);
    out.add(utf8.encode(entry.value));
    out.add([0]);
  }
  out.add([0]);
  return out.toBytes();
}

void main() {
  group('err framing', () {
    test('decodeHeader rejects invalid header length', () {
      expect(
        () => decodeHeader(Uint8List(8)),
        throwsA(isA<ScratchBirdProtocolException>()),
      );
    });

    test('decodeHeader rejects invalid protocol magic', () {
      final bytes =
          encodeMessage(_header(), Uint8List(0)).sublist(0, headerSize);
      final data = ByteData.sublistView(bytes);
      data.setUint32(0, 0x00000000, Endian.little);
      expect(
        () => decodeHeader(bytes),
        throwsA(isA<ScratchBirdProtocolException>()),
      );
    });

    test('decodeHeader rejects unsupported protocol version', () {
      final bytes =
          encodeMessage(_header(), Uint8List(0)).sublist(0, headerSize);
      final data = ByteData.sublistView(bytes);
      data.setUint8(4, 99);
      data.setUint8(5, 99);
      expect(
        () => decodeHeader(bytes),
        throwsA(isA<ScratchBirdProtocolException>()),
      );
    });

    test('decodeHeader rejects payloads above max message size', () {
      final bytes =
          encodeMessage(_header(), Uint8List(0)).sublist(0, headerSize);
      final data = ByteData.sublistView(bytes);
      data.setUint32(8, maxMessageSize + 1, Endian.little);
      expect(
        () => decodeHeader(bytes),
        throwsA(isA<ScratchBirdProtocolException>()),
      );
    });
  });

  group('err payload', () {
    test('parseErrorMessage extracts severity/sqlstate/message/detail/hint',
        () {
      final payload = _errorPayload({
        'S': 'ERROR',
        'C': '23505',
        'M': 'duplicate key value violates unique constraint',
        'D': 'Key (id)=(1) already exists.',
        'H': 'Use a different primary key value.',
        'N': '9001',
      });
      final parsed = parseErrorMessage(payload);
      expect(parsed.severity, equals('ERROR'));
      expect(parsed.sqlState, equals('23505'));
      expect(
        parsed.message,
        equals('duplicate key value violates unique constraint'),
      );
      expect(parsed.detail, equals('Key (id)=(1) already exists.'));
      expect(parsed.hint, equals('Use a different primary key value.'));
      expect(parsed.code, equals(9001));

      final formatted = formatProtocolErrorMessage(
        parsed,
        fallbackMessage: 'query failed',
      );
      expect(formatted,
          contains('[23505] duplicate key value violates unique constraint'));
      expect(formatted, contains('Detail: Key (id)=(1) already exists.'));
      expect(formatted, contains('Hint: Use a different primary key value.'));
    });

    test('formatProtocolErrorMessage uses fallback when message is absent', () {
      final parsed = parseErrorMessage(_errorPayload({
        'S': 'ERROR',
        'C': '42000',
      }));
      final formatted = formatProtocolErrorMessage(
        parsed,
        fallbackMessage: 'query failed',
      );
      expect(formatted, equals('[42000] query failed'));
    });
  });

  group('res circuit breaker', () {
    test('transitions closed -> open -> halfOpen -> closed', () async {
      final breaker = CircuitBreaker(
        CircuitBreakerConfig(
          failureThreshold: 2,
          recoveryTimeoutMs: 20,
          successThreshold: 2,
          halfOpenMaxRequests: 1,
        ),
      );

      expect(breaker.state, equals(CircuitState.closed));
      expect(breaker.allowRequest(), isTrue);

      breaker.recordFailure();
      breaker.recordFailure();
      expect(breaker.state, equals(CircuitState.open));
      expect(breaker.allowRequest(), isFalse);

      await Future<void>.delayed(const Duration(milliseconds: 25));
      expect(breaker.allowRequest(), isTrue);
      expect(breaker.state, equals(CircuitState.halfOpen));
      expect(breaker.allowRequest(), isFalse);

      breaker.recordSuccess();
      expect(breaker.state, equals(CircuitState.halfOpen));
      expect(breaker.allowRequest(), isTrue);
      breaker.recordSuccess();
      expect(breaker.state, equals(CircuitState.closed));
    });

    test('halfOpen failure re-opens the circuit', () async {
      final breaker = CircuitBreaker(
        CircuitBreakerConfig(
          failureThreshold: 1,
          recoveryTimeoutMs: 10,
          successThreshold: 1,
          halfOpenMaxRequests: 1,
        ),
      );

      breaker.recordFailure();
      expect(breaker.state, equals(CircuitState.open));

      await Future<void>.delayed(const Duration(milliseconds: 15));
      expect(breaker.allowRequest(), isTrue);
      expect(breaker.state, equals(CircuitState.halfOpen));

      breaker.recordFailure();
      expect(breaker.state, equals(CircuitState.open));
    });
  });

  group('res keepalive', () {
    test('tracker marks idle validation requirement', () async {
      final tracker = KeepaliveTracker(
        KeepaliveConfig(
          intervalMs: 20,
          maxIdleBeforeCheckMs: 1,
          validationTimeoutMs: 20,
        ),
      );

      expect(tracker.needsValidation(), isFalse);
      await Future<void>.delayed(const Duration(milliseconds: 5));
      expect(tracker.needsValidation(), isTrue);

      tracker.markActive();
      expect(tracker.needsValidation(), isFalse);
    });

    test('manager triggers ping checks for idle connections', () async {
      final manager = KeepaliveManager(
        KeepaliveConfig(
          intervalMs: 5,
          maxIdleBeforeCheckMs: 0,
          validationTimeoutMs: 20,
        ),
      );
      final firstPing = Completer<void>();
      var pingCount = 0;

      manager.register('conn-1', () async {
        pingCount += 1;
        if (!firstPing.isCompleted) {
          firstPing.complete();
        }
        return true;
      });
      manager.start();
      await firstPing.future.timeout(const Duration(milliseconds: 250));
      manager.stop();

      expect(pingCount, greaterThanOrEqualTo(1));
    });
  });

  group('res leak detector', () {
    test('guard release is idempotent and checkin-safe', () {
      final detector = LeakDetector(
        LeakDetectionConfig(
          thresholdMs: 5,
          checkIntervalMs: 5,
          captureStackTrace: true,
        ),
      );
      final guard = detector.checkout(
        'conn-2',
        metadata: {'driver': 'dart'},
      );
      expect(() => guard.release(), returnsNormally);
      expect(() => guard.release(), returnsNormally);
      expect(() => detector.checkin('conn-2'), returnsNormally);
    });

    test('CheckoutInfo captures stack trace when enabled', () {
      final info = CheckoutInfo({'lane': 'dart'}, captureStackTrace: true);
      expect(info.stackTrace, isNotNull);
      expect(info.metadata['lane'], equals('dart'));
      expect(info.heldDurationMs(), greaterThanOrEqualTo(0));
    });

    test('timer callback reports held checkout beyond threshold', () async {
      final reports = <String>[];
      final firstReport = Completer<void>();
      final detector = LeakDetector(
        LeakDetectionConfig(
          thresholdMs: 0,
          checkIntervalMs: 5,
          onLeakDetected: (connectionId, _, heldDurationMs) {
            reports.add('$connectionId:$heldDurationMs');
            if (!firstReport.isCompleted) {
              firstReport.complete();
            }
          },
        ),
      );
      detector.start();
      detector.checkout('conn-leak');

      await firstReport.future.timeout(const Duration(milliseconds: 250));
      detector.stop();

      expect(reports, isNotEmpty);
      expect(reports.first.startsWith('conn-leak:'), isTrue);
    });

    test('timer callback does not fire after checkout is released', () async {
      var reportCount = 0;
      final detector = LeakDetector(
        LeakDetectionConfig(
          thresholdMs: 0,
          checkIntervalMs: 10,
          onLeakDetected: (_, __, ___) {
            reportCount += 1;
          },
        ),
      );
      detector.start();
      final guard = detector.checkout('conn-clean');
      guard.release();

      await Future<void>.delayed(const Duration(milliseconds: 40));
      detector.stop();

      expect(reportCount, equals(0));
    });
  });

  group('res telemetry', () {
    test('tracing disabled returns null spans', () {
      final cfg = TelemetryConfig()..enableTracing = false;
      final collector = TelemetryCollector(cfg);
      expect(collector.startSpan('query'), isNull);
    });

    test('metrics record success and failure query outcomes', () async {
      final collector = TelemetryCollector(TelemetryConfig());

      final successSpan = collector.startSpan('success_query');
      expect(successSpan, isNotNull);
      await Future<void>.delayed(const Duration(milliseconds: 2));
      collector.endSpan(successSpan, true);

      final failureSpan = collector.startSpan('failure_query');
      expect(failureSpan, isNotNull);
      await Future<void>.delayed(const Duration(milliseconds: 2));
      collector.endSpan(failureSpan, false);

      expect(collector.totalQueries, equals(2));
      expect(collector.successfulQueries, equals(1));
      expect(collector.failedQueries, equals(1));
      expect(collector.totalQueryTimeMs, greaterThanOrEqualTo(0));
    });

    test('sanitizeQuery redacts quoted literals', () {
      final sanitized = TelemetryCollector.sanitizeQuery(
        "SELECT * FROM users WHERE name='alice' AND city='NYC'",
      );
      expect(
          sanitized, equals("SELECT * FROM users WHERE name='?' AND city='?'"));
    });

    test('slow query log enforces retention cap of 100 entries', () {
      final cfg = TelemetryConfig()
        ..slowQueryThresholdMs = -1
        ..enableSlowQueryLog = true;
      final collector = TelemetryCollector(cfg);

      for (var i = 0; i < 105; i++) {
        final span = collector.startSpan('slow_$i');
        expect(span, isNotNull);
        collector.endSpan(span, true);
      }

      expect(collector.slowQueryCount, equals(100));
      final spanNames = collector.slowQueries
          .map((entry) => entry['spanName'] as String)
          .toList(growable: false);
      expect(spanNames.first, equals('slow_5'));
      expect(spanNames.last, equals('slow_104'));
    });

    test('slow query log respects enableSlowQueryLog flag', () {
      final cfg = TelemetryConfig()
        ..slowQueryThresholdMs = -1
        ..enableSlowQueryLog = false;
      final collector = TelemetryCollector(cfg);

      final span = collector.startSpan('slow_disabled');
      expect(span, isNotNull);
      collector.endSpan(span, true);

      expect(collector.slowQueryCount, equals(0));
      expect(collector.slowQueries, isEmpty);
    });
  });

  group('res reconnect boundary', () {
    test('close clears abandoned session state for fresh reconnect boundaries',
        () async {
      final client = ScratchBirdClient(
        const ScratchBirdConfig(
          host: 'localhost',
          port: 3092,
          database: 'mydb',
          user: 'user',
        ),
      );
      _setPrivateField(client, '_sequence', 9);
      _setPrivateField(client, '_lastQuerySequence', 7);
      _setPrivateField(
        client,
        '_attachmentId',
        Uint8List.fromList(List<int>.generate(16, (i) => i + 1)),
      );
      _setPrivateField(client, '_txnId', 42);
      _setPrivateField(client, '_transactionActive', true);
      _setPrivateField(client, '_explicitTransaction', true);
      (_getPrivateField(client, '_parameters') as Map<String, String>)
        ..['attachment_id'] = '11111111-1111-1111-1111-111111111111'
        ..['current_txn_id'] = '42';
      _setPrivateField(
        client,
        '_lastPlan',
        QueryPlanMessage(1, 2, 3, 4, Uint8List.fromList([1, 2, 3])),
      );
      _setPrivateField(
        client,
        '_lastSblr',
        SblrCompiledMessage(5, 6, Uint8List.fromList([4, 5, 6])),
      );

      await client.close();

      expect(client.lastQueryPlan, isNull);
      expect(client.lastSblrCompiled, isNull);
      expect(_getPrivateField(client, '_sequence'), equals(0));
      expect(_getPrivateField(client, '_lastQuerySequence'), isNull);
      expect(_getPrivateField(client, '_txnId'), equals(0));
      expect(_getPrivateField(client, '_transactionActive'), isFalse);
      expect(_getPrivateField(client, '_explicitTransaction'), isFalse);
      expect(
        (_getPrivateField(client, '_attachmentId') as Uint8List)
            .every((value) => value == 0),
        isTrue,
      );
      expect(
        _getPrivateField(client, '_parameters') as Map<String, String>,
        isEmpty,
      );
    });
  });
}

LibraryMirror _clientLibrary(Object object) {
  return reflect(object).type.owner as LibraryMirror;
}

Symbol _privateSymbol(Object object, String name) {
  return MirrorSystem.getSymbol(name, _clientLibrary(object));
}

dynamic _getPrivateField(Object object, String name) {
  return reflect(object).getField(_privateSymbol(object, name)).reflectee;
}

void _setPrivateField(Object object, String name, Object? value) {
  reflect(object).setField(_privateSymbol(object, name), value);
}
