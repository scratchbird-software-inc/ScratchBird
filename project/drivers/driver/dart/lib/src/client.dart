// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'dart:convert';
import 'dart:math';

import 'auth_bootstrap.dart';
import 'config.dart';
import 'protocol.dart';
import 'scram.dart';
import 'types.dart';
import 'metadata.dart';
import 'errors.dart';
import 'circuit_breaker.dart';
import 'keepalive.dart';
import 'leak_detector.dart';
import 'telemetry.dart';

const MANAGER_PROTOCOL_MAGIC = 0x42444253; // SBDB
const MANAGER_PROTOCOL_VERSION = 0x0101;
const MANAGER_HEADER_SIZE = 12;
const MANAGER_MAX_PAYLOAD_SIZE = 16 * 1024 * 1024;
const MCP_PROTOCOL_VERSION = 0x0100;

const MCP_MSG_CONNECT_RESPONSE = 0x02;
const MCP_MSG_AUTH_CHALLENGE = 0x12;
const MCP_MSG_AUTH_RESPONSE = 0x11;
const MCP_MSG_STATUS_RESPONSE = 0x64;
const MCP_MSG_HELLO = 0x65;
const MCP_MSG_AUTH_START = 0x66;
const MCP_MSG_AUTH_CONTINUE = 0x67;
const MCP_MSG_DB_CONNECT = 0x69;
const MCP_AUTH_METHOD_TOKEN = 4;

const int readCommittedModeDefault = 0;
const int readCommittedModeReadConsistency = 1;
const int readCommittedModeRecordVersion = 2;
const int readCommittedModeNoRecordVersion = 3;

String canonicalReadCommittedModeLabel(int mode) {
  switch (mode) {
    case readCommittedModeDefault:
      return 'READ COMMITTED';
    case readCommittedModeReadConsistency:
      return 'READ COMMITTED READ CONSISTENCY';
    case readCommittedModeRecordVersion:
      return 'READ COMMITTED RECORD VERSION';
    case readCommittedModeNoRecordVersion:
      return 'READ COMMITTED NO RECORD VERSION';
    default:
      return 'UNKNOWN($mode)';
  }
}

int _normalizePortalResumeMaxRows({required int fetchSize}) {
  if (fetchSize <= 0) {
    return 0;
  }
  if (fetchSize > 0xffffffff) {
    return 0xffffffff;
  }
  return fetchSize;
}

class ScratchBirdColumn {
  final String name;
  final int tableOid;
  final int columnIndex;
  final int typeOid;
  final int typeSize;
  final int typeModifier;
  final int format;
  final bool nullable;

  ScratchBirdColumn(
    this.name,
    this.typeOid,
    this.format, {
    this.tableOid = 0,
    this.columnIndex = 0,
    this.typeSize = 0,
    this.typeModifier = 0,
    this.nullable = false,
  });
}

class ScratchBirdResult {
  final List<List<dynamic>> rows;
  final List<ScratchBirdColumn> columns;

  ScratchBirdResult(this.rows, this.columns);
}

class NotificationMessage {
  final int processId;
  final String channel;
  final Uint8List payload;
  final String? changeType;
  final int? rowId;

  NotificationMessage(
      this.processId, this.channel, this.payload, this.changeType, this.rowId);
}

class QueryPlanMessage {
  final int format;
  final int planningTimeUs;
  final int estimatedRows;
  final int estimatedCost;
  final Uint8List plan;

  QueryPlanMessage(this.format, this.planningTimeUs, this.estimatedRows,
      this.estimatedCost, this.plan);
}

class SblrCompiledMessage {
  final int hash;
  final int version;
  final Uint8List bytecode;

  SblrCompiledMessage(this.hash, this.version, this.bytecode);
}

class ScratchBirdClient {
  final ScratchBirdConfig config;
  late final StreamIterator<Uint8List> _iter;
  late final _SocketReader _reader;
  Socket? _socket;
  int _sequence = 0;
  int? _lastQuerySequence;
  Uint8List _attachmentId = Uint8List(16);
  int _txnId = 0;
  bool _transactionActive = false;
  bool _explicitTransaction = false;
  bool _portalResumePending = false;
  final Map<String, String> _parameters = {};
  final List<void Function(NotificationMessage)> _notificationHandlers = [];
  QueryPlanMessage? _lastPlan;
  SblrCompiledMessage? _lastSblr;
  final String _connectionId =
      '${DateTime.now().microsecondsSinceEpoch}-${Random().nextInt(1 << 32)}';
  final CircuitBreaker _circuitBreaker = CircuitBreaker();
  final TelemetryCollector _telemetry = TelemetryCollector();
  final KeepaliveManager _keepaliveManager = KeepaliveManager();
  KeepaliveTracker? _keepaliveTracker;
  final LeakDetector _leakDetector = LeakDetector();
  LeakDetectionGuard? _leakGuard;
  ScratchBirdResolvedAuthContext _resolvedAuthContext;

  ScratchBirdClient(this.config)
      : _resolvedAuthContext = defaultResolvedAuthContext(
          normalizeFrontDoorMode(config.frontDoorMode),
        );

  static Future<ScratchBirdClient> connect(ScratchBirdConfig config) async {
    final client = ScratchBirdClient(config);
    await client._connect();
    return client;
  }

  static Future<ScratchBirdAuthProbeResult> probeAuthSurface(
    ScratchBirdConfig config,
  ) async {
    final client = ScratchBirdClient(config);
    try {
      return await client._probeAuthSurface();
    } finally {
      await client.close();
    }
  }

  Future<void> _connect() async {
    _resetResolvedAuthContext();
    normalizeNativeProtocol(config.protocol);
    await _connectTransport();
    if (normalizeFrontDoorMode(config.frontDoorMode) == 'manager_proxy') {
      await _performManagerConnect();
    }
    await _handshake();
    await _ensureImplicitTransaction();
    _startResilience();
  }

  Future<void> _connectTransport() async {
    final host = config.host;
    final port = config.port;
    final sslmode = _normalizeSslMode(config.sslmode);
    if (sslmode == 'disable') {
      _socket = await Socket.connect(
        host,
        port,
        timeout: Duration(milliseconds: config.connectTimeoutMs),
      );
    } else {
      final strictVerify = sslmode == 'verify-ca' ||
          sslmode == 'verify-full' ||
          sslmode == 'require';
      final context = SecurityContext(withTrustedRoots: true);
      if (config.sslrootcert != null && config.sslrootcert!.isNotEmpty) {
        context.setTrustedCertificates(config.sslrootcert!);
      }
      if (config.sslcert != null &&
          config.sslcert!.isNotEmpty &&
          config.sslkey != null &&
          config.sslkey!.isNotEmpty) {
        context.useCertificateChain(config.sslcert!);
        context.usePrivateKey(
          config.sslkey!,
          password: config.sslpassword?.isNotEmpty == true
              ? config.sslpassword
              : null,
        );
      }

      _socket = await SecureSocket.connect(
        host,
        port,
        context: context,
        onBadCertificate: strictVerify ? null : (_) => true,
        supportedProtocols: ['tlsv1.3'],
        timeout: Duration(milliseconds: config.connectTimeoutMs),
      );
    }
    _iter = StreamIterator(_socket!);
    _reader = _SocketReader(_iter);
  }

