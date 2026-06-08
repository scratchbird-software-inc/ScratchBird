// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import { Buffer } from "node:buffer";

export const PROTOCOL_MAGIC_BYTES = Buffer.from("SBWP");
export const PROTOCOL_VERSION_MAJOR = 1;
export const PROTOCOL_VERSION_MINOR = 1;
export const PROTOCOL_VERSION = (PROTOCOL_VERSION_MAJOR << 8) | PROTOCOL_VERSION_MINOR;
export const HEADER_SIZE = 40;
export const MAX_MESSAGE_SIZE = 1024 * 1024 * 1024;

export enum MessageType {
  STARTUP = 0x01,
  AUTH_RESPONSE = 0x02,
  QUERY = 0x03,
  PARSE = 0x04,
  BIND = 0x05,
  DESCRIBE = 0x06,
  EXECUTE = 0x07,
  CLOSE = 0x08,
  SYNC = 0x09,
  FLUSH = 0x0a,
  CANCEL = 0x0b,
  TERMINATE = 0x0c,
  COPY_DATA = 0x0d,
  COPY_DONE = 0x0e,
  COPY_FAIL = 0x0f,
  SBLR_EXECUTE = 0x10,
  SUBSCRIBE = 0x11,
  UNSUBSCRIBE = 0x12,
  FEDERATED_QUERY = 0x13,
  STREAM_CONTROL = 0x14,
  TXN_BEGIN = 0x15,
  TXN_COMMIT = 0x16,
  TXN_ROLLBACK = 0x17,
  TXN_SAVEPOINT = 0x18,
  TXN_RELEASE = 0x19,
  TXN_ROLLBACK_TO = 0x1a,
  PING = 0x1b,
  SET_OPTION = 0x1c,
  CLUSTER_AUTH = 0x1d,
  ATTACH_CREATE = 0x1e,
  ATTACH_DETACH = 0x1f,
  ATTACH_LIST = 0x20,

  AUTH_REQUEST = 0x40,
  AUTH_OK = 0x41,
  AUTH_CONTINUE = 0x42,
  READY = 0x43,
  ROW_DESCRIPTION = 0x44,
  DATA_ROW = 0x45,
  COMMAND_COMPLETE = 0x46,
  EMPTY_QUERY = 0x47,
  ERROR = 0x48,
  NOTICE = 0x49,
  PARSE_COMPLETE = 0x4a,
  BIND_COMPLETE = 0x4b,
  CLOSE_COMPLETE = 0x4c,
  PORTAL_SUSPENDED = 0x4d,
  NO_DATA = 0x4e,
  PARAMETER_STATUS = 0x4f,
  PARAMETER_DESCRIPTION = 0x50,
  COPY_IN_RESPONSE = 0x51,
  COPY_OUT_RESPONSE = 0x52,
  COPY_BOTH_RESPONSE = 0x53,
  NOTIFICATION = 0x54,
  FUNCTION_RESULT = 0x55,
  NEGOTIATE_VERSION = 0x56,
  SBLR_COMPILED = 0x57,
  QUERY_PLAN = 0x58,
  STREAM_READY = 0x59,
  STREAM_DATA = 0x5a,
  STREAM_END = 0x5b,
  TXN_STATUS = 0x5c,
  PONG = 0x5d,
  CLUSTER_AUTH_OK = 0x5e,
  FEDERATED_RESULT = 0x5f,
  HEARTBEAT = 0x80,
  EXTENSION = 0x81,
}

export enum AuthMethod {
  OK = 0,
  PASSWORD = 1,
  MD5 = 2,
  SCRAM_SHA_256 = 3,
  SCRAM_SHA_512 = 4,
  TOKEN = 5,
  PEER = 6,
  REATTACH = 7,
  CERTIFICATE = 8,
  GSSAPI = 9,
  SSPI = 10,
  LDAP = 11,
  SAML = 12,
  OIDC = 13,
  MFA_TOTP = 14,
  CLUSTER_PKI = 15,
}

export const MSG_FLAG_COMPRESSED = 0x01;
export const MSG_FLAG_CONTINUED = 0x02;
export const MSG_FLAG_FINAL = 0x04;
export const MSG_FLAG_URGENT = 0x08;
export const MSG_FLAG_ENCRYPTED = 0x10;
export const MSG_FLAG_CHECKSUM = 0x20;

