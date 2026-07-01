// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import net from "node:net";
import tls from "node:tls";
import fs from "node:fs";
import { randomBytes, randomUUID } from "node:crypto";
import {
  AuthMethod,
  MessageType,
  MSG_FLAG_URGENT,
  FEATURE_COMPRESSION,
  FEATURE_STREAMING,
  HEADER_SIZE,
  QUERY_FLAG_DESCRIBE_ONLY,
  QUERY_FLAG_INCLUDE_PLAN,
  QUERY_FLAG_RETURN_SBLR,
  QUERY_FLAG_NO_CACHE,
  ISOLATION_READ_COMMITTED,
  ISOLATION_READ_UNCOMMITTED,
  ISOLATION_REPEATABLE_READ,
  ISOLATION_SERIALIZABLE,
  READ_COMMITTED_MODE_DEFAULT,
  TXN_FLAG_HAS_ACCESS,
  TXN_FLAG_HAS_AUTOCOMMIT,
  TXN_FLAG_HAS_DEFERRABLE,
  TXN_FLAG_HAS_ISOLATION,
  TXN_FLAG_HAS_READ_COMMITTED_MODE,
  TXN_FLAG_HAS_TIMEOUT,
  TXN_FLAG_HAS_WAIT,
  buildStartupPayload,
  buildQueryPayload,
  buildParsePayload,
  buildBindPayload,
  buildExecutePayload,
  buildCancelPayload,
  buildSblrExecutePayload,
  buildDescribePayload,
  buildSubscribePayload,
  buildUnsubscribePayload,
  buildTxnBeginPayload,
  buildTxnCommitPayload,
  buildTxnRollbackPayload,
  buildTxnSavepointPayload,
  buildTxnReleasePayload,
  buildTxnRollbackToPayload,
  buildSetOptionPayload,
  buildStreamControlPayload,
  buildAttachCreatePayload,
  encodeMessage,
  decodeHeader,
  parseAuthRequest,
  parseAuthContinue,
  parseAuthOk,
  parseReady,
  parseTxnStatus,
  parseParameterStatus,
  parseParameterDescription,
  parseRowDescription,
  parseDataRow,
  parseCommandComplete,
  parseNotification,
  parseQueryPlan,
  parseSblrCompiled,
  parseErrorMessage,
  applyAuthPluginSelection,
  MessageHeader,
  NotificationMessage,
  QueryPlanMessage,
  SblrCompiledMessage,
} from "./protocol";
import { ScramExchange } from "./scram";
import { parseDsn, normalizeFrontDoorMode, normalizeNativeProtocol } from "./dsn";
import { normalizeCallableQuery, normalizePreparedQuery, normalizeQuery, splitTopLevelStatements } from "./sql";
import {
  AuthMethodSurface,
  AuthProbeResult,
  BootstrapAuthMethod,
  BootstrapIngressMode,
  BatchItemResult,
  BatchResult,
  ClientConfig,
  FieldDef,
  QueryResult,
  ParamValue,
  ResolvedAuthContext,
  FORMAT_BINARY,
  oidToString,
  encodeParam,
  decodeValue,
} from "./types";
import {
  ScratchbirdAuthError,
  mapSqlState,
  retryScopeForSqlState,
  ScratchbirdConnectionError,
  ScratchbirdError,
  ScratchbirdNotSupportedError,
  ScratchbirdSyntaxError,
} from "./errors";
import { CircuitBreaker } from "./circuit_breaker";
import { KeepaliveManager, KeepaliveTracker } from "./keepalive";
import { LeakDetector, LeakDetectionGuard } from "./leak_detector";
import { TelemetryCollector, SpanContext } from "./telemetry";
import {
  MetadataCollectionName,
  MetadataRestrictions,
  MetadataSchemaTree,
  MetadataSchemaTreeOptions,
  buildMetadataSchemaTree,
  expandSchemaMetadataRows,
  filterMetadataRowsForCollectionFamily,
  filterMetadataRowsByRestrictions,
  normalizeMetadataCollectionName,
  resolveMetadataCollectionQuery,
  shapeMetadataRowsForCollection,
} from "./metadata";

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

const DEFAULT_AUTH_PLUGIN_IDS: Record<number, string> = {
  [AuthMethod.PASSWORD]: "scratchbird.auth.password_compat",
  [AuthMethod.MD5]: "scratchbird.auth.md5_legacy",
  [AuthMethod.SCRAM_SHA_256]: "scratchbird.auth.scram_sha_256",
  [AuthMethod.SCRAM_SHA_512]: "scratchbird.auth.scram_sha_512",
  [AuthMethod.TOKEN]: "scratchbird.auth.authkey_token",
  [AuthMethod.PEER]: "scratchbird.auth.peer_uid",
  [AuthMethod.REATTACH]: "scratchbird.auth.reattach",
};

function authMethodName(method: number): BootstrapAuthMethod | null {
  switch (method) {
    case AuthMethod.PASSWORD:
      return "PASSWORD";
    case AuthMethod.MD5:
      return "MD5";
    case AuthMethod.SCRAM_SHA_256:
      return "SCRAM_SHA_256";
    case AuthMethod.SCRAM_SHA_512:
      return "SCRAM_SHA_512";
    case AuthMethod.TOKEN:
      return "TOKEN";
    case AuthMethod.PEER:
      return "PEER";
    case AuthMethod.REATTACH:
      return "REATTACH";
    default:
      return null;
  }
}

function authPluginIdForMethod(method: number, configuredMethodId?: string): string | null {
  if (configuredMethodId?.trim()) {
    return configuredMethodId.trim();
  }
  return DEFAULT_AUTH_PLUGIN_IDS[method] ?? null;
}

function executableLocally(method: number): boolean {
  return (
    method === AuthMethod.PASSWORD ||
    method === AuthMethod.SCRAM_SHA_256 ||
    method === AuthMethod.SCRAM_SHA_512 ||
    method === AuthMethod.TOKEN
  );
}

function brokerRequired(method: number): boolean {
  return method === AuthMethod.PEER;
}