  String _normalizeSslMode(String value) {
    final normalized = value.trim().toLowerCase();
    switch (normalized) {
      case 'disable':
      case 'allow':
      case 'prefer':
      case 'require':
        return normalized;
      case 'verify-ca':
      case 'verify_ca':
        return 'verify-ca';
      case 'verify-full':
      case 'verify_full':
        return 'verify-full';
      default:
        throw ScratchBirdConnectionException(
          'Unsupported sslmode value: $value',
        );
    }
  }

  void _appendLengthPrefixedString(BytesBuilder out, String value) {
    final bytes = utf8.encode(value);
    final len = ByteData(4)..setUint32(0, bytes.length, Endian.little);
    out.add(len.buffer.asUint8List());
    out.add(bytes);
  }

  Uint8List _randomBytes(int length) {
    final rng = Random.secure();
    return Uint8List.fromList(
        List<int>.generate(length, (_) => rng.nextInt(256)));
  }

  ScratchBirdResolvedAuthContext getResolvedAuthContext() {
    return _resolvedAuthContext;
  }

  void _resetResolvedAuthContext() {
    _resolvedAuthContext = defaultResolvedAuthContext(
      normalizeFrontDoorMode(config.frontDoorMode),
    );
  }

  void _markResolvedAuthContextDetached() {
    _resolvedAuthContext = _resolvedAuthContext.copyWith(attached: false);
  }

  int _buildStartupFeatures() {
    var features = 0;
    if (config.compression.trim().toLowerCase() != 'off') {
      features |= featureCompression;
    }
    if (config.binaryTransfer) {
      features |= featureStreaming;
    }
    return features;
  }

  Map<String, String> _buildStartupParams() {
    final params = <String, String>{
      'database': config.database,
      'user': config.user,
      'client_flags': '${config.connectClientFlags}',
    };
    if ((config.dormantId?.isNotEmpty == true) !=
        (config.dormantReattachToken?.isNotEmpty == true)) {
      throw const ScratchBirdProgrammingException(
        'dormantId and dormantReattachToken must be provided together',
        sqlState: '42601',
      );
    }
    if (config.role != null && config.role!.isNotEmpty) {
      params['role'] = config.role!;
    }
    if (config.applicationName != null && config.applicationName!.isNotEmpty) {
      params['application_name'] = config.applicationName!;
    }
    if (config.dormantId?.isNotEmpty == true) {
      params['dormant_id'] = config.dormantId!;
      params['dormant_reattach_token'] = config.dormantReattachToken!;
    }
    applyAuthPluginSelection(params, config);
    return params;
  }

  Future<ScratchBirdAuthProbeResult> _probeAuthSurface() async {
    _resetResolvedAuthContext();
    normalizeNativeProtocol(config.protocol);
    await _connectTransport();
    final frontDoorMode = normalizeFrontDoorMode(config.frontDoorMode);
    if (frontDoorMode == 'manager_proxy') {
      return _probeManagerAuthSurface();
    }
    return _probeDirectAuthSurface();
  }

  Future<ScratchBirdAuthProbeResult> _probeDirectAuthSurface() async {
    final startup =
        buildStartupPayload(_buildStartupFeatures(), _buildStartupParams());
    await _sendMessage(MessageType.startup, startup, forceZero: true);
    while (true) {
      final msg = await _recvMessage();
      if (_handleAsyncMessage(msg)) {
        continue;
      }
      switch (msg.header.type) {
        case MessageType.negotiateVersion:
          continue;
        case MessageType.authRequest:
          final parsed = parseAuthRequest(msg.payload);
          final methodSurface = describeAuthMethod(
            parsed.method,
            configuredMethodId: config.authMethodId,
          );
          return ScratchBirdAuthProbeResult(
            reachable: true,
            ingressMode: 'direct',
            resolvedHost: config.host,
            resolvedPort: config.port,
            admittedMethods: methodSurface == null ? const [] : [methodSurface],
            requiredMethod: methodSurface?.wireMethod,
            requiredPluginMethodId: methodSurface?.pluginMethodId,
            allowedTransportMask: null,
            additionalContinuationPossible:
                additionalContinuationPossibleForAuthMethod(parsed.method),
          );
        case MessageType.authOk:
        case MessageType.ready:
          return ScratchBirdAuthProbeResult(
            reachable: true,
            ingressMode: 'direct',
            resolvedHost: config.host,
            resolvedPort: config.port,
            admittedMethods: const [],
            requiredMethod: null,
            requiredPluginMethodId: null,
            allowedTransportMask: null,
            additionalContinuationPossible: false,
          );
        case MessageType.error:
          throw _authExceptionFromPayload(
            msg.payload,
            fallbackMessage: 'Authentication probe failed',
          );
      }
    }
  }

  Future<ScratchBirdAuthProbeResult> _probeManagerAuthSurface() async {
    final hello = ByteData(4);
    hello.setUint16(0, MCP_PROTOCOL_VERSION, Endian.little);
    hello.setUint16(2, config.managerClientFlags & 0xffff, Endian.little);
    await _sendManagerFrame(MCP_MSG_HELLO, hello.buffer.asUint8List());
    final frame = await _recvManagerFrame();
    if (frame.type != MCP_MSG_STATUS_RESPONSE) {
      throw const ScratchBirdProtocolException(
        'Expected MCP hello status response',
      );
    }
    final methodSurface = describeAuthMethod(authTokenMethod);
    return ScratchBirdAuthProbeResult(
      reachable: true,
      ingressMode: 'manager_proxy',
      resolvedHost: config.host,
      resolvedPort: config.port,
      admittedMethods: methodSurface == null ? const [] : [methodSurface],
      requiredMethod: methodSurface?.wireMethod,
      requiredPluginMethodId: methodSurface?.pluginMethodId,
      allowedTransportMask: null,
      additionalContinuationPossible: true,
    );
  }