export const FEATURE_COMPRESSION = 1n << 0n;
export const FEATURE_STREAMING = 1n << 1n;
export const FEATURE_SBLR = 1n << 2n;
export const FEATURE_FEDERATION = 1n << 3n;
export const FEATURE_NOTIFICATIONS = 1n << 4n;
export const FEATURE_QUERY_PLAN = 1n << 5n;
export const FEATURE_BATCH = 1n << 6n;
export const FEATURE_PIPELINE = 1n << 7n;
export const FEATURE_BINARY_COPY = 1n << 8n;
export const FEATURE_SAVEPOINTS = 1n << 9n;
export const FEATURE_2PC = 1n << 10n;
export const FEATURE_CHECKSUMS = 1n << 11n;

export const QUERY_FLAG_DESCRIBE_ONLY = 0x01;
export const QUERY_FLAG_NO_PORTAL = 0x02;
export const QUERY_FLAG_BINARY_RESULT = 0x04;
export const QUERY_FLAG_INCLUDE_PLAN = 0x08;
export const QUERY_FLAG_RETURN_SBLR = 0x10;
export const QUERY_FLAG_NO_CACHE = 0x20;

export const ISOLATION_READ_UNCOMMITTED = 0;
export const ISOLATION_READ_COMMITTED = 1;
export const ISOLATION_REPEATABLE_READ = 2;
export const ISOLATION_SERIALIZABLE = 3;

export const READ_COMMITTED_MODE_DEFAULT = 0;
export const READ_COMMITTED_MODE_READ_CONSISTENCY = 1;
export const READ_COMMITTED_MODE_RECORD_VERSION = 2;
export const READ_COMMITTED_MODE_NO_RECORD_VERSION = 3;

export const TXN_FLAG_HAS_ISOLATION = 0x0001;
export const TXN_FLAG_HAS_ACCESS = 0x0002;
export const TXN_FLAG_HAS_DEFERRABLE = 0x0004;
export const TXN_FLAG_HAS_WAIT = 0x0008;
export const TXN_FLAG_HAS_TIMEOUT = 0x0010;
export const TXN_FLAG_HAS_AUTOCOMMIT = 0x0020;
export const TXN_FLAG_HAS_READ_COMMITTED_MODE = 0x0100;

export const STREAM_START = 0;
export const STREAM_PAUSE = 1;
export const STREAM_RESUME = 2;
export const STREAM_CANCEL = 3;
export const STREAM_ACK = 4;

export const SUB_TYPE_CHANNEL = 0;
export const SUB_TYPE_TABLE = 1;
export const SUB_TYPE_QUERY = 2;
export const SUB_TYPE_EVENT = 3;

export const AUTH_PARAM_METHOD_ID = "auth_method_id";
export const AUTH_PARAM_METHOD_PAYLOAD = "auth_method_payload";
export const AUTH_PARAM_PAYLOAD_JSON = "auth_payload_json";
export const AUTH_PARAM_PAYLOAD_B64 = "auth_payload_b64";
export const AUTH_PARAM_PROVIDER_PROFILE = "auth_provider_profile";
export const AUTH_PARAM_REQUIRED_METHODS = "auth_required_methods";
export const AUTH_PARAM_FORBIDDEN_METHODS = "auth_forbidden_methods";
export const AUTH_PARAM_REQUIRE_CHANNEL_BINDING = "auth_require_channel_binding";
export const AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN = "workload_identity_token";
export const AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION = "proxy_principal_assertion";

export interface AuthPluginSelection {
  methodId?: string;
  methodPayload?: string;
  payloadJson?: string;
  payloadB64?: string;
  providerProfile?: string;
  requiredMethods?: string;
  forbiddenMethods?: string;
  requireChannelBinding?: boolean;
  workloadIdentityToken?: string;
  proxyPrincipalAssertion?: string;
}

