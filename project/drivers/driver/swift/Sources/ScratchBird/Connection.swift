// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

public struct ScratchBirdColumn {
    public let name: String
    public let tableOid: UInt32
    public let columnIndex: UInt16
    public let typeOid: UInt32
    public let typeSize: Int16
    public let typeModifier: Int32
    public let format: UInt16
    public let nullable: Bool

    public init(
        name: String,
        tableOid: UInt32 = 0,
        columnIndex: UInt16 = 0,
        typeOid: UInt32,
        typeSize: Int16 = 0,
        typeModifier: Int32 = 0,
        format: UInt16,
        nullable: Bool = false
    ) {
        self.name = name
        self.tableOid = tableOid
        self.columnIndex = columnIndex
        self.typeOid = typeOid
        self.typeSize = typeSize
        self.typeModifier = typeModifier
        self.format = format
        self.nullable = nullable
    }
}

public struct ScratchBirdResult {
    public let rows: [[Any?]]
    public let columns: [ScratchBirdColumn]
}

public struct NotificationMessage {
    public let processId: UInt32
    public let channel: String
    public let payload: Data
    public let changeType: String?
    public let rowId: UInt64?
}

public struct QueryPlanMessage {
    public let format: UInt32
    public let planningTimeUs: UInt64
    public let estimatedRows: UInt64
    public let estimatedCost: UInt64
    public let plan: Data
}

public struct SblrCompiledMessage {
    public let hash: UInt64
    public let version: UInt32
    public let bytecode: Data
}

public enum ScratchBirdReadCommittedMode: UInt8 {
    case defaultMode = 0
    case readConsistency = 1
    case recordVersion = 2
    case noRecordVersion = 3
}

struct ScratchBirdResilienceDebugStats {
    let keepaliveValidationAttempts: Int
    let keepaliveValidationSuccesses: Int
    let keepaliveValidationFailures: Int
    let keepaliveRegisteredConnections: Int
    let activeLeakCheckouts: Int
    let detectedLeaks: Int
}

private let managerProtocolMagic: UInt32 = 0x42444253 // SBDB
private let managerProtocolVersion: UInt16 = 0x0101
private let managerHeaderSize = 12
private let managerMaxPayloadSize: UInt32 = 16 * 1024 * 1024
private let mcpProtocolVersion: UInt16 = 0x0100

private let mcpMsgConnectResponse: UInt8 = 0x02
private let mcpMsgAuthChallenge: UInt8 = 0x12
private let mcpMsgAuthResponse: UInt8 = 0x11
private let mcpMsgStatusResponse: UInt8 = 0x64
private let mcpMsgHello: UInt8 = 0x65
private let mcpMsgAuthStart: UInt8 = 0x66
private let mcpMsgAuthContinue: UInt8 = 0x67
private let mcpMsgDbConnect: UInt8 = 0x69
private let mcpAuthMethodToken: UInt8 = 4

public final class ScratchBirdConnection {
    private let config: ScratchBirdConfig
    private let socket: ScratchBirdSocket
    private var sequence: UInt32 = 0
    private var lastQuerySequence: UInt32 = 0
    private var attachmentId = Data(repeating: 0, count: 16)
    private var txnId: UInt64 = 0
    private var transactionActive = false
    private var explicitTransaction = false
    private var portalResumePending = false
    private var parameters: [String: String] = [:]
    private var notificationHandlers: [(NotificationMessage) -> Void] = []
    private var lastPlan: QueryPlanMessage?
    private var lastSblr: SblrCompiledMessage?
    private let connectionId = UUID().uuidString
    private let circuitBreaker = CircuitBreaker()
    private let telemetry = TelemetryCollector()
    private let keepaliveManager: KeepaliveManager
    private var keepaliveTracker: KeepaliveTracker?
    private let leakDetector: LeakDetector
    private var leakGuard: LeakDetector.Guard?
    private let operationQueue = DispatchQueue(label: "scratchbird.connection.operation")
    private var resolvedAuthContext: ScratchBirdResolvedAuthContext

    private init(config: ScratchBirdConfig, socket: ScratchBirdSocket) {
        self.config = config
        self.socket = socket
        self.resolvedAuthContext = defaultResolvedAuthContext(config.frontDoorMode)
        self.keepaliveManager = KeepaliveManager(config: KeepaliveManager.Config(
            intervalMs: max(1, config.keepaliveIntervalMs),
            maxIdleBeforeCheckMs: max(0, config.keepaliveMaxIdleBeforeCheckMs),
            validationTimeoutMs: max(1, config.keepaliveValidationTimeoutMs)
        ))
        self.leakDetector = LeakDetector(config: LeakDetector.Config(
            thresholdMs: max(1, config.leakDetectionThresholdMs),
            captureStackTrace: config.leakDetectionCaptureStackTrace,
            checkIntervalMs: max(1, config.leakDetectionCheckIntervalMs)
        ))
    }

    private static func normalizedConfig(_ config: ScratchBirdConfig) throws -> ScratchBirdConfig {
        var normalized = config
        normalized.protocolName = try normalizeNativeProtocol(config.protocolName)
        normalized.frontDoorMode = try normalizeFrontDoorMode(config.frontDoorMode)
        normalized.sslmode = try normalizeSslMode(config.sslmode)
        return normalized
    }

    private static func connectTransport(_ config: ScratchBirdConfig) throws -> ScratchBirdSocket {
        let socket = ScratchBirdSocket()
        if let ipcPath = config.ipcPath?.trimmingCharacters(in: .whitespacesAndNewlines), !ipcPath.isEmpty {
            try socket.connectUnix(path: ipcPath)
            return socket
        }
        try socket.connect(
            host: config.host,
            port: config.port,
            tlsConfig: config.sslmode == "disable" ? nil : ScratchBirdTlsConfig(
                sslmode: config.sslmode,
                sslrootcert: config.sslrootcert,
                sslcert: config.sslcert,
                sslkey: config.sslkey,
                sslpassword: config.sslpassword
            )
        )
        return socket
    }

    private static func newBootstrapConnection(_ config: ScratchBirdConfig) throws -> ScratchBirdConnection {
        let normalized = try normalizedConfig(config)
        let socket = try connectTransport(normalized)
        return ScratchBirdConnection(config: normalized, socket: socket)
    }

    public static func connect(_ config: ScratchBirdConfig) async throws -> ScratchBirdConnection {
        return try await Task.detached { () -> ScratchBirdConnection in
            let normalizedConfig = try normalizedConfig(config)
            let socket = try connectTransport(normalizedConfig)
            let conn = ScratchBirdConnection(config: normalizedConfig, socket: socket)
            if normalizedConfig.frontDoorMode == "manager_proxy" {
                try conn.performManagerConnect()
            }
            try conn.handshake()
            try conn.ensureImplicitTransaction()
            conn.startResilience()
            return conn
        }.value
    }

    public static func probeAuthSurface(_ config: ScratchBirdConfig) async throws -> ScratchBirdAuthProbeResult {
        return try await Task.detached {
            let conn = try newBootstrapConnection(config)
            defer {
                conn.socket.close()
            }
            return try conn.probeAuthSurfaceInternal()
        }.value
    }

    public func getResolvedAuthContext() -> ScratchBirdResolvedAuthContext {
        resolvedAuthContext
    }

    static func debugCreateForTesting(config: ScratchBirdConfig) -> ScratchBirdConnection {
        ScratchBirdConnection(config: config, socket: ScratchBirdSocket())
    }