  Future<void> _performManagerConnect() async {
    final token = config.managerAuthToken ?? '';
    if (token.isEmpty) {
      throw const ScratchBirdAuthException(
        'manager_proxy mode requires manager_auth_token',
        sqlState: '28000',
      );
    }
    final managerUser =
        (config.managerUsername != null && config.managerUsername!.isNotEmpty)
            ? config.managerUsername!
            : (config.user.isNotEmpty ? config.user : 'admin');
    final managerDatabase =
        (config.managerDatabase != null && config.managerDatabase!.isNotEmpty)
            ? config.managerDatabase!
            : config.database;
    final managerProfile = (config.managerConnectionProfile != null &&
            config.managerConnectionProfile!.isNotEmpty)
        ? config.managerConnectionProfile!
        : 'SBsql';
    final managerIntent = (config.managerClientIntent != null &&
            config.managerClientIntent!.isNotEmpty)
        ? config.managerClientIntent!
        : 'SBsql';
    final managerFlags = config.managerClientFlags;
    final authFastPath = config.managerAuthFastPath;

    final hello = ByteData(4);
    hello.setUint16(0, MCP_PROTOCOL_VERSION, Endian.little);
    hello.setUint16(2, managerFlags & 0xffff, Endian.little);
    await _sendManagerFrame(MCP_MSG_HELLO, hello.buffer.asUint8List());
    var frame = await _recvManagerFrame();
    if (frame.type != MCP_MSG_STATUS_RESPONSE) {
      throw const ScratchBirdProtocolException(
        'Expected MCP hello status response',
      );
    }

    final authStart = BytesBuilder();
    _appendLengthPrefixedString(authStart, managerUser);
    authStart.add([MCP_AUTH_METHOD_TOKEN]);
    if (authFastPath) {
      final tokenBytes = utf8.encode(token);
      final tokenLen = ByteData(4)
        ..setUint32(0, tokenBytes.length, Endian.little);
      authStart.add(tokenLen.buffer.asUint8List());
      authStart.add(tokenBytes);
    } else {
      authStart.add(Uint8List(4));
    }
    await _sendManagerFrame(MCP_MSG_AUTH_START, authStart.toBytes());
    frame = await _recvManagerFrame();
    if (frame.type == MCP_MSG_AUTH_CHALLENGE) {
      final tokenBytes = utf8.encode(token);
      final authContinue = BytesBuilder();
      final tokenLen = ByteData(4)
        ..setUint32(0, tokenBytes.length, Endian.little);
      authContinue.add(tokenLen.buffer.asUint8List());
      authContinue.add(tokenBytes);
      await _sendManagerFrame(MCP_MSG_AUTH_CONTINUE, authContinue.toBytes());
      frame = await _recvManagerFrame();
    }
    if (frame.type != MCP_MSG_AUTH_RESPONSE) {
      throw const ScratchBirdProtocolException('Expected MCP auth response');
    }
    if (frame.payload.length < 1 + 4 + 256) {
      throw const ScratchBirdProtocolException('Truncated MCP auth response');
    }
    if (frame.payload[0] != 0) {
      final err = utf8
          .decode(frame.payload.sublist(5, 261), allowMalformed: true)
          .replaceAll(RegExp(r'\x00+$'), '');
      throw ScratchBirdAuthException(
        err.isEmpty ? 'MCP authentication failed' : err,
      );
    }

    final dbConnect = BytesBuilder();
    dbConnect.add(utf8.encode('MCP1'));
    _appendLengthPrefixedString(dbConnect, managerDatabase);
    _appendLengthPrefixedString(dbConnect, managerProfile);
    _appendLengthPrefixedString(dbConnect, managerIntent);
    final nonce = _randomBytes(16);
    final nonceLen = ByteData(2)..setUint16(0, nonce.length, Endian.little);
    dbConnect.add(nonceLen.buffer.asUint8List());
    dbConnect.add(nonce);

    await _sendManagerFrame(MCP_MSG_DB_CONNECT, dbConnect.toBytes());
    frame = await _recvManagerFrame();
    if (frame.type != MCP_MSG_CONNECT_RESPONSE) {
      throw const ScratchBirdProtocolException('Expected MCP connect response');
    }
    if (frame.payload.length < 1 + 2 + 2 + 16 + 64 + 32) {
      throw const ScratchBirdProtocolException(
        'Truncated MCP connect response',
      );
    }
    if (frame.payload[0] != 0) {
      var message = 'MCP database connect failed';
      final errOffset = 1 + 2 + 2 + 16 + 64 + 32;
      if (frame.payload.length >= errOffset + 4) {
        final errLen =
            ByteData.sublistView(frame.payload, errOffset, errOffset + 4)
                .getUint32(0, Endian.little);
        if (frame.payload.length >= errOffset + 4 + errLen) {
          message = utf8.decode(
            frame.payload.sublist(errOffset + 4, errOffset + 4 + errLen),
            allowMalformed: true,
          );
        }
      }
      throw ScratchBirdConnectionException(message);
    }
    _resolvedAuthContext = _resolvedAuthContext.copyWith(
      ingressMode: 'manager_proxy',
      managerAuthenticated: true,
      attached: false,
    );
  }

  Future<void> close() async {
    _clearAbandonedSessionState();
    _markResolvedAuthContextDetached();
    await _socket?.close();
    _stopResilience();
  }

  Future<ScratchBirdResult> query(String sql,
      [List<dynamic> params = const []]) async {
    if (sql.trim().isEmpty) {
      throw ArgumentError.value(sql, 'sql', 'SQL text must not be empty');
    }
    return _withResilience("query", sql, () async {
      if (params.isEmpty) {
        await _sendSimpleQuery(sql, 0, 0);
        return _collectResults();
      }
      await _sendExtendedQuery(sql, params, 0);
      return _collectResults();
    });
  }

  Future<ScratchBirdResult> queryMetadata([String collectionName = 'tables']) {
    final sql = resolveMetadataCollectionQuery(collectionName);
    return query(sql);
  }

  Future<ScratchBirdResult> metadataSchemas() => queryMetadata('schemas');

  Future<ScratchBirdResult> metadataTables() => queryMetadata('tables');

  Future<ScratchBirdResult> metadataColumns() => queryMetadata('columns');

  Future<ScratchBirdResult> metadataIndexes() => queryMetadata('indexes');

  Future<ScratchBirdResult> metadataIndexColumns() =>
      queryMetadata('index_columns');

  Future<ScratchBirdResult> metadataConstraints() =>
      queryMetadata('constraints');

  Future<ScratchBirdResult> metadataProcedures() => queryMetadata('procedures');

  Future<ScratchBirdResult> metadataFunctions() => queryMetadata('functions');

  Future<ScratchBirdResult> metadataRoutines() => queryMetadata('routines');

  Future<ScratchBirdResult> metadataCatalogs() => queryMetadata('catalogs');

  Future<ScratchBirdResult> metadataPrimaryKeys() =>
      queryMetadata('primary_keys');

  Future<ScratchBirdResult> metadataForeignKeys() =>
      queryMetadata('foreign_keys');

  Future<ScratchBirdResult> metadataTablePrivileges() =>
      queryMetadata('table_privileges');

  Future<ScratchBirdResult> metadataColumnPrivileges() =>
      queryMetadata('column_privileges');

  Future<ScratchBirdResult> metadataTypeInfo() => queryMetadata('type_info');

  Future<List<Map<String, dynamic>>> getSchema({
    String collectionName = 'tables',
    bool? expandParents,
  }) async {
    final normalizedCollection =
        normalizeMetadataCollectionName(collectionName);
    final result = await queryMetadata(collectionName);
    final rows = _resultRowsToMaps(result);
    final shouldExpand = expandParents ?? config.metadataExpandSchemaParents;
    if (normalizedCollection != MetadataCollectionName.schemas ||
        !shouldExpand) {
      return rows;
    }
    return expandSchemaMetadataRows(rows, expandParents: true);
  }

  Future<MetadataSchemaTree> getSchemaTree({
    bool? expandParents,
    String? database,
  }) async {
    final shouldExpand = expandParents ?? config.metadataExpandSchemaParents;
    final rows = await getSchema(
      collectionName: 'schemas',
      expandParents: shouldExpand,
    );
    return buildMetadataSchemaTree(
      rows,
      expandParents: shouldExpand,
      database: database ?? config.database,
    );
  }

  void onNotification(void Function(NotificationMessage) handler) {
    _notificationHandlers.add(handler);
  }

  QueryPlanMessage? get lastQueryPlan => _lastPlan;
  SblrCompiledMessage? get lastSblrCompiled => _lastSblr;