export function applyAuthPluginSelection(
  params: Record<string, string>,
  selection: AuthPluginSelection,
): void {
  const methodId = (selection.methodId ?? "").trim();
  if (methodId && !methodId.startsWith("scratchbird.auth.")) {
    throw new Error("Invalid auth_method_id namespace");
  }
  if (methodId) {
    params[AUTH_PARAM_METHOD_ID] = methodId;
  }
  if (selection.methodPayload) {
    params[AUTH_PARAM_METHOD_PAYLOAD] = selection.methodPayload;
  }
  if (selection.payloadJson) {
    params[AUTH_PARAM_PAYLOAD_JSON] = selection.payloadJson;
  }
  if (selection.payloadB64) {
    params[AUTH_PARAM_PAYLOAD_B64] = selection.payloadB64;
  }
  if (selection.providerProfile) {
    params[AUTH_PARAM_PROVIDER_PROFILE] = selection.providerProfile;
  }
  if (selection.requiredMethods) {
    params[AUTH_PARAM_REQUIRED_METHODS] = selection.requiredMethods;
  }
  if (selection.forbiddenMethods) {
    params[AUTH_PARAM_FORBIDDEN_METHODS] = selection.forbiddenMethods;
  }
  if (selection.requireChannelBinding) {
    params[AUTH_PARAM_REQUIRE_CHANNEL_BINDING] = "1";
  }
  if (selection.workloadIdentityToken) {
    params[AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN] = selection.workloadIdentityToken;
  }
  if (selection.proxyPrincipalAssertion) {
    params[AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION] = selection.proxyPrincipalAssertion;
  }
}

export interface MessageHeader {
  type: number;
  flags: number;
  length: number;
  sequence: number;
  attachmentId: Buffer;
  txnId: bigint;
}

export interface Message {
  header: MessageHeader;
  payload: Buffer;
}

export interface ColumnInfo {
  name: string;
  tableOid: number;
  columnIndex: number;
  typeOid: number;
  typeSize: number;
  typeModifier: number;
  format: number;
  nullable: boolean;
}

export interface ColumnValue {
  data: Buffer | null;
}

export interface NotificationMessage {
  processId: number;
  channel: string;
  payload: Buffer;
  changeType?: string;
  rowId?: bigint;
}

export interface QueryPlanMessage {
  format: number;
  planningTimeUs: bigint;
  estimatedRows: bigint;
  estimatedCost: bigint;
  plan: Buffer;
}

export interface SblrCompiledMessage {
  hash: bigint;
  version: number;
  bytecode: Buffer;
}

export function encodeMessage(header: MessageHeader, payload: Buffer): Buffer {
  const out = Buffer.alloc(HEADER_SIZE + payload.length);
  PROTOCOL_MAGIC_BYTES.copy(out, 0);
  out.writeUInt8(PROTOCOL_VERSION_MAJOR, 4);
  out.writeUInt8(PROTOCOL_VERSION_MINOR, 5);
  out.writeUInt8(header.type, 6);
  out.writeUInt8(header.flags ?? 0, 7);
  out.writeUInt32LE(payload.length, 8);
  out.writeUInt32LE(header.sequence ?? 0, 12);
  header.attachmentId.copy(out, 16);
  out.writeBigUInt64LE(header.txnId ?? 0n, 32);
  payload.copy(out, HEADER_SIZE);
  return out;
}

export function decodeHeader(data: Buffer): MessageHeader {
  if (data.length !== HEADER_SIZE) {
    throw new Error("Invalid header length");
  }
  if (!data.subarray(0, 4).equals(PROTOCOL_MAGIC_BYTES)) {
    throw new Error("Invalid protocol magic");
  }
  const major = data.readUInt8(4);
  const minor = data.readUInt8(5);
  if (major !== PROTOCOL_VERSION_MAJOR || minor !== PROTOCOL_VERSION_MINOR) {
    throw new Error("Unsupported protocol version");
  }
  const type = data.readUInt8(6);
  const flags = data.readUInt8(7);
  const length = data.readUInt32LE(8);
  if (length > MAX_MESSAGE_SIZE) {
    throw new Error("Payload too large");
  }
  const sequence = data.readUInt32LE(12);
  const attachmentId = data.subarray(16, 32);
  const txnId = data.readBigUInt64LE(32);
  return { type, flags, length, sequence, attachmentId, txnId };
}