    func debugSeedAbandonedSessionStateForTesting(
        sequence: UInt32,
        lastQuerySequence: UInt32,
        attachmentId: Data,
        txnId: UInt64,
        transactionActive: Bool,
        explicitTransaction: Bool,
        parameters: [String: String],
        lastPlan: QueryPlanMessage?,
        lastSblr: SblrCompiledMessage?
    ) {
        self.sequence = sequence
        self.lastQuerySequence = lastQuerySequence
        self.attachmentId = attachmentId
        self.txnId = txnId
        self.transactionActive = transactionActive
        self.explicitTransaction = explicitTransaction
        self.parameters = parameters
        self.lastPlan = lastPlan
        self.lastSblr = lastSblr
    }

    func debugApplyRuntimeReadyStateForTesting(status: UInt8, txnId: UInt64) {
        applyRuntimeReadyState(status: status, txnId: txnId)
    }

    public func close() async throws {
        clearAbandonedSessionState()
        markResolvedAuthContextDetached()
        socket.close()
        stopResilience()
    }

    public func query(_ sql: String, _ params: [Any?] = []) async throws -> ScratchBirdResult {
        return try await Task.detached { () -> ScratchBirdResult in
            return try await self.withResilience(operation: "query", sql: sql) {
                if params.isEmpty {
                    try self.sendSimpleQuery(sql, maxRows: 0, timeoutMs: 0)
                } else {
                    try self.sendExtendedQuery(sql, params: params, maxRows: 0)
                }
                return try self.collectResults()
            }
        }.value
    }

    public func executeBatch(_ sql: String, _ paramsBatch: [[Any?]]) async throws -> [ScratchBirdResult] {
        var results: [ScratchBirdResult] = []
        results.reserveCapacity(paramsBatch.count)
        for params in paramsBatch {
            results.append(try await query(sql, params))
        }
        return results
    }

    public func queryMulti(_ statements: [String]) async throws -> [ScratchBirdResult] {
        let filtered = statements.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
        if filtered.isEmpty {
            return []
        }
        var results: [ScratchBirdResult] = []
        results.reserveCapacity(filtered.count)
        for sql in filtered {
            results.append(try await query(sql))
        }
        return results
    }

    public func executeReturningFirstColumn(_ sql: String, _ params: [Any?] = []) async throws -> Any? {
        let result = try await query(sql, params)
        if result.rows.isEmpty || result.rows[0].isEmpty {
            return nil
        }
        return result.rows[0][0]
    }