  /// Public isolation aliases map onto the canonical MGA modes:
  /// READ COMMITTED => READ COMMITTED
  /// REPEATABLE READ => SNAPSHOT
  /// SERIALIZABLE => SNAPSHOT TABLE STABILITY
  /// readCommittedMode selects the canonical READ COMMITTED sub-mode.
  Future<void> begin({
    int? isolationLevel,
    int? readCommittedMode,
    int? accessMode,
    bool? deferrable,
    bool? wait,
    int? timeoutMs,
    int? autocommitMode,
    int conflictAction = 0,
  }) async {
    if (_hasActiveTransaction()) {
      if (_explicitTransaction) {
        throw const ScratchBirdTransactionException(
          'Transaction already active',
        );
      }
      _explicitTransaction = true;
      return;
    }
    await _withResilience("txn_begin", null, () async {
      var flags = 0;
      var isolation = isolationLevel ?? isolationReadCommitted;
      if (isolationLevel != null) flags |= txnFlagHasIsolation;
      if (readCommittedMode != null) {
        if (isolationLevel != null &&
            isolationLevel != isolationReadUncommitted &&
            isolationLevel != isolationReadCommitted) {
          throw const ScratchBirdNotSupportedException(
            'readCommittedMode requires a READ COMMITTED isolation alias',
            sqlState: '0A000',
          );
        }
        flags |= txnFlagHasReadCommittedMode;
        if (isolationLevel == null) {
          isolation = isolationReadCommitted;
          flags |= txnFlagHasIsolation;
        }
      }
      if (accessMode != null) flags |= txnFlagHasAccess;
      if (deferrable != null) flags |= txnFlagHasDeferrable;
      if (wait != null) flags |= txnFlagHasWait;
      if (timeoutMs != null) flags |= txnFlagHasTimeout;
      if (autocommitMode != null) flags |= txnFlagHasAutocommit;
      final payload = buildTxnBeginPayload(
        flags,
        conflictAction,
        autocommitMode ?? 0,
        isolation,
        accessMode ?? 0,
        deferrable == true ? 1 : 0,
        wait == true ? 1 : 0,
        timeoutMs ?? 0,
        readCommittedMode ?? readCommittedModeDefault,
      );
      await _sendMessage(MessageType.txnBegin, payload);
      await _drainUntilReady();
    });
    _transactionActive = true;
    _explicitTransaction = true;
  }

  Future<void> commit([int flags = 0]) async {
    _ensureActiveTransaction('commit');
    await _withResilience("txn_commit", null, () async {
      await _sendSimpleQuery('COMMIT', 0, 0);
      await _collectResults();
    });
    _transactionActive = true;
    _explicitTransaction = false;
  }

  Future<void> rollback([int flags = 0]) async {
    _ensureActiveTransaction('rollback');
    await _withResilience("txn_rollback", null, () async {
      await _sendSimpleQuery('ROLLBACK', 0, 0);
      await _collectResults();
    });
    _transactionActive = true;
    _explicitTransaction = false;
  }

  bool supportsPreparedTransactions() => true;

  Future<void> prepareTransaction(String globalTransactionId) async {
    await query(_buildPreparedTransactionSql(
      'PREPARE TRANSACTION',
      globalTransactionId,
    ));
  }

  Future<void> commitPrepared(String globalTransactionId) async {
    await query(_buildPreparedTransactionSql(
      'COMMIT PREPARED',
      globalTransactionId,
    ));
  }

  Future<void> rollbackPrepared(String globalTransactionId) async {
    await query(_buildPreparedTransactionSql(
      'ROLLBACK PREPARED',
      globalTransactionId,
    ));
  }

  bool supportsDormantReattach() => false;

  Future<void> detachToDormant() async {
    throw const ScratchBirdNotSupportedException(
      'Dormant detach requires an explicit public token flow and is not yet exposed in this lane',
      sqlState: '0A000',
    );
  }

  Future<void> reattachDormant(String dormantId, [String? authToken]) async {
    throw const ScratchBirdNotSupportedException(
      'Dormant reattach requires an explicit engine-issued token and is not yet exposed in this lane',
      sqlState: '0A000',
    );
  }

  Future<void> savepoint(String name) async {
    _ensureActiveTransaction('savepoint');
    _validateSavepointName(name);
    await _withResilience("txn_savepoint", null, () async {
      await _sendSimpleQuery('SAVEPOINT ${_quoteIdentifier(name)}', 0, 0);
      await _collectResults();
    });
  }

  Future<void> releaseSavepoint(String name) async {
    _ensureActiveTransaction('release savepoint');
    _validateSavepointName(name);
    await _withResilience("txn_release", null, () async {
      await _sendSimpleQuery(
        'RELEASE SAVEPOINT ${_quoteIdentifier(name)}',
        0,
        0,
      );
      await _collectResults();
    });
  }

  Future<void> rollbackToSavepoint(String name) async {
    _ensureActiveTransaction('rollback to savepoint');
    _validateSavepointName(name);
    await _withResilience("txn_rollback_to", null, () async {
      await _sendSimpleQuery(
        'ROLLBACK TO SAVEPOINT ${_quoteIdentifier(name)}',
        0,
        0,
      );
      await _collectResults();
    });
  }

  Future<void> setOption(String name, String value) async {
    await _withResilience("set_option", null, () async {
      await _sendMessage(
          MessageType.setOption, buildSetOptionPayload(name, value));
      await _drainUntilReady();
    });
  }

  Future<void> ping() async {
    await _sendMessage(MessageType.ping, Uint8List(0));
    while (true) {
      final msg = await _recvMessage();
      if (_handleAsyncMessage(msg)) {
        continue;
      }
      if (msg.header.type == MessageType.pong) {
        return;
      }
      if (msg.header.type == MessageType.ready) {
        _applyTxnState(
          _readTxnId(msg.payload, fallback: msg.header.txnId, offset: 4),
        );
        return;
      }
      if (msg.header.type == MessageType.error) {
        throw _executionExceptionFromPayload(
          msg.payload,
          fallbackMessage: 'Ping failed',
        );
      }
    }
  }

  Future<void> terminate() async {
    if (_socket == null) return;
    _lastQuerySequence = null;
    await _sendMessage(MessageType.terminate, Uint8List(0));
    _markResolvedAuthContextDetached();
    await close();
  }

  Future<void> subscribe(String channel,
      {int subscribeType = subscribeTypeChannel,
      String filterExpr = ''}) async {
    await _sendMessage(MessageType.subscribe,
        buildSubscribePayload(subscribeType, channel, filterExpr));
    await _drainUntilReady();
  }

  Future<void> unsubscribe(String channel) async {
    await _sendMessage(
        MessageType.unsubscribe, buildUnsubscribePayload(channel));
    await _drainUntilReady();
  }

  Future<ScratchBirdResult> executeSblr(int sblrHash, Uint8List? bytecode,
      [List<dynamic> params = const []]) async {
    final encoded = params.map(encodeParam).toList();
    final payload = buildSblrExecutePayload(
        sblrHash, bytecode, encoded.map((e) => e.param).toList());
    _lastPlan = null;
    _lastSblr = null;
    _lastQuerySequence = await _sendMessage(MessageType.sblrExecute, payload);
    await _sendMessage(MessageType.sync, Uint8List(0));
    return _collectResults();
  }