export function buildStartupPayload(features: bigint, params: Record<string, string>): Buffer {
  const paramBytes = buildParamList(params);
  const payload = Buffer.alloc(2 + 2 + 8 + paramBytes.length);
  payload.writeUInt8(PROTOCOL_VERSION_MAJOR, 0);
  payload.writeUInt8(PROTOCOL_VERSION_MINOR, 1);
  payload.writeUInt16LE(0, 2);
  payload.writeBigUInt64LE(features, 4);
  paramBytes.copy(payload, 12);
  return payload;
}

function buildParamList(params: Record<string, string>): Buffer {
  const parts: Buffer[] = [];
  for (const [key, value] of Object.entries(params)) {
    parts.push(Buffer.from(key, "utf8"));
    parts.push(Buffer.from([0]));
    parts.push(Buffer.from(value, "utf8"));
    parts.push(Buffer.from([0]));
  }
  parts.push(Buffer.from([0]));
  return Buffer.concat(parts);
}

export function parseAuthRequest(payload: Buffer): { method: number; data: Buffer } {
  if (payload.length < 4) throw new Error("Auth request truncated");
  const method = payload.readUInt8(0);
  const data = payload.subarray(4);
  return { method, data };
}

export function parseAuthContinue(payload: Buffer): { method: number; stage: number; data: Buffer } {
  if (payload.length < 8) throw new Error("Auth continue truncated");
  const method = payload.readUInt8(0);
  const stage = payload.readUInt8(1);
  const dataLen = payload.readUInt32LE(4);
  if (8 + dataLen > payload.length) throw new Error("Auth continue truncated");
  return { method, stage, data: payload.subarray(8, 8 + dataLen) };
}

export function parseAuthOk(payload: Buffer): { sessionId: Buffer; serverInfo: Buffer } {
  if (payload.length < 20) throw new Error("Auth ok truncated");
  const sessionId = payload.subarray(0, 16);
  const infoLen = payload.readUInt32LE(16);
  if (20 + infoLen > payload.length) throw new Error("Auth ok truncated");
  return { sessionId, serverInfo: payload.subarray(20, 20 + infoLen) };
}

export function buildQueryPayload(sql: string, flags: number, maxRows: number, timeoutMs: number): Buffer {
  const sqlBytes = Buffer.from(sql + "\0", "utf8");
  const payload = Buffer.alloc(12 + sqlBytes.length);
  payload.writeUInt32LE(flags, 0);
  payload.writeUInt32LE(maxRows, 4);
  payload.writeUInt32LE(timeoutMs, 8);
  sqlBytes.copy(payload, 12);
  return payload;
}

export function buildParsePayload(statementName: string, sql: string, paramTypes: number[]): Buffer {
  const nameBytes = Buffer.from(statementName, "utf8");
  const sqlBytes = Buffer.from(sql, "utf8");
  const payload = Buffer.alloc(4 + nameBytes.length + 4 + sqlBytes.length + 2 + 2 + paramTypes.length * 4);
  let offset = 0;
  payload.writeUInt32LE(nameBytes.length, offset);
  offset += 4;
  nameBytes.copy(payload, offset);
  offset += nameBytes.length;
  payload.writeUInt32LE(sqlBytes.length, offset);
  offset += 4;
  sqlBytes.copy(payload, offset);
  offset += sqlBytes.length;
  payload.writeUInt16LE(paramTypes.length, offset);
  offset += 2;
  payload.writeUInt16LE(0, offset);
  offset += 2;
  for (const oid of paramTypes) {
    payload.writeUInt32LE(oid, offset);
    offset += 4;
  }
  return payload;
}

export interface ParamValue {
  format: number;
  data?: Buffer;
  isNull?: boolean;
}