    public func metadataSchemas() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.schemasQuery)
    }

    public func metadataTables() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.tablesQuery)
    }

    public func metadataColumns() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.columnsQuery)
    }

    public func metadataIndexes() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.indexesQuery)
    }

    public func metadataIndexColumns() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.indexColumnsQuery)
    }

    public func metadataConstraints() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.constraintsQuery)
    }

    public func metadataProcedures() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.proceduresQuery)
    }

    public func metadataFunctions() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.functionsQuery)
    }

    public func metadataRoutines() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.routinesQuery)
    }

    public func metadataCatalogs() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.catalogsQuery)
    }

    public func metadataPrimaryKeys() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.primaryKeysQuery)
    }

    public func metadataForeignKeys() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.foreignKeysQuery)
    }

    public func metadataTablePrivileges() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.tablePrivilegesQuery)
    }

    public func metadataColumnPrivileges() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.columnPrivilegesQuery)
    }

    public func metadataTypeInfo() async throws -> ScratchBirdResult {
        return try await query(ScratchBirdMetadata.typeInfoQuery)
    }

    public func metadataSchemaTree(expandSchemaParents: Bool = false) async throws -> ScratchBirdMetadataSchemaTree {
        let schemas = try await metadataSchemas()
        let schemaNames = metadataSchemaNames(from: schemas)
        return buildMetadataSchemaTree(
            schemaNames,
            database: config.database,
            expandSchemaParents: expandSchemaParents
        )
    }

    public func metadataSchemaTreeRows(expandSchemaParents: Bool = false) async throws -> [ScratchBirdMetadataSchemaTreeRow] {
        let schemas = try await metadataSchemas()
        let schemaNames = metadataSchemaNames(from: schemas)
        return buildMetadataSchemaTreeRows(
            schemaNames,
            database: config.database,
            expandSchemaParents: expandSchemaParents
        )
    }

    func debugResilienceStats() -> ScratchBirdResilienceDebugStats {
        let keepalive = keepaliveManager.stats()
        let leak = leakDetector.stats()
        return ScratchBirdResilienceDebugStats(
            keepaliveValidationAttempts: keepalive.validationAttempts,
            keepaliveValidationSuccesses: keepalive.validationSuccesses,
            keepaliveValidationFailures: keepalive.validationFailures,
            keepaliveRegisteredConnections: keepalive.registeredConnections,
            activeLeakCheckouts: leak.activeCheckouts,
            detectedLeaks: leak.detectedLeaks
        )
    }

    public func onNotification(_ handler: @escaping (NotificationMessage) -> Void) {
        notificationHandlers.append(handler)
    }

    public func lastQueryPlan() -> QueryPlanMessage? {
        return lastPlan
    }

    public func lastSblrCompiled() -> SblrCompiledMessage? {
        return lastSblr
    }

    /// Public isolation aliases map onto the canonical MGA modes:
    /// READ COMMITTED => READ COMMITTED
    /// REPEATABLE READ => SNAPSHOT
    /// SERIALIZABLE => SNAPSHOT TABLE STABILITY
    /// readCommittedMode selects the canonical READ COMMITTED sub-mode.
    public func begin(
        isolationLevel: UInt8? = nil,
        readCommittedMode: ScratchBirdReadCommittedMode? = nil,
        accessMode: UInt8? = nil,
        deferrable: Bool? = nil,
        wait: Bool? = nil,
        timeoutMs: UInt32? = nil,
        autocommitMode: UInt8? = nil,
        conflictAction: UInt8 = 0
    ) async throws {
        try await Task.detached {
            try validateTxnBeginOptions(
                isolationLevel: isolationLevel,
                readCommittedMode: readCommittedMode,
                accessMode: accessMode,
                autocommitMode: autocommitMode
            )
            if self.hasActiveTransaction() {
                if self.explicitTransaction {
                    throw ScratchBirdTransactionException(
                        message: "Transaction already active",
                        sqlState: "25001"
                    )
                }
                if !self.canAdoptFreshNativeBoundary(
                    isolationLevel: isolationLevel,
                    readCommittedMode: readCommittedMode,
                    accessMode: accessMode,
                    deferrable: deferrable,
                    wait: wait,
                    timeoutMs: timeoutMs,
                    autocommitMode: autocommitMode,
                    conflictAction: conflictAction
                ) {
                    throw ScratchBirdNotSupportedException(
                        message: "fresh native MGA boundaries can only be adopted as default READ COMMITTED transactions on the live Swift lane",
                        sqlState: "0A000"
                    )
                }
                self.explicitTransaction = true
                return
            }
            try await self.withResilience(operation: "txn_begin") {
                var flags: UInt16 = 0
                var isolation = isolationLevel ?? isolationReadCommitted
                if isolationLevel != nil { flags |= txnFlagHasIsolation }
                if readCommittedMode != nil {
                    if isolationLevel == nil {
                        isolation = isolationReadCommitted
                        flags |= txnFlagHasIsolation
                    }
                    flags |= txnFlagHasReadCommittedMode
                }
                if accessMode != nil { flags |= txnFlagHasAccess }
                if deferrable != nil { flags |= txnFlagHasDeferrable }
                if wait != nil { flags |= txnFlagHasWait }
                if timeoutMs != nil { flags |= txnFlagHasTimeout }
                if autocommitMode != nil { flags |= txnFlagHasAutocommit }
                let payload = buildTxnBeginPayload(
                    flags: flags,
                    conflictAction: conflictAction,
                    autocommitMode: autocommitMode ?? 0,
                    isolationLevel: isolation,
                    accessMode: accessMode ?? 0,
                    deferrable: deferrable == true ? 1 : 0,
                    waitMode: wait == true ? 1 : 0,
                    timeoutMs: timeoutMs ?? 0,
                    readCommittedMode: readCommittedMode?.rawValue ?? readCommittedModeDefault
                )
                _ = try self.sendMessage(type: .txnBegin, payload: payload)
                _ = try self.drainUntilReady()
            }
            self.explicitTransaction = true
        }.value
    }

    public func supportsPreparedTransactions() -> Bool {
        true
    }

    public func prepareTransaction(_ globalTransactionId: String) async throws {
        _ = try await query(try buildPreparedTransactionSql(
            verb: "PREPARE TRANSACTION",
            globalTransactionId: globalTransactionId
        ))
    }

    public func commitPrepared(_ globalTransactionId: String) async throws {
        _ = try await query(try buildPreparedTransactionSql(
            verb: "COMMIT PREPARED",
            globalTransactionId: globalTransactionId
        ))
    }

    public func rollbackPrepared(_ globalTransactionId: String) async throws {
        _ = try await query(try buildPreparedTransactionSql(
            verb: "ROLLBACK PREPARED",
            globalTransactionId: globalTransactionId
        ))
    }

    public func supportsDormantReattach() -> Bool {
        false
    }

    public func detachToDormant() async throws {
        throw ScratchBirdNotSupportedException(
            message: "Dormant detach is not supported by the current public front door",
            sqlState: "0A000"
        )
    }

    public func reattachDormant(_ dormantId: String, authToken: String? = nil) async throws {
        _ = dormantId
        _ = authToken
        throw ScratchBirdNotSupportedException(
            message: "Dormant reattach is not supported by the current public front door",
            sqlState: "0A000"
        )
    }

    public func commit(flags: UInt8 = 0) async throws {
        try await Task.detached {
            try self.requireActiveTransaction("commit")
            try await self.withResilience(operation: "txn_commit") {
                try self.sendSimpleQuery("COMMIT", maxRows: 0, timeoutMs: 0)
                _ = try self.collectResults()
            }
            self.explicitTransaction = false
            try self.drainImmediateReopenBoundary()
        }.value
    }

    public func rollback(flags: UInt8 = 0) async throws {
        try await Task.detached {
            try self.requireActiveTransaction("rollback")
            try await self.withResilience(operation: "txn_rollback") {
                try self.sendSimpleQuery("ROLLBACK", maxRows: 0, timeoutMs: 0)
                _ = try self.collectResults()
            }
            self.explicitTransaction = false
            try self.drainImmediateReopenBoundary()
        }.value
    }

    public func savepoint(_ name: String) async throws {
        let normalizedName = try normalizeSavepointName(name)
        try await Task.detached {
            try self.requireActiveTransaction("savepoint")
            try await self.withResilience(operation: "txn_savepoint") {
                try self.sendSimpleQuery("SAVEPOINT \(self.quoteIdentifier(normalizedName))", maxRows: 0, timeoutMs: 0)
                _ = try self.collectResults()
            }
        }.value
    }

    public func releaseSavepoint(_ name: String) async throws {
        let normalizedName = try normalizeSavepointName(name)
        try await Task.detached {
            try self.requireActiveTransaction("release savepoint")
            try await self.withResilience(operation: "txn_release") {
                try self.sendSimpleQuery("RELEASE SAVEPOINT \(self.quoteIdentifier(normalizedName))", maxRows: 0, timeoutMs: 0)
                _ = try self.collectResults()
            }
        }.value
    }

    public func rollbackToSavepoint(_ name: String) async throws {
        let normalizedName = try normalizeSavepointName(name)
        try await Task.detached {
            try self.requireActiveTransaction("rollback to savepoint")
            try await self.withResilience(operation: "txn_rollback_to") {
                try self.sendSimpleQuery("ROLLBACK TO SAVEPOINT \(self.quoteIdentifier(normalizedName))", maxRows: 0, timeoutMs: 0)
                _ = try self.collectResults()
            }
        }.value
    }

    public func setOption(_ name: String, value: String) async throws {
        try await Task.detached {
            try await self.withResilience(operation: "set_option") {
                _ = try self.sendMessage(type: .setOption, payload: buildSetOptionPayload(name: name, value: value))
                _ = try self.drainUntilReady()
            }
        }.value
    }

    public func ping() async throws {
        try await Task.detached {
            try self.operationQueue.sync {
                _ = try self.sendMessage(type: .ping, payload: Data())
                while true {
                    let msg = try self.recvMessage()
                    if self.handleAsyncMessage(msg) {
                        continue
                    }
                    if msg.header.type == .pong {
                        return
                    }
                    if msg.header.type == .ready {
                        let ready = self.parseReadyState(msg.payload, fallback: msg.header.txnId)
                        self.applyRuntimeReadyState(status: ready.status, txnId: ready.txnId)
                        self.portalResumePending = false
                        return
                    }
                    if msg.header.type == .error {
                        throw buildScratchBirdError(
                            from: msg.payload,
                            fallbackMessage: "Ping failed",
                            defaultSqlState: "08006"
                        )
                    }
                }
            }
        }.value
    }

    public func terminate() async throws {
        try await Task.detached {
            _ = try self.sendMessage(type: .terminate, payload: Data())
            self.markResolvedAuthContextDetached()
            self.socket.close()
        }.value
    }

    public func subscribe(_ channel: String, subscribeType: UInt8 = 0, filterExpr: String = "") async throws {
        try await Task.detached {
            _ = try self.sendMessage(type: .subscribe, payload: buildSubscribePayload(subscribeType: subscribeType, channel: channel, filterExpr: filterExpr))
            _ = try self.drainUntilReady()
        }.value
    }

    public func unsubscribe(_ channel: String) async throws {
        try await Task.detached {
            _ = try self.sendMessage(type: .unsubscribe, payload: buildUnsubscribePayload(channel: channel))
            _ = try self.drainUntilReady()
        }.value
    }

    public func executeSblr(_ sblrHash: UInt64, bytecode: Data, params: [Any?] = []) async throws -> ScratchBirdResult {
        return try await Task.detached { () -> ScratchBirdResult in
            let encoded = try params.map { try encodeParam($0) }
            let paramValues = encoded.map { $0.param }
            let payload = buildSblrExecutePayload(sblrHash: sblrHash, sblrBytecode: bytecode, params: paramValues)
            self.portalResumePending = false
            self.lastPlan = nil
            self.lastSblr = nil
            self.lastQuerySequence = try self.sendMessage(type: .sblrExecute, payload: payload)
            _ = try self.sendMessage(type: .sync, payload: Data())
            return try self.collectResults()
        }.value
    }

    public func streamControl(controlType: UInt8, windowSize: UInt32 = 0, timeoutMs: UInt32 = 0) async throws {
        try await Task.detached {
            _ = try self.sendMessage(type: .streamControl, payload: buildStreamControlPayload(controlType: controlType, windowSize: windowSize, timeoutMs: timeoutMs))
        }.value
    }

    public func attachCreate(emulationMode: String, dbName: String) async throws {
        try await Task.detached {
            _ = try self.sendMessage(type: .attachCreate, payload: buildAttachCreatePayload(emulationMode: emulationMode, dbName: dbName))
            _ = try self.drainUntilReady()
        }.value
    }

    public func attachDetach() async throws {
        try await Task.detached {
            _ = try self.sendMessage(type: .attachDetach, payload: Data())
            _ = try self.drainUntilReady()
        }.value
    }

    public func attachList() async throws -> ScratchBirdResult {
        return try await Task.detached { () -> ScratchBirdResult in
            _ = try self.sendMessage(type: .attachList, payload: Data())
            _ = try self.sendMessage(type: .sync, payload: Data())
            return try self.collectResults()
        }.value
    }

    public func cancel() async throws {
        try await Task.detached {
            self.portalResumePending = false
            let targetSequence = try requireCancelableSequence(self.lastQuerySequence)
            let payload = buildCancelPayload(cancelType: 0, targetSequence: targetSequence)
            _ = try self.sendMessage(type: .cancel, payload: payload, flags: messageFlagUrgent)
        }.value
    }

    private func resetResolvedAuthContext() {
        resolvedAuthContext = defaultResolvedAuthContext(config.frontDoorMode)
    }

    private func markResolvedAuthContextDetached() {
        resolvedAuthContext = ScratchBirdResolvedAuthContext(
            ingressMode: resolvedAuthContext.ingressMode,
            resolvedAuthMethod: resolvedAuthContext.resolvedAuthMethod,
            resolvedAuthPluginId: resolvedAuthContext.resolvedAuthPluginId,
            managerAuthenticated: resolvedAuthContext.managerAuthenticated,
            attached: false
        )
    }

    private func buildProbeResult(method: UInt8) -> ScratchBirdAuthProbeResult {
        let admitted: [ScratchBirdAuthMethodSurface]
        if method == authOkMethod {
            admitted = []
        } else {
            admitted = describeAuthMethod(method, configuredMethodId: config.authMethodId).map { [$0] } ?? []
        }
        return ScratchBirdAuthProbeResult(
            reachable: true,
            ingressMode: config.frontDoorMode,
            resolvedHost: config.host,
            resolvedPort: config.port,
            admittedMethods: admitted,
            requiredMethod: authMethodName(method).isEmpty ? nil : authMethodName(method),
            requiredPluginMethodId: {
                let pluginId = authPluginIdForMethod(method, configuredMethodId: config.authMethodId)
                return pluginId.isEmpty ? nil : pluginId
            }(),
            allowedTransportMask: nil,
            additionalContinuationPossible: additionalContinuationPossible(method)
        )
    }

    private func buildStartupFeatures() -> UInt64 {
        var features: UInt64 = 0
        if config.compression.lowercased() == "zstd" {
            features |= featureCompression
        }
        if config.binaryTransfer {
            features |= featureStreaming
        }
        return features
    }

    private func buildStartupParams(requireIdentity: Bool) throws -> [String: String] {
        var params: [String: String] = [
            "client_flags": String(config.connectClientFlags)
        ]
        if requireIdentity || !config.database.isEmpty {
            params["database"] = config.database
        }
        if requireIdentity || !config.user.isEmpty {
            params["user"] = config.user
        }
        if let role = config.role, !role.isEmpty {
            params["role"] = role
        }
        if let app = config.applicationName, !app.isEmpty {
            params["application_name"] = app
        }
        try applyAuthPluginSelection(params: &params, config: config)
        return params
    }

    private func resolveTokenAuthPayload() throws -> Data {
        if let authToken = config.authToken, !authToken.isEmpty {
            return Data(authToken.utf8)
        }
        if let managerToken = config.managerAuthToken, !managerToken.isEmpty, config.frontDoorMode == "manager_proxy" {
            return Data(managerToken.utf8)
        }
        throw ScratchBirdAuthorizationException(
            message: "TOKEN authentication requires auth_token",
            sqlState: "28000"
        )
    }

    private func probeAuthSurfaceInternal() throws -> ScratchBirdAuthProbeResult {
        resetResolvedAuthContext()
        if config.frontDoorMode == "manager_proxy" {
            return try probeManagerAuthSurface()
        }
        return try probeDirectAuthSurface()
    }

    private func probeManagerAuthSurface() throws -> ScratchBirdAuthProbeResult {
        let managerUser: String
        if let configured = config.managerUsername, !configured.isEmpty {
            managerUser = configured
        } else if !config.user.isEmpty {
            managerUser = config.user
        } else {
            managerUser = "admin"
        }

        var helloPayload = Data()
        helloPayload.append(contentsOf: withUnsafeBytes(of: mcpProtocolVersion.littleEndian, Array.init))
        helloPayload.append(contentsOf: withUnsafeBytes(of: UInt16(config.managerClientFlags & 0xFFFF).littleEndian, Array.init))
        try sendManagerFrame(type: mcpMsgHello, payload: helloPayload)

        var frame = try recvManagerFrame()
        if frame.0 != mcpMsgStatusResponse {
            throw ScratchBirdConnectionException(
                message: "Expected MCP hello status response",
                sqlState: "08P01"
            )
        }

        var authStart = Data()
        authStart.append(buildLengthPrefixedString(managerUser))
        authStart.append(mcpAuthMethodToken)
        authStart.append(contentsOf: [0, 0, 0, 0])
        try sendManagerFrame(type: mcpMsgAuthStart, payload: authStart)
        frame = try recvManagerFrame()

        switch frame.0 {
        case mcpMsgAuthChallenge, mcpMsgAuthResponse:
            return buildProbeResult(method: authTokenMethod)
        case mcpMsgStatusResponse:
            return buildProbeResult(method: authOkMethod)
        default:
            throw ScratchBirdConnectionException(
                message: "Expected MCP auth challenge or auth response",
                sqlState: "08P01"
            )
        }
    }

    private func probeDirectAuthSurface() throws -> ScratchBirdAuthProbeResult {
        let startup = buildStartupPayload(features: buildStartupFeatures(), params: try buildStartupParams(requireIdentity: false))
        _ = try sendMessage(type: .startup, payload: startup, forceZero: true)

        while true {
            let msg = try recvMessage()
            if msg.header.type == .negotiateVersion || msg.header.type == .parameterStatus {
                continue
            }
            if msg.header.type == .authRequest {
                let parsed = try parseAuthRequest(msg.payload)
                return buildProbeResult(method: parsed.method)
            }
            if msg.header.type == .authOk || msg.header.type == .ready {
                return buildProbeResult(method: authOkMethod)
            }
            if msg.header.type == .error {
                throw buildScratchBirdError(
                    from: msg.payload,
                    fallbackMessage: "Authentication probe failed",
                    defaultSqlState: "28000"
                )
            }
        }
    }

    private func handshake() throws {
        resetResolvedAuthContext()
        let startup = buildStartupPayload(features: buildStartupFeatures(), params: try buildStartupParams(requireIdentity: true))
        _ = try sendMessage(type: .startup, payload: startup, forceZero: true)

        var scram: ScramClient? = nil
        var activeAuthMethod = authOkMethod
        while true {
            let msg = try recvMessage()
            switch msg.header.type {
            case .negotiateVersion:
                continue
            case .authRequest:
                let parsed = try parseAuthRequest(msg.payload)
                let method = parsed.method
                activeAuthMethod = method
                resolvedAuthContext = ScratchBirdResolvedAuthContext(
                    ingressMode: resolvedAuthContext.ingressMode,
                    resolvedAuthMethod: authMethodName(method),
                    resolvedAuthPluginId: authPluginIdForMethod(method, configuredMethodId: config.authMethodId),
                    managerAuthenticated: resolvedAuthContext.managerAuthenticated,
                    attached: resolvedAuthContext.attached
                )
                if method == authOkMethod {
                    continue
                }
                if method == authPasswordMethod {
                    let password = config.password ?? ""
                    _ = try sendMessage(type: .authResponse, payload: Data(password.utf8), forceZero: true)
                } else if method == authScramSha256Method || method == authScramSha512Method {
                    scram = scram ?? ScramClient(
                        username: config.user,
                        algorithm: method == authScramSha512Method ? .sha512 : .sha256
                    )
                    let first = scram!.clientFirstMessage()
                    _ = try sendMessage(type: .authResponse, payload: Data(first.utf8), forceZero: true)
                } else if method == authTokenMethod {
                    _ = try sendMessage(type: .authResponse, payload: try resolveTokenAuthPayload(), forceZero: true)
                } else if method == authMd5Method || method == authPeerMethod || method == authReattachMethod {
                    throw ScratchBirdNotSupportedException(
                        message: "\(authMethodName(method)) authentication is not supported by the Swift driver",
                        sqlState: "0A000"
                    )
                }
            case .authContinue:
                let parsed = try parseAuthContinue(msg.payload)
                let method = parsed.method
                if (method == authScramSha256Method || method == authScramSha512Method), let scram = scram {
                    let serverFirst = String(data: parsed.data, encoding: .utf8) ?? ""
                    let final = try scram.handleServerFirst(password: config.password ?? "", serverFirst: serverFirst)
                    _ = try sendMessage(type: .authResponse, payload: Data(final.utf8), forceZero: true)
                } else if method == authTokenMethod {
                    _ = try sendMessage(type: .authResponse, payload: try resolveTokenAuthPayload(), forceZero: true)
                } else {
                    throw ScratchBirdAuthorizationException(
                        message: "Unsupported auth continue",
                        sqlState: "28000"
                    )
                }
            case .authOk:
                let parsed = try parseAuthOk(msg.payload)
                attachmentId = msg.header.attachmentId
                applyRuntimeTxnId(msg.header.txnId)
                if activeAuthMethod == authOkMethod {
                    resolvedAuthContext = ScratchBirdResolvedAuthContext(
                        ingressMode: resolvedAuthContext.ingressMode,
                        resolvedAuthMethod: authMethodName(authOkMethod),
                        resolvedAuthPluginId: authPluginIdForMethod(authOkMethod, configuredMethodId: config.authMethodId),
                        managerAuthenticated: resolvedAuthContext.managerAuthenticated,
                        attached: resolvedAuthContext.attached
                    )
                }
                if let scram, !parsed.serverInfo.isEmpty {
                    let serverInfo = String(data: parsed.serverInfo, encoding: .utf8) ?? ""
                    if serverInfo.hasPrefix("v=") {
                        try scram.verifyServerFinal(serverInfo)
                    }
                }
            case .parameterStatus:
                handleParameterStatus(msg.payload)
            case .ready:
                let ready = parseReadyState(msg.payload, fallback: msg.header.txnId)
                applyRuntimeReadyState(status: ready.status, txnId: ready.txnId)
                resolvedAuthContext = ScratchBirdResolvedAuthContext(
                    ingressMode: resolvedAuthContext.ingressMode,
                    resolvedAuthMethod: resolvedAuthContext.resolvedAuthMethod,
                    resolvedAuthPluginId: resolvedAuthContext.resolvedAuthPluginId,
                    managerAuthenticated: resolvedAuthContext.managerAuthenticated,
                    attached: true
                )
                return
            case .error:
                throw buildScratchBirdError(
                    from: msg.payload,
                    fallbackMessage: "Authentication failed",
                    defaultSqlState: "28000"
                )
            default:
                continue
            }
        }
    }

    private func collectResults() throws -> ScratchBirdResult {
        var columns: [ScratchBirdColumn] = []
        var rows: [[Any?]] = []
        while true {
            let msg = try recvMessage()
            if handleAsyncMessage(msg) {
                continue
            }
            switch msg.header.type {
            case .rowDescription:
                columns = parseRowDescription(msg.payload)
            case .dataRow:
                let values = parseDataRow(msg.payload)
                let decoded = values.enumerated().map { idx, value -> Any? in
                    guard let value = value else { return nil }
                    return decodeValue(oid: columns[idx].typeOid, data: value, format: columns[idx].format)
                }
                rows.append(decoded)
            case .ready:
                let ready = parseReadyState(msg.payload, fallback: msg.header.txnId)
                applyRuntimeReadyState(status: ready.status, txnId: ready.txnId)
                portalResumePending = false
                lastQuerySequence = 0
                return ScratchBirdResult(rows: rows, columns: columns)
            case .error:
                let error = buildScratchBirdError(
                    from: msg.payload,
                    fallbackMessage: "Query failed",
                    defaultSqlState: "42000"
                )
                portalResumePending = false
                try drainReadyAfterError()
                lastQuerySequence = 0
                throw error
            case .portalSuspended:
                let resumeMaxRows = normalizePortalResumeMaxRows(fetchSize: config.fetchSize)
                allowPortalResume()
                try resumeSuspendedPortal(resumeMaxRows)
            default:
                continue
            }
        }
    }

    private func sendSimpleQuery(_ sql: String, maxRows: UInt32, timeoutMs: UInt32) throws {
        let flags: UInt32 = config.binaryTransfer ? queryFlagBinaryResult : 0
        portalResumePending = false
        lastPlan = nil
        lastSblr = nil
        lastQuerySequence = try sendMessage(
            type: .query,
            payload: buildQueryPayload(sql: sql, flags: flags, maxRows: maxRows, timeoutMs: timeoutMs)
        )
    }

    private func sendExtendedQuery(_ sql: String, params: [Any?], maxRows: UInt32) throws {
        let normalized = try normalizeQuery(sql, params: params)
        let encoded = try normalized.params.map { try encodeParam($0) }
        let paramValues = encoded.map { $0.param }
        let paramTypes = encoded.map { $0.oid }
        _ = try sendMessage(type: .parse, payload: buildParsePayload(statement: "", sql: normalized.sql, paramTypes: paramTypes))
        _ = try sendMessage(type: .describe, payload: buildDescribePayload(kind: "S".utf8.first ?? 83, name: ""))
        _ = try sendMessage(type: .sync, payload: Data())
        _ = try drainUntilReady()
        _ = try sendMessage(type: .bind, payload: buildBindPayload(portal: "", statement: "", params: paramValues, resultFormats: [1]))
        portalResumePending = false
        lastPlan = nil
        lastSblr = nil
        lastQuerySequence = try sendMessage(type: .execute, payload: buildExecutePayload(portal: "", maxRows: maxRows))
        _ = try sendMessage(type: .sync, payload: Data())
    }

    private func handleAsyncMessage(_ msg: ProtocolMessage) -> Bool {
        switch msg.header.type {
        case .parameterStatus:
            handleParameterStatus(msg.payload)
            return true
        case .notification:
            if let notice = parseNotification(msg.payload) {
                for handler in notificationHandlers {
                    handler(notice)
                }
            }
            return true
        case .queryPlan:
            if let plan = parseQueryPlan(msg.payload) {
                lastPlan = plan
            }
            return true
        case .sblrCompiled:
            if let compiled = parseSblrCompiled(msg.payload) {
                lastSblr = compiled
            }
            return true
        case .txnStatus:
            let status = parseTxnStatus(msg.payload, fallback: msg.header.txnId)
            if status.status == Character("T").asciiValue {
                applyRuntimeTxnId(status.txnId)
                transactionActive = true
            } else {
                clearTransactionState()
            }
            return true
        default:
            return false
        }
    }

    private func startResilience() {
        keepaliveManager.start()
        keepaliveTracker = keepaliveManager.register(connectionId: connectionId) { [weak self] in
            guard let self else { return false }
            do {
                try await self.ping()
                return true
            } catch {
                return false
            }
        }
        leakDetector.start()
        leakGuard = leakDetector.checkout(connectionId: connectionId, metadata: ["driver": "swift"])
    }

    private func stopResilience() {
        if let _ = keepaliveTracker {
            keepaliveManager.unregister(connectionId: connectionId)
            keepaliveTracker = nil
        }
        keepaliveManager.stop()
        if let leakGuard {
            leakGuard.release()
            self.leakGuard = nil
        }
        leakDetector.stop()
    }

    private func validateIfIdle() async throws {
        if let keepaliveTracker, keepaliveTracker.needsValidation() {
            try await ping()
            keepaliveTracker.markActive()
        }
    }

    private func withResilience<T>(operation: String, sql: String? = nil, _ body: () throws -> T) async throws -> T {
        if !circuitBreaker.allowRequest() {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Circuit breaker is OPEN"])
        }
        try await validateIfIdle()
        let span = telemetry.startSpan(operation)
        if let span, let sql {
            _ = span.withAttribute("db.statement", TelemetryCollector.sanitizeQuery(sql))
        }
        do {
            let result = try operationQueue.sync {
                try body()
            }
            finishOperation(span: span, success: true)
            return result
        } catch {
            finishOperation(span: span, success: false)
            throw error
        }
    }

    private func finishOperation(span: SpanContext?, success: Bool) {
        if success {
            circuitBreaker.recordSuccess()
            keepaliveTracker?.markActive()
        } else {
            circuitBreaker.recordFailure()
        }
        telemetry.endSpan(span, success: success)
    }

    private func handleParameterStatus(_ payload: Data) {
        if payload.count < 8 { return }
        let nameLen = Int(readUInt32LE(payload, 0))
        if 4 + nameLen + 4 > payload.count { return }
        let name = String(data: payload.subdata(in: 4..<(4 + nameLen)), encoding: .utf8) ?? ""
        let valueLen = Int(readUInt32LE(payload, 4 + nameLen))
        let valueStart = 8 + nameLen
        if valueStart + valueLen > payload.count { return }
        let value = String(data: payload.subdata(in: valueStart..<(valueStart + valueLen)), encoding: .utf8) ?? ""
        parameters[name] = value
        if name == "attachment_id", let parsed = parseUuidBytes(value) {
            attachmentId = parsed
        }
        if name == "current_txn_id", let parsed = UInt64(value.trimmingCharacters(in: .whitespaces)) {
            applyRuntimeTxnId(parsed)
        }
    }

    private func parseNotification(_ payload: Data) -> NotificationMessage? {
        if payload.count < 12 { return nil }
        var offset = 0
        let processId = readUInt32LE(payload, offset)
        offset += 4
        let channelLen = Int(readUInt32LE(payload, offset))
        offset += 4
        if offset + channelLen + 4 > payload.count { return nil }
        let channel = String(data: payload.subdata(in: offset..<(offset + channelLen)), encoding: .utf8) ?? ""
        offset += channelLen
        let payloadLen = Int(readUInt32LE(payload, offset))
        offset += 4
        if offset + payloadLen > payload.count { return nil }
        let data = payload.subdata(in: offset..<(offset + payloadLen))
        offset += payloadLen
        var changeType: String?
        var rowId: UInt64?
        if offset < payload.count {
            changeType = String(bytes: [payload[offset]], encoding: .utf8)
            offset += 1
            if offset + 8 <= payload.count {
                rowId = readUInt64LE(payload, offset)
            }
        }
        return NotificationMessage(processId: processId, channel: channel, payload: data, changeType: changeType, rowId: rowId)
    }

    private func parseQueryPlan(_ payload: Data) -> QueryPlanMessage? {
        if payload.count < 32 { return nil }
        let format = readUInt32LE(payload, 0)
        let planLen = Int(readUInt32LE(payload, 4))
        let planningTimeUs = readUInt64LE(payload, 8)
        let estimatedRows = readUInt64LE(payload, 16)
        let estimatedCost = readUInt64LE(payload, 24)
        if 32 + planLen > payload.count { return nil }
        let plan = payload.subdata(in: 32..<(32 + planLen))
        return QueryPlanMessage(
            format: format,
            planningTimeUs: planningTimeUs,
            estimatedRows: estimatedRows,
            estimatedCost: estimatedCost,
            plan: plan
        )
    }

    private func parseSblrCompiled(_ payload: Data) -> SblrCompiledMessage? {
        if payload.count < 16 { return nil }
        let hash = readUInt64LE(payload, 0)
        let version = readUInt32LE(payload, 8)
        let length = Int(readUInt32LE(payload, 12))
        if 16 + length > payload.count { return nil }
        let bytecode = payload.subdata(in: 16..<(16 + length))
        return SblrCompiledMessage(hash: hash, version: version, bytecode: bytecode)
    }

    private func parseUuidBytes(_ value: String) -> Data? {
        let hex = value.replacingOccurrences(of: "-", with: "").trimmingCharacters(in: .whitespacesAndNewlines)
        let pattern = "^[0-9A-Fa-f]{32}$"
        if hex.range(of: pattern, options: .regularExpression) == nil {
            return nil
        }
        var data = Data()
        var index = hex.startIndex
        for _ in 0..<16 {
            let next = hex.index(index, offsetBy: 2)
            let byteString = String(hex[index..<next])
            if let byte = UInt8(byteString, radix: 16) {
                data.append(byte)
            } else {
                return nil
            }
            index = next
        }
        return data
    }

    private func hasActiveTransaction() -> Bool {
        return transactionActive
    }

    private func clearTransactionState() {
        txnId = 0
        transactionActive = false
    }

    private func clearAbandonedSessionState() {
        sequence = 0
        lastQuerySequence = 0
        attachmentId = Data(repeating: 0, count: 16)
        clearTransactionState()
        explicitTransaction = false
        portalResumePending = false
        parameters.removeAll()
        lastPlan = nil
        lastSblr = nil
    }

    func allowPortalResume() {
        portalResumePending = true
    }

    func resumeSuspendedPortal(_ maxRows: UInt32) throws {
        guard portalResumePending else {
            throw ScratchBirdOperationalException(
                message: "Portal resume requires an explicit suspended result state",
                sqlState: "55000"
            )
        }
        portalResumePending = false
        lastQuerySequence = try sendMessage(
            type: .execute,
            payload: buildExecutePayload(portal: "", maxRows: maxRows)
        )
        try sendMessage(type: .sync, payload: Data())
    }

    private func applyRuntimeTxnId(_ txnId: UInt64) {
        self.txnId = txnId
        if txnId != 0 {
            transactionActive = true
        }
    }

    private func applyRuntimeReadyState(status: UInt8, txnId: UInt64) {
        if status != 0 {
            self.txnId = txnId
            transactionActive = true
        } else {
            clearTransactionState()
        }
    }

    private func canAdoptFreshNativeBoundary(
        isolationLevel: UInt8?,
        readCommittedMode: ScratchBirdReadCommittedMode?,
        accessMode: UInt8?,
        deferrable: Bool?,
        wait: Bool?,
        timeoutMs: UInt32?,
        autocommitMode: UInt8?,
        conflictAction: UInt8
    ) -> Bool {
        return (isolationLevel == nil || isolationLevel == isolationReadCommitted)
            && (readCommittedMode == nil || readCommittedMode == .defaultMode)
            && accessMode == nil
            && deferrable == nil
            && wait == nil
            && timeoutMs == nil
            && autocommitMode == nil
            && conflictAction == 0
    }

    private func requireActiveTransaction(_ operation: String) throws {
        if hasActiveTransaction() {
            return
        }
        throw ScratchBirdTransactionException(
            message: "\(operation) requires an active transaction",
            sqlState: "25000"
        )
    }

    private func ensureImplicitTransaction() throws {
        transactionActive = true
        explicitTransaction = false
    }

    private func parseReadyState(_ payload: Data, fallback: UInt64) -> (status: UInt8, txnId: UInt64) {
        if payload.count >= 20 {
            return (payload[0], readUInt64LE(payload, 4))
        }
        return (fallback == 0 ? 0 : Character("T").asciiValue ?? 0, fallback)
    }

    private func parseTxnStatus(_ payload: Data, fallback: UInt64) -> (status: UInt8, txnId: UInt64) {
        if payload.count >= 12 {
            return (payload[0], readUInt64LE(payload, 4))
        }
        return (fallback == 0 ? 0 : Character("T").asciiValue ?? 0, fallback)
    }

    private func drainImmediateReopenBoundary() throws {
        while socket.hasPendingData() {
            let msg = try recvMessage()
            if handleAsyncMessage(msg) {
                continue
            }
            if msg.header.type == .ready {
                let ready = parseReadyState(msg.payload, fallback: msg.header.txnId)
                applyRuntimeReadyState(status: ready.status, txnId: ready.txnId)
                portalResumePending = false
                continue
            }
            if msg.header.type == .error {
                throw buildScratchBirdError(
                    from: msg.payload,
                    fallbackMessage: "Request failed",
                    defaultSqlState: "42000"
                )
            }
            return
        }
    }

    private func quoteIdentifier(_ value: String) -> String {
        let escaped = value.replacingOccurrences(of: "\"", with: "\"\"")
        return "\"\(escaped)\""
    }

    private func readUInt32LE(_ data: Data, _ offset: Int) -> UInt32 {
        let slice = data.subdata(in: offset..<(offset + 4))
        return UInt32(littleEndian: slice.withUnsafeBytes { $0.load(as: UInt32.self) })
    }

    private func readUInt64LE(_ data: Data, _ offset: Int) -> UInt64 {
        let slice = data.subdata(in: offset..<(offset + 8))
        return UInt64(littleEndian: slice.withUnsafeBytes { $0.load(as: UInt64.self) })
    }

    private func buildLengthPrefixedString(_ value: String) -> Data {
        let bytes = Data(value.utf8)
        var data = Data()
        data.append(contentsOf: withUnsafeBytes(of: UInt32(bytes.count).littleEndian, Array.init))
        data.append(bytes)
        return data
    }

    private func sendManagerFrame(type: UInt8, payload: Data) throws {
        var frame = Data()
        frame.append(contentsOf: withUnsafeBytes(of: managerProtocolMagic.littleEndian, Array.init))
        frame.append(contentsOf: withUnsafeBytes(of: managerProtocolVersion.littleEndian, Array.init))
        frame.append(type)
        frame.append(0)
        frame.append(contentsOf: withUnsafeBytes(of: UInt32(payload.count).littleEndian, Array.init))
        frame.append(payload)
        try socket.write(frame)
    }

    private func recvManagerFrame() throws -> (UInt8, Data) {
        let header = try socket.readExact(managerHeaderSize)
        let magic = readUInt32LE(header, 0)
        if magic != managerProtocolMagic {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Manager frame magic mismatch"])
        }
        let version = UInt16(littleEndian: header.subdata(in: 4..<6).withUnsafeBytes { $0.load(as: UInt16.self) })
        if version != managerProtocolVersion {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Manager frame version mismatch"])
        }
        let type = header[6]
        let length = readUInt32LE(header, 8)
        if length > managerMaxPayloadSize {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Manager payload too large"])
        }
        let payload = length > 0 ? try socket.readExact(Int(length)) : Data()
        return (type, payload)
    }

    private func performManagerConnect() throws {
        let token = config.managerAuthToken ?? ""
        if token.isEmpty {
            throw ScratchBirdAuthorizationException(
                message: "manager_proxy mode requires manager_auth_token",
                sqlState: "28000"
            )
        }
        let managerUser: String
        if let configured = config.managerUsername, !configured.isEmpty {
            managerUser = configured
        } else if !config.user.isEmpty {
            managerUser = config.user
        } else {
            managerUser = "admin"
        }
        let managerDatabase =
            (config.managerDatabase?.isEmpty == false) ? config.managerDatabase! : config.database
        let managerProfile = config.managerConnectionProfile.isEmpty ? "SBsql" : config.managerConnectionProfile
        let managerIntent = config.managerClientIntent.isEmpty ? "SBsql" : config.managerClientIntent

        var hello = Data()
        hello.append(contentsOf: withUnsafeBytes(of: mcpProtocolVersion.littleEndian, Array.init))
        hello.append(contentsOf: withUnsafeBytes(of: UInt16(config.managerClientFlags & 0xFFFF).littleEndian, Array.init))
        try sendManagerFrame(type: mcpMsgHello, payload: hello)
        var frame = try recvManagerFrame()
        if frame.0 != mcpMsgStatusResponse {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Expected MCP hello status response"])
        }

        var authStart = Data()
        authStart.append(buildLengthPrefixedString(managerUser))
        authStart.append(mcpAuthMethodToken)
        if config.managerAuthFastPath {
            let tokenBytes = Data(token.utf8)
            authStart.append(contentsOf: withUnsafeBytes(of: UInt32(tokenBytes.count).littleEndian, Array.init))
            authStart.append(tokenBytes)
        } else {
            authStart.append(contentsOf: [0, 0, 0, 0])
        }
        try sendManagerFrame(type: mcpMsgAuthStart, payload: authStart)
        frame = try recvManagerFrame()
        if frame.0 == mcpMsgAuthChallenge {
            let tokenBytes = Data(token.utf8)
            var authContinue = Data()
            authContinue.append(contentsOf: withUnsafeBytes(of: UInt32(tokenBytes.count).littleEndian, Array.init))
            authContinue.append(tokenBytes)
            try sendManagerFrame(type: mcpMsgAuthContinue, payload: authContinue)
            frame = try recvManagerFrame()
        }
        if frame.0 != mcpMsgAuthResponse {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Expected MCP auth response"])
        }
        if frame.1.count < 1 + 4 + 256 {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Truncated MCP auth response"])
        }
        if frame.1[0] != 0 {
            let errSlice = frame.1.subdata(in: 5..<261)
            let err = String(data: errSlice, encoding: .utf8)?
                .trimmingCharacters(in: CharacterSet(charactersIn: "\0")) ?? ""
            throw ScratchBirdAuthorizationException(
                message: err.isEmpty ? "MCP authentication failed" : err,
                sqlState: "28000"
            )
        }

        var dbConnect = Data("MCP1".utf8)
        dbConnect.append(buildLengthPrefixedString(managerDatabase))
        dbConnect.append(buildLengthPrefixedString(managerProfile))
        dbConnect.append(buildLengthPrefixedString(managerIntent))
        let nonce = Data((0..<16).map { _ in UInt8.random(in: 0...255) })
        dbConnect.append(contentsOf: withUnsafeBytes(of: UInt16(nonce.count).littleEndian, Array.init))
        dbConnect.append(nonce)
        try sendManagerFrame(type: mcpMsgDbConnect, payload: dbConnect)
        frame = try recvManagerFrame()
        if frame.0 != mcpMsgConnectResponse {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Expected MCP connect response"])
        }
        if frame.1.count < 1 + 2 + 2 + 16 + 64 + 32 {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Truncated MCP connect response"])
        }
        if frame.1[0] != 0 {
            var message = "MCP database connect failed"
            let errOffset = 1 + 2 + 2 + 16 + 64 + 32
            if frame.1.count >= errOffset + 4 {
                let errLen = Int(readUInt32LE(frame.1, errOffset))
                if frame.1.count >= errOffset + 4 + errLen {
                    let errData = frame.1.subdata(in: (errOffset + 4)..<(errOffset + 4 + errLen))
                    if let decoded = String(data: errData, encoding: .utf8), !decoded.isEmpty {
                        message = decoded
                    }
                }
            }
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: message])
        }
        resolvedAuthContext = ScratchBirdResolvedAuthContext(
            ingressMode: resolvedAuthContext.ingressMode,
            resolvedAuthMethod: resolvedAuthContext.resolvedAuthMethod,
            resolvedAuthPluginId: resolvedAuthContext.resolvedAuthPluginId,
            managerAuthenticated: true,
            attached: resolvedAuthContext.attached
        )
    }

    private func drainUntilReady() throws -> Bool {
        while true {
            let msg = try recvMessage()
            if handleAsyncMessage(msg) {
                continue
            }
            if msg.header.type == .ready {
                let ready = parseReadyState(msg.payload, fallback: msg.header.txnId)
                applyRuntimeReadyState(status: ready.status, txnId: ready.txnId)
                portalResumePending = false
                return true
            }
            if msg.header.type == .error {
                throw buildScratchBirdError(
                    from: msg.payload,
                    fallbackMessage: "Request failed",
                    defaultSqlState: "42000"
                )
            }
        }
    }

    private func drainReadyAfterError() throws {
        while true {
            let msg = try recvMessage()
            if handleAsyncMessage(msg) {
                continue
            }
            if msg.header.type == .ready {
                let ready = parseReadyState(msg.payload, fallback: msg.header.txnId)
                applyRuntimeReadyState(status: ready.status, txnId: ready.txnId)
                portalResumePending = false
                return
            }
            if msg.header.type == .error {
                continue
            }
        }
    }

    @discardableResult
    private func sendMessage(type: MessageType, payload: Data, flags: UInt8 = 0, forceZero: Bool = false) throws -> UInt32 {
        let currentSequence = sequence
        let attachment = forceZero ? Data(repeating: 0, count: 16) : attachmentId
        let txn = forceZero ? 0 : txnId
        let header = MessageHeader(type: type, flags: flags, length: UInt32(payload.count), sequence: currentSequence, attachmentId: attachment, txnId: txn)
        sequence += 1
        let data = encodeMessage(header: header, payload: payload)
        try socket.write(data)
        return currentSequence
    }

    private func recvMessage() throws -> ProtocolMessage {
        let headerBytes = try socket.readExact(headerSize)
        let header = try decodeHeader(headerBytes)
        let payload = header.length > 0 ? try socket.readExact(Int(header.length)) : Data()
        return ProtocolMessage(header: header, payload: payload)
    }

    private func parseRowDescription(_ payload: Data) -> [ScratchBirdColumn] {
        if payload.count < 4 { return [] }
        let count = UInt16(littleEndian: payload.withUnsafeBytes { $0.load(as: UInt16.self) })
        var offset = 4
        var columns: [ScratchBirdColumn] = []
        for _ in 0..<count {
            if offset + 4 > payload.count { return [] }
            let nameLen = Int(readUInt32LE(payload, offset))
            offset += 4
            if offset + nameLen + 14 > payload.count { return [] }
            let name = String(data: payload.subdata(in: offset..<(offset + nameLen)), encoding: .utf8) ?? ""
            offset += nameLen
            let tableOid = readUInt32LE(payload, offset)
            offset += 4
            let columnIndex = UInt16(littleEndian: payload.subdata(in: offset..<(offset + 2)).withUnsafeBytes { $0.load(as: UInt16.self) })
            offset += 2
            let typeOid = UInt32(littleEndian: payload.subdata(in: offset..<(offset + 4)).withUnsafeBytes { $0.load(as: UInt32.self) })
            offset += 4
            let typeSize = Int16(littleEndian: payload.subdata(in: offset..<(offset + 2)).withUnsafeBytes { $0.load(as: Int16.self) })
            offset += 2
            let typeModifier = Int32(littleEndian: payload.subdata(in: offset..<(offset + 4)).withUnsafeBytes { $0.load(as: Int32.self) })
            offset += 4
            let format = UInt16(payload[offset])
            offset += 1
            let nullable = payload[offset] == 1
            offset += 1
            offset += 2
            columns.append(
                ScratchBirdColumn(
                    name: name,
                    tableOid: tableOid,
                    columnIndex: columnIndex,
                    typeOid: typeOid,
                    typeSize: typeSize,
                    typeModifier: typeModifier,
                    format: format,
                    nullable: nullable
                )
            )
        }
        return columns
    }

    private func parseDataRow(_ payload: Data) -> [Data?] {
        if payload.count < 4 {
            return []
        }
        let count = UInt16(littleEndian: payload.withUnsafeBytes { $0.load(fromByteOffset: 0, as: UInt16.self) })
        let nullBytes = Int(UInt16(littleEndian: payload.withUnsafeBytes { $0.load(fromByteOffset: 2, as: UInt16.self) }))
        var offset = 4
        if offset + nullBytes > payload.count {
            return []
        }
        let nullBitmap = payload.subdata(in: offset..<(offset + nullBytes))
        offset += nullBytes
        var out: [Data?] = []
        for index in 0..<Int(count) {
            let byteIndex = index / 8
            let bitIndex = UInt8(index % 8)
            let isNull = byteIndex < nullBitmap.count && (nullBitmap[nullBitmap.startIndex + byteIndex] & (1 << bitIndex)) != 0
            if isNull {
                out.append(nil)
                continue
            }
            if offset + 4 > payload.count {
                return []
            }
            let len = Int32(littleEndian: payload.subdata(in: offset..<(offset + 4)).withUnsafeBytes { $0.load(as: Int32.self) })
            offset += 4
            if len < 0 {
                out.append(nil)
            } else {
                if offset + Int(len) > payload.count {
                    return []
                }
                out.append(payload.subdata(in: offset..<(offset + Int(len))))
                offset += Int(len)
            }
        }
        return out
    }

    private func readCString(_ data: Data, _ offset: Int) -> (String, Int) {
        var idx = offset
        while idx < data.count && data[idx] != 0 { idx += 1 }
        let name = String(data: data.subdata(in: offset..<idx), encoding: .utf8) ?? ""
        return (name, idx + 1)
    }

    private func normalizeQuery(_ sql: String, params: [Any?]) throws -> (sql: String, params: [Any?]) {
        if params.isEmpty || !sql.contains("?") {
            return (sql, params)
        }
        var result = ""
        var ordered: [Any?] = []
        var inString = false
        var index = 0
        for ch in sql {
            if ch == "'" {
                inString.toggle()
                result.append(ch)
                continue
            }
            if !inString && ch == "?" {
                if index >= params.count {
                    throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Not enough parameters"])
                }
                ordered.append(params[index])
                index += 1
                result.append("$\(ordered.count)")
                continue
            }
            result.append(ch)
        }
        if index < params.count {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Too many parameters"])
        }
        return (result, ordered)
    }
}