function describeAuthMethod(method: number, configuredMethodId?: string): AuthMethodSurface | null {
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

function resolveTokenAuthPayload(config: ClientConfig): Buffer | null {
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

interface Message {
  header: MessageHeader;
  payload: Buffer;
}

interface QueryOptions {
  signal?: AbortSignal;
  maxRows?: number;
  timeoutMs?: number;
  includePlan?: boolean;
  returnSblr?: boolean;
  describeOnly?: boolean;
  noCache?: boolean;
}

interface TxnBeginOptions {
  // Public isolation aliases map onto the canonical MGA modes:
  // READ COMMITTED => READ COMMITTED
  // REPEATABLE READ => SNAPSHOT
  // SERIALIZABLE => SNAPSHOT TABLE STABILITY
  // readCommittedMode adds the canonical READ COMMITTED sub-mode selector.
  isolationLevel?: number;
  readCommittedMode?: number;
  accessMode?: number;
  deferrable?: boolean;
  wait?: boolean;
  timeoutMs?: number;
  autocommitMode?: number;
  conflictAction?: number;
}

interface TxnEndOptions {
  flags?: number;
}

interface SubscribeOptions {
  type?: number;
  filter?: string;
}

class SocketReader {
  private buffer: Buffer = Buffer.alloc(0);
  private pending: Array<{ len: number; resolve: (buf: Buffer) => void; reject: (err: Error) => void }> = [];
  private closed = false;

  constructor(private socket: net.Socket) {
    socket.on("data", (chunk) => this.onData(chunk));
    socket.on("error", (err) => this.fail(err instanceof Error ? err : new Error(String(err))));
    socket.on("close", () => this.fail(new Error("Connection closed")));
  }

  readExact(len: number): Promise<Buffer> {
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

  private onData(chunk: Buffer): void {
    this.buffer = this.buffer.length ? (Buffer.concat([this.buffer, chunk]) as Buffer) : chunk;
    this.flush();
  }

  private flush(): void {
    while (this.pending.length && this.buffer.length >= this.pending[0].len) {
      const next = this.pending.shift();
      if (!next) break;
      const out = this.buffer.subarray(0, next.len);
      this.buffer = this.buffer.subarray(next.len);
      next.resolve(out);
    }
  }

  private fail(err: Error): void {
    if (this.closed) return;
    this.closed = true;
    while (this.pending.length) {
      const next = this.pending.shift();
      if (next) next.reject(err);
    }
  }
}

class ProtocolConnection {
  private socket?: net.Socket;
  private reader?: SocketReader;
  private attachmentId = Buffer.alloc(16);
  private txnId = 0n;
  private sequence = 0;

  async connect(config: ClientConfig): Promise<void> {
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

  setAttachment(id: Buffer, txnId: bigint): void {
    this.attachmentId = Buffer.from(id);
    this.txnId = txnId;
  }

  setTxnId(txnId: bigint): void {
    this.txnId = txnId;
  }

  getTxnId(): bigint {
    return this.txnId;
  }

  async sendMessage(type: number, payload: Buffer, flags: number, forceZero: boolean): Promise<number> {
    if (!this.socket) throw new Error("Socket not connected");
    const seq = this.sequence++;
    const header: MessageHeader = {
      type,
      flags,
      length: payload.length,
      sequence: seq,
      attachmentId: forceZero ? Buffer.alloc(16) : this.attachmentId,
      txnId: forceZero ? 0n : this.txnId,
    };
    const data = encodeMessage(header, payload);
    await this.sendRaw(data);
    return seq;
  }

  private async sendRaw(data: Buffer): Promise<void> {
    if (!this.socket) throw new Error("Socket not connected");
    await new Promise<void>((resolve, reject) => {
      this.socket!.write(data, (err) => {
        if (err) reject(err);
        else resolve();
      });
    });
  }

  async recv(): Promise<Message> {
    if (!this.reader) throw new Error("Socket not connected");
    const headerBuf = await this.reader.readExact(HEADER_SIZE);
    const header = decodeHeader(headerBuf);
    const payload = header.length ? await this.reader.readExact(header.length) : Buffer.alloc(0);
    return { header, payload };
  }

  async sendManagerFrame(type: number, payload: Buffer): Promise<void> {
    const header = Buffer.alloc(MANAGER_HEADER_SIZE);
    header.writeUInt32LE(MANAGER_PROTOCOL_MAGIC, 0);
    header.writeUInt16LE(MANAGER_PROTOCOL_VERSION, 4);
    header.writeUInt8(type, 6);
    header.writeUInt8(0, 7);
    header.writeUInt32LE(payload.length, 8);
    await this.sendRaw(Buffer.concat([header, payload]));
  }

  async recvManagerFrame(): Promise<{ type: number; payload: Buffer }> {
    if (!this.reader) throw new Error("Socket not connected");
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

  close(): void {
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

export class Client {
  private config: ClientConfig;
  private protocol = new ProtocolConnection();
  private connected = false;
  private transactionActive = false;
  private portalResumePending = false;
  private autoCommit = true;
  private sessionSchema: string | null = null;
  private prepared = new Map<string, { sql: string; paramCount: number; namedOrder: string[] | null }>();
  private parameters: Record<string, string> = {};
  private notificationHandlers: Array<(notice: NotificationMessage) => void> = [];
  private lastPlan?: QueryPlanMessage;
  private lastSblr?: SblrCompiledMessage;
  private readonly connectionId = randomUUID();
  private readonly circuitBreaker = new CircuitBreaker({}, "node");
  private readonly telemetry = new TelemetryCollector();
  private readonly keepaliveManager = new KeepaliveManager();
  private keepaliveTracker?: KeepaliveTracker;
  private readonly leakDetector = new LeakDetector();
  private leakGuard?: LeakDetectionGuard;
  private skipSchemaApplyOnce = false;
  private resolvedAuthContext: ResolvedAuthContext = {
    ingressMode: "direct",
    resolvedAuthMethod: null,
    resolvedAuthPluginId: null,
    managerAuthenticated: false,
    attached: false,
  };

  constructor(config?: ClientConfig | string) {
    const parsed = typeof config === "string" ? parseDsn(config) : {};
    this.config = { ...parsed, ...(typeof config === "object" ? config : {}) };
    this.config.protocol = normalizeNativeProtocol(this.config.protocol ?? this.config.parser ?? this.config.dialect);
    this.config.frontDoorMode = normalizeFrontDoorMode(this.config.frontDoorMode);
    if (!this.config.host) this.config.host = "localhost";
    if (!this.config.port) this.config.port = 3092;
    if (!this.config.applicationName) this.config.applicationName = "scratchbird_node";
    if (!this.config.sslmode) this.config.sslmode = "require";
    if (this.config.binaryTransfer === undefined) this.config.binaryTransfer = true;
    if (!this.config.compression) this.config.compression = "off";
    if (this.config.metadataExpandSchemaParents === undefined) this.config.metadataExpandSchemaParents = false;
    if (!this.config.managerConnectionProfile) this.config.managerConnectionProfile = "SBsql";
    if (!this.config.managerClientIntent) this.config.managerClientIntent = "SBsql";
    if (this.config.managerClientFlags === undefined) this.config.managerClientFlags = 0;
    if (this.config.managerAuthFastPath === undefined) this.config.managerAuthFastPath = true;
    if (this.config.connectClientFlags === undefined) this.config.connectClientFlags = 0x0100;
    this.sessionSchema = normalizeSessionSchema(this.config.schema);
    this.resolvedAuthContext.ingressMode = this.config.frontDoorMode as BootstrapIngressMode;
  }

  getResolvedAuthContext(): ResolvedAuthContext {
    return { ...this.resolvedAuthContext };
  }

  async probeAuthSurface(): Promise<AuthProbeResult> {
    this.config.protocol = normalizeNativeProtocol(this.config.protocol ?? this.config.parser ?? this.config.dialect);
    this.config.frontDoorMode = normalizeFrontDoorMode(this.config.frontDoorMode);
    const resolvedHost = this.config.host ?? "localhost";
    const resolvedPort = this.config.port ?? 3092;

    this.protocol.close();
    await this.protocol.connect(this.config);
    try {
      if (this.config.frontDoorMode === "manager_proxy") {
        return await this.probeManagerAuthSurface(resolvedHost, resolvedPort);
      }
      return await this.probeDirectAuthSurface(resolvedHost, resolvedPort);
    } finally {
      this.protocol.close();
    }
  }

  private resetResolvedAuthContext(): void {
    this.resolvedAuthContext = {
      ingressMode: (this.config.frontDoorMode as BootstrapIngressMode) ?? "direct",
      resolvedAuthMethod: null,
      resolvedAuthPluginId: null,
      managerAuthenticated: false,
      attached: false,
    };
  }

  private buildStartupParams(): Record<string, string> {
    const params: Record<string, string> = {
      database: this.config.database ?? "",
      user: this.config.user ?? "",
      client_flags: String(this.config.connectClientFlags ?? 0x0100),
    };
    if (!!this.config.dormantId !== !!this.config.dormantReattachToken) {
      throw new ScratchbirdSyntaxError(
        "dormantId and dormantReattachToken must be provided together",
        "42601",
      );
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
    applyAuthPluginSelection(params, {
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

  private async probeDirectAuthSurface(resolvedHost: string, resolvedPort: number): Promise<AuthProbeResult> {
    const startup = buildStartupPayload(this.requestedFeatures(), this.buildStartupParams());
    await this.protocol.sendMessage(MessageType.STARTUP, startup, 0, true);

    while (true) {
      const msg = await this.protocol.recv();
      if (this.handleAsyncMessage(msg)) {
        continue;
      }
      switch (msg.header.type) {
        case MessageType.NEGOTIATE_VERSION:
          continue;
        case MessageType.AUTH_REQUEST: {
          const { method } = parseAuthRequest(msg.payload);
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
            additionalContinuationPossible:
              method === AuthMethod.SCRAM_SHA_256 ||
              method === AuthMethod.SCRAM_SHA_512 ||
              method === AuthMethod.TOKEN ||
              method === AuthMethod.PEER,
          };
        }
        case MessageType.AUTH_OK:
        case MessageType.READY:
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
        case MessageType.ERROR:
          throw this.raiseProtocolError(msg.payload);
        default:
          continue;
      }
    }
  }

  private async probeManagerAuthSurface(resolvedHost: string, resolvedPort: number): Promise<AuthProbeResult> {
    const managerUser = this.config.managerUsername || this.config.user || "admin";
    const hello = Buffer.alloc(4);
    hello.writeUInt16LE(MCP_PROTOCOL_VERSION, 0);
    hello.writeUInt16LE((this.config.managerClientFlags ?? 0) & 0xffff, 2);
    await this.protocol.sendManagerFrame(MCP_MSG_HELLO, hello);
    let frame = await this.protocol.recvManagerFrame();
    if (frame.type !== MCP_MSG_STATUS_RESPONSE) {
      throw new ScratchbirdConnectionError("expected MCP hello status response", "08P01");
    }

    const authStartParts: Buffer[] = [];
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

  async connect(): Promise<void> {
    this.config.protocol = normalizeNativeProtocol(this.config.protocol ?? this.config.parser ?? this.config.dialect);
    this.config.frontDoorMode = normalizeFrontDoorMode(this.config.frontDoorMode);
    if (!this.config.user || !this.config.database) {
      throw new Error("user and database are required");
    }
    if (this.config.binaryTransfer === false) {
      throw new ScratchbirdNotSupportedError("binary_transfer=false is not supported", "0A000");
    }
    if (this.config.compression === "zstd") {
      throw new ScratchbirdNotSupportedError("compression=zstd is not supported", "0A000");
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
    } else {
      await this.applySchema();
    }
    this.keepaliveManager.start();
    this.keepaliveTracker = this.keepaliveManager.register(this.connectionId, async () => {
      try {
        await this.ping();
        return true;
      } catch {
        return false;
      }
    });
    this.leakDetector.start();
    this.leakGuard = this.leakDetector.checkout(this.connectionId, { driver: "node" });
    this.connected = true;
    this.resolvedAuthContext.attached = true;
  }

  async query<T = any>(text: string, params?: any[] | Record<string, any>, options?: QueryOptions): Promise<QueryResult<T>> {
    this.ensureConnected();
    const normalized = this.normalizeQueryOrThrow(text, params);
    return (await this.executeQuery(normalized.sql, normalized.params, options)) as QueryResult<T>;
  }

  async queryMulti<T = any>(
    text: string,
    params?: any[] | Record<string, any>,
    options?: QueryOptions,
  ): Promise<Array<QueryResult<T>>> {
    this.ensureConnected();
    const normalized = this.normalizeQueryOrThrow(text, params);
    const results = await this.executeQueryMulti(normalized.sql, normalized.params, options);
    return results as Array<QueryResult<T>>;
  }

  async queryBatch(
    text: string,
    batchParams: Array<any[] | Record<string, any>>,
    options?: QueryOptions,
  ): Promise<BatchResult> {
    this.ensureConnected();
    if (!batchParams.length) {
      throw new ScratchbirdSyntaxError("batch parameters are required", "07001");
    }
    const summaries: BatchItemResult[] = [];
    for (let i = 0; i < batchParams.length; i++) {
      const result = await this.query(text, batchParams[i], options);
      summaries.push(this.toBatchSummary(i, result));
    }
    return {
      items: summaries,
      totalRowCount: summaries.reduce((sum, item) => sum + item.rowCount, 0),
    };
  }

  async queryStream(text: string, params?: any[] | Record<string, any>, options?: QueryOptions): Promise<AsyncGenerator<any, void, void>> {
    this.ensureConnected();
    const normalized = this.normalizeQueryOrThrow(text, params);
    return this.executeQueryStream(normalized.sql, normalized.params, options);
  }

  nativeSQL(text: string, params?: any[] | Record<string, any>): string {
    return this.normalizeQueryOrThrow(text, params).sql;
  }

  nativeCallableSQL(text: string, params?: any[] | Record<string, any>): string {
    return this.normalizeCallableQueryOrThrow(text, params).sql;
  }

  async call<T = any>(text: string, params?: any[] | Record<string, any>, options?: QueryOptions): Promise<QueryResult<T>> {
    this.ensureConnected();
    const normalized = this.normalizeCallableQueryOrThrow(text, params);
    return (await this.executeQuery(normalized.sql, normalized.params, options)) as QueryResult<T>;
  }

  async executeWithGeneratedKeys(
    text: string,
    params?: any[] | Record<string, any>,
    options?: QueryOptions,
  ): Promise<bigint[]> {
    this.ensureConnected();
    const normalized = this.normalizeQueryOrThrow(text, params);
    const results = await this.executeQueryMulti(normalized.sql, normalized.params, options);
    const keys: bigint[] = [];
    for (const result of results) {
      if (result.lastId !== null && result.lastId !== 0n) {
        keys.push(result.lastId);
      }
    }
    return keys;
  }

  async queryMetadata(collectionName: string = "tables", restrictions?: MetadataRestrictions): Promise<QueryResult> {
    this.ensureConnected();
    let normalizedCollection: MetadataCollectionName;
    try {
      normalizedCollection = normalizeMetadataCollectionName(collectionName);
    } catch (err) {
      throw new ScratchbirdNotSupportedError((err as Error).message, "0A000");
    }

    if (normalizedCollection === "catalogs") {
      const catalogName = this.config.database?.trim() ?? "";
      const baseRows = catalogName ? [{ catalog_name: catalogName }] : [];
      const shapedRows = shapeMetadataRowsForCollection(baseRows, normalizedCollection, {
        database: this.config.database ?? null,
      });
      const familyRows = filterMetadataRowsForCollectionFamily(shapedRows, normalizedCollection);
      const rows = filterMetadataRowsByRestrictions(familyRows, restrictions, normalizedCollection);
      return {
        rows,
        rowCount: rows.length,
        fields: [],
        command: "SELECT",
        lastId: null,
      };
    }

    const sql = resolveMetadataCollectionQuery(normalizedCollection);
    const result = await this.query(sql);
    const shapedRows = shapeMetadataRowsForCollection(result.rows as Array<Record<string, unknown>>, normalizedCollection, {
      database: this.config.database ?? null,
    });
    const familyRows = filterMetadataRowsForCollectionFamily(shapedRows, normalizedCollection);
    const restrictedRows = filterMetadataRowsByRestrictions(
      familyRows,
      restrictions,
      normalizedCollection,
    );
    const restrictedResult = {
      ...result,
      rows: restrictedRows,
      rowCount: restrictedRows.length,
    };
    if (normalizedCollection !== "schemas" || !this.config.metadataExpandSchemaParents) {
      return restrictedResult;
    }
    const expandedRows = expandSchemaMetadataRows(restrictedRows);
    return {
      ...restrictedResult,
      rows: expandedRows,
      rowCount: expandedRows.length,
    };
  }

  async getSchema(collectionName: string = "tables", restrictions?: MetadataRestrictions): Promise<QueryResult> {
    return this.queryMetadata(collectionName, restrictions);
  }

  async getSchemaTree(options?: MetadataSchemaTreeOptions): Promise<MetadataSchemaTree> {
    this.ensureConnected();
    const schemas = await this.getSchema("schemas", options?.restrictions);
    return buildMetadataSchemaTree(schemas.rows as Array<Record<string, unknown>>, {
      expandParents: options?.expandParents ?? this.config.metadataExpandSchemaParents === true,
      database: options?.database ?? this.config.database,
    });
  }

  async schemas(catalog?: string): Promise<QueryResult> {
    return this.getSchema("schemas", metadataRestrictions({ catalog }));
  }

  async tables(schema?: string, table?: string, type?: string): Promise<QueryResult> {
    return this.getSchema("tables", metadataRestrictions({ schema, table, type }));
  }

  async columns(schema?: string, table?: string, column?: string, type?: string): Promise<QueryResult> {
    return this.getSchema("columns", metadataRestrictions({ schema, table, column, type }));
  }

  async indexes(schema?: string, table?: string, index?: string): Promise<QueryResult> {
    return this.getSchema("indexes", metadataRestrictions({ schema, table, index }));
  }

  async indexColumns(schema?: string, table?: string, index?: string, column?: string): Promise<QueryResult> {
    return this.getSchema("index_columns", metadataRestrictions({ schema, table, index, column }));
  }

  async constraints(schema?: string, table?: string, constraint?: string): Promise<QueryResult> {
    return this.getSchema("constraints", metadataRestrictions({ schema, table, constraint }));
  }

  async catalogs(catalog?: string): Promise<QueryResult> {
    return this.getSchema("catalogs", metadataRestrictions({ catalog }));
  }

  async primaryKeys(catalog?: string, schema?: string, table?: string, constraint?: string): Promise<QueryResult> {
    return this.getSchema("primary_keys", metadataRestrictions({ catalog, schema, table, constraint }));
  }

  async foreignKeys(catalog?: string, schema?: string, table?: string, constraint?: string): Promise<QueryResult> {
    return this.getSchema("foreign_keys", metadataRestrictions({ catalog, schema, table, constraint }));
  }

  async procedures(catalog?: string, schema?: string, procedure?: string): Promise<QueryResult> {
    return this.getSchema("procedures", metadataRestrictions({ catalog, schema, procedure }));
  }

  async functions(catalog?: string, schema?: string, fn?: string): Promise<QueryResult> {
    return this.getSchema("functions", metadataRestrictions({ catalog, schema, function: fn }));
  }

  async routines(catalog?: string, schema?: string, routine?: string): Promise<QueryResult> {
    return this.getSchema("routines", metadataRestrictions({ catalog, schema, routine }));
  }

  async tablePrivileges(catalog?: string, schema?: string, table?: string): Promise<QueryResult> {
    return this.getSchema("table_privileges", metadataRestrictions({ catalog, schema, table }));
  }

  async columnPrivileges(catalog?: string, schema?: string, table?: string, column?: string): Promise<QueryResult> {
    return this.getSchema("column_privileges", metadataRestrictions({ catalog, schema, table, column }));
  }

  async typeInfo(type?: string): Promise<QueryResult> {
    return this.getSchema("type_info", metadataRestrictions({ type }));
  }

  getAutoCommit(): boolean {
    return this.autoCommit;
  }

  async setAutoCommit(enabled: boolean): Promise<void> {
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

  getSessionSchema(): string | null {
    return this.sessionSchema;
  }

  async setSessionSchema(schema: string | null | undefined): Promise<void> {
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

  async prepare(name: string, text: string, _paramTypes?: string[]): Promise<void> {
    if (!name) throw new Error("name is required");
    this.ensureConnected();
    const normalized = normalizePreparedQuery(text);
    await this.protocol.sendMessage(MessageType.PARSE, buildParsePayload(name, normalized.sql, []), 0, false);
    const describedParamCount = await this.describeStatement(name);
    this.prepared.set(name, {
      sql: normalized.sql,
      paramCount: describedParamCount >= 0 ? describedParamCount : normalized.paramCount,
      namedOrder: normalized.namedOrder,
    });
  }

  async execute<T = any>(name: string, params?: any[] | Record<string, any>, options?: QueryOptions): Promise<QueryResult<T>> {
    this.ensureConnected();
    const prepared = this.prepared.get(name);
    if (!prepared) throw new Error(`Unknown prepared statement: ${name}`);
    const normalized = this.normalizePreparedParamsOrThrow(prepared, params);
    if (prepared.paramCount >= 0 && prepared.paramCount !== normalized.params.length) {
      throw new ScratchbirdError("parameter count mismatch", "07001");
    }
    return (await this.executePrepared(name, normalized.params, options)) as QueryResult<T>;
  }

  async executeMulti<T = any>(
    name: string,
    params?: any[] | Record<string, any>,
    options?: QueryOptions,
  ): Promise<Array<QueryResult<T>>> {
    this.ensureConnected();
    const prepared = this.prepared.get(name);
    if (!prepared) throw new Error(`Unknown prepared statement: ${name}`);
    const normalized = this.normalizePreparedParamsOrThrow(prepared, params);
    if (prepared.paramCount >= 0 && prepared.paramCount !== normalized.params.length) {
      throw new ScratchbirdError("parameter count mismatch", "07001");
    }
    const results = await this.executePreparedMulti(name, normalized.params, options);
    return results as Array<QueryResult<T>>;
  }

  async executeBatch(
    name: string,
    batchParams: Array<any[] | Record<string, any>>,
    options?: QueryOptions,
  ): Promise<BatchResult> {
    this.ensureConnected();
    if (!batchParams.length) {
      throw new ScratchbirdSyntaxError("batch parameters are required", "07001");
    }
    const summaries: BatchItemResult[] = [];
    for (let i = 0; i < batchParams.length; i++) {
      const result = await this.execute(name, batchParams[i], options);
      summaries.push(this.toBatchSummary(i, result));
    }
    return {
      items: summaries,
      totalRowCount: summaries.reduce((sum, item) => sum + item.rowCount, 0),
    };
  }

  async begin(options?: TxnBeginOptions): Promise<void> {
    await this.beginTransaction(options);
  }

  async commit(options?: TxnEndOptions): Promise<void> {
    await this.commitTransaction(options);
  }

  async rollback(options?: TxnEndOptions): Promise<void> {
    await this.rollbackTransaction(options);
  }

  supportsPreparedTransactions(): boolean {
    return true;
  }

  supportsDormantReattach(): boolean {
    return true;
  }

  async prepareTransaction(gid: string): Promise<void> {
    this.ensureConnected();
    const sql = this.buildPreparedTransactionSql("PREPARE TRANSACTION", gid);
    await this.withResilience("prepare_transaction", sql, async () => {
      await this.sendSimpleQuery(sql);
      await this.drainUntilReady();
    });
  }

  async commitPrepared(gid: string): Promise<void> {
    this.ensureConnected();
    const sql = this.buildPreparedTransactionSql("COMMIT PREPARED", gid);
    await this.withResilience("commit_prepared", sql, async () => {
      await this.sendSimpleQuery(sql);
      await this.drainUntilReady();
    });
  }

  async rollbackPrepared(gid: string): Promise<void> {
    this.ensureConnected();
    const sql = this.buildPreparedTransactionSql("ROLLBACK PREPARED", gid);
    await this.withResilience("rollback_prepared", sql, async () => {
      await this.sendSimpleQuery(sql);
      await this.drainUntilReady();
    });
  }

  async detachToDormant(): Promise<{ dormantId: string; reattachToken: string }> {
    this.ensureConnected();
    delete this.parameters.dormant_id;
    delete this.parameters.dormant_reattach_token;
    await this.attachDetach();
    const dormantId = this.parameters.dormant_id;
    const reattachToken = this.parameters.dormant_reattach_token;
    if (!dormantId || !reattachToken) {
      throw new ScratchbirdConnectionError(
        "expected dormant detach identifiers from the server",
        "08006",
      );
    }
    return {
      dormantId: normalizeUuidText(dormantId, "dormantId"),
      reattachToken: normalizeUuidText(reattachToken, "dormantReattachToken"),
    };
  }

  async reattachDormant(dormantId: string, authToken?: string): Promise<void> {
    this.ensureConnected();
    if (!authToken) {
      throw new ScratchbirdSyntaxError(
        "dormant reattach requires the engine-issued auth token",
        "42601",
      );
    }
    await this.reconnectWithDormantParams(
      normalizeUuidText(dormantId, "dormantId"),
      normalizeUuidText(authToken, "dormantReattachToken"),
    );
  }

  async beginTransaction(options?: TxnBeginOptions): Promise<void> {
    this.ensureConnected();
    await this.withResilience("txn_begin", undefined, async () => {
      const readCommittedMode = options?.readCommittedMode;
      let isolation = options?.isolationLevel ?? ISOLATION_READ_COMMITTED;
      let flags = 0;
      if (options?.isolationLevel !== undefined) flags |= TXN_FLAG_HAS_ISOLATION;
      if (readCommittedMode !== undefined) {
        if (
          options?.isolationLevel !== undefined &&
          options.isolationLevel !== ISOLATION_READ_UNCOMMITTED &&
          options.isolationLevel !== ISOLATION_READ_COMMITTED
        ) {
          throw new ScratchbirdNotSupportedError(
            "readCommittedMode requires a READ COMMITTED isolation alias",
            "0A000",
          );
        }
        flags |= TXN_FLAG_HAS_READ_COMMITTED_MODE;
        if (options?.isolationLevel === undefined) {
          isolation = ISOLATION_READ_COMMITTED;
          flags |= TXN_FLAG_HAS_ISOLATION;
        }
      }
      if (options?.accessMode !== undefined) flags |= TXN_FLAG_HAS_ACCESS;
      if (options?.deferrable !== undefined) flags |= TXN_FLAG_HAS_DEFERRABLE;
      if (options?.wait !== undefined) flags |= TXN_FLAG_HAS_WAIT;
      if (options?.timeoutMs !== undefined) flags |= TXN_FLAG_HAS_TIMEOUT;
      if (options?.autocommitMode !== undefined) flags |= TXN_FLAG_HAS_AUTOCOMMIT;
      const payload = buildTxnBeginPayload(
        flags,
        options?.conflictAction ?? 0,
        options?.autocommitMode ?? 0,
        isolation,
        options?.accessMode ?? 0,
        options?.deferrable ? 1 : 0,
        options?.wait ? 1 : 0,
        options?.timeoutMs ?? 0,
        readCommittedMode ?? READ_COMMITTED_MODE_DEFAULT,
      );
      await this.protocol.sendMessage(MessageType.TXN_BEGIN, payload, 0, false);
      await this.drainUntilReady();
    });
  }

  async commitTransaction(options?: TxnEndOptions): Promise<void> {
    this.ensureConnected();
    this.ensureTransactionActive("commit");
    await this.withResilience("txn_commit", undefined, async () => {
      const payload = buildTxnCommitPayload(options?.flags ?? 0);
      await this.protocol.sendMessage(MessageType.TXN_COMMIT, payload, 0, false);
      await this.drainUntilReady();
    });
  }

  async rollbackTransaction(options?: TxnEndOptions): Promise<void> {
    this.ensureConnected();
    this.ensureTransactionActive("rollback");
    await this.withResilience("txn_rollback", undefined, async () => {
      const payload = buildTxnRollbackPayload(options?.flags ?? 0);
      await this.protocol.sendMessage(MessageType.TXN_ROLLBACK, payload, 0, false);
      await this.drainUntilReady();
    });
  }

  async savepoint(name: string): Promise<void> {
    this.ensureConnected();
    this.ensureTransactionActive("savepoint");
    if (!name.trim()) {
      throw new ScratchbirdError("savepoint name is required", "42601");
    }
    await this.withResilience("txn_savepoint", undefined, async () => {
      const payload = buildTxnSavepointPayload(name);
      await this.protocol.sendMessage(MessageType.TXN_SAVEPOINT, payload, 0, false);
      await this.drainUntilReady();
    });
  }

  async releaseSavepoint(name: string): Promise<void> {
    this.ensureConnected();
    this.ensureTransactionActive("release savepoint");
    if (!name.trim()) {
      throw new ScratchbirdError("savepoint name is required", "42601");
    }
    await this.withResilience("txn_release", undefined, async () => {
      const payload = buildTxnReleasePayload(name);
      await this.protocol.sendMessage(MessageType.TXN_RELEASE, payload, 0, false);
      await this.drainUntilReady();
    });
  }

  async rollbackToSavepoint(name: string): Promise<void> {
    this.ensureConnected();
    this.ensureTransactionActive("rollback to savepoint");
    if (!name.trim()) {
      throw new ScratchbirdError("savepoint name is required", "42601");
    }
    await this.withResilience("txn_rollback_to", undefined, async () => {
      const payload = buildTxnRollbackToPayload(name);
      await this.protocol.sendMessage(MessageType.TXN_ROLLBACK_TO, payload, 0, false);
      await this.drainUntilReady();
    });
  }

  async setOption(name: string, value: string): Promise<void> {
    this.ensureConnected();
    await this.withResilience("set_option", undefined, async () => {
      const payload = buildSetOptionPayload(name, value);
      await this.protocol.sendMessage(MessageType.SET_OPTION, payload, 0, false);
      await this.drainUntilReady();
    });
  }

  async ping(): Promise<void> {
    this.ensureConnected();
    await this.protocol.sendMessage(MessageType.PING, Buffer.alloc(0), 0, false);
    while (true) {
      const msg = await this.protocol.recv();
      if (this.handleAsyncMessage(msg)) {
        continue;
      }
      if (msg.header.type === MessageType.PONG || msg.header.type === MessageType.READY) {
        return;
      }
      if (msg.header.type === MessageType.ERROR) {
        throw this.raiseProtocolError(msg.payload);
      }
    }
  }

  async terminate(): Promise<void> {
    if (this.connected) {
      await this.protocol.sendMessage(MessageType.TERMINATE, Buffer.alloc(0), 0, false);
    }
    this.protocol.close();
    this.cleanupResilience();
    this.clearAbandonedSessionState();
    this.connected = false;
  }

  async subscribe(channel: string, options?: SubscribeOptions): Promise<void> {
    this.ensureConnected();
    const payload = buildSubscribePayload(options?.type ?? 0, channel, options?.filter ?? "");
    await this.protocol.sendMessage(MessageType.SUBSCRIBE, payload, 0, false);
    await this.drainUntilReady();
  }

  async unsubscribe(channel: string): Promise<void> {
    this.ensureConnected();
    const payload = buildUnsubscribePayload(channel);
    await this.protocol.sendMessage(MessageType.UNSUBSCRIBE, payload, 0, false);
    await this.drainUntilReady();
  }

  async executeSblr(hash: bigint, bytecode: Buffer | null, params?: any[], options?: QueryOptions): Promise<QueryResult> {
    this.ensureConnected();
    await this.ensureImplicitTransaction();
    return this.withResilience("sblr_execute", undefined, async () => {
      const paramValues: ParamValue[] = [];
      if (params) {
        for (const param of params) {
          const encoded = encodeParam(param);
          paramValues.push(encoded.param);
        }
      }
      const payload = buildSblrExecutePayload(hash, bytecode ?? Buffer.alloc(0), paramValues);
      await this.protocol.sendMessage(MessageType.SBLR_EXECUTE, payload, 0, false);
      await this.protocol.sendMessage(MessageType.SYNC, Buffer.alloc(0), 0, false);
      return this.collectResults(options?.maxRows ?? 0, options);
    });
  }

  async compileSblr(text: string, options?: QueryOptions): Promise<SblrCompiledMessage> {
    this.ensureConnected();
    this.lastSblr = undefined;
    await this.query(text, undefined, {
      ...options,
      returnSblr: true,
      maxRows: 0,
    });
    const compiled: SblrCompiledMessage | undefined = this.getLastSblr();
    if (!compiled || compiled.bytecode.length === 0) {
      throw new ScratchbirdConnectionError("parser endpoint did not return SBLR for RETURN_SBLR request", "08P01");
    }
    return {
      hash: compiled.hash,
      version: compiled.version,
      bytecode: Buffer.from(compiled.bytecode),
    };
  }

  async streamControl(controlType: number, windowSize: number, timeoutMs: number): Promise<void> {
    this.ensureConnected();
    const payload = buildStreamControlPayload(controlType, windowSize, timeoutMs);
    await this.protocol.sendMessage(MessageType.STREAM_CONTROL, payload, 0, false);
  }

  async attachCreate(emulationMode: string, dbName: string): Promise<void> {
    this.ensureConnected();
    await this.withResilience("attach_create", undefined, async () => {
      const payload = buildAttachCreatePayload(emulationMode, dbName);
      await this.protocol.sendMessage(MessageType.ATTACH_CREATE, payload, 0, false);
      await this.drainUntilReady();
    });
  }

  async attachDetach(): Promise<void> {
    this.ensureConnected();
    await this.withResilience("attach_detach", undefined, async () => {
      await this.protocol.sendMessage(MessageType.ATTACH_DETACH, Buffer.alloc(0), 0, false);
      await this.drainUntilReady();
    });
  }

  async attachList(): Promise<QueryResult> {
    this.ensureConnected();
    return this.withResilience("attach_list", undefined, async () => {
      await this.protocol.sendMessage(MessageType.ATTACH_LIST, Buffer.alloc(0), 0, false);
      await this.protocol.sendMessage(MessageType.SYNC, Buffer.alloc(0), 0, false);
      return this.collectResults(0, {});
    });
  }

  onNotification(handler: (notice: NotificationMessage) => void): void {
    this.notificationHandlers.push(handler);
  }

  getLastPlan(): QueryPlanMessage | undefined {
    return this.lastPlan;
  }

  getLastSblr(): SblrCompiledMessage | undefined {
    return this.lastSblr;
  }

  async end(): Promise<void> {
    this.protocol.close();
    this.cleanupResilience();
    this.clearAbandonedSessionState();
    this.connected = false;
    this.resolvedAuthContext.attached = false;
  }

  private ensureConnected(): void {
    if (!this.connected) {
      throw new Error("Client is not connected");
    }
  }

  private async reconnectWithDormantParams(dormantId: string, dormantReattachToken: string): Promise<void> {
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
    } finally {
      this.config.dormantId = priorDormantId;
      this.config.dormantReattachToken = priorDormantToken;
      this.skipSchemaApplyOnce = priorSkipSchema;
    }
  }

  private async ensureImplicitTransaction(): Promise<void> {
    if (this.autoCommit || this.transactionActive) {
      return;
    }
    await this.beginTransaction();
  }

  private ensureNoActiveTransaction(): void {
    if (this.transactionActive) {
      throw new ScratchbirdError("transaction already active", "25001");
    }
  }

  private ensureTransactionActive(operation: string): void {
    if (!this.transactionActive) {
      throw new ScratchbirdError(`${operation} requires an active transaction`, "25000");
    }
  }

  private cleanupResilience(): void {
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

  private clearAbandonedSessionState(): void {
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

  private async validateIfIdle(): Promise<void> {
    if (this.keepaliveTracker && this.keepaliveTracker.needsValidation()) {
      await this.ping();
      this.keepaliveTracker.markActive();
    }
  }

  private async withResilience<T>(operation: string, sql: string | undefined, fn: () => Promise<T>): Promise<T> {
    if (!this.circuitBreaker.allowRequest()) {
      throw new ScratchbirdError("Circuit breaker is OPEN", "08006");
    }
    await this.validateIfIdle();
    const span = this.telemetry.startSpan(operation);
    if (span && sql) {
      span.withAttribute("db.statement", TelemetryCollector.sanitizeQuery(sql));
    }
    try {
      const result = await fn();
      this.finishOperation(span, true);
      return result;
    } catch (err) {
      this.finishOperation(span, false, err);
      throw err;
    }
  }

  private finishOperation(span: SpanContext | null, success: boolean, error?: unknown): void {
    if (success) {
      this.circuitBreaker.recordSuccess();
      this.keepaliveTracker?.markActive();
    } else if (this.shouldRecordCircuitFailure(error)) {
      this.circuitBreaker.recordFailure();
    }
    this.telemetry.endSpan(span, success);
  }

  private shouldRecordCircuitFailure(error?: unknown): boolean {
    if (error instanceof ScratchbirdConnectionError) {
      return true;
    }
    if (error instanceof ScratchbirdError) {
      return retryScopeForSqlState(error.code) === "reconnect";
    }
    return true;
  }

  private requestedFeatures(): bigint {
    let features = 0n;
    if (this.config.compression === "zstd") {
      features |= FEATURE_COMPRESSION;
    }
    if (this.config.binaryTransfer) {
      features |= FEATURE_STREAMING;
    }
    return features;
  }

  private appendLengthPrefixedString(out: Buffer[], value: string): void {
    const bytes = Buffer.from(value, "utf8");
    const len = Buffer.alloc(4);
    len.writeUInt32LE(bytes.length, 0);
    out.push(len, bytes);
  }

  private async performManagerConnect(): Promise<void> {
    if (!this.config.managerAuthToken) {
      throw new ScratchbirdAuthError("manager_proxy mode requires manager_auth_token", "28000");
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
      throw new ScratchbirdError("expected MCP hello status response", "08P01");
    }

    const authStartParts: Buffer[] = [];
    this.appendLengthPrefixedString(authStartParts, managerUser);
    authStartParts.push(Buffer.from([MCP_AUTH_METHOD_TOKEN]));
    if (authFastPath) {
      const token = Buffer.from(this.config.managerAuthToken, "utf8");
      const len = Buffer.alloc(4);
      len.writeUInt32LE(token.length, 0);
      authStartParts.push(len, token);
    } else {
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
      throw new ScratchbirdError("expected MCP auth response", "08P01");
    }
    if (frame.payload.length < 1 + 4 + 256) {
      throw new ScratchbirdError("truncated MCP auth response", "08P01");
    }
    if (frame.payload.readUInt8(0) !== 0) {
      const errText = frame.payload.subarray(5, 261).toString("utf8").replace(/\0+$/, "") || "MCP authentication failed";
      throw new ScratchbirdAuthError(errText, "28000");
    }
    this.resolvedAuthContext.managerAuthenticated = true;
    this.resolvedAuthContext.ingressMode = "manager_proxy";

    const nonce = randomBytes(16);
    const dbConnectParts: Buffer[] = [Buffer.from("MCP1", "ascii")];
    this.appendLengthPrefixedString(dbConnectParts, managerDatabase);
    this.appendLengthPrefixedString(dbConnectParts, managerProfile);
    this.appendLengthPrefixedString(dbConnectParts, managerIntent);
    const nonceLen = Buffer.alloc(2);
    nonceLen.writeUInt16LE(nonce.length, 0);
    dbConnectParts.push(nonceLen, nonce);
    await this.protocol.sendManagerFrame(MCP_MSG_DB_CONNECT, Buffer.concat(dbConnectParts));
    frame = await this.protocol.recvManagerFrame();
    if (frame.type !== MCP_MSG_CONNECT_RESPONSE) {
      throw new ScratchbirdError("expected MCP connect response", "08P01");
    }
    if (frame.payload.length < 1 + 2 + 2 + 16 + 64 + 32) {
      throw new ScratchbirdError("truncated MCP connect response", "08P01");
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
      throw new ScratchbirdError(errText, "28000");
    }
  }

  private async handshake(): Promise<void> {
    const startup = buildStartupPayload(this.requestedFeatures(), this.buildStartupParams());
    await this.protocol.sendMessage(MessageType.STARTUP, startup, 0, true);

    let scram: ScramExchange | null = null;

    while (true) {
      const msg = await this.protocol.recv();
      if (this.handleAsyncMessage(msg)) {
        continue;
      }
      switch (msg.header.type) {
        case MessageType.NEGOTIATE_VERSION:
          continue;
        case MessageType.AUTH_REQUEST: {
          const { method } = parseAuthRequest(msg.payload);
          if (method === AuthMethod.OK) {
            continue;
          }
          const resolvedMethod = authMethodName(method);
          this.resolvedAuthContext.resolvedAuthMethod = resolvedMethod;
          this.resolvedAuthContext.resolvedAuthPluginId = authPluginIdForMethod(method, this.config.authMethodId);
          if (method === AuthMethod.PASSWORD) {
            await this.protocol.sendMessage(MessageType.AUTH_RESPONSE, Buffer.from(this.config.password ?? ""), 0, true);
            continue;
          }
          if (method === AuthMethod.SCRAM_SHA_256 || method === AuthMethod.SCRAM_SHA_512) {
            if (!scram) {
              scram = new ScramExchange(
                this.config.user ?? "",
                method === AuthMethod.SCRAM_SHA_512 ? "sha512" : "sha256",
              );
            }
            const clientFirst = Buffer.from(scram.clientFirstMessage(), "utf8");
            await this.protocol.sendMessage(MessageType.AUTH_RESPONSE, clientFirst, 0, true);
            continue;
          }
          if (method === AuthMethod.TOKEN) {
            const tokenPayload = resolveTokenAuthPayload(this.config);
            if (!tokenPayload) {
              throw new ScratchbirdAuthError(
                "TOKEN authentication requires authToken, authMethodPayload, authPayloadJson, authPayloadB64, workloadIdentityToken, or proxyPrincipalAssertion",
                "28000",
              );
            }
            await this.protocol.sendMessage(MessageType.AUTH_RESPONSE, tokenPayload, 0, true);
            continue;
          }
          if (method === AuthMethod.MD5) {
            throw new ScratchbirdNotSupportedError(
              "MD5 authentication is admitted by the server but not executable in the Node lane",
              "0A000",
            );
          }
          if (method === AuthMethod.PEER) {
            throw new ScratchbirdNotSupportedError(
              "PEER authentication requires broker or platform assistance in the Node lane",
              "0A000",
            );
          }
          if (method === AuthMethod.REATTACH) {
            throw new ScratchbirdNotSupportedError(
              "REATTACH authentication negotiation is not executable through the generic Node auth lane",
              "0A000",
            );
          }
          throw new ScratchbirdNotSupportedError("Unsupported auth method", "0A000");
        }
        case MessageType.AUTH_CONTINUE: {
          const { method, data } = parseAuthContinue(msg.payload);
          if (method === AuthMethod.TOKEN) {
            const tokenPayload = resolveTokenAuthPayload(this.config);
            if (!tokenPayload) {
              throw new ScratchbirdAuthError(
                "TOKEN authentication requires authToken, authMethodPayload, authPayloadJson, authPayloadB64, workloadIdentityToken, or proxyPrincipalAssertion",
                "28000",
              );
            }
            await this.protocol.sendMessage(MessageType.AUTH_RESPONSE, tokenPayload, 0, true);
            continue;
          }
          if (method !== AuthMethod.SCRAM_SHA_256 && method !== AuthMethod.SCRAM_SHA_512) {
            throw new ScratchbirdNotSupportedError("Unsupported auth continue", "0A000");
          }
          if (!scram) {
            throw new ScratchbirdConnectionError("SCRAM state missing", "08001");
          }
          const clientFinal = scram.handleServerFirst(this.config.password ?? "", data.toString("utf8"));
          await this.protocol.sendMessage(MessageType.AUTH_RESPONSE, Buffer.from(clientFinal, "utf8"), 0, true);
          continue;
        }
        case MessageType.AUTH_OK: {
          const { serverInfo } = parseAuthOk(msg.payload);
          this.protocol.setAttachment(msg.header.attachmentId, msg.header.txnId);
          if (scram && serverInfo.length && serverInfo.toString("utf8").startsWith("v=")) {
            scram.verifyServerFinal(serverInfo.toString("utf8"));
          }
          continue;
        }
        case MessageType.READY: {
          const { status, txnId } = parseReady(msg.payload);
          this.applyRuntimeReadyState(status, txnId);
          if (!this.resolvedAuthContext.resolvedAuthMethod && this.config.dormantId) {
            this.resolvedAuthContext.resolvedAuthMethod = "REATTACH";
            this.resolvedAuthContext.resolvedAuthPluginId = authPluginIdForMethod(
              AuthMethod.REATTACH,
              this.config.authMethodId,
            );
          }
          return;
        }
        case MessageType.ERROR:
          throw this.raiseProtocolError(msg.payload);
        default:
          continue;
      }
    }
  }

  private async applySchema(): Promise<void> {
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

  private async collectResults(pageSize: number, options?: QueryOptions): Promise<QueryResult> {
    const results = await this.collectResultSets(pageSize, options);
    if (results.length === 0) {
      return this.emptyQueryResult();
    }
    if (results.length === 1) {
      return results[0];
    }
    return this.mergeLegacyResults(results);
  }

  private async collectResultSets(pageSize: number, options?: QueryOptions): Promise<QueryResult[]> {
    const results: QueryResult[] = [];
    let columns: ReturnType<typeof parseRowDescription> = [];
    let rows: any[] = [];
    let fields: FieldDef[] = [];
    let rowCount = -1;
    let command = "";
    let lastId: bigint | null = null;
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
        throw new ScratchbirdError("query canceled", "57014");
      }
      const msg = await this.protocol.recv();
      if (this.handleAsyncMessage(msg)) {
        continue;
      }
      switch (msg.header.type) {
        case MessageType.ERROR:
          await this.drainReadyAfterError();
          throw this.raiseProtocolError(msg.payload);
        case MessageType.ROW_DESCRIPTION:
          columns = parseRowDescription(msg.payload);
          fields = columns.map((col) => ({
            name: col.name,
            dataType: oidToString(col.typeOid),
            format: col.format === FORMAT_TEXT ? "text" : "binary",
            nullable: col.nullable,
            typeOid: col.typeOid,
            typeModifier: col.typeModifier,
          }));
          hasCurrentResult = true;
          continue;
        case MessageType.DATA_ROW: {
          const values = parseDataRow(msg.payload, columns.length);
          rows.push(buildRow(columns, values));
          hasCurrentResult = true;
          continue;
        }
        case MessageType.COMMAND_COMPLETE: {
          const parsed = parseCommandComplete(msg.payload);
          command = parsed.tag;
          rowCount = Number(parsed.rows);
          lastId = parsed.lastId;
          hasCurrentResult = true;
          finalizeResult();
          continue;
        }
        case MessageType.EMPTY_QUERY:
          command = "";
          rowCount = 0;
          lastId = null;
          hasCurrentResult = true;
          finalizeResult();
          continue;
        case MessageType.PORTAL_SUSPENDED: {
          if (pageSize > 0) {
            this.portalResumePending = true;
            await this.resumePortal(pageSize);
          }
          continue;
        }
        case MessageType.READY: {
          const { status, txnId } = parseReady(msg.payload);
          this.applyRuntimeReadyState(status, txnId);
          finalizeResult();
          return results;
        }
        default:
          continue;
      }
    }
  }

  private mergeLegacyResults(results: QueryResult[]): QueryResult {
    const mergedRows: any[] = [];
    let mergedFields: FieldDef[] = [];
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

  private emptyQueryResult(): QueryResult {
    return {
      rows: [],
      rowCount: 0,
      fields: [],
      command: "",
      lastId: null,
    };
  }

  private toBatchSummary(index: number, result: QueryResult): BatchItemResult {
    return {
      index,
      rowCount: result.rowCount,
      fields: result.fields,
      command: result.command,
      lastId: result.lastId,
    };
  }

  private handleParameterStatus(name: string, value: string): void {
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

  private handleAsyncMessage(msg: Message): boolean {
    switch (msg.header.type) {
      case MessageType.PARAMETER_STATUS: {
        const { name, value } = parseParameterStatus(msg.payload);
        this.handleParameterStatus(name, value);
        return true;
      }
      case MessageType.NOTIFICATION: {
        const notice = parseNotification(msg.payload);
        for (const handler of this.notificationHandlers) {
          handler(notice);
        }
        return true;
      }
      case MessageType.QUERY_PLAN: {
        this.lastPlan = parseQueryPlan(msg.payload);
        return true;
      }
      case MessageType.SBLR_COMPILED: {
        this.lastSblr = parseSblrCompiled(msg.payload);
        return true;
      }
      case MessageType.TXN_STATUS: {
        const { status, txnId } = parseTxnStatus(msg.payload);
        if (status === "T") {
          this.applyRuntimeTxnId(txnId);
          this.transactionActive = true;
        } else {
          this.clearTransactionState();
        }
        return true;
      }
      default:
        return false;
    }
  }

  private async executeQuery(sql: string, params: any[], options?: QueryOptions): Promise<QueryResult> {
    const pageSize = options?.maxRows ?? 0;
    await this.ensureImplicitTransaction();
    return this.withResilience("query", sql, async () => {
      if (params.length === 0) {
        await this.sendSimpleQuery(sql, options);
      } else {
        await this.sendExtendedQuery(sql, params, options);
      }
      return this.collectResults(pageSize, options);
    });
  }

  private async executeQueryMulti(sql: string, params: any[], options?: QueryOptions): Promise<QueryResult[]> {
    const pageSize = options?.maxRows ?? 0;
    const splitStatements = this.splitExecutableStatements(sql, params);
    await this.ensureImplicitTransaction();
    return this.withResilience("query_multi", sql, async () => {
      if (splitStatements) {
        const results: QueryResult[] = [];
        for (const statement of splitStatements) {
          if (statement.params.length === 0) {
            await this.sendSimpleQuery(statement.sql, options);
          } else {
            await this.sendExtendedQuery(statement.sql, statement.params, options);
          }
          results.push(...(await this.collectResultSets(pageSize, options)));
        }
        return results;
      }
      if (params.length === 0) {
        await this.sendSimpleQuery(sql, options);
      } else {
        await this.sendExtendedQuery(sql, params, options);
      }
      return this.collectResultSets(pageSize, options);
    });
  }

  private async executePrepared(name: string, params: any[], options?: QueryOptions): Promise<QueryResult> {
    const pageSize = options?.maxRows ?? 0;
    const prepared = this.prepared.get(name);
    await this.ensureImplicitTransaction();
    return this.withResilience("execute_prepared", prepared?.sql, async () => {
      await this.sendBindExecute(name, params, options);
      return this.collectResults(pageSize, options);
    });
  }

  private async executePreparedMulti(name: string, params: any[], options?: QueryOptions): Promise<QueryResult[]> {
    const pageSize = options?.maxRows ?? 0;
    const prepared = this.prepared.get(name);
    const splitStatements = prepared ? this.splitExecutableStatements(prepared.sql, params) : null;
    await this.ensureImplicitTransaction();
    return this.withResilience("execute_prepared_multi", prepared?.sql, async () => {
      if (splitStatements) {
        const results: QueryResult[] = [];
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

  private splitExecutableStatements(sql: string, params: any[]): Array<{ sql: string; params: any[] }> | null {
    const statements = splitTopLevelStatements(sql);
    if (statements.length <= 1) {
      return null;
    }
    return statements.map((statement) => this.remapStatementParams(statement, params));
  }

  private remapStatementParams(sql: string, params: any[]): { sql: string; params: any[] } {
    if (params.length === 0) {
      return { sql, params: [] };
    }
    let result = "";
    let inSingle = false;
    let inDouble = false;
    const remap = new Map<number, number>();
    const ordered: number[] = [];
    for (let i = 0; i < sql.length; ) {
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
        while (j < sql.length && /\d/.test(sql[j])) j++;
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
        throw new ScratchbirdError("parameter count mismatch", "07001");
      }
      return params[originalIndex - 1];
    });
    return { sql: result, params: remappedParams };
  }

  private async executeQueryStream(sql: string, params: any[], options?: QueryOptions): Promise<AsyncGenerator<any, void, void>> {
    const pageSize = options?.maxRows ?? 0;
    await this.ensureImplicitTransaction();
    if (!this.circuitBreaker.allowRequest()) {
      throw new ScratchbirdError("Circuit breaker is OPEN", "08006");
    }
    await this.validateIfIdle();
    const span = this.telemetry.startSpan("query_stream");
    if (span) {
      span.withAttribute("db.statement", TelemetryCollector.sanitizeQuery(sql));
    }
    try {
      if (params.length === 0) {
        await this.sendSimpleQuery(sql, options);
      } else {
        await this.sendExtendedQuery(sql, params, options);
      }
    } catch (err) {
      this.finishOperation(span, false, err);
      throw err;
    }

    const self = this;
    async function* iterator() {
      let columns: ReturnType<typeof parseRowDescription> = [];
      let success = false;
      let operationError: unknown;
      try {
        while (true) {
          if (options?.signal?.aborted) {
            await self.cancelQuery();
            throw new ScratchbirdError("query canceled", "57014");
          }
          const msg = await self.protocol.recv();
          if (self.handleAsyncMessage(msg)) {
            continue;
          }
          switch (msg.header.type) {
            case MessageType.ERROR:
              await self.drainReadyAfterError();
              throw self.raiseProtocolError(msg.payload);
            case MessageType.ROW_DESCRIPTION:
              columns = parseRowDescription(msg.payload);
              continue;
            case MessageType.DATA_ROW: {
              const values = parseDataRow(msg.payload, columns.length);
              yield buildRow(columns, values);
              continue;
            }
            case MessageType.PORTAL_SUSPENDED: {
              if (pageSize > 0) {
                self.portalResumePending = true;
                await self.resumePortal(pageSize);
              }
              continue;
            }
            case MessageType.READY: {
              const { status, txnId } = parseReady(msg.payload);
              self.applyRuntimeReadyState(status, txnId);
              success = true;
              return;
            }
            default:
              continue;
          }
        }
      } catch (err) {
        operationError = err;
        throw err;
      } finally {
        self.finishOperation(span, success, operationError);
      }
    }
    return iterator();
  }

  private async sendSimpleQuery(sql: string, options?: QueryOptions): Promise<void> {
    let flags = this.config.binaryTransfer ? QUERY_FLAG_BINARY_RESULT : 0;
    if (options?.includePlan) flags |= QUERY_FLAG_INCLUDE_PLAN;
    if (options?.returnSblr) flags |= QUERY_FLAG_RETURN_SBLR;
    if (options?.describeOnly) flags |= QUERY_FLAG_DESCRIBE_ONLY;
    if (options?.noCache) flags |= QUERY_FLAG_NO_CACHE;
    const maxRows = options?.maxRows ?? 0;
    const timeoutMs = options?.timeoutMs ?? 0;
    const payload = buildQueryPayload(sql, flags, maxRows, timeoutMs);
    await this.protocol.sendMessage(MessageType.QUERY, payload, 0, false);
  }

  private async sendExtendedQuery(sql: string, params: any[], options?: QueryOptions): Promise<void> {
    const paramValues: ParamValue[] = [];
    const paramTypes: number[] = [];
    for (const param of params) {
      const encoded = encodeParam(param);
      paramValues.push(encoded.param);
      paramTypes.push(encoded.oid);
    }
    const parsePayload = buildParsePayload("", sql, paramTypes);
    await this.protocol.sendMessage(MessageType.PARSE, parsePayload, 0, false);
    const paramCount = await this.describeStatement("");
    if (paramCount >= 0 && paramCount !== params.length) {
      throw new ScratchbirdError("parameter count mismatch", "07001");
    }
    const resultFormats = this.config.binaryTransfer ? [FORMAT_BINARY] : [];
    const bindPayload = buildBindPayload("", "", paramValues, resultFormats);
    await this.protocol.sendMessage(MessageType.BIND, bindPayload, 0, false);
    const maxRows = options?.maxRows ?? 0;
    const execPayload = buildExecutePayload("", maxRows);
    await this.protocol.sendMessage(MessageType.EXECUTE, execPayload, 0, false);
    if (maxRows === 0) {
      await this.protocol.sendMessage(MessageType.SYNC, Buffer.alloc(0), 0, false);
    }
  }

  private async sendBindExecute(statementName: string, params: any[], options?: QueryOptions): Promise<void> {
    const paramValues: ParamValue[] = [];
    for (const param of params) {
      const encoded = encodeParam(param);
      paramValues.push(encoded.param);
    }
    const resultFormats = this.config.binaryTransfer ? [FORMAT_BINARY] : [];
    const bindPayload = buildBindPayload("", statementName, paramValues, resultFormats);
    await this.protocol.sendMessage(MessageType.BIND, bindPayload, 0, false);
    const maxRows = options?.maxRows ?? 0;
    const execPayload = buildExecutePayload("", maxRows);
    await this.protocol.sendMessage(MessageType.EXECUTE, execPayload, 0, false);
    if (maxRows === 0) {
      await this.protocol.sendMessage(MessageType.SYNC, Buffer.alloc(0), 0, false);
    }
  }

  private async resumePortal(maxRows: number): Promise<void> {
    if (!this.portalResumePending) {
      throw new ScratchbirdError("portal resume requires explicit suspended state", "55000");
    }
    this.portalResumePending = false;
    const execPayload = buildExecutePayload("", maxRows);
    await this.protocol.sendMessage(MessageType.EXECUTE, execPayload, 0, false);
  }

  private async describeStatement(statementName: string): Promise<number> {
    const describePayload = buildDescribePayload("S".charCodeAt(0), statementName);
    await this.protocol.sendMessage(MessageType.DESCRIBE, describePayload, 0, false);
    await this.protocol.sendMessage(MessageType.SYNC, Buffer.alloc(0), 0, false);
    let paramCount = -1;
    while (true) {
      const msg = await this.protocol.recv();
      if (this.handleAsyncMessage(msg)) {
        continue;
      }
      switch (msg.header.type) {
        case MessageType.ERROR:
          await this.drainReadyAfterError();
          throw this.raiseProtocolError(msg.payload);
        case MessageType.PARAMETER_DESCRIPTION:
          paramCount = parseParameterDescription(msg.payload).length;
          continue;
        case MessageType.READY: {
          const { status, txnId } = parseReady(msg.payload);
          this.applyRuntimeReadyState(status, txnId);
          return paramCount;
        }
        default:
          continue;
      }
    }
  }

  private async cancelQuery(): Promise<void> {
    await this.protocol.sendMessage(MessageType.CANCEL, buildCancelPayload(0, 0), MSG_FLAG_URGENT, false);
  }

  private async drainUntilReady(): Promise<void> {
    while (true) {
      const msg = await this.protocol.recv();
      if (this.handleAsyncMessage(msg)) {
        continue;
      }
      switch (msg.header.type) {
        case MessageType.ERROR: {
          const error = this.raiseProtocolError(msg.payload);
          await this.drainReadyAfterError();
          throw error;
        }
        case MessageType.READY: {
          const { status, txnId } = parseReady(msg.payload);
          this.applyRuntimeReadyState(status, txnId);
          return;
        }
        default:
          continue;
      }
    }
  }

  private async drainReadyAfterError(): Promise<void> {
    while (true) {
      const msg = await this.protocol.recv();
      if (this.handleAsyncMessage(msg)) {
        continue;
      }
      if (msg.header.type === MessageType.READY) {
        const { status, txnId } = parseReady(msg.payload);
        this.applyRuntimeReadyState(status, txnId);
        return;
      }
      if (msg.header.type === MessageType.ERROR) {
        continue;
      }
    }
  }

  private applyRuntimeTxnId(txnId: bigint): void {
    this.protocol.setTxnId(txnId);
    if (txnId > 0n) {
      this.transactionActive = true;
    }
  }

  private applyRuntimeReadyState(status: number, txnId: bigint): void {
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

  private clearTransactionState(): void {
    this.protocol.setTxnId(0n);
    this.transactionActive = false;
  }

  private buildPreparedTransactionSql(verb: string, gid: string): string {
    const normalized = gid.trim();
    if (!normalized) {
      throw new ScratchbirdSyntaxError("global transaction id is required", "42601");
    }
    return `${verb} '${normalized.replace(/'/g, "''")}'`;
  }

  private normalizeQueryOrThrow(text: string, params?: any[] | Record<string, any>): ReturnType<typeof normalizeQuery> {
    try {
      return normalizeQuery(text, params);
    } catch (err) {
      throw this.wrapNormalizationError(err);
    }
  }

  private normalizePreparedParamsOrThrow(
    prepared: { sql: string; paramCount: number; namedOrder: string[] | null },
    params?: any[] | Record<string, any>,
  ): ReturnType<typeof normalizeQuery> {
    try {
      if (prepared.namedOrder?.length) {
        if (params === undefined) {
          return { sql: prepared.sql, params: [] };
        }
        if (Array.isArray(params)) {
          return { sql: prepared.sql, params };
        }
        const lookup: Record<string, any> = {};
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
    } catch (err) {
      throw this.wrapNormalizationError(err);
    }
  }

  private normalizeCallableQueryOrThrow(text: string, params?: any[] | Record<string, any>): ReturnType<typeof normalizeCallableQuery> {
    try {
      return normalizeCallableQuery(text, params);
    } catch (err) {
      throw this.wrapNormalizationError(err);
    }
  }

  private wrapNormalizationError(err: unknown): ScratchbirdSyntaxError {
    const message = err instanceof Error ? err.message : String(err);
    return new ScratchbirdSyntaxError(message, "07001");
  }

  private raiseProtocolError(payload: Buffer): ScratchbirdError {
    try {
      const { sqlState, message, detail, hint } = parseErrorMessage(payload);
      const ErrorClass = mapSqlState(sqlState);
      const full = [message, detail ? `DETAIL: ${detail}` : "", hint ? `HINT: ${hint}` : ""]
        .filter(Boolean)
        .join("\n");
      return new ErrorClass(full || "query failed", sqlState, detail, hint);
    } catch {
      return new ScratchbirdError("query failed");
    }
  }
}

export class Pool {
  private config: ClientConfig;
  private max: number;
  private idleTimeoutMs: number;
  private active = 0;
  private idle: Array<{ client: Client; lastUsed: number }> = [];
  private waiters: Array<(client: Client) => void> = [];

  constructor(config?: ClientConfig | string) {
    const parsed = typeof config === "string" ? parseDsn(config) : {};
    const merged = { ...parsed, ...(typeof config === "object" ? config : {}) };
    this.config = merged;
    this.max = (merged as any).max ?? 10;
    this.idleTimeoutMs = (merged as any).idleTimeoutMs ?? 30000;
  }

  async connect(): Promise<Client> {
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
    return new Promise<Client>((resolve) => {
      this.waiters.push(resolve);
    });
  }

  async query<T = any>(text: string, params?: any[] | Record<string, any>, options?: QueryOptions): Promise<QueryResult<T>> {
    const client = await this.connect();
    try {
      return await client.query<T>(text, params, options);
    } finally {
      await (client as any).release();
    }
  }

  async end(): Promise<void> {
    for (const item of this.idle) {
      await item.client.end();
    }
    this.idle = [];
    this.active = 0;
  }

  private wrapClient(client: Client): Client {
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
    (client as any).release = release;
    return client;
  }

  private cleanup(): void {
    const cutoff = Date.now() - this.idleTimeoutMs;
    const remaining: Array<{ client: Client; lastUsed: number }> = [];
    for (const item of this.idle) {
      if (item.lastUsed < cutoff) {
        item.client.end();
        this.active = Math.max(0, this.active - 1);
      } else {
        remaining.push(item);
      }
    }
    this.idle = remaining;
  }
}

function parseUuidBytes(value: string): Buffer | null {
  const hex = value.replace(/-/g, "").trim();
  if (!/^[0-9a-fA-F]{32}$/.test(hex)) {
    return null;
  }
  return Buffer.from(hex, "hex");
}

function normalizeUuidText(value: string, label: string): string {
  const parsed = parseUuidBytes(value);
  if (!parsed) {
    throw new ScratchbirdSyntaxError(`${label} must be a UUID`, "42601");
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

function parseBigInt(value: string): bigint | null {
  try {
    return BigInt(value.trim());
  } catch {
    return null;
  }
}

function buildRow(columns: Array<{ name: string; typeOid: number; format: number }>, values: { data: Buffer | null }[]): Record<string, any> {
  const row: Record<string, any> = {};
  const limit = Math.min(values.length, columns.length);
  for (let i = 0; i < limit; i++) {
    const column = columns[i];
    const data = values[i];
    const decoded = decodeValue(column.typeOid, data.data, column.format);
    if (column?.name) {
      row[column.name] = decoded;
    } else {
      row[i] = decoded;
    }
  }
  return row;
}

function buildSchemaStatement(schema: string): string {
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

function normalizeSessionSchema(schema: string | null | undefined): string | null {
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

function quoteIdentifier(name: string): string {
  return `"${name.replace(/"/g, "\"\"")}"`;
}

function metadataRestrictions(restrictions: MetadataRestrictions): MetadataRestrictions | undefined {
  const filtered = Object.fromEntries(
    Object.entries(restrictions).filter(([, value]) => value !== undefined && value !== null),
  );
  return Object.keys(filtered).length ? filtered : undefined;
}

function resolveSslMode(config: ClientConfig): string {
  if (config.ssl === false) return "disable";
  if (typeof config.ssl === "object") return config.sslmode ?? "require";
  if (config.ssl === true) return config.sslmode ?? "require";
  return config.sslmode ?? "require";
}

function normalizeTransportMode(value?: string): string {
  const normalized = (value ?? "inet_listener").trim().toLowerCase().replace(/-/g, "_");
  if (normalized === "local" || normalized === "ipc") return "local_ipc";
  return normalized || "inet_listener";
}

async function connectLocalIpc(path: string | undefined, timeoutMs: number): Promise<net.Socket> {
  if (!path?.trim()) {
    throw new Error("ipc_path is required for local_ipc");
  }
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ path });
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

async function connectTcp(host: string, port: number, timeoutMs: number): Promise<net.Socket> {
  return new Promise((resolve, reject) => {
    const socket = net.connect({ host, port });
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

async function upgradeTls(socket: net.Socket, host: string, sslMode: string, config: ClientConfig): Promise<tls.TLSSocket> {
  const rejectUnauthorized = sslMode === "verify-ca" || sslMode === "verify-full";
  const tlsOptions: tls.ConnectionOptions = {
    socket,
    servername: host,
    rejectUnauthorized,
    minVersion: "TLSv1.3",
    maxVersion: "TLSv1.3",
  };

  if (config.sslrootcert) {
    tlsOptions.ca = fs.readFileSync(config.sslrootcert);
  }
  if (config.sslcert) {
    tlsOptions.cert = fs.readFileSync(config.sslcert);
  }
  if (config.sslkey) {
    tlsOptions.key = fs.readFileSync(config.sslkey);
  }
  if (config.sslpassword) {
    tlsOptions.passphrase = config.sslpassword;
  }

  if (typeof config.ssl === "object") {
    Object.assign(tlsOptions, config.ssl);
  }

  const tlsSocket = tls.connect(tlsOptions);
  return new Promise<tls.TLSSocket>((resolve, reject) => {
    tlsSocket.once("secureConnect", () => resolve(tlsSocket));
    tlsSocket.once("error", (err) => reject(err));
  });
}