export function buildBindPayload(portalName: string, statementName: string, params: ParamValue[], resultFormats: number[]): Buffer {
  const portalBytes = Buffer.from(portalName, "utf8");
  const stmtBytes = Buffer.from(statementName, "utf8");
  const paramFormats = params.map((param) => param.format);

  let payloadLen = 4 + portalBytes.length + 4 + stmtBytes.length;
  payloadLen += 2 + paramFormats.length * 2;
  payloadLen += 2 + 2;
  for (const param of params) {
    payloadLen += 4;
    if (!param.isNull && param.data) {
      payloadLen += param.data.length;
    }
  }
  payloadLen += 2 + resultFormats.length * 2;

  const payload = Buffer.alloc(payloadLen);
  let offset = 0;
  payload.writeUInt32LE(portalBytes.length, offset);
  offset += 4;
  portalBytes.copy(payload, offset);
  offset += portalBytes.length;
  payload.writeUInt32LE(stmtBytes.length, offset);
  offset += 4;
  stmtBytes.copy(payload, offset);
  offset += stmtBytes.length;
  payload.writeUInt16LE(paramFormats.length, offset);
  offset += 2;
  for (const fmt of paramFormats) {
    payload.writeUInt16LE(fmt, offset);
    offset += 2;
  }
  payload.writeUInt16LE(params.length, offset);
  offset += 2;
  payload.writeUInt16LE(0, offset);
  offset += 2;
  for (const param of params) {
    if (param.isNull) {
      payload.writeUInt32LE(0xffffffff, offset);
      offset += 4;
      continue;
    }
    const data = param.data ?? Buffer.alloc(0);
    payload.writeUInt32LE(data.length, offset);
    offset += 4;
    data.copy(payload, offset);
    offset += data.length;
  }
  payload.writeUInt16LE(resultFormats.length, offset);
  offset += 2;
  for (const fmt of resultFormats) {
    payload.writeUInt16LE(fmt, offset);
    offset += 2;
  }
  return payload;
}

export function buildDescribePayload(describeType: number, name: string): Buffer {
  const nameBytes = Buffer.from(name, "utf8");
  const payload = Buffer.alloc(8 + nameBytes.length);
  payload.writeUInt8(describeType, 0);
  payload.writeUInt32LE(nameBytes.length, 4);
  nameBytes.copy(payload, 8);
  return payload;
}

export function buildExecutePayload(portalName: string, maxRows: number): Buffer {
  const portalBytes = Buffer.from(portalName, "utf8");
  const payload = Buffer.alloc(4 + portalBytes.length + 4);
  payload.writeUInt32LE(portalBytes.length, 0);
  portalBytes.copy(payload, 4);
  payload.writeUInt32LE(maxRows, 4 + portalBytes.length);
  return payload;
}

export function buildClosePayload(closeType: number, name: string): Buffer {
  const nameBytes = Buffer.from(name, "utf8");
  const payload = Buffer.alloc(8 + nameBytes.length);
  payload.writeUInt8(closeType, 0);
  payload.writeUInt32LE(nameBytes.length, 4);
  nameBytes.copy(payload, 8);
  return payload;
}

export function buildCancelPayload(cancelType: number, targetSequence: number): Buffer {
  const payload = Buffer.alloc(8);
  payload.writeUInt32LE(cancelType, 0);
  payload.writeUInt32LE(targetSequence, 4);
  return payload;
}

export function buildSblrExecutePayload(sblrHash: bigint, bytecode: Buffer, params: ParamValue[]): Buffer {
  let payloadLen = 8 + 4 + 2 + 2 + bytecode.length;
  for (const param of params) {
    payloadLen += 4;
    if (!param.isNull && param.data) {
      payloadLen += param.data.length;
    }
  }
  const payload = Buffer.alloc(payloadLen);
  let offset = 0;
  payload.writeBigUInt64LE(sblrHash, offset);
  offset += 8;
  payload.writeUInt32LE(bytecode.length, offset);
  offset += 4;
  payload.writeUInt16LE(params.length, offset);
  offset += 2;
  payload.writeUInt16LE(0, offset);
  offset += 2;
  bytecode.copy(payload, offset);
  offset += bytecode.length;
  for (const param of params) {
    if (param.isNull) {
      payload.writeUInt32LE(0xffffffff, offset);
      offset += 4;
      continue;
    }
    const data = param.data ?? Buffer.alloc(0);
    payload.writeUInt32LE(data.length, offset);
    offset += 4;
    data.copy(payload, offset);
    offset += data.length;
  }
  return payload;
}

