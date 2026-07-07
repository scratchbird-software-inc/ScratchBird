"use strict";
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
exports.Pool = exports.Client = void 0;
const node_net_1 = __importDefault(require("node:net"));
const node_tls_1 = __importDefault(require("node:tls"));
const node_fs_1 = __importDefault(require("node:fs"));
const node_crypto_1 = require("node:crypto");
const protocol_1 = require("./protocol");
const scram_1 = require("./scram");
const dsn_1 = require("./dsn");
const sql_1 = require("./sql");
const types_1 = require("./types");
const errors_1 = require("./errors");
const circuit_breaker_1 = require("./circuit_breaker");
const keepalive_1 = require("./keepalive");
const leak_detector_1 = require("./leak_detector");
const telemetry_1 = require("./telemetry");
const metadata_1 = require("./metadata");
const QUERY_FLAG_BINARY_RESULT = 0x04;
const FORMAT_TEXT = 0;
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
const DEFAULT_SESSION_SCHEMA = "users.public";
const DEFAULT_AUTH_PLUGIN_IDS = {
    [protocol_1.AuthMethod.PASSWORD]: "scratchbird.auth.password_compat",
    [protocol_1.AuthMethod.MD5]: "scratchbird.auth.md5_legacy",
    [protocol_1.AuthMethod.SCRAM_SHA_256]: "scratchbird.auth.scram_sha_256",
    [protocol_1.AuthMethod.SCRAM_SHA_512]: "scratchbird.auth.scram_sha_512",
    [protocol_1.AuthMethod.TOKEN]: "scratchbird.auth.authkey_token",
    [protocol_1.AuthMethod.PEER]: "scratchbird.auth.peer_uid",
    [protocol_1.AuthMethod.REATTACH]: "scratchbird.auth.reattach",
};
function authMethodName(method) {
    switch (method) {
        case protocol_1.AuthMethod.PASSWORD:
            return "PASSWORD";
        case protocol_1.AuthMethod.MD5:
            return "MD5";
        case protocol_1.AuthMethod.SCRAM_SHA_256:
            return "SCRAM_SHA_256";
        case protocol_1.AuthMethod.SCRAM_SHA_512:
            return "SCRAM_SHA_512";
        case protocol_1.AuthMethod.TOKEN:
            return "TOKEN";
        case protocol_1.AuthMethod.PEER:
            return "PEER";
        case protocol_1.AuthMethod.REATTACH:
            return "REATTACH";
        default:
            return null;
    }
}
function authPluginIdForMethod(method, configuredMethodId) {
    if (configuredMethodId?.trim()) {
        return configuredMethodId.trim();
    }
    return DEFAULT_AUTH_PLUGIN_IDS[method] ?? null;
}
function executableLocally(method) {
    return (method === protocol_1.AuthMethod.PASSWORD ||
        method === protocol_1.AuthMethod.SCRAM_SHA_256 ||
        method === protocol_1.AuthMethod.SCRAM_SHA_512 ||
        method === protocol_1.AuthMethod.TOKEN);
}
function brokerRequired(method) {
    return method === protocol_1.AuthMethod.PEER;
}
function describeAuthMethod(method, configuredMethodId) {
    const wireMethod = authMethodName(method);
    if (!wireMethod) {
        return null;
    }
    return {
        wireMethod,
        pluginMethodId: authPluginIdForMethod(method, configuredMethodId),
        executableLocally: executableLocally(method),
        brokerRequired: brokerRequired(method),
    };
}
function resolveTokenAuthPayload(config) {
    if (config.authToken?.length) {
        return Buffer.from(config.authToken, "utf8");
    }
    if (config.authMethodPayload?.length) {
        return Buffer.from(config.authMethodPayload, "utf8");
    }
    if (config.authPayloadB64?.length) {
        return Buffer.from(config.authPayloadB64, "base64");
    }
    if (config.authPayloadJson?.length) {
        return Buffer.from(config.authPayloadJson, "utf8");
    }
    if (config.workloadIdentityToken?.length) {
        return Buffer.from(config.workloadIdentityToken, "utf8");
    }
    if (config.proxyPrincipalAssertion?.length) {
        return Buffer.from(config.proxyPrincipalAssertion, "utf8");
    }
    return null;
}
class SocketReader {
    constructor(socket) {
        this.socket = socket;
        this.buffer = Buffer.alloc(0);
        this.pending = [];
        this.closed = false;
        socket.on("data", (chunk) => this.onData(chunk));
        socket.on("error", (err) => this.fail(err instanceof Error ? err : new Error(String(err))));
        socket.on("close", () => this.fail(new Error("Connection closed")));
    }
    readExact(len) {
        if (this.closed) {
            return Promise.reject(new Error("Connection closed"));
        }
        if (this.buffer.length >= len) {
            const out = this.buffer.subarray(0, len);
            this.buffer = this.buffer.subarray(len);
            return Promise.resolve(out);
        }
        return new Promise((resolve, reject) => {
            this.pending.push({ len, resolve, reject });
        });
    }
    onData(chunk) {
        this.buffer = this.buffer.length ? Buffer.concat([this.buffer, chunk]) : chunk;
        this.flush();
    }
    flush() {
        while (this.pending.length && this.buffer.length >= this.pending[0].len) {
            const next = this.pending.shift();
            if (!next)
                break;
            const out = this.buffer.subarray(0, next.len);
            this.buffer = this.buffer.subarray(next.len);
            next.resolve(out);
        }
    }
    fail(err) {
        if (this.closed)
            return;
        this.closed = true;
        while (this.pending.length) {
            const next = this.pending.shift();
            if (next)
                next.reject(err);
        }
    }
}
class ProtocolConnection {
    constructor() {
        this.attachmentId = Buffer.alloc(16);
        this.txnId = 0n;
        this.sequence = 0;
    }
    async connect(config) {
        this.close();
        const host = config.host ?? "localhost";
        const port = config.port ?? 3092;
        const sslMode = resolveSslMode(config);
        const transportMode = normalizeTransportMode(config.transportMode);
        let rawSocket = transportMode === "local_ipc"
            ? await connectLocalIpc(config.ipcPath, config.connectTimeoutMs ?? 30000)
            : await connectTcp(host, port, config.connectTimeoutMs ?? 30000);
        if (sslMode !== "disable") {
            if (transportMode === "local_ipc") {
                throw new Error("local_ipc uses Unix-domain sockets and does not negotiate TLS");
            }
            rawSocket = await upgradeTls(rawSocket, host, sslMode, config);
        }
        if (config.socketTimeoutMs && config.socketTimeoutMs > 0) {
            rawSocket.setTimeout(config.socketTimeoutMs);
        }
        this.socket = rawSocket;
        this.reader = new SocketReader(rawSocket);
    }
    setAttachment(id, txnId) {
        this.attachmentId = Buffer.from(id);
        this.txnId = txnId;
    }
    setTxnId(txnId) {
        this.txnId = txnId;
    }
    getTxnId() {
        return this.txnId;
    }
    async sendMessage(type, payload, flags, forceZero) {
        if (!this.socket)
            throw new Error("Socket not connected");
        const seq = this.sequence++;
        const header = {
            type,
            flags,
            length: payload.length,
            sequence: seq,
            attachmentId: forceZero ? Buffer.alloc(16) : this.attachmentId,
            txnId: forceZero ? 0n : this.txnId,
        };
        const data = (0, protocol_1.encodeMessage)(header, payload);
        await this.sendRaw(data);
        return seq;
    }
    async sendRaw(data) {
        if (!this.socket)
            throw new Error("Socket not connected");
        await new Promise((resolve, reject) => {
            this.socket.write(data, (err) => {
                if (err)
                    reject(err);
                else
                    resolve();
            });
        });
    }
    async recv() {
        if (!this.reader)
            throw new Error("Socket not connected");
        const headerBuf = await this.reader.readExact(protocol_1.HEADER_SIZE);
        const header = (0, protocol_1.decodeHeader)(headerBuf);
        const payload = header.length ? await this.reader.readExact(header.length) : Buffer.alloc(0);
        return { header, payload };
    }
    async sendManagerFrame(type, payload) {
        const header = Buffer.alloc(MANAGER_HEADER_SIZE);
        header.writeUInt32LE(MANAGER_PROTOCOL_MAGIC, 0);
        header.writeUInt16LE(MANAGER_PROTOCOL_VERSION, 4);
        header.writeUInt8(type, 6);
        header.writeUInt8(0, 7);
        header.writeUInt32LE(payload.length, 8);
        await this.sendRaw(Buffer.concat([header, payload]));
    }
    async recvManagerFrame() {
        if (!this.reader)
            throw new Error("Socket not connected");
        const header = await this.reader.readExact(MANAGER_HEADER_SIZE);
        const magic = header.readUInt32LE(0);
        if (magic !== MANAGER_PROTOCOL_MAGIC) {
            throw new Error("Manager frame magic mismatch");
        }
        const version = header.readUInt16LE(4);
        if (version !== MANAGER_PROTOCOL_VERSION) {
            throw new Error("Manager frame version mismatch");
        }
        const type = header.readUInt8(6);
        const length = header.readUInt32LE(8);
        if (length > MANAGER_MAX_PAYLOAD_SIZE) {
            throw new Error("Manager payload too large");
        }
        const payload = length > 0 ? await this.reader.readExact(length) : Buffer.alloc(0);
        return { type, payload };
    }
    close() {
        if (this.socket) {
            this.socket.destroy();
        }
        this.socket = undefined;
        this.reader = undefined;
        this.attachmentId = Buffer.alloc(16);
        this.txnId = 0n;
        this.sequence = 0;
    }
}
class Client {
    constructor(config) {
        this.protocol = new ProtocolConnection();
        this.connected = false;
        this.transactionActive = false;
        this.portalResumePending = false;
        this.autoCommit = true;
        this.sessionSchema = null;
        this.prepared = new Map();
        this.parameters = {};
        this.notificationHandlers = [];
        this.connectionId = (0, node_crypto_1.randomUUID)();
        this.circuitBreaker = new circuit_breaker_1.CircuitBreaker({}, "node");
        this.telemetry = new telemetry_1.TelemetryCollector();
        this.keepaliveManager = new keepalive_1.KeepaliveManager();
        this.leakDetector = new leak_detector_1.LeakDetector();
        this.skipSchemaApplyOnce = false;
        this.resolvedAuthContext = {
            ingressMode: "direct",
            resolvedAuthMethod: null,
            resolvedAuthPluginId: null,
            managerAuthenticated: false,
            attached: false,
        };
        const parsed = typeof config === "string" ? (0, dsn_1.parseDsn)(config) : {};
        this.config = { ...parsed, ...(typeof config === "object" ? config : {}) };
        this.config.protocol = (0, dsn_1.normalizeNativeProtocol)(this.config.protocol ?? this.config.parser ?? this.config.dialect);
        this.config.frontDoorMode = (0, dsn_1.normalizeFrontDoorMode)(this.config.frontDoorMode);
        if (!this.config.host)
            this.config.host = "localhost";
        if (!this.config.port)
            this.config.port = 3092;
        if (!this.config.applicationName)
            this.config.applicationName = "scratchbird_node";
        if (!this.config.sslmode)
            this.config.sslmode = "require";
        if (this.config.binaryTransfer === undefined)
            this.config.binaryTransfer = true;
        if (!this.config.compression)
            this.config.compression = "off";
        if (this.config.metadataExpandSchemaParents === undefined)
            this.config.metadataExpandSchemaParents = false;
        if (!this.config.managerConnectionProfile)
            this.config.managerConnectionProfile = "SBsql";
        if (!this.config.managerClientIntent)
            this.config.managerClientIntent = "SBsql";
        if (this.config.managerClientFlags === undefined)
            this.config.managerClientFlags = 0;
        if (this.config.managerAuthFastPath === undefined)
            this.config.managerAuthFastPath = true;
        if (this.config.connectClientFlags === undefined)
            this.config.connectClientFlags = 0x0100;
        this.sessionSchema = normalizeSessionSchema(this.config.schema);
        this.resolvedAuthContext.ingressMode = this.config.frontDoorMode;
    }
    getResolvedAuthContext() {
        return { ...this.resolvedAuthContext };
    }
    async probeAuthSurface() {
        this.config.protocol = (0, dsn_1.normalizeNativeProtocol)(this.config.protocol ?? this.config.parser ?? this.config.dialect);
        this.config.frontDoorMode = (0, dsn_1.normalizeFrontDoorMode)(this.config.frontDoorMode);
        const resolvedHost = this.config.host ?? "localhost";
        const resolvedPort = this.config.port ?? 3092;
        this.protocol.close();
        await this.protocol.connect(this.config);
        try {
            if (this.config.frontDoorMode === "manager_proxy") {
                return await this.probeManagerAuthSurface(resolvedHost, resolvedPort);
            }
            return await this.probeDirectAuthSurface(resolvedHost, resolvedPort);
        }
        finally {
            this.protocol.close();
        }
    }
    resetResolvedAuthContext() {
        this.resolvedAuthContext = {
            ingressMode: this.config.frontDoorMode ?? "direct",
            resolvedAuthMethod: null,
            resolvedAuthPluginId: null,
            managerAuthenticated: false,
            attached: false,
        };
    }
    buildStartupParams() {
        const params = {
            database: this.config.database ?? "",
            user: this.config.user ?? "",
            client_flags: String(this.config.connectClientFlags ?? 0x0100),
        };
        if (!!this.config.dormantId !== !!this.config.dormantReattachToken) {
            throw new errors_1.ScratchbirdSyntaxError("dormantId and dormantReattachToken must be provided together", "42601");
        }
        if (this.config.role) {
            params.role = this.config.role;
        }
        if (this.config.applicationName) {
            params.application_name = this.config.applicationName;
        }
        if (this.config.dormantId) {
            params.dormant_id = this.config.dormantId;
            params.dormant_reattach_token = this.config.dormantReattachToken ?? "";
        }
        (0, protocol_1.applyAuthPluginSelection)(params, {
            methodId: this.config.authMethodId,
            methodPayload: this.config.authMethodPayload,
            payloadJson: this.config.authPayloadJson,
            payloadB64: this.config.authPayloadB64,
            providerProfile: this.config.authProviderProfile,
            requiredMethods: this.config.authRequiredMethods,
            forbiddenMethods: this.config.authForbiddenMethods,
            requireChannelBinding: this.config.authRequireChannelBinding === true,
            workloadIdentityToken: this.config.workloadIdentityToken,
            proxyPrincipalAssertion: this.config.proxyPrincipalAssertion,
        });
        return params;
    }
    async probeDirectAuthSurface(resolvedHost, resolvedPort) {
        const startup = (0, protocol_1.buildStartupPayload)(this.requestedFeatures(), this.buildStartupParams());
        await this.protocol.sendMessage(protocol_1.MessageType.STARTUP, startup, 0, true);
        while (true) {
            const msg = await this.protocol.recv();
            if (this.handleAsyncMessage(msg)) {
                continue;
            }
            switch (msg.header.type) {
                case protocol_1.MessageType.NEGOTIATE_VERSION:
                    continue;
                case protocol_1.MessageType.AUTH_REQUEST: {
                    const { method } = (0, protocol_1.parseAuthRequest)(msg.payload);
                    const methodSurface = describeAuthMethod(method, this.config.authMethodId);
                    return {
                        reachable: true,
                        ingressMode: "direct",
                        resolvedHost,
                        resolvedPort,
                        admittedMethods: methodSurface ? [methodSurface] : [],
                        requiredMethod: methodSurface?.wireMethod ?? null,
                        requiredPluginMethodId: methodSurface?.pluginMethodId ?? null,
                        allowedTransportMask: null,
                        additionalContinuationPossible: method === protocol_1.AuthMethod.SCRAM_SHA_256 ||
                            method === protocol_1.AuthMethod.SCRAM_SHA_512 ||
                            method === protocol_1.AuthMethod.TOKEN ||
                            method === protocol_1.AuthMethod.PEER,
                    };
                }
                case protocol_1.MessageType.AUTH_OK:
                case protocol_1.MessageType.READY:
                    return {
                        reachable: true,
                        ingressMode: "direct",
                        resolvedHost,
                        resolvedPort,
                        admittedMethods: [],
                        requiredMethod: null,
                        requiredPluginMethodId: null,
                        allowedTransportMask: null,
                        additionalContinuationPossible: false,
                    };
                case protocol_1.MessageType.ERROR:
                    throw this.raiseProtocolError(msg.payload);
                default:
                    continue;
            }
        }
    }
    async probeManagerAuthSurface(resolvedHost, resolvedPort) {
        const managerUser = this.config.managerUsername || this.config.user || "admin";
        const hello = Buffer.alloc(4);
        hello.writeUInt16LE(MCP_PROTOCOL_VERSION, 0);
        hello.writeUInt16LE((this.config.managerClientFlags ?? 0) & 0xffff, 2);
        await this.protocol.sendManagerFrame(MCP_MSG_HELLO, hello);
        let frame = await this.protocol.recvManagerFrame();
        if (frame.type !== MCP_MSG_STATUS_RESPONSE) {
            throw new errors_1.ScratchbirdConnectionError("expected MCP hello status response", "08P01");
        }
        const authStartParts = [];
        this.appendLengthPrefixedString(authStartParts, managerUser);
        authStartParts.push(Buffer.from([MCP_AUTH_METHOD_TOKEN]));
        authStartParts.push(Buffer.alloc(4));
        await this.protocol.sendManagerFrame(MCP_MSG_AUTH_START, Buffer.concat(authStartParts));
        frame = await this.protocol.recvManagerFrame();
        return {
            reachable: true,
            ingressMode: "manager_proxy",
            resolvedHost,
            resolvedPort,
            admittedMethods: [
                {
                    wireMethod: "TOKEN",
                    pluginMethodId: "scratchbird.auth.authkey_token",
                    executableLocally: true,
                    brokerRequired: false,
                },
            ],
            requiredMethod: "TOKEN",
            requiredPluginMethodId: "scratchbird.auth.authkey_token",
            allowedTransportMask: null,
            additionalContinuationPossible: frame.type === MCP_MSG_AUTH_CHALLENGE,
        };
    }
    async connect() {
        this.config.protocol = (0, dsn_1.normalizeNativeProtocol)(this.config.protocol ?? this.config.parser ?? this.config.dialect);
        this.config.frontDoorMode = (0, dsn_1.normalizeFrontDoorMode)(this.config.frontDoorMode);
        if (!this.config.user || !this.config.database) {
            throw new Error("user and database are required");
        }
        if (this.config.binaryTransfer === false) {
            throw new errors_1.ScratchbirdNotSupportedError("binary_transfer=false is not supported", "0A000");
        }
        if (this.config.compression === "zstd") {
            throw new errors_1.ScratchbirdNotSupportedError("compression=zstd is not supported", "0A000");
        }
        this.protocol.close();
        this.cleanupResilience();
        this.clearAbandonedSessionState();
        this.resetResolvedAuthContext();
        this.connected = false;
        await this.protocol.connect(this.config);
        if (this.config.frontDoorMode === "manager_proxy") {
            await this.performManagerConnect();
        }
        await this.handshake();
        if (this.skipSchemaApplyOnce) {
            this.skipSchemaApplyOnce = false;
        }
        else {
            await this.applySchema();
        }
        this.keepaliveManager.start();
        this.keepaliveTracker = this.keepaliveManager.register(this.connectionId, async () => {
            try {
                await this.ping();
                return true;
            }
            catch {
                return false;
            }
        });
        this.leakDetector.start();
        this.leakGuard = this.leakDetector.checkout(this.connectionId, { driver: "node" });
        this.connected = true;
        this.resolvedAuthContext.attached = true;
    }
    async query(text, params, options) {
        this.ensureConnected();
        const normalized = this.normalizeQueryOrThrow(text, params);
        return (await this.executeQuery(normalized.sql, normalized.params, options));
    }
    async queryMulti(text, params, options) {
        this.ensureConnected();
        const normalized = this.normalizeQueryOrThrow(text, params);
        const results = await this.executeQueryMulti(normalized.sql, normalized.params, options);
        return results;
    }
    async queryBatch(text, batchParams, options) {
        this.ensureConnected();
        if (!batchParams.length) {
            throw new errors_1.ScratchbirdSyntaxError("batch parameters are required", "07001");
        }
        const summaries = [];
        for (let i = 0; i < batchParams.length; i++) {
            const result = await this.query(text, batchParams[i], options);
            summaries.push(this.toBatchSummary(i, result));
        }
        return {
            items: summaries,
            totalRowCount: summaries.reduce((sum, item) => sum + item.rowCount, 0),
        };
    }
    async queryStream(text, params, options) {
        this.ensureConnected();
        const normalized = this.normalizeQueryOrThrow(text, params);
        return this.executeQueryStream(normalized.sql, normalized.params, options);
    }
    nativeSQL(text, params) {
        return this.normalizeQueryOrThrow(text, params).sql;
    }
    nativeCallableSQL(text, params) {
        return this.normalizeCallableQueryOrThrow(text, params).sql;
    }
    async call(text, params, options) {
        this.ensureConnected();
        const normalized = this.normalizeCallableQueryOrThrow(text, params);
        return (await this.executeQuery(normalized.sql, normalized.params, options));
    }
    async executeWithGeneratedKeys(text, params, options) {
        this.ensureConnected();
        const normalized = this.normalizeQueryOrThrow(text, params);
        const results = await this.executeQueryMulti(normalized.sql, normalized.params, options);
        const keys = [];
        for (const result of results) {
            if (result.lastId !== null && result.lastId !== 0n) {
                keys.push(result.lastId);
            }
        }
        return keys;
    }
    async queryMetadata(collectionName = "tables", restrictions) {
        this.ensureConnected();
        let normalizedCollection;
        try {
            normalizedCollection = (0, metadata_1.normalizeMetadataCollectionName)(collectionName);
        }
        catch (err) {
            throw new errors_1.ScratchbirdNotSupportedError(err.message, "0A000");
        }
        if (normalizedCollection === "catalogs") {
            const catalogName = this.config.database?.trim() ?? "";
            const baseRows = catalogName ? [{ catalog_name: catalogName }] : [];
            const shapedRows = (0, metadata_1.shapeMetadataRowsForCollection)(baseRows, normalizedCollection, {
                database: this.config.database ?? null,
            });
            const familyRows = (0, metadata_1.filterMetadataRowsForCollectionFamily)(shapedRows, normalizedCollection);
            const rows = (0, metadata_1.filterMetadataRowsByRestrictions)(familyRows, restrictions, normalizedCollection);
            return {
                rows,
                rowCount: rows.length,
                fields: [],
                command: "SELECT",
                lastId: null,
            };
        }
        const sql = (0, metadata_1.resolveMetadataCollectionQuery)(normalizedCollection);
        const result = await this.query(sql);
        const shapedRows = (0, metadata_1.shapeMetadataRowsForCollection)(result.rows, normalizedCollection, {
            database: this.config.database ?? null,
        });
        const familyRows = (0, metadata_1.filterMetadataRowsForCollectionFamily)(shapedRows, normalizedCollection);
        const restrictedRows = (0, metadata_1.filterMetadataRowsByRestrictions)(familyRows, restrictions, normalizedCollection);
        const restrictedResult = {
            ...result,
            rows: restrictedRows,
            rowCount: restrictedRows.length,
        };
        if (normalizedCollection !== "schemas" || !this.config.metadataExpandSchemaParents) {
            return restrictedResult;
        }
        const expandedRows = (0, metadata_1.expandSchemaMetadataRows)(restrictedRows);
        return {
            ...restrictedResult,
            rows: expandedRows,
            rowCount: expandedRows.length,
        };
    }
    async getSchema(collectionName = "tables", restrictions) {
        return this.queryMetadata(collectionName, restrictions);
    }
    async getSchemaTree(options) {
        this.ensureConnected();
        const schemas = await this.getSchema("schemas", options?.restrictions);
        return (0, metadata_1.buildMetadataSchemaTree)(schemas.rows, {
            expandParents: options?.expandParents ?? this.config.metadataExpandSchemaParents === true,
            database: options?.database ?? this.config.database,
        });
    }
    async schemas(catalog) {
        return this.getSchema("schemas", metadataRestrictions({ catalog }));
    }
    async tables(schema, table, type) {
        return this.getSchema("tables", metadataRestrictions({ schema, table, type }));
    }
    async columns(schema, table, column, type) {
        return this.getSchema("columns", metadataRestrictions({ schema, table, column, type }));
    }
    async indexes(schema, table, index) {
        return this.getSchema("indexes", metadataRestrictions({ schema, table, index }));
    }
    async indexColumns(schema, table, index, column) {
        return this.getSchema("index_columns", metadataRestrictions({ schema, table, index, column }));
    }
    async constraints(schema, table, constraint) {
        return this.getSchema("constraints", metadataRestrictions({ schema, table, constraint }));
    }
    async catalogs(catalog) {
        return this.getSchema("catalogs", metadataRestrictions({ catalog }));
    }
    async primaryKeys(catalog, schema, table, constraint) {
        return this.getSchema("primary_keys", metadataRestrictions({ catalog, schema, table, constraint }));
    }
    async foreignKeys(catalog, schema, table, constraint) {
        return this.getSchema("foreign_keys", metadataRestrictions({ catalog, schema, table, constraint }));
    }
    async procedures(catalog, schema, procedure) {
        return this.getSchema("procedures", metadataRestrictions({ catalog, schema, procedure }));
    }
    async functions(catalog, schema, fn) {
        return this.getSchema("functions", metadataRestrictions({ catalog, schema, function: fn }));
    }
    async routines(catalog, schema, routine) {
        return this.getSchema("routines", metadataRestrictions({ catalog, schema, routine }));
    }
    async tablePrivileges(catalog, schema, table) {
        return this.getSchema("table_privileges", metadataRestrictions({ catalog, schema, table }));
    }
    async columnPrivileges(catalog, schema, table, column) {
        return this.getSchema("column_privileges", metadataRestrictions({ catalog, schema, table, column }));
    }
    async typeInfo(type) {
        return this.getSchema("type_info", metadataRestrictions({ type }));
    }
    getAutoCommit() {
        return this.autoCommit;
    }
    async setAutoCommit(enabled) {
        this.ensureConnected();
        if (this.autoCommit === enabled) {
            return;
        }
        if (enabled && this.transactionActive) {
            await this.commitTransaction();
        }
        // The native engine-endpoint lane owns the replacement session boundary.
        // Client-side autocommit toggles must stay local instead of inventing a
        // SET_OPTION/BEGIN protocol that the server does not treat as authoritative.
        this.autoCommit = enabled;
    }
    getSessionSchema() {
        return this.sessionSchema;
    }
    async setSessionSchema(schema) {
        const normalized = normalizeSessionSchema(schema);
        this.sessionSchema = normalized;
        this.config.schema = normalized ?? undefined;
        if (!this.connected) {
            return;
        }
        const statement = buildSchemaStatement(normalized ?? DEFAULT_SESSION_SCHEMA);
        if (!statement) {
            return;
        }
        await this.withResilience("set_session_schema", statement, async () => {
            await this.sendSimpleQuery(statement);
            await this.drainUntilReady();
        });
    }
    async prepare(name, text, _paramTypes) {
        if (!name)
            throw new Error("name is required");
        this.ensureConnected();
        const normalized = (0, sql_1.normalizePreparedQuery)(text);
        await this.protocol.sendMessage(protocol_1.MessageType.PARSE, (0, protocol_1.buildParsePayload)(name, normalized.sql, []), 0, false);
        const describedParamCount = await this.describeStatement(name);
        this.prepared.set(name, {
            sql: normalized.sql,
            paramCount: describedParamCount >= 0 ? describedParamCount : normalized.paramCount,
            namedOrder: normalized.namedOrder,
        });
    }
    async execute(name, params, options) {
        this.ensureConnected();
        const prepared = this.prepared.get(name);
        if (!prepared)
            throw new Error(`Unknown prepared statement: ${name}`);
        const normalized = this.normalizePreparedParamsOrThrow(prepared, params);
        if (prepared.paramCount >= 0 && prepared.paramCount !== normalized.params.length) {
            throw new errors_1.ScratchbirdError("parameter count mismatch", "07001");
        }
        return (await this.executePrepared(name, normalized.params, options));
    }
    async executeMulti(name, params, options) {
        this.ensureConnected();
        const prepared = this.prepared.get(name);
        if (!prepared)
            throw new Error(`Unknown prepared statement: ${name}`);
        const normalized = this.normalizePreparedParamsOrThrow(prepared, params);
        if (prepared.paramCount >= 0 && prepared.paramCount !== normalized.params.length) {
            throw new errors_1.ScratchbirdError("parameter count mismatch", "07001");
        }
        const results = await this.executePreparedMulti(name, normalized.params, options);
        return results;
    }
    async executeBatch(name, batchParams, options) {
        this.ensureConnected();
        if (!batchParams.length) {
            throw new errors_1.ScratchbirdSyntaxError("batch parameters are required", "07001");
        }
        const summaries = [];
        for (let i = 0; i < batchParams.length; i++) {
            const result = await this.execute(name, batchParams[i], options);
            summaries.push(this.toBatchSummary(i, result));
        }
        return {
            items: summaries,
            totalRowCount: summaries.reduce((sum, item) => sum + item.rowCount, 0),
        };
    }
    async begin(options) {
        await this.beginTransaction(options);
    }
    async commit(options) {
        await this.commitTransaction(options);
    }
    async rollback(options) {
        await this.rollbackTransaction(options);
    }
    supportsPreparedTransactions() {
        return true;
    }
    supportsDormantReattach() {
        return true;
    }
    async prepareTransaction(gid) {
        this.ensureConnected();
        const sql = this.buildPreparedTransactionSql("PREPARE TRANSACTION", gid);
        await this.withResilience("prepare_transaction", sql, async () => {
            await this.sendSimpleQuery(sql);
            await this.drainUntilReady();
        });
    }
    async commitPrepared(gid) {
        this.ensureConnected();
        const sql = this.buildPreparedTransactionSql("COMMIT PREPARED", gid);
        await this.withResilience("commit_prepared", sql, async () => {
            await this.sendSimpleQuery(sql);
            await this.drainUntilReady();
        });
    }
    async rollbackPrepared(gid) {
        this.ensureConnected();
        const sql = this.buildPreparedTransactionSql("ROLLBACK PREPARED", gid);
        await this.withResilience("rollback_prepared", sql, async () => {
            await this.sendSimpleQuery(sql);
            await this.drainUntilReady();
        });
    }
    async detachToDormant() {
        this.ensureConnected();
        delete this.parameters.dormant_id;
        delete this.parameters.dormant_reattach_token;
        await this.attachDetach();
        const dormantId = this.parameters.dormant_id;
        const reattachToken = this.parameters.dormant_reattach_token;
        if (!dormantId || !reattachToken) {
            throw new errors_1.ScratchbirdConnectionError("expected dormant detach identifiers from the server", "08006");
        }
        return {
            dormantId: normalizeUuidText(dormantId, "dormantId"),
            reattachToken: normalizeUuidText(reattachToken, "dormantReattachToken"),
        };
    }
    async reattachDormant(dormantId, authToken) {
        this.ensureConnected();
        if (!authToken) {
            throw new errors_1.ScratchbirdSyntaxError("dormant reattach requires the engine-issued auth token", "42601");
        }
        await this.reconnectWithDormantParams(normalizeUuidText(dormantId, "dormantId"), normalizeUuidText(authToken, "dormantReattachToken"));
    }
    async beginTransaction(options) {
        this.ensureConnected();
        await this.withResilience("txn_begin", undefined, async () => {
            const readCommittedMode = options?.readCommittedMode;
            let isolation = options?.isolationLevel ?? protocol_1.ISOLATION_READ_COMMITTED;
            let flags = 0;
            if (options?.isolationLevel !== undefined)
                flags |= protocol_1.TXN_FLAG_HAS_ISOLATION;
            if (readCommittedMode !== undefined) {
                if (options?.isolationLevel !== undefined &&
                    options.isolationLevel !== protocol_1.ISOLATION_READ_UNCOMMITTED &&
                    options.isolationLevel !== protocol_1.ISOLATION_READ_COMMITTED) {
                    throw new errors_1.ScratchbirdNotSupportedError("readCommittedMode requires a READ COMMITTED isolation alias", "0A000");
                }
                flags |= protocol_1.TXN_FLAG_HAS_READ_COMMITTED_MODE;
                if (options?.isolationLevel === undefined) {
                    isolation = protocol_1.ISOLATION_READ_COMMITTED;
                    flags |= protocol_1.TXN_FLAG_HAS_ISOLATION;
                }
            }
            if (options?.accessMode !== undefined)
                flags |= protocol_1.TXN_FLAG_HAS_ACCESS;
            if (options?.deferrable !== undefined)
                flags |= protocol_1.TXN_FLAG_HAS_DEFERRABLE;
            if (options?.wait !== undefined)
                flags |= protocol_1.TXN_FLAG_HAS_WAIT;
            if (options?.timeoutMs !== undefined)
                flags |= protocol_1.TXN_FLAG_HAS_TIMEOUT;
            if (options?.autocommitMode !== undefined)
                flags |= protocol_1.TXN_FLAG_HAS_AUTOCOMMIT;
            const payload = (0, protocol_1.buildTxnBeginPayload)(flags, options?.conflictAction ?? 0, options?.autocommitMode ?? 0, isolation, options?.accessMode ?? 0, options?.deferrable ? 1 : 0, options?.wait ? 1 : 0, options?.timeoutMs ?? 0, readCommittedMode ?? protocol_1.READ_COMMITTED_MODE_DEFAULT);
            await this.protocol.sendMessage(protocol_1.MessageType.TXN_BEGIN, payload, 0, false);
            await this.drainUntilReady();
        });
    }
    async commitTransaction(options) {
        this.ensureConnected();
        this.ensureTransactionActive("commit");
        await this.withResilience("txn_commit", undefined, async () => {
            const payload = (0, protocol_1.buildTxnCommitPayload)(options?.flags ?? 0);
            await this.protocol.sendMessage(protocol_1.MessageType.TXN_COMMIT, payload, 0, false);
            await this.drainUntilReady();
        });
    }
    async rollbackTransaction(options) {
        this.ensureConnected();
        this.ensureTransactionActive("rollback");
        await this.withResilience("txn_rollback", undefined, async () => {
            const payload = (0, protocol_1.buildTxnRollbackPayload)(options?.flags ?? 0);
            await this.protocol.sendMessage(protocol_1.MessageType.TXN_ROLLBACK, payload, 0, false);
            await this.drainUntilReady();
        });
    }
    async savepoint(name) {
        this.ensureConnected();
        this.ensureTransactionActive("savepoint");
        if (!name.trim()) {
            throw new errors_1.ScratchbirdError("savepoint name is required", "42601");
        }
        await this.withResilience("txn_savepoint", undefined, async () => {
            const payload = (0, protocol_1.buildTxnSavepointPayload)(name);
            await this.protocol.sendMessage(protocol_1.MessageType.TXN_SAVEPOINT, payload, 0, false);
            await this.drainUntilReady();
        });
    }
    async releaseSavepoint(name) {
        this.ensureConnected();
        this.ensureTransactionActive("release savepoint");
        if (!name.trim()) {
            throw new errors_1.ScratchbirdError("savepoint name is required", "42601");
        }
        await this.withResilience("txn_release", undefined, async () => {
            const payload = (0, protocol_1.buildTxnReleasePayload)(name);
            await this.protocol.sendMessage(protocol_1.MessageType.TXN_RELEASE, payload, 0, false);
            await this.drainUntilReady();
        });
    }
    async rollbackToSavepoint(name) {
        this.ensureConnected();
        this.ensureTransactionActive("rollback to savepoint");
        if (!name.trim()) {
            throw new errors_1.ScratchbirdError("savepoint name is required", "42601");
        }
        await this.withResilience("txn_rollback_to", undefined, async () => {
            const payload = (0, protocol_1.buildTxnRollbackToPayload)(name);
            await this.protocol.sendMessage(protocol_1.MessageType.TXN_ROLLBACK_TO, payload, 0, false);
            await this.drainUntilReady();
        });
    }
    async setOption(name, value) {
        this.ensureConnected();
        await this.withResilience("set_option", undefined, async () => {
            const payload = (0, protocol_1.buildSetOptionPayload)(name, value);
            await this.protocol.sendMessage(protocol_1.MessageType.SET_OPTION, payload, 0, false);
            await this.drainUntilReady();
        });
    }
    async ping() {
        this.ensureConnected();
        await this.protocol.sendMessage(protocol_1.MessageType.PING, Buffer.alloc(0), 0, false);
        while (true) {
            const msg = await this.protocol.recv();
            if (this.handleAsyncMessage(msg)) {
                continue;
            }
            if (msg.header.type === protocol_1.MessageType.PONG || msg.header.type === protocol_1.MessageType.READY) {
                return;
            }
            if (msg.header.type === protocol_1.MessageType.ERROR) {
                throw this.raiseProtocolError(msg.payload);
            }
        }
    }
    async terminate() {
        if (this.connected) {
            await this.protocol.sendMessage(protocol_1.MessageType.TERMINATE, Buffer.alloc(0), 0, false);
        }
        this.protocol.close();
        this.cleanupResilience();
        this.clearAbandonedSessionState();
        this.connected = false;
    }
    async subscribe(channel, options) {
        this.ensureConnected();
        const payload = (0, protocol_1.buildSubscribePayload)(options?.type ?? 0, channel, options?.filter ?? "");
        await this.protocol.sendMessage(protocol_1.MessageType.SUBSCRIBE, payload, 0, false);
        await this.drainUntilReady();
    }
    async unsubscribe(channel) {
        this.ensureConnected();
        const payload = (0, protocol_1.buildUnsubscribePayload)(channel);
        await this.protocol.sendMessage(protocol_1.MessageType.UNSUBSCRIBE, payload, 0, false);
        await this.drainUntilReady();
    }
    async executeSblr(hash, bytecode, params, options) {
        this.ensureConnected();
        await this.ensureImplicitTransaction();
        return this.withResilience("sblr_execute", undefined, async () => {
            const paramValues = [];
            if (params) {
                for (const param of params) {
                    const encoded = (0, types_1.encodeParam)(param);
                    paramValues.push(encoded.param);
                }
            }
            const payload = (0, protocol_1.buildSblrExecutePayload)(hash, bytecode ?? Buffer.alloc(0), paramValues);
            await this.protocol.sendMessage(protocol_1.MessageType.SBLR_EXECUTE, payload, 0, false);
            await this.protocol.sendMessage(protocol_1.MessageType.SYNC, Buffer.alloc(0), 0, false);
            return this.collectResults(options?.maxRows ?? 0, options);
        });
    }
    async compileSblr(text, options) {
        this.ensureConnected();
        this.lastSblr = undefined;
        await this.query(text, undefined, {
            ...options,
            returnSblr: true,
            maxRows: 0,
        });
        const compiled = this.getLastSblr();
        if (!compiled || compiled.bytecode.length === 0) {
            throw new errors_1.ScratchbirdConnectionError("parser endpoint did not return SBLR for RETURN_SBLR request", "08P01");
        }
        return {
            hash: compiled.hash,
            version: compiled.version,
            bytecode: Buffer.from(compiled.bytecode),
        };
    }
    async streamControl(controlType, windowSize, timeoutMs) {
        this.ensureConnected();
        const payload = (0, protocol_1.buildStreamControlPayload)(controlType, windowSize, timeoutMs);
        await this.protocol.sendMessage(protocol_1.MessageType.STREAM_CONTROL, payload, 0, false);
    }
    async attachCreate(emulationMode, dbName) {
        this.ensureConnected();
        await this.withResilience("attach_create", undefined, async () => {
            const payload = (0, protocol_1.buildAttachCreatePayload)(emulationMode, dbName);
            await this.protocol.sendMessage(protocol_1.MessageType.ATTACH_CREATE, payload, 0, false);
            await this.drainUntilReady();
        });
    }
    async attachDetach() {
        this.ensureConnected();
        await this.withResilience("attach_detach", undefined, async () => {
            await this.protocol.sendMessage(protocol_1.MessageType.ATTACH_DETACH, Buffer.alloc(0), 0, false);
            await this.drainUntilReady();
        });
    }
    async attachList() {
        this.ensureConnected();
        return this.withResilience("attach_list", undefined, async () => {
            await this.protocol.sendMessage(protocol_1.MessageType.ATTACH_LIST, Buffer.alloc(0), 0, false);
            await this.protocol.sendMessage(protocol_1.MessageType.SYNC, Buffer.alloc(0), 0, false);
            return this.collectResults(0, {});
        });
    }
    onNotification(handler) {
        this.notificationHandlers.push(handler);
    }
    getLastPlan() {
        return this.lastPlan;
    }
    getLastSblr() {
        return this.lastSblr;
    }
    async end() {
        this.protocol.close();
        this.cleanupResilience();
        this.clearAbandonedSessionState();
        this.connected = false;
        this.resolvedAuthContext.attached = false;
    }
    ensureConnected() {
        if (!this.connected) {
            throw new Error("Client is not connected");
        }
    }
    async reconnectWithDormantParams(dormantId, dormantReattachToken) {
        const priorDormantId = this.config.dormantId;
        const priorDormantToken = this.config.dormantReattachToken;
        const priorSkipSchema = this.skipSchemaApplyOnce;
        this.config.dormantId = dormantId;
        this.config.dormantReattachToken = dormantReattachToken;
        this.skipSchemaApplyOnce = true;
        this.protocol.close();
        this.cleanupResilience();
        this.clearAbandonedSessionState();
        this.connected = false;
        try {
            await this.connect();
        }
        finally {
            this.config.dormantId = priorDormantId;
            this.config.dormantReattachToken = priorDormantToken;
            this.skipSchemaApplyOnce = priorSkipSchema;
        }
    }
    async ensureImplicitTransaction() {
        if (this.autoCommit || this.transactionActive) {
            return;
        }
        await this.beginTransaction();
    }
    ensureNoActiveTransaction() {
        if (this.transactionActive) {
            throw new errors_1.ScratchbirdError("transaction already active", "25001");
        }
    }
    ensureTransactionActive(operation) {
        if (!this.transactionActive) {
            throw new errors_1.ScratchbirdError(`${operation} requires an active transaction`, "25000");
        }
    }
    cleanupResilience() {
        if (this.keepaliveTracker) {
            this.keepaliveManager.unregister(this.connectionId);
            this.keepaliveTracker = undefined;
        }
        this.keepaliveManager.stop();
        if (this.leakGuard) {
            this.leakGuard.release();
            this.leakGuard = undefined;
        }
        this.leakDetector.stop();
    }
    clearAbandonedSessionState() {
        // MGA reconnect creates a new attachment/transaction boundary. Prepared handles,
        // attachment parameters, and cached plan/SBLR frames from the abandoned session
        // must be discarded rather than treated as resumable local state.
        this.clearTransactionState();
        this.portalResumePending = false;
        this.prepared.clear();
        this.parameters = {};
        this.lastPlan = undefined;
        this.lastSblr = undefined;
    }
    async validateIfIdle() {
        if (this.keepaliveTracker && this.keepaliveTracker.needsValidation()) {
            await this.ping();
            this.keepaliveTracker.markActive();
        }
    }
    async withResilience(operation, sql, fn) {
        if (!this.circuitBreaker.allowRequest()) {
            throw new errors_1.ScratchbirdError("Circuit breaker is OPEN", "08006");
        }
        await this.validateIfIdle();
        const span = this.telemetry.startSpan(operation);
        if (span && sql) {
            span.withAttribute("db.statement", telemetry_1.TelemetryCollector.sanitizeQuery(sql));
        }
        try {
            const result = await fn();
            this.finishOperation(span, true);
            return result;
        }
        catch (err) {
            this.finishOperation(span, false, err);
            throw err;
        }
    }
    finishOperation(span, success, error) {
        if (success) {
            this.circuitBreaker.recordSuccess();
            this.keepaliveTracker?.markActive();
        }
        else if (this.shouldRecordCircuitFailure(error)) {
            this.circuitBreaker.recordFailure();
        }
        this.telemetry.endSpan(span, success);
    }
    shouldRecordCircuitFailure(error) {
        if (error instanceof errors_1.ScratchbirdConnectionError) {
            return true;
        }
        if (error instanceof errors_1.ScratchbirdError) {
            return (0, errors_1.retryScopeForSqlState)(error.code) === "reconnect";
        }
        return true;
    }
    requestedFeatures() {
        let features = protocol_1.FEATURE_SBLR |
            protocol_1.FEATURE_NOTIFICATIONS |
            protocol_1.FEATURE_QUERY_PLAN |
            protocol_1.FEATURE_SAVEPOINTS |
            protocol_1.FEATURE_BATCH |
            protocol_1.FEATURE_PIPELINE;
        if (this.config.compression === "zstd") {
            features |= protocol_1.FEATURE_COMPRESSION;
        }
        if (this.config.binaryTransfer) {
            features |= protocol_1.FEATURE_STREAMING;
            features |= protocol_1.FEATURE_BINARY_COPY;
        }
        return features;
    }
    appendLengthPrefixedString(out, value) {
        const bytes = Buffer.from(value, "utf8");
        const len = Buffer.alloc(4);
        len.writeUInt32LE(bytes.length, 0);
        out.push(len, bytes);
    }
    async performManagerConnect() {
        if (!this.config.managerAuthToken) {
            throw new errors_1.ScratchbirdAuthError("manager_proxy mode requires manager_auth_token", "28000");
        }
        const managerUser = this.config.managerUsername || this.config.user || "admin";
        const managerDatabase = this.config.managerDatabase || this.config.database || "";
        const managerProfile = this.config.managerConnectionProfile || "SBsql";
        const managerIntent = this.config.managerClientIntent || "SBsql";
        const managerFlags = this.config.managerClientFlags ?? 0;
        const authFastPath = this.config.managerAuthFastPath !== false;
        const hello = Buffer.alloc(4);
        hello.writeUInt16LE(MCP_PROTOCOL_VERSION, 0);
        hello.writeUInt16LE(managerFlags & 0xffff, 2);
        await this.protocol.sendManagerFrame(MCP_MSG_HELLO, hello);
        let frame = await this.protocol.recvManagerFrame();
        if (frame.type !== MCP_MSG_STATUS_RESPONSE) {
            throw new errors_1.ScratchbirdError("expected MCP hello status response", "08P01");
        }
        const authStartParts = [];
        this.appendLengthPrefixedString(authStartParts, managerUser);
        authStartParts.push(Buffer.from([MCP_AUTH_METHOD_TOKEN]));
        if (authFastPath) {
            const token = Buffer.from(this.config.managerAuthToken, "utf8");
            const len = Buffer.alloc(4);
            len.writeUInt32LE(token.length, 0);
            authStartParts.push(len, token);
        }
        else {
            authStartParts.push(Buffer.alloc(4));
        }
        await this.protocol.sendManagerFrame(MCP_MSG_AUTH_START, Buffer.concat(authStartParts));
        frame = await this.protocol.recvManagerFrame();
        if (frame.type === MCP_MSG_AUTH_CHALLENGE) {
            const token = Buffer.from(this.config.managerAuthToken, "utf8");
            const authContinue = Buffer.alloc(4 + token.length);
            authContinue.writeUInt32LE(token.length, 0);
            token.copy(authContinue, 4);
            await this.protocol.sendManagerFrame(MCP_MSG_AUTH_CONTINUE, authContinue);
            frame = await this.protocol.recvManagerFrame();
        }
        if (frame.type !== MCP_MSG_AUTH_RESPONSE) {
            throw new errors_1.ScratchbirdError("expected MCP auth response", "08P01");
        }
        if (frame.payload.length < 1 + 4 + 256) {
            throw new errors_1.ScratchbirdError("truncated MCP auth response", "08P01");
        }
        if (frame.payload.readUInt8(0) !== 0) {
            const errText = frame.payload.subarray(5, 261).toString("utf8").replace(/\0+$/, "") || "MCP authentication failed";
            throw new errors_1.ScratchbirdAuthError(errText, "28000");
        }
        this.resolvedAuthContext.managerAuthenticated = true;
        this.resolvedAuthContext.ingressMode = "manager_proxy";
        const nonce = (0, node_crypto_1.randomBytes)(16);
        const dbConnectParts = [Buffer.from("MCP1", "ascii")];
        this.appendLengthPrefixedString(dbConnectParts, managerDatabase);
        this.appendLengthPrefixedString(dbConnectParts, managerProfile);
        this.appendLengthPrefixedString(dbConnectParts, managerIntent);
        const nonceLen = Buffer.alloc(2);
        nonceLen.writeUInt16LE(nonce.length, 0);
        dbConnectParts.push(nonceLen, nonce);
        await this.protocol.sendManagerFrame(MCP_MSG_DB_CONNECT, Buffer.concat(dbConnectParts));
        frame = await this.protocol.recvManagerFrame();
        if (frame.type !== MCP_MSG_CONNECT_RESPONSE) {
            throw new errors_1.ScratchbirdError("expected MCP connect response", "08P01");
        }
        if (frame.payload.length < 1 + 2 + 2 + 16 + 64 + 32) {
            throw new errors_1.ScratchbirdError("truncated MCP connect response", "08P01");
        }
        if (frame.payload.readUInt8(0) !== 0) {
            const errOffset = 1 + 2 + 2 + 16 + 64 + 32;
            let errText = "MCP database connect failed";
            if (frame.payload.length >= errOffset + 4) {
                const errLen = frame.payload.readUInt32LE(errOffset);
                if (frame.payload.length >= errOffset + 4 + errLen) {
                    errText = frame.payload.subarray(errOffset + 4, errOffset + 4 + errLen).toString("utf8");
                }
            }
            throw new errors_1.ScratchbirdError(errText, "28000");
        }
    }
    async handshake() {
        const startup = (0, protocol_1.buildStartupPayload)(this.requestedFeatures(), this.buildStartupParams());
        await this.protocol.sendMessage(protocol_1.MessageType.STARTUP, startup, 0, true);
        let scram = null;
        while (true) {
            const msg = await this.protocol.recv();
            if (this.handleAsyncMessage(msg)) {
                continue;
            }
            switch (msg.header.type) {
                case protocol_1.MessageType.NEGOTIATE_VERSION:
                    continue;
                case protocol_1.MessageType.AUTH_REQUEST: {
                    const { method } = (0, protocol_1.parseAuthRequest)(msg.payload);
                    if (method === protocol_1.AuthMethod.OK) {
                        continue;
                    }
                    const resolvedMethod = authMethodName(method);
                    this.resolvedAuthContext.resolvedAuthMethod = resolvedMethod;
                    this.resolvedAuthContext.resolvedAuthPluginId = authPluginIdForMethod(method, this.config.authMethodId);
                    if (method === protocol_1.AuthMethod.PASSWORD) {
                        await this.protocol.sendMessage(protocol_1.MessageType.AUTH_RESPONSE, Buffer.from(this.config.password ?? ""), 0, true);
                        continue;
                    }
                    if (method === protocol_1.AuthMethod.SCRAM_SHA_256 || method === protocol_1.AuthMethod.SCRAM_SHA_512) {
                        if (!scram) {
                            scram = new scram_1.ScramExchange(this.config.user ?? "", method === protocol_1.AuthMethod.SCRAM_SHA_512 ? "sha512" : "sha256");
                        }
                        const clientFirst = Buffer.from(scram.clientFirstMessage(), "utf8");
                        await this.protocol.sendMessage(protocol_1.MessageType.AUTH_RESPONSE, clientFirst, 0, true);
                        continue;
                    }
                    if (method === protocol_1.AuthMethod.TOKEN) {
                        const tokenPayload = resolveTokenAuthPayload(this.config);
                        if (!tokenPayload) {
                            throw new errors_1.ScratchbirdAuthError("TOKEN authentication requires authToken, authMethodPayload, authPayloadJson, authPayloadB64, workloadIdentityToken, or proxyPrincipalAssertion", "28000");
                        }
                        await this.protocol.sendMessage(protocol_1.MessageType.AUTH_RESPONSE, tokenPayload, 0, true);
                        continue;
                    }
                    if (method === protocol_1.AuthMethod.MD5) {
                        throw new errors_1.ScratchbirdNotSupportedError("MD5 authentication is admitted by the server but not executable in the Node lane", "0A000");
                    }
                    if (method === protocol_1.AuthMethod.PEER) {
                        throw new errors_1.ScratchbirdNotSupportedError("PEER authentication requires broker or platform assistance in the Node lane", "0A000");
                    }
                    if (method === protocol_1.AuthMethod.REATTACH) {
                        throw new errors_1.ScratchbirdNotSupportedError("REATTACH authentication negotiation is not executable through the generic Node auth lane", "0A000");
                    }
                    throw new errors_1.ScratchbirdNotSupportedError("Unsupported auth method", "0A000");
                }
                case protocol_1.MessageType.AUTH_CONTINUE: {
                    const { method, data } = (0, protocol_1.parseAuthContinue)(msg.payload);
                    if (method === protocol_1.AuthMethod.TOKEN) {
                        const tokenPayload = resolveTokenAuthPayload(this.config);
                        if (!tokenPayload) {
                            throw new errors_1.ScratchbirdAuthError("TOKEN authentication requires authToken, authMethodPayload, authPayloadJson, authPayloadB64, workloadIdentityToken, or proxyPrincipalAssertion", "28000");
                        }
                        await this.protocol.sendMessage(protocol_1.MessageType.AUTH_RESPONSE, tokenPayload, 0, true);
                        continue;
                    }
                    if (method !== protocol_1.AuthMethod.SCRAM_SHA_256 && method !== protocol_1.AuthMethod.SCRAM_SHA_512) {
                        throw new errors_1.ScratchbirdNotSupportedError("Unsupported auth continue", "0A000");
                    }
                    if (!scram) {
                        throw new errors_1.ScratchbirdConnectionError("SCRAM state missing", "08001");
                    }
                    const clientFinal = scram.handleServerFirst(this.config.password ?? "", data.toString("utf8"));
                    await this.protocol.sendMessage(protocol_1.MessageType.AUTH_RESPONSE, Buffer.from(clientFinal, "utf8"), 0, true);
                    continue;
                }
                case protocol_1.MessageType.AUTH_OK: {
                    const { serverInfo } = (0, protocol_1.parseAuthOk)(msg.payload);
                    this.protocol.setAttachment(msg.header.attachmentId, msg.header.txnId);
                    if (scram && serverInfo.length && serverInfo.toString("utf8").startsWith("v=")) {
                        scram.verifyServerFinal(serverInfo.toString("utf8"));
                    }
                    continue;
                }
                case protocol_1.MessageType.READY: {
                    const { status, txnId } = (0, protocol_1.parseReady)(msg.payload);
                    this.applyRuntimeReadyState(status, txnId);
                    if (!this.resolvedAuthContext.resolvedAuthMethod && this.config.dormantId) {
                        this.resolvedAuthContext.resolvedAuthMethod = "REATTACH";
                        this.resolvedAuthContext.resolvedAuthPluginId = authPluginIdForMethod(protocol_1.AuthMethod.REATTACH, this.config.authMethodId);
                    }
                    return;
                }
                case protocol_1.MessageType.ERROR:
                    throw this.raiseProtocolError(msg.payload);
                default:
                    continue;
            }
        }
    }
    async applySchema() {
        const schema = this.config.schema?.trim();
        if (!schema) {
            return;
        }
        const statement = buildSchemaStatement(schema);
        if (!statement) {
            return;
        }
        await this.sendSimpleQuery(statement);
        await this.drainUntilReady();
    }
    async collectResults(pageSize, options) {
        const results = await this.collectResultSets(pageSize, options);
        if (results.length === 0) {
            return this.emptyQueryResult();
        }
        if (results.length === 1) {
            return results[0];
        }
        return this.mergeLegacyResults(results);
    }
    async collectResultSets(pageSize, options) {
        const results = [];
        let columns = [];
        let rows = [];
        let fields = [];
        let rowCount = -1;
        let command = "";
        let lastId = null;
        let hasCurrentResult = false;
        const finalizeResult = () => {
            if (!hasCurrentResult) {
                return;
            }
            results.push({
                rows,
                rowCount: rowCount >= 0 ? rowCount : rows.length,
                fields,
                command,
                lastId,
            });
            columns = [];
            rows = [];
            fields = [];
            rowCount = -1;
            command = "";
            lastId = null;
            hasCurrentResult = false;
        };
        while (true) {
            if (options?.signal?.aborted) {
                await this.cancelQuery();
                throw new errors_1.ScratchbirdError("query canceled", "57014");
            }
            const msg = await this.protocol.recv();
            if (this.handleAsyncMessage(msg)) {
                continue;
            }
            switch (msg.header.type) {
                case protocol_1.MessageType.ERROR:
                    await this.drainReadyAfterError();
                    throw this.raiseProtocolError(msg.payload);
                case protocol_1.MessageType.ROW_DESCRIPTION:
                    columns = (0, protocol_1.parseRowDescription)(msg.payload);
                    fields = columns.map((col) => ({
                        name: col.name,
                        dataType: (0, types_1.oidToString)(col.typeOid),
                        format: col.format === FORMAT_TEXT ? "text" : "binary",
                        nullable: col.nullable,
                        typeOid: col.typeOid,
                        typeModifier: col.typeModifier,
                    }));
                    hasCurrentResult = true;
                    continue;
                case protocol_1.MessageType.DATA_ROW: {
                    const values = (0, protocol_1.parseDataRow)(msg.payload, columns.length);
                    rows.push(buildRow(columns, values));
                    hasCurrentResult = true;
                    continue;
                }
                case protocol_1.MessageType.COMMAND_COMPLETE: {
                    const parsed = (0, protocol_1.parseCommandComplete)(msg.payload);
                    command = parsed.tag;
                    rowCount = Number(parsed.rows);
                    lastId = parsed.lastId;
                    hasCurrentResult = true;
                    finalizeResult();
                    continue;
                }
                case protocol_1.MessageType.EMPTY_QUERY:
                    command = "";
                    rowCount = 0;
                    lastId = null;
                    hasCurrentResult = true;
                    finalizeResult();
                    continue;
                case protocol_1.MessageType.PORTAL_SUSPENDED: {
                    if (pageSize > 0) {
                        this.portalResumePending = true;
                        await this.resumePortal(pageSize);
                    }
                    continue;
                }
                case protocol_1.MessageType.READY: {
                    const { status, txnId } = (0, protocol_1.parseReady)(msg.payload);
                    this.applyRuntimeReadyState(status, txnId);
                    finalizeResult();
                    return results;
                }
                default:
                    continue;
            }
        }
    }
    mergeLegacyResults(results) {
        const mergedRows = [];
        let mergedFields = [];
        for (const result of results) {
            mergedRows.push(...result.rows);
            if (result.fields.length > 0) {
                mergedFields = result.fields;
            }
        }
        const last = results[results.length - 1];
        return {
            rows: mergedRows,
            rowCount: last.rowCount >= 0 ? last.rowCount : mergedRows.length,
            fields: mergedFields,
            command: last.command,
            lastId: last.lastId,
        };
    }
    emptyQueryResult() {
        return {
            rows: [],
            rowCount: 0,
            fields: [],
            command: "",
            lastId: null,
        };
    }
    toBatchSummary(index, result) {
        return {
            index,
            rowCount: result.rowCount,
            fields: result.fields,
            command: result.command,
            lastId: result.lastId,
        };
    }
    handleParameterStatus(name, value) {
        this.parameters[name] = value;
        if (name === "attachment_id") {
            const attachment = parseUuidBytes(value);
            if (attachment) {
                this.protocol.setAttachment(attachment, this.protocol.getTxnId());
            }
        }
        if (name === "current_txn_id") {
            const parsed = parseBigInt(value);
            if (parsed !== null) {
                this.applyRuntimeTxnId(parsed);
            }
        }
    }
    handleAsyncMessage(msg) {
        switch (msg.header.type) {
            case protocol_1.MessageType.PARAMETER_STATUS: {
                const { name, value } = (0, protocol_1.parseParameterStatus)(msg.payload);
                this.handleParameterStatus(name, value);
                return true;
            }
            case protocol_1.MessageType.NOTIFICATION: {
                const notice = (0, protocol_1.parseNotification)(msg.payload);
                for (const handler of this.notificationHandlers) {
                    handler(notice);
                }
                return true;
            }
            case protocol_1.MessageType.QUERY_PLAN: {
                this.lastPlan = (0, protocol_1.parseQueryPlan)(msg.payload);
                return true;
            }
            case protocol_1.MessageType.SBLR_COMPILED: {
                this.lastSblr = (0, protocol_1.parseSblrCompiled)(msg.payload);
                return true;
            }
            case protocol_1.MessageType.TXN_STATUS: {
                const { status, txnId } = (0, protocol_1.parseTxnStatus)(msg.payload);
                if (status === "T") {
                    this.applyRuntimeTxnId(txnId);
                    this.transactionActive = true;
                }
                else {
                    this.clearTransactionState();
                }
                return true;
            }
            default:
                return false;
        }
    }
    async executeQuery(sql, params, options) {
        const pageSize = options?.maxRows ?? 0;
        await this.ensureImplicitTransaction();
        return this.withResilience("query", sql, async () => {
            if (params.length === 0) {
                await this.sendSimpleQuery(sql, options);
            }
            else {
                await this.sendExtendedQuery(sql, params, options);
            }
            return this.collectResults(pageSize, options);
        });
    }
    async executeQueryMulti(sql, params, options) {
        const pageSize = options?.maxRows ?? 0;
        const splitStatements = this.splitExecutableStatements(sql, params);
        await this.ensureImplicitTransaction();
        return this.withResilience("query_multi", sql, async () => {
            if (splitStatements) {
                const results = [];
                for (const statement of splitStatements) {
                    if (statement.params.length === 0) {
                        await this.sendSimpleQuery(statement.sql, options);
                    }
                    else {
                        await this.sendExtendedQuery(statement.sql, statement.params, options);
                    }
                    results.push(...(await this.collectResultSets(pageSize, options)));
                }
                return results;
            }
            if (params.length === 0) {
                await this.sendSimpleQuery(sql, options);
            }
            else {
                await this.sendExtendedQuery(sql, params, options);
            }
            return this.collectResultSets(pageSize, options);
        });
    }
    async executePrepared(name, params, options) {
        const pageSize = options?.maxRows ?? 0;
        const prepared = this.prepared.get(name);
        await this.ensureImplicitTransaction();
        return this.withResilience("execute_prepared", prepared?.sql, async () => {
            await this.sendBindExecute(name, params, options);
            return this.collectResults(pageSize, options);
        });
    }
    async executePreparedMulti(name, params, options) {
        const pageSize = options?.maxRows ?? 0;
        const prepared = this.prepared.get(name);
        const splitStatements = prepared ? this.splitExecutableStatements(prepared.sql, params) : null;
        await this.ensureImplicitTransaction();
        return this.withResilience("execute_prepared_multi", prepared?.sql, async () => {
            if (splitStatements) {
                const results = [];
                for (const statement of splitStatements) {
                    await this.sendExtendedQuery(statement.sql, statement.params, options);
                    results.push(...(await this.collectResultSets(pageSize, options)));
                }
                return results;
            }
            await this.sendBindExecute(name, params, options);
            return this.collectResultSets(pageSize, options);
        });
    }
    splitExecutableStatements(sql, params) {
        const statements = (0, sql_1.splitTopLevelStatements)(sql);
        if (statements.length <= 1) {
            return null;
        }
        return statements.map((statement) => this.remapStatementParams(statement, params));
    }
    remapStatementParams(sql, params) {
        if (params.length === 0) {
            return { sql, params: [] };
        }
        let result = "";
        let inSingle = false;
        let inDouble = false;
        const remap = new Map();
        const ordered = [];
        for (let i = 0; i < sql.length;) {
            const ch = sql[i];
            if (ch === "'" && !inDouble) {
                inSingle = !inSingle;
                result += ch;
                i++;
                continue;
            }
            if (ch === '"' && !inSingle) {
                inDouble = !inDouble;
                result += ch;
                i++;
                continue;
            }
            if (!inSingle && !inDouble && ch === "$" && i + 1 < sql.length && /\d/.test(sql[i + 1])) {
                let j = i + 1;
                while (j < sql.length && /\d/.test(sql[j]))
                    j++;
                const originalIndex = Number(sql.slice(i + 1, j));
                if (!remap.has(originalIndex)) {
                    remap.set(originalIndex, ordered.length + 1);
                    ordered.push(originalIndex);
                }
                result += `$${remap.get(originalIndex)}`;
                i = j;
                continue;
            }
            result += ch;
            i++;
        }
        const remappedParams = ordered.map((originalIndex) => {
            if (originalIndex < 1 || originalIndex > params.length) {
                throw new errors_1.ScratchbirdError("parameter count mismatch", "07001");
            }
            return params[originalIndex - 1];
        });
        return { sql: result, params: remappedParams };
    }
    async executeQueryStream(sql, params, options) {
        const pageSize = options?.maxRows ?? 0;
        await this.ensureImplicitTransaction();
        if (!this.circuitBreaker.allowRequest()) {
            throw new errors_1.ScratchbirdError("Circuit breaker is OPEN", "08006");
        }
        await this.validateIfIdle();
        const span = this.telemetry.startSpan("query_stream");
        if (span) {
            span.withAttribute("db.statement", telemetry_1.TelemetryCollector.sanitizeQuery(sql));
        }
        try {
            if (params.length === 0) {
                await this.sendSimpleQuery(sql, options);
            }
            else {
                await this.sendExtendedQuery(sql, params, options);
            }
        }
        catch (err) {
            this.finishOperation(span, false, err);
            throw err;
        }
        const self = this;
        async function* iterator() {
            let columns = [];
            let success = false;
            let operationError;
            try {
                while (true) {
                    if (options?.signal?.aborted) {
                        await self.cancelQuery();
                        throw new errors_1.ScratchbirdError("query canceled", "57014");
                    }
                    const msg = await self.protocol.recv();
                    if (self.handleAsyncMessage(msg)) {
                        continue;
                    }
                    switch (msg.header.type) {
                        case protocol_1.MessageType.ERROR:
                            await self.drainReadyAfterError();
                            throw self.raiseProtocolError(msg.payload);
                        case protocol_1.MessageType.ROW_DESCRIPTION:
                            columns = (0, protocol_1.parseRowDescription)(msg.payload);
                            continue;
                        case protocol_1.MessageType.DATA_ROW: {
                            const values = (0, protocol_1.parseDataRow)(msg.payload, columns.length);
                            yield buildRow(columns, values);
                            continue;
                        }
                        case protocol_1.MessageType.PORTAL_SUSPENDED: {
                            if (pageSize > 0) {
                                self.portalResumePending = true;
                                await self.resumePortal(pageSize);
                            }
                            continue;
                        }
                        case protocol_1.MessageType.READY: {
                            const { status, txnId } = (0, protocol_1.parseReady)(msg.payload);
                            self.applyRuntimeReadyState(status, txnId);
                            success = true;
                            return;
                        }
                        default:
                            continue;
                    }
                }
            }
            catch (err) {
                operationError = err;
                throw err;
            }
            finally {
                self.finishOperation(span, success, operationError);
            }
        }
        return iterator();
    }
    async sendSimpleQuery(sql, options) {
        let flags = this.config.binaryTransfer ? QUERY_FLAG_BINARY_RESULT : 0;
        if (options?.includePlan)
            flags |= protocol_1.QUERY_FLAG_INCLUDE_PLAN;
        if (options?.returnSblr)
            flags |= protocol_1.QUERY_FLAG_RETURN_SBLR;
        if (options?.describeOnly)
            flags |= protocol_1.QUERY_FLAG_DESCRIBE_ONLY;
        if (options?.noCache)
            flags |= protocol_1.QUERY_FLAG_NO_CACHE;
        const maxRows = options?.maxRows ?? 0;
        const timeoutMs = options?.timeoutMs ?? 0;
        const payload = (0, protocol_1.buildQueryPayload)(sql, flags, maxRows, timeoutMs);
        await this.protocol.sendMessage(protocol_1.MessageType.QUERY, payload, 0, false);
    }
    async sendExtendedQuery(sql, params, options) {
        const paramValues = [];
        const paramTypes = [];
        for (const param of params) {
            const encoded = (0, types_1.encodeParam)(param);
            paramValues.push(encoded.param);
            paramTypes.push(encoded.oid);
        }
        const parsePayload = (0, protocol_1.buildParsePayload)("", sql, paramTypes);
        await this.protocol.sendMessage(protocol_1.MessageType.PARSE, parsePayload, 0, false);
        const paramCount = await this.describeStatement("");
        if (paramCount >= 0 && paramCount !== params.length) {
            throw new errors_1.ScratchbirdError("parameter count mismatch", "07001");
        }
        const resultFormats = this.config.binaryTransfer ? [types_1.FORMAT_BINARY] : [];
        const bindPayload = (0, protocol_1.buildBindPayload)("", "", paramValues, resultFormats);
        await this.protocol.sendMessage(protocol_1.MessageType.BIND, bindPayload, 0, false);
        const maxRows = options?.maxRows ?? 0;
        const execPayload = (0, protocol_1.buildExecutePayload)("", maxRows);
        await this.protocol.sendMessage(protocol_1.MessageType.EXECUTE, execPayload, 0, false);
        if (maxRows === 0) {
            await this.protocol.sendMessage(protocol_1.MessageType.SYNC, Buffer.alloc(0), 0, false);
        }
    }
    async sendBindExecute(statementName, params, options) {
        const paramValues = [];
        for (const param of params) {
            const encoded = (0, types_1.encodeParam)(param);
            paramValues.push(encoded.param);
        }
        const resultFormats = this.config.binaryTransfer ? [types_1.FORMAT_BINARY] : [];
        const bindPayload = (0, protocol_1.buildBindPayload)("", statementName, paramValues, resultFormats);
        await this.protocol.sendMessage(protocol_1.MessageType.BIND, bindPayload, 0, false);
        const maxRows = options?.maxRows ?? 0;
        const execPayload = (0, protocol_1.buildExecutePayload)("", maxRows);
        await this.protocol.sendMessage(protocol_1.MessageType.EXECUTE, execPayload, 0, false);
        if (maxRows === 0) {
            await this.protocol.sendMessage(protocol_1.MessageType.SYNC, Buffer.alloc(0), 0, false);
        }
    }
    async resumePortal(maxRows) {
        if (!this.portalResumePending) {
            throw new errors_1.ScratchbirdError("portal resume requires explicit suspended state", "55000");
        }
        this.portalResumePending = false;
        const execPayload = (0, protocol_1.buildExecutePayload)("", maxRows);
        await this.protocol.sendMessage(protocol_1.MessageType.EXECUTE, execPayload, 0, false);
    }
    async describeStatement(statementName) {
        const describePayload = (0, protocol_1.buildDescribePayload)("S".charCodeAt(0), statementName);
        await this.protocol.sendMessage(protocol_1.MessageType.DESCRIBE, describePayload, 0, false);
        await this.protocol.sendMessage(protocol_1.MessageType.SYNC, Buffer.alloc(0), 0, false);
        let paramCount = -1;
        while (true) {
            const msg = await this.protocol.recv();
            if (this.handleAsyncMessage(msg)) {
                continue;
            }
            switch (msg.header.type) {
                case protocol_1.MessageType.ERROR:
                    await this.drainReadyAfterError();
                    throw this.raiseProtocolError(msg.payload);
                case protocol_1.MessageType.PARAMETER_DESCRIPTION:
                    paramCount = (0, protocol_1.parseParameterDescription)(msg.payload).length;
                    continue;
                case protocol_1.MessageType.READY: {
                    const { status, txnId } = (0, protocol_1.parseReady)(msg.payload);
                    this.applyRuntimeReadyState(status, txnId);
                    return paramCount;
                }
                default:
                    continue;
            }
        }
    }
    async cancelQuery() {
        await this.protocol.sendMessage(protocol_1.MessageType.CANCEL, (0, protocol_1.buildCancelPayload)(0, 0), protocol_1.MSG_FLAG_URGENT, false);
    }
    async drainUntilReady() {
        while (true) {
            const msg = await this.protocol.recv();
            if (this.handleAsyncMessage(msg)) {
                continue;
            }
            switch (msg.header.type) {
                case protocol_1.MessageType.ERROR: {
                    const error = this.raiseProtocolError(msg.payload);
                    await this.drainReadyAfterError();
                    throw error;
                }
                case protocol_1.MessageType.READY: {
                    const { status, txnId } = (0, protocol_1.parseReady)(msg.payload);
                    this.applyRuntimeReadyState(status, txnId);
                    return;
                }
                default:
                    continue;
            }
        }
    }
    async drainReadyAfterError() {
        while (true) {
            const msg = await this.protocol.recv();
            if (this.handleAsyncMessage(msg)) {
                continue;
            }
            if (msg.header.type === protocol_1.MessageType.READY) {
                const { status, txnId } = (0, protocol_1.parseReady)(msg.payload);
                this.applyRuntimeReadyState(status, txnId);
                return;
            }
            if (msg.header.type === protocol_1.MessageType.ERROR) {
                continue;
            }
        }
    }
    applyRuntimeTxnId(txnId) {
        this.protocol.setTxnId(txnId);
        if (txnId > 0n) {
            this.transactionActive = true;
        }
    }
    applyRuntimeReadyState(status, txnId) {
        this.protocol.setTxnId(txnId);
        if (status !== 0) {
            // READY is authoritative for native session activity. Live listeners
            // also publish `current_txn_id`, so ScratchBird stays
            // always-in-transaction even as COMMIT / ROLLBACK reopen the next
            // boundary.
            this.transactionActive = true;
            return;
        }
        this.clearTransactionState();
    }
    clearTransactionState() {
        this.protocol.setTxnId(0n);
        this.transactionActive = false;
    }
    buildPreparedTransactionSql(verb, gid) {
        const normalized = gid.trim();
        if (!normalized) {
            throw new errors_1.ScratchbirdSyntaxError("global transaction id is required", "42601");
        }
        return `${verb} '${normalized.replace(/'/g, "''")}'`;
    }
    normalizeQueryOrThrow(text, params) {
        try {
            return (0, sql_1.normalizeQuery)(text, params);
        }
        catch (err) {
            throw this.wrapNormalizationError(err);
        }
    }
    normalizePreparedParamsOrThrow(prepared, params) {
        try {
            if (prepared.namedOrder?.length) {
                if (params === undefined) {
                    return { sql: prepared.sql, params: [] };
                }
                if (Array.isArray(params)) {
                    return { sql: prepared.sql, params };
                }
                const lookup = {};
                for (const [key, value] of Object.entries(params)) {
                    lookup[key.replace(/^[@:]/, "")] = value;
                }
                return {
                    sql: prepared.sql,
                    params: prepared.namedOrder.map((key) => {
                        if (!(key in lookup)) {
                            throw new Error(`missing named parameter: ${key}`);
                        }
                        return lookup[key];
                    }),
                };
            }
            if (params === undefined) {
                return { sql: prepared.sql, params: [] };
            }
            if (Array.isArray(params)) {
                return { sql: prepared.sql, params };
            }
            throw new Error("named parameters provided but prepared statement uses positional parameters");
        }
        catch (err) {
            throw this.wrapNormalizationError(err);
        }
    }
    normalizeCallableQueryOrThrow(text, params) {
        try {
            return (0, sql_1.normalizeCallableQuery)(text, params);
        }
        catch (err) {
            throw this.wrapNormalizationError(err);
        }
    }
    wrapNormalizationError(err) {
        const message = err instanceof Error ? err.message : String(err);
        return new errors_1.ScratchbirdSyntaxError(message, "07001");
    }
    raiseProtocolError(payload) {
        try {
            const { sqlState, message, detail, hint } = (0, protocol_1.parseErrorMessage)(payload);
            const ErrorClass = (0, errors_1.mapSqlState)(sqlState);
            const full = [message, detail ? `DETAIL: ${detail}` : "", hint ? `HINT: ${hint}` : ""]
                .filter(Boolean)
                .join("\n");
            return new ErrorClass(full || "query failed", sqlState, detail, hint);
        }
        catch {
            return new errors_1.ScratchbirdError("query failed");
        }
    }
}
exports.Client = Client;
class Pool {
    constructor(config) {
        this.active = 0;
        this.idle = [];
        this.waiters = [];
        const parsed = typeof config === "string" ? (0, dsn_1.parseDsn)(config) : {};
        const merged = { ...parsed, ...(typeof config === "object" ? config : {}) };
        this.config = merged;
        this.max = merged.max ?? 10;
        this.idleTimeoutMs = merged.idleTimeoutMs ?? 30000;
    }
    async connect() {
        const cached = this.idle.pop();
        if (cached) {
            return this.wrapClient(cached.client);
        }
        if (this.active < this.max) {
            this.active++;
            const client = new Client(this.config);
            await client.connect();
            return this.wrapClient(client);
        }
        return new Promise((resolve) => {
            this.waiters.push(resolve);
        });
    }
    async query(text, params, options) {
        const client = await this.connect();
        try {
            return await client.query(text, params, options);
        }
        finally {
            await client.release();
        }
    }
    async end() {
        for (const item of this.idle) {
            await item.client.end();
        }
        this.idle = [];
        this.active = 0;
    }
    wrapClient(client) {
        const pool = this;
        const release = async () => {
            const now = Date.now();
            pool.idle.push({ client, lastUsed: now });
            pool.cleanup();
            const waiter = pool.waiters.shift();
            if (waiter) {
                const next = pool.idle.pop();
                if (next) {
                    waiter(pool.wrapClient(next.client));
                }
            }
        };
        client.release = release;
        return client;
    }
    cleanup() {
        const cutoff = Date.now() - this.idleTimeoutMs;
        const remaining = [];
        for (const item of this.idle) {
            if (item.lastUsed < cutoff) {
                item.client.end();
                this.active = Math.max(0, this.active - 1);
            }
            else {
                remaining.push(item);
            }
        }
        this.idle = remaining;
    }
}
exports.Pool = Pool;
function parseUuidBytes(value) {
    const hex = value.replace(/-/g, "").trim();
    if (!/^[0-9a-fA-F]{32}$/.test(hex)) {
        return null;
    }
    return Buffer.from(hex, "hex");
}
function normalizeUuidText(value, label) {
    const parsed = parseUuidBytes(value);
    if (!parsed) {
        throw new errors_1.ScratchbirdSyntaxError(`${label} must be a UUID`, "42601");
    }
    const hex = parsed.toString("hex");
    return [
        hex.slice(0, 8),
        hex.slice(8, 12),
        hex.slice(12, 16),
        hex.slice(16, 20),
        hex.slice(20),
    ].join("-");
}
function parseBigInt(value) {
    try {
        return BigInt(value.trim());
    }
    catch {
        return null;
    }
}
function buildRow(columns, values) {
    const row = {};
    const limit = Math.min(values.length, columns.length);
    for (let i = 0; i < limit; i++) {
        const column = columns[i];
        const data = values[i];
        const decoded = (0, types_1.decodeValue)(column.typeOid, data.data, column.format);
        if (column?.name) {
            row[column.name] = decoded;
        }
        else {
            row[i] = decoded;
        }
    }
    return row;
}
function buildSchemaStatement(schema) {
    const trimmed = schema.trim();
    if (!trimmed) {
        return "";
    }
    if (trimmed.includes(",")) {
        const parts = trimmed
            .split(",")
            .map((part) => part.trim())
            .filter((part) => part.length > 0)
            .map((part) => quoteIdentifier(part));
        if (!parts.length) {
            return "";
        }
        return `SET SEARCH_PATH TO ${parts.join(", ")}`;
    }
    return `SET SCHEMA ${quoteIdentifier(trimmed)}`;
}
function normalizeSessionSchema(schema) {
    if (schema === undefined || schema === null) {
        return null;
    }
    const trimmed = schema.trim();
    if (!trimmed.length) {
        return null;
    }
    if (trimmed.toLowerCase() === "public") {
        return DEFAULT_SESSION_SCHEMA;
    }
    return trimmed;
}
function quoteIdentifier(name) {
    return `"${name.replace(/"/g, "\"\"")}"`;
}
function metadataRestrictions(restrictions) {
    const filtered = Object.fromEntries(Object.entries(restrictions).filter(([, value]) => value !== undefined && value !== null));
    return Object.keys(filtered).length ? filtered : undefined;
}
function resolveSslMode(config) {
    if (config.ssl === false)
        return "disable";
    if (typeof config.ssl === "object")
        return config.sslmode ?? "require";
    if (config.ssl === true)
        return config.sslmode ?? "require";
    return config.sslmode ?? "require";
}
function normalizeTransportMode(value) {
    const normalized = (value ?? "inet_listener").trim().toLowerCase().replace(/-/g, "_");
    if (normalized === "local" || normalized === "ipc")
        return "local_ipc";
    return normalized || "inet_listener";
}
async function connectLocalIpc(path, timeoutMs) {
    if (!path?.trim()) {
        throw new Error("ipc_path is required for local_ipc");
    }
    return new Promise((resolve, reject) => {
        const socket = node_net_1.default.createConnection({ path });
        socket.setKeepAlive(true);
        const timer = setTimeout(() => {
            socket.destroy();
            reject(new Error("Connection timeout"));
        }, timeoutMs);
        socket.once("error", (err) => {
            clearTimeout(timer);
            reject(err);
        });
        socket.once("connect", () => {
            clearTimeout(timer);
            resolve(socket);
        });
    });
}
async function connectTcp(host, port, timeoutMs) {
    return new Promise((resolve, reject) => {
        const socket = node_net_1.default.connect({ host, port });
        socket.setNoDelay(true);
        socket.setKeepAlive(true);
        const timer = setTimeout(() => {
            socket.destroy();
            reject(new Error("Connection timeout"));
        }, timeoutMs);
        socket.once("error", (err) => {
            clearTimeout(timer);
            reject(err);
        });
        socket.once("connect", () => {
            clearTimeout(timer);
            resolve(socket);
        });
    });
}
async function upgradeTls(socket, host, sslMode, config) {
    const rejectUnauthorized = sslMode === "verify-ca" || sslMode === "verify-full";
    const tlsOptions = {
        socket,
        servername: host,
        rejectUnauthorized,
        minVersion: "TLSv1.3",
        maxVersion: "TLSv1.3",
    };
    if (config.sslrootcert) {
        tlsOptions.ca = node_fs_1.default.readFileSync(config.sslrootcert);
    }
    if (config.sslcert) {
        tlsOptions.cert = node_fs_1.default.readFileSync(config.sslcert);
    }
    if (config.sslkey) {
        tlsOptions.key = node_fs_1.default.readFileSync(config.sslkey);
    }
    if (config.sslpassword) {
        tlsOptions.passphrase = config.sslpassword;
    }
    if (typeof config.ssl === "object") {
        Object.assign(tlsOptions, config.ssl);
    }
    const tlsSocket = node_tls_1.default.connect(tlsOptions);
    return new Promise((resolve, reject) => {
        tlsSocket.once("secureConnect", () => resolve(tlsSocket));
        tlsSocket.once("error", (err) => reject(err));
    });
}