  Future<void> streamControl(int controlType,
      {int windowSize = 0, int timeoutMs = 0}) async {
    await _sendMessage(MessageType.streamControl,
        buildStreamControlPayload(controlType, windowSize, timeoutMs));
  }

  Future<void> attachCreate(String emulationMode, String dbName) async {
    await _sendMessage(MessageType.attachCreate,
        buildAttachCreatePayload(emulationMode, dbName));
    await _drainUntilReady();
  }

  Future<void> attachDetach() async {
    await _sendMessage(MessageType.attachDetach, Uint8List(0));
    await _drainUntilReady();
  }

  Future<ScratchBirdResult> attachList() async {
    await _sendMessage(MessageType.attachList, Uint8List(0));
    await _sendMessage(MessageType.sync, Uint8List(0));
    return _collectResults();
  }

  Future<void> cancel() async {
    final sequence = _lastQuerySequence;
    if (sequence == null) {
      throw const ScratchBirdExecutionException('No active query to cancel');
    }
    _portalResumePending = false;
    final payload = buildCancelPayload(0, sequence);
    await _sendMessage(MessageType.cancel, payload, flags: 0x08);
  }

  Future<void> _handshake() async {
    final startup =
        buildStartupPayload(_buildStartupFeatures(), _buildStartupParams());
    await _sendMessage(MessageType.startup, startup, forceZero: true);

    ScramClient? scram;
    while (true) {
      final msg = await _recvMessage();
      switch (msg.header.type) {
        case MessageType.negotiateVersion:
          continue;
        case MessageType.authRequest:
          final parsed = parseAuthRequest(msg.payload);
          final method = parsed.method;
          final pluginId = authPluginIdForMethod(method, config.authMethodId);
          switch (method) {
            case authPasswordMethod:
              _resolvedAuthContext = _resolvedAuthContext.copyWith(
                resolvedAuthMethod: 'PASSWORD',
                resolvedAuthPluginId: pluginId,
              );
              await _sendMessage(
                MessageType.authResponse,
                Uint8List.fromList(utf8.encode(config.password ?? '')),
                forceZero: true,
              );
              break;
            case authScramSha256Method:
              scram ??=
                  ScramClient(config.user, algorithm: ScramAlgorithm.sha256);
              _resolvedAuthContext = _resolvedAuthContext.copyWith(
                resolvedAuthMethod: 'SCRAM_SHA_256',
                resolvedAuthPluginId: pluginId,
              );
              final clientFirst = scram.clientFirstMessage();
              await _sendMessage(
                MessageType.authResponse,
                Uint8List.fromList(utf8.encode(clientFirst)),
                forceZero: true,
              );
              break;
            case authScramSha512Method:
              scram ??=
                  ScramClient(config.user, algorithm: ScramAlgorithm.sha512);
              _resolvedAuthContext = _resolvedAuthContext.copyWith(
                resolvedAuthMethod: 'SCRAM_SHA_512',
                resolvedAuthPluginId: pluginId,
              );
              final clientFirst = scram.clientFirstMessage();
              await _sendMessage(
                MessageType.authResponse,
                Uint8List.fromList(utf8.encode(clientFirst)),
                forceZero: true,
              );
              break;
            case authTokenMethod:
              final tokenPayload = resolveTokenAuthPayload(config);
              if (tokenPayload == null || tokenPayload.isEmpty) {
                throw const ScratchBirdAuthException(
                  'TOKEN auth requires auth_token or equivalent auth payload input',
                  sqlState: '28000',
                );
              }
              _resolvedAuthContext = _resolvedAuthContext.copyWith(
                resolvedAuthMethod: 'TOKEN',
                resolvedAuthPluginId: pluginId,
              );
              await _sendMessage(
                MessageType.authResponse,
                tokenPayload,
                forceZero: true,
              );
              break;
            case authMd5Method:
            case authPeerMethod:
            case authReattachMethod:
              final methodName = authMethodName(method);
              final message = method == authPeerMethod
                  ? 'Admitted auth method $methodName requires external broker support in this lane'
                  : 'Admitted auth method $methodName is not executable locally in this lane';
              throw ScratchBirdAuthException(
                message,
                sqlState: '28000',
              );
            default:
              throw ScratchBirdAuthException(
                'Unsupported auth method ${parsed.method}',
                sqlState: '28000',
              );
          }
          break;
        case MessageType.authContinue:
          final parsed = parseAuthContinue(msg.payload);
          final method = parsed.method;
          if ((method == authScramSha256Method ||
                  method == authScramSha512Method) &&
              scram != null) {
            final data = utf8.decode(parsed.data);
            final clientFinal =
                scram.handleServerFirst(config.password ?? '', data);
            await _sendMessage(
              MessageType.authResponse,
              Uint8List.fromList(utf8.encode(clientFinal)),
              forceZero: true,
            );
          } else if (method == authTokenMethod) {
            final tokenPayload = resolveTokenAuthPayload(config);
            if (tokenPayload == null || tokenPayload.isEmpty) {
              throw const ScratchBirdAuthException(
                'TOKEN auth requires auth_token or equivalent auth payload input',
                sqlState: '28000',
              );
            }
            _resolvedAuthContext = _resolvedAuthContext.copyWith(
              resolvedAuthMethod: 'TOKEN',
              resolvedAuthPluginId:
                  authPluginIdForMethod(method, config.authMethodId),
            );
            await _sendMessage(
              MessageType.authResponse,
              tokenPayload,
              forceZero: true,
            );
          } else if (method == authPeerMethod) {
            throw const ScratchBirdAuthException(
              'Admitted auth method PEER requires external broker support in this lane',
              sqlState: '28000',
            );
          }
          break;
        case MessageType.authOk:
          final parsed = parseAuthOk(msg.payload);
          if (parsed.sessionId.length == 16) {
            _attachmentId = parsed.sessionId;
          } else {
            _attachmentId = msg.header.attachmentId;
          }
          if (scram != null && parsed.serverInfo.isNotEmpty) {
            final serverFinal = utf8.decode(
              parsed.serverInfo,
              allowMalformed: true,
            );
            if (serverFinal.startsWith('v=')) {
              scram.verifyServerFinal(serverFinal);
            }
          }
          _applyTxnState(msg.header.txnId);
          break;
        case MessageType.parameterStatus:
          _handleParameterStatus(msg.payload);
          continue;
        case MessageType.ready:
          _applyTxnState(
            _readTxnId(msg.payload, fallback: msg.header.txnId, offset: 4),
          );
          _resolvedAuthContext = _resolvedAuthContext.copyWith(attached: true);
          return;
        case MessageType.error:
          throw _authExceptionFromPayload(
            msg.payload,
            fallbackMessage: 'Authentication failed',
          );
      }
    }
  }