export function buildSubscribePayload(subscribeType: number, channel: string, filter: string): Buffer {
  const channelBytes = Buffer.from(channel, "utf8");
  const filterBytes = Buffer.from(filter, "utf8");
  const payload = Buffer.alloc(4 + 4 + channelBytes.length + 4 + filterBytes.length);
  payload.writeUInt8(subscribeType, 0);
  let offset = 4;
  payload.writeUInt32LE(channelBytes.length, offset);
  offset += 4;
  channelBytes.copy(payload, offset);
  offset += channelBytes.length;
  payload.writeUInt32LE(filterBytes.length, offset);
  offset += 4;
  filterBytes.copy(payload, offset);
  return payload;
}

export function buildUnsubscribePayload(channel: string): Buffer {
  const channelBytes = Buffer.from(channel, "utf8");
  const payload = Buffer.alloc(4 + channelBytes.length);
  payload.writeUInt32LE(channelBytes.length, 0);
  channelBytes.copy(payload, 4);
  return payload;
}

export function buildTxnBeginPayload(
  flags: number,
  conflictAction: number,
  autocommitMode: number,
  isolationLevel: number,
  accessMode: number,
  deferrable: number,
  waitMode: number,
  timeoutMs: number,
  readCommittedMode = READ_COMMITTED_MODE_DEFAULT,
): Buffer {
  const payload = Buffer.alloc(flags & TXN_FLAG_HAS_READ_COMMITTED_MODE ? 16 : 12);
  payload.writeUInt16LE(flags, 0);
  payload.writeUInt8(conflictAction, 2);
  payload.writeUInt8(autocommitMode, 3);
  payload.writeUInt8(isolationLevel, 4);
  payload.writeUInt8(accessMode, 5);
  payload.writeUInt8(deferrable, 6);
  payload.writeUInt8(waitMode, 7);
  payload.writeUInt32LE(timeoutMs, 8);
  if (flags & TXN_FLAG_HAS_READ_COMMITTED_MODE) {
    payload.writeUInt8(readCommittedMode, 12);
  }
  return payload;
}

export function canonicalReadCommittedModeLabel(mode: number): string {
  switch (mode) {
    case READ_COMMITTED_MODE_DEFAULT:
      return "READ COMMITTED";
    case READ_COMMITTED_MODE_READ_CONSISTENCY:
      return "READ COMMITTED READ CONSISTENCY";
    case READ_COMMITTED_MODE_RECORD_VERSION:
      return "READ COMMITTED RECORD VERSION";
    case READ_COMMITTED_MODE_NO_RECORD_VERSION:
      return "READ COMMITTED NO RECORD VERSION";
    default:
      return `UNKNOWN(${mode})`;
  }
}

export function buildTxnCommitPayload(flags: number): Buffer {
  const payload = Buffer.alloc(4);
  payload.writeUInt8(flags, 0);
  return payload;
}

export function buildTxnRollbackPayload(flags: number): Buffer {
  const payload = Buffer.alloc(4);
  payload.writeUInt8(flags, 0);
  return payload;
}

export function buildTxnSavepointPayload(name: string): Buffer {
  const nameBytes = Buffer.from(name, "utf8");
  const payload = Buffer.alloc(4 + nameBytes.length);
  payload.writeUInt32LE(nameBytes.length, 0);
  nameBytes.copy(payload, 4);
  return payload;
}

export function buildTxnReleasePayload(name: string): Buffer {
  return buildTxnSavepointPayload(name);
}

export function buildTxnRollbackToPayload(name: string): Buffer {
  return buildTxnSavepointPayload(name);
}

export function buildSetOptionPayload(name: string, value: string): Buffer {
  const nameBytes = Buffer.from(name, "utf8");
  const valueBytes = Buffer.from(value, "utf8");
  const payload = Buffer.alloc(4 + nameBytes.length + 4 + valueBytes.length);
  payload.writeUInt32LE(nameBytes.length, 0);
  nameBytes.copy(payload, 4);
  const offset = 4 + nameBytes.length;
  payload.writeUInt32LE(valueBytes.length, offset);
  valueBytes.copy(payload, offset + 4);
  return payload;
}

export function buildStreamControlPayload(controlType: number, windowSize: number, timeoutMs: number): Buffer {
  const payload = Buffer.alloc(12);
  payload.writeUInt8(controlType, 0);
  payload.writeUInt32LE(windowSize, 4);
  payload.writeUInt32LE(timeoutMs, 8);
  return payload;
}

export function buildAttachCreatePayload(mode: string, dbName: string): Buffer {
  const modeBytes = Buffer.from(mode, "utf8");
  const dbBytes = Buffer.from(dbName, "utf8");
  const payload = Buffer.alloc(4 + modeBytes.length + 4 + dbBytes.length);
  payload.writeUInt32LE(modeBytes.length, 0);
  modeBytes.copy(payload, 4);
  const offset = 4 + modeBytes.length;
  payload.writeUInt32LE(dbBytes.length, offset);
  dbBytes.copy(payload, offset + 4);
  return payload;
}

export function parseReady(payload: Buffer): { status: number; txnId: bigint; visibility: bigint } {
  if (payload.length < 20) throw new Error("Ready truncated");
  const status = payload.readUInt8(0);
  const txnId = payload.readBigUInt64LE(4);
  const visibility = payload.readBigUInt64LE(12);
  return { status, txnId, visibility };
}

export function parseTxnStatus(payload: Buffer): { status: string; txnId: bigint } {
  if (payload.length < 12) throw new Error("Txn status truncated");
  const status = String.fromCharCode(payload.readUInt8(0));
  const txnId = payload.readBigUInt64LE(4);
  return { status, txnId };
}

export function parseParameterStatus(payload: Buffer): { name: string; value: string } {
  if (payload.length < 8) throw new Error("Parameter status truncated");
  let offset = 0;
  const nameLen = payload.readUInt32LE(offset);
  offset += 4;
  const name = payload.subarray(offset, offset + nameLen).toString("utf8");
  offset += nameLen;
  const valueLen = payload.readUInt32LE(offset);
  offset += 4;
  const value = payload.subarray(offset, offset + valueLen).toString("utf8");
  return { name, value };
}

export function parseParameterDescription(payload: Buffer): number[] {
  if (payload.length < 4) throw new Error("Parameter description truncated");
  let offset = 0;
  const count = payload.readUInt16LE(offset);
  offset += 4;
  const types: number[] = [];
  for (let i = 0; i < count; i++) {
    if (offset + 4 > payload.length) throw new Error("Parameter description truncated");
    types.push(payload.readUInt32LE(offset));
    offset += 4;
  }
  return types;
}

export function parseRowDescription(payload: Buffer): ColumnInfo[] {
  if (payload.length < 4) throw new Error("Row description truncated");
  let offset = 0;
  const count = payload.readUInt16LE(offset);
  offset += 4;
  const cols: ColumnInfo[] = [];
  for (let i = 0; i < count; i++) {
    const nameLen = payload.readUInt32LE(offset);
    offset += 4;
    const name = payload.subarray(offset, offset + nameLen).toString("utf8");
    offset += nameLen;
    const tableOid = payload.readUInt32LE(offset);
    offset += 4;
    const columnIndex = payload.readUInt16LE(offset);
    offset += 2;
    const typeOid = payload.readUInt32LE(offset);
    offset += 4;
    const typeSize = payload.readInt16LE(offset);
    offset += 2;
    const typeModifier = payload.readInt32LE(offset);
    offset += 4;
    const format = payload.readUInt8(offset);
    offset += 1;
    const nullable = payload.readUInt8(offset) === 1;
    offset += 1;
    offset += 2;
    cols.push({ name, tableOid, columnIndex, typeOid, typeSize, typeModifier, format, nullable });
  }
  return cols;
}

export function parseDataRow(payload: Buffer, columnCount: number): ColumnValue[] {
  if (payload.length < 4) throw new Error("Row data truncated");
  let offset = 0;
  const count = payload.readUInt16LE(offset);
  offset += 2;
  const nullBytes = payload.readUInt16LE(offset);
  offset += 2;
  if (count !== columnCount) throw new Error("Row data column count mismatch");
  const nullBitmap = payload.subarray(offset, offset + nullBytes);
  offset += nullBytes;
  const values: ColumnValue[] = [];
  for (let i = 0; i < count; i++) {
    const byteIndex = Math.floor(i / 8);
    const bitIndex = i % 8;
    const isNull = byteIndex < nullBitmap.length && (nullBitmap[byteIndex] & (1 << bitIndex)) !== 0;
    if (isNull) {
      values.push({ data: null });
      continue;
    }
    const length = payload.readInt32LE(offset);
    offset += 4;
    if (length < 0) {
      values.push({ data: null });
      continue;
    }
    const data = payload.subarray(offset, offset + length);
    offset += length;
    values.push({ data: Buffer.from(data) });
  }
  return values;
}