  Future<ScratchBirdResult> _collectResults() async {
    List<ScratchBirdColumn> columns = [];
    final rows = <List<dynamic>>[];
    while (true) {
      final msg = await _recvMessage();
      if (_handleAsyncMessage(msg)) {
        continue;
      }
      switch (msg.header.type) {
        case MessageType.rowDescription:
          columns = _parseRowDescription(msg.payload);
          break;
        case MessageType.dataRow:
          final values = _parseDataRow(msg.payload);
          final decoded = <dynamic>[];
          for (var i = 0; i < values.length; i++) {
            final value = values[i];
            if (value == null) {
              decoded.add(null);
            } else {
              decoded.add(
                  decodeValue(columns[i].typeOid, value, columns[i].format));
            }
          }
          rows.add(decoded);
          break;
        case MessageType.portalSuspended:
          _allowPortalResume();
          await _resumeSuspendedPortal(
            _normalizePortalResumeMaxRows(fetchSize: config.fetchSize),
          );
          break;
        case MessageType.ready:
          _applyTxnState(
            _readTxnId(msg.payload, fallback: msg.header.txnId, offset: 4),
          );
          _portalResumePending = false;
          _lastQuerySequence = null;
          return ScratchBirdResult(rows, columns);
        case MessageType.error:
          final error = _executionExceptionFromPayload(
            msg.payload,
            fallbackMessage: 'Query failed',
          );
          await _drainReadyAfterError();
          _portalResumePending = false;
          _lastQuerySequence = null;
          throw error;
      }
    }
  }

  Future<void> _sendSimpleQuery(String sql, int maxRows, int timeoutMs) async {
    final flags = config.binaryTransfer ? queryFlagBinaryResult : 0;
    _lastPlan = null;
    _lastSblr = null;
    _portalResumePending = false;
    _lastQuerySequence = await _sendMessage(
        MessageType.query, buildQueryPayload(sql, flags, maxRows, timeoutMs));
  }

  Future<void> _sendExtendedQuery(
      String sql, List<dynamic> params, int maxRows) async {
    final normalized = _normalizeQuery(sql, params);
    final enc = normalized.params.map(encodeParam).toList();
    final paramValues = enc.map((e) => e.param).toList();
    final paramTypes = enc.map((e) => e.oid).toList();
    await _sendMessage(
        MessageType.parse, buildParsePayload('', normalized.sql, paramTypes));
    await _sendMessage(
        MessageType.describe, buildDescribePayload('S'.codeUnitAt(0), ''));
    await _sendMessage(MessageType.sync, Uint8List(0));
    await _drainUntilReady();
    await _sendMessage(
        MessageType.bind, buildBindPayload('', '', paramValues, [1]));
    _lastPlan = null;
    _lastSblr = null;
    _portalResumePending = false;
    _lastQuerySequence = await _sendMessage(
        MessageType.execute, buildExecutePayload('', maxRows));
    await _sendMessage(MessageType.sync, Uint8List(0));
  }

  bool _handleAsyncMessage(ScratchBirdMessage msg) {
    switch (msg.header.type) {
      case MessageType.parameterStatus:
        _handleParameterStatus(msg.payload);
        return true;
      case MessageType.notification:
        final notice = _parseNotification(msg.payload);
        for (final handler in _notificationHandlers) {
          handler(notice);
        }
        return true;
      case MessageType.queryPlan:
        _lastPlan = _parseQueryPlan(msg.payload);
        return true;
      case MessageType.sblrCompiled:
        _lastSblr = _parseSblrCompiled(msg.payload);
        return true;
      case MessageType.txnStatus:
        _applyTxnState(
          _readTxnId(
            msg.payload,
            fallback: msg.header.txnId,
            offset: msg.payload.length >= 12 ? 4 : 0,
          ),
        );
        return true;
      default:
        return false;
    }
  }

  void _handleParameterStatus(Uint8List payload) {
    if (payload.length < 8) return;
    final data = ByteData.sublistView(payload);
    final nameLen = data.getUint32(0, Endian.little);
    if (4 + nameLen + 4 > payload.length) return;
    final name = utf8.decode(payload.sublist(4, 4 + nameLen));
    final valueLen = data.getUint32(4 + nameLen, Endian.little);
    final valueStart = 8 + nameLen;
    if (valueStart + valueLen > payload.length) return;
    final value =
        utf8.decode(payload.sublist(valueStart, valueStart + valueLen));
    _parameters[name] = value;
    if (name == 'attachment_id') {
      final parsed = _parseUuidBytes(value);
      if (parsed != null) _attachmentId = parsed;
    }
    if (name == 'current_txn_id') {
      final parsed = int.tryParse(value.trim());
      if (parsed != null) _applyTxnState(parsed);
    }
  }

  NotificationMessage _parseNotification(Uint8List payload) {
    if (payload.length < 12) {
      throw const ScratchBirdProtocolException('Notification truncated');
    }
    var offset = 0;
    final data = ByteData.sublistView(payload);
    final processId = data.getUint32(offset, Endian.little);
    offset += 4;
    final channelLen = data.getUint32(offset, Endian.little);
    offset += 4;
    if (offset + channelLen + 4 > payload.length) {
      throw const ScratchBirdProtocolException('Notification truncated');
    }
    final channel = utf8.decode(payload.sublist(offset, offset + channelLen));
    offset += channelLen;
    final payloadLen = data.getUint32(offset, Endian.little);
    offset += 4;
    if (offset + payloadLen > payload.length) {
      throw const ScratchBirdProtocolException('Notification truncated');
    }
    final noticePayload = payload.sublist(offset, offset + payloadLen);
    offset += payloadLen;
    String? changeType;
    int? rowId;
    if (offset < payload.length) {
      changeType = String.fromCharCode(payload[offset]);
      offset += 1;
      if (offset + 8 <= payload.length) {
        rowId = data.getUint64(offset, Endian.little);
      }
    }
    return NotificationMessage(
        processId, channel, noticePayload, changeType, rowId);
  }

  QueryPlanMessage _parseQueryPlan(Uint8List payload) {
    if (payload.length < 32) {
      throw const ScratchBirdProtocolException('Query plan truncated');
    }
    final data = ByteData.sublistView(payload);
    final format = data.getUint32(0, Endian.little);
    final planLen = data.getUint32(4, Endian.little);
    final planningTimeUs = data.getUint64(8, Endian.little);
    final estimatedRows = data.getUint64(16, Endian.little);
    final estimatedCost = data.getUint64(24, Endian.little);
    if (32 + planLen > payload.length) {
      throw const ScratchBirdProtocolException('Query plan truncated');
    }
    final plan = payload.sublist(32, 32 + planLen);
    return QueryPlanMessage(
        format, planningTimeUs, estimatedRows, estimatedCost, plan);
  }

  SblrCompiledMessage _parseSblrCompiled(Uint8List payload) {
    if (payload.length < 16) {
      throw const ScratchBirdProtocolException('SBLR compiled truncated');
    }
    final data = ByteData.sublistView(payload);
    final hash = data.getUint64(0, Endian.little);
    final version = data.getUint32(8, Endian.little);
    final length = data.getUint32(12, Endian.little);
    if (16 + length > payload.length) {
      throw const ScratchBirdProtocolException('SBLR compiled truncated');
    }
    final bytecode = payload.sublist(16, 16 + length);
    return SblrCompiledMessage(hash, version, bytecode);
  }

  List<Map<String, dynamic>> _resultRowsToMaps(ScratchBirdResult result) {
    final fieldNames =
        result.columns.map((column) => column.name).toList(growable: false);
    final rows = <Map<String, dynamic>>[];
    for (final row in result.rows) {
      final out = <String, dynamic>{};
      for (var i = 0; i < row.length; i++) {
        final key = i < fieldNames.length ? fieldNames[i] : 'col_${i + 1}';
        out[key] = row[i];
      }
      rows.add(out);
    }
    return rows;
  }