export function parseCommandComplete(payload: Buffer): { commandType: number; rows: bigint; lastId: bigint; tag: string } {
  if (payload.length < 20) throw new Error("Command complete truncated");
  const commandType = payload.readUInt8(0);
  const rows = payload.readBigUInt64LE(4);
  const lastId = payload.readBigUInt64LE(12);
  const tagBytes = payload.subarray(20);
  const nullIdx = tagBytes.indexOf(0);
  const tag = (nullIdx >= 0 ? tagBytes.subarray(0, nullIdx) : tagBytes).toString("utf8");
  return { commandType, rows, lastId, tag };
}

export function parseNotification(payload: Buffer): NotificationMessage {
  if (payload.length < 12) throw new Error("Notification truncated");
  let offset = 0;
  const processId = payload.readUInt32LE(offset);
  offset += 4;
  const channelLen = payload.readUInt32LE(offset);
  offset += 4;
  if (offset + channelLen + 4 > payload.length) throw new Error("Notification truncated");
  const channel = payload.subarray(offset, offset + channelLen).toString("utf8");
  offset += channelLen;
  const payloadLen = payload.readUInt32LE(offset);
  offset += 4;
  if (offset + payloadLen > payload.length) throw new Error("Notification truncated");
  const data = payload.subarray(offset, offset + payloadLen);
  offset += payloadLen;
  let changeType: string | undefined;
  let rowId: bigint | undefined;
  if (offset + 1 <= payload.length) {
    changeType = String.fromCharCode(payload[offset]);
    offset += 1;
    if (offset + 8 <= payload.length) {
      rowId = payload.readBigUInt64LE(offset);
    }
  }
  return { processId, channel, payload: data, changeType, rowId };
}

export function parseQueryPlan(payload: Buffer): QueryPlanMessage {
  if (payload.length < 32) throw new Error("Query plan truncated");
  const format = payload.readUInt32LE(0);
  const planLength = payload.readUInt32LE(4);
  const planningTimeUs = payload.readBigUInt64LE(8);
  const estimatedRows = payload.readBigUInt64LE(16);
  const estimatedCost = payload.readBigUInt64LE(24);
  const planStart = 32;
  if (planStart + planLength > payload.length) throw new Error("Query plan truncated");
  const plan = payload.subarray(planStart, planStart + planLength);
  return { format, planningTimeUs, estimatedRows, estimatedCost, plan };
}

export function parseSblrCompiled(payload: Buffer): SblrCompiledMessage {
  if (payload.length < 16) throw new Error("SBLR compiled truncated");
  const hash = payload.readBigUInt64LE(0);
  const version = payload.readUInt32LE(8);
  const length = payload.readUInt32LE(12);
  if (16 + length > payload.length) throw new Error("SBLR compiled truncated");
  const bytecode = payload.subarray(16, 16 + length);
  return { hash, version, bytecode };
}

export function parseErrorMessage(payload: Buffer): { severity: string; sqlState: string; message: string; detail: string; hint: string } {
  let offset = 0;
  let severity = "";
  let sqlState = "";
  let message = "";
  let detail = "";
  let hint = "";
  while (offset < payload.length) {
    const field = payload.readUInt8(offset);
    offset += 1;
    if (field === 0) break;
    const start = offset;
    while (offset < payload.length && payload[offset] !== 0) offset += 1;
    if (offset >= payload.length) break;
    const value = payload.subarray(start, offset).toString("utf8");
    offset += 1;
    switch (String.fromCharCode(field)) {
      case "S":
        severity = value;
        break;
      case "C":
        sqlState = value;
        break;
      case "M":
        message = value;
        break;
      case "D":
        detail = value;
        break;
      case "H":
        hint = value;
        break;
    }
  }
  return { severity, sqlState, message, detail, hint };
}