  Uint8List? _parseUuidBytes(String value) {
    final hex = value.replaceAll('-', '').trim();
    if (!RegExp(r'^[0-9a-fA-F]{32}$').hasMatch(hex)) {
      return null;
    }
    final bytes = Uint8List(16);
    for (var i = 0; i < 16; i++) {
      final part = hex.substring(i * 2, i * 2 + 2);
      bytes[i] = int.parse(part, radix: 16);
    }
    return bytes;
  }

  Future<void> _drainUntilReady() async {
    while (true) {
      final msg = await _recvMessage();
      if (_handleAsyncMessage(msg)) {
        continue;
      }
      if (msg.header.type == MessageType.ready) {
        _applyTxnState(
          _readTxnId(msg.payload, fallback: msg.header.txnId, offset: 4),
        );
        _portalResumePending = false;
        return;
      }
      if (msg.header.type == MessageType.error) {
        throw _executionExceptionFromPayload(
          msg.payload,
          fallbackMessage: 'Describe failed',
        );
      }
    }
  }

  Future<void> _drainReadyAfterError() async {
    while (true) {
      final msg = await _recvMessage();
      if (_handleAsyncMessage(msg)) {
        continue;
      }
      if (msg.header.type == MessageType.ready) {
        _applyTxnState(
          _readTxnId(msg.payload, fallback: msg.header.txnId, offset: 4),
        );
        _portalResumePending = false;
        return;
      }
      if (msg.header.type == MessageType.error) {
        continue;
      }
    }
  }

  ScratchBirdException _authExceptionFromPayload(
    Uint8List payload, {
    required String fallbackMessage,
  }) {
    final parsed = parseErrorMessage(payload);
    final sqlState = parsed.sqlState.trim();
    return mapSqlStateAuthException(
      formatProtocolErrorMessage(parsed, fallbackMessage: fallbackMessage),
      sqlState: sqlState.isEmpty ? null : sqlState,
      code: parsed.code,
    );
  }

  ScratchBirdExecutionException _executionExceptionFromPayload(
    Uint8List payload, {
    required String fallbackMessage,
  }) {
    final parsed = parseErrorMessage(payload);
    final sqlState = parsed.sqlState.trim();
    return mapSqlStateExecutionException(
      formatProtocolErrorMessage(parsed, fallbackMessage: fallbackMessage),
      sqlState: sqlState.isEmpty ? null : sqlState,
      code: parsed.code,
    );
  }

  int _readTxnId(Uint8List payload,
      {required int fallback, required int offset}) {
    if (payload.length >= offset + 8) {
      return ByteData.sublistView(payload, offset, offset + 8)
          .getUint64(0, Endian.little);
    }
    return fallback;
  }

  bool _hasActiveTransaction() {
    return _transactionActive;
  }

  void _clearAbandonedSessionState() {
    _sequence = 0;
    _lastQuerySequence = null;
    _attachmentId = Uint8List(16);
    _txnId = 0;
    _transactionActive = false;
    _explicitTransaction = false;
    _portalResumePending = false;
    _parameters.clear();
    _lastPlan = null;
    _lastSblr = null;
  }

  void _applyTxnState(int txnId) {
    _txnId = txnId;
    if (txnId != 0) {
      _transactionActive = true;
    }
  }

  Future<void> _ensureImplicitTransaction() async {
    _transactionActive = true;
    _explicitTransaction = false;
  }

  void _ensureActiveTransaction(String operation) {
    if (!_hasActiveTransaction()) {
      throw ScratchBirdTransactionException(
        '$operation requires an active transaction',
      );
    }
  }

  void _validateSavepointName(String name) {
    if (name.trim().isEmpty) {
      throw ArgumentError.value(
          name, 'name', 'Savepoint name must not be empty');
    }
  }

  String _quoteStringLiteral(String value) {
    return "'${value.replaceAll("'", "''")}'";
  }

  String _buildPreparedTransactionSql(String verb, String globalTransactionId) {
    final normalized = globalTransactionId.trim();
    if (normalized.isEmpty) {
      throw const ScratchBirdProgrammingException(
        'Global transaction id must not be empty',
        sqlState: '42601',
      );
    }
    return '$verb ${_quoteStringLiteral(normalized)}';
  }

  void _allowPortalResume() {
    _portalResumePending = true;
  }

  Future<void> _resumeSuspendedPortal(int maxRows) async {
    if (!_portalResumePending) {
      throw const ScratchBirdExecutionException(
        'Portal resume requires an explicit suspended result state',
        sqlState: '55000',
      );
    }
    _portalResumePending = false;
    _lastQuerySequence = await _sendMessage(
      MessageType.execute,
      buildExecutePayload('', maxRows),
    );
    await _sendMessage(MessageType.sync, Uint8List(0));
  }

  String _quoteIdentifier(String value) {
    return '"${value.replaceAll('"', '""')}"';
  }

  Future<int> _sendMessage(int type, Uint8List payload,
      {int flags = 0, bool forceZero = false}) async {
    final sequence = _sequence;
    final header = MessageHeader(
      type: type,
      flags: flags,
      length: payload.length,
      sequence: sequence,
      attachmentId: forceZero ? Uint8List(16) : _attachmentId,
      txnId: forceZero ? 0 : _txnId,
    );
    _sequence += 1;
    final data = encodeMessage(header, payload);
    _socket!.add(data);
    await _socket!.flush();
    return sequence;
  }

  Future<ScratchBirdMessage> _recvMessage() async {
    final headerBytes = await _reader.readExact(headerSize);
    final header = decodeHeader(headerBytes);
    final payload = header.length == 0
        ? Uint8List(0)
        : await _reader.readExact(header.length);
    return ScratchBirdMessage(header, payload);
  }

  Future<void> _sendManagerFrame(int type, Uint8List payload) async {
    final header = ByteData(MANAGER_HEADER_SIZE);
    header.setUint32(0, MANAGER_PROTOCOL_MAGIC, Endian.little);
    header.setUint16(4, MANAGER_PROTOCOL_VERSION, Endian.little);
    header.setUint8(6, type);
    header.setUint8(7, 0);
    header.setUint32(8, payload.length, Endian.little);
    _socket!.add(Uint8List.fromList([
      ...header.buffer.asUint8List(),
      ...payload,
    ]));
    await _socket!.flush();
  }

  Future<({int type, Uint8List payload})> _recvManagerFrame() async {
    final header = await _reader.readExact(MANAGER_HEADER_SIZE);
    final data = ByteData.sublistView(header);
    final magic = data.getUint32(0, Endian.little);
    if (magic != MANAGER_PROTOCOL_MAGIC) {
      throw const ScratchBirdProtocolException('Manager frame magic mismatch');
    }
    final version = data.getUint16(4, Endian.little);
    if (version != MANAGER_PROTOCOL_VERSION) {
      throw const ScratchBirdProtocolException(
          'Manager frame version mismatch');
    }
    final type = data.getUint8(6);
    final length = data.getUint32(8, Endian.little);
    if (length > MANAGER_MAX_PAYLOAD_SIZE) {
      throw const ScratchBirdProtocolException('Manager payload too large');
    }
    final payload =
        length == 0 ? Uint8List(0) : await _reader.readExact(length);
    return (type: type, payload: payload);
  }

  List<ScratchBirdColumn> _parseRowDescription(Uint8List payload) {
    if (payload.length < 4) {
      throw const ScratchBirdProtocolException('Row description truncated');
    }
    final count =
        ByteData.sublistView(payload, 0, 2).getUint16(0, Endian.little);
    var offset = 4;
    final cols = <ScratchBirdColumn>[];
    for (var i = 0; i < count; i++) {
      if (offset + 4 > payload.length) {
        throw const ScratchBirdProtocolException('Row description truncated');
      }
      final nameLen = ByteData.sublistView(payload, offset, offset + 4)
          .getUint32(0, Endian.little);
      offset += 4;
      if (offset + nameLen + 14 > payload.length) {
        throw const ScratchBirdProtocolException('Row description truncated');
      }
      final name = utf8.decode(payload.sublist(offset, offset + nameLen));
      offset += nameLen;
      final tableOid = ByteData.sublistView(payload, offset, offset + 4)
          .getUint32(0, Endian.little);
      offset += 4;
      final columnIndex = ByteData.sublistView(payload, offset, offset + 2)
          .getUint16(0, Endian.little);
      offset += 2; // column index
      final typeOid = ByteData.sublistView(payload, offset, offset + 4)
          .getUint32(0, Endian.little);
      offset += 4;
      final typeSize = ByteData.sublistView(payload, offset, offset + 2)
          .getInt16(0, Endian.little);
      offset += 2; // type size
      final typeModifier = ByteData.sublistView(payload, offset, offset + 4)
          .getInt32(0, Endian.little);
      offset += 4; // type modifier
      final format = payload[offset];
      offset += 1;
      final nullable = payload[offset] != 0;
      offset += 1;
      offset += 2; // reserved
      cols.add(ScratchBirdColumn(name, typeOid, format,
          tableOid: tableOid,
          columnIndex: columnIndex,
          typeSize: typeSize,
          typeModifier: typeModifier,
          nullable: nullable));
    }
    return cols;
  }

  List<Uint8List?> _parseDataRow(Uint8List payload) {
    if (payload.length < 4) {
      throw const ScratchBirdProtocolException('Row data truncated');
    }
    final header = ByteData.sublistView(payload, 0, 4);
    final count = header.getUint16(0, Endian.little);
    final nullBytes = header.getUint16(2, Endian.little);
    var offset = 4;
    if (offset + nullBytes > payload.length) {
      throw const ScratchBirdProtocolException('Row data truncated');
    }
    final nullBitmap = payload.sublist(offset, offset + nullBytes);
    offset += nullBytes;
    final out = <Uint8List?>[];
    for (var i = 0; i < count; i++) {
      final byteIndex = i ~/ 8;
      final bitIndex = i % 8;
      final isNull = byteIndex < nullBitmap.length &&
          (nullBitmap[byteIndex] & (1 << bitIndex)) != 0;
      if (isNull) {
        out.add(null);
        continue;
      }
      if (offset + 4 > payload.length) {
        throw const ScratchBirdProtocolException('Row data truncated');
      }
      final len = ByteData.sublistView(payload, offset, offset + 4)
          .getInt32(0, Endian.little);
      offset += 4;
      if (len < 0) {
        out.add(null);
      } else {
        if (offset + len > payload.length) {
          throw const ScratchBirdProtocolException('Row data truncated');
        }
        out.add(payload.sublist(offset, offset + len));
        offset += len;
      }
    }
    return out;
  }

  _CStringResult _readCString(Uint8List buffer, int offset) {
    var idx = offset;
    while (idx < buffer.length && buffer[idx] != 0) {
      idx += 1;
    }
    final name = utf8.decode(buffer.sublist(offset, idx));
    return _CStringResult(name, idx + 1);
  }

  _NormalizedQuery _normalizeQuery(String sql, List<dynamic> params) {
    if (params.isEmpty || !sql.contains('?')) {
      return _NormalizedQuery(sql, List<dynamic>.from(params));
    }
    final out = StringBuffer();
    final ordered = <dynamic>[];
    var inString = false;
    var index = 0;
    for (var i = 0; i < sql.length; i++) {
      final ch = sql[i];
      if (ch == "'") {
        inString = !inString;
        out.write(ch);
        continue;
      }
      if (!inString && ch == '?') {
        if (index >= params.length) {
          throw const ScratchBirdProtocolException('Not enough parameters');
        }
        ordered.add(params[index]);
        index += 1;
        out.write('\$${ordered.length}');
        continue;
      }
      out.write(ch);
    }
    if (index < params.length) {
      throw const ScratchBirdProtocolException('Too many parameters');
    }
    return _NormalizedQuery(out.toString(), ordered);
  }

  void _startResilience() {
    _keepaliveManager.start();
    _keepaliveTracker = _keepaliveManager.register(_connectionId, () async {
      try {
        await ping();
        return true;
      } catch (_) {
        return false;
      }
    });
    _leakDetector.start();
    _leakGuard =
        _leakDetector.checkout(_connectionId, metadata: {'driver': 'dart'});
  }

  void _stopResilience() {
    if (_keepaliveTracker != null) {
      _keepaliveManager.unregister(_connectionId);
      _keepaliveTracker = null;
    }
    _keepaliveManager.stop();
    _leakGuard?.release();
    _leakGuard = null;
    _leakDetector.stop();
  }

  Future<void> _validateIfIdle() async {
    if (_keepaliveTracker != null && _keepaliveTracker!.needsValidation()) {
      await ping();
      _keepaliveTracker!.markActive();
    }
  }

  Future<T> _withResilience<T>(
      String operation, String? sql, Future<T> Function() body) async {
    if (!_circuitBreaker.allowRequest()) {
      throw const ScratchBirdExecutionException('Circuit breaker is OPEN');
    }
    await _validateIfIdle();
    final span = _telemetry.startSpan(operation);
    if (span != null && sql != null) {
      span.withAttribute('db.statement', TelemetryCollector.sanitizeQuery(sql));
    }
    try {
      final result = await body();
      _finishOperation(span, true);
      return result;
    } catch (e) {
      _finishOperation(span, false);
      rethrow;
    }
  }

  void _finishOperation(SpanContext? span, bool success) {
    if (success) {
      _circuitBreaker.recordSuccess();
      _keepaliveTracker?.markActive();
    } else {
      _circuitBreaker.recordFailure();
    }
    _telemetry.endSpan(span, success);
  }
}

class _CStringResult {
  final String item1;
  final int item2;
  _CStringResult(this.item1, this.item2);
}

class _NormalizedQuery {
  final String sql;
  final List<dynamic> params;
  _NormalizedQuery(this.sql, this.params);
}

class _SocketReader {
  final StreamIterator<Uint8List> iterator;
  final BytesBuilder _buffer = BytesBuilder();

  _SocketReader(this.iterator);

  Future<Uint8List> readExact(int length) async {
    while (_buffer.length < length) {
      if (!await iterator.moveNext()) {
        throw const ScratchBirdConnectionException('Socket closed');
      }
      _buffer.add(iterator.current);
    }
    final data = _buffer.takeBytes();
    final result = data.sublist(0, length);
    final rest = data.sublist(length);
    _buffer.add(rest);
    return result;
  }
}
