"use strict";
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
Object.defineProperty(exports, "__esModule", { value: true });
exports.STREAM_RESUME = exports.STREAM_PAUSE = exports.STREAM_START = exports.TXN_FLAG_HAS_READ_COMMITTED_MODE = exports.TXN_FLAG_HAS_AUTOCOMMIT = exports.TXN_FLAG_HAS_TIMEOUT = exports.TXN_FLAG_HAS_WAIT = exports.TXN_FLAG_HAS_DEFERRABLE = exports.TXN_FLAG_HAS_ACCESS = exports.TXN_FLAG_HAS_ISOLATION = exports.READ_COMMITTED_MODE_NO_RECORD_VERSION = exports.READ_COMMITTED_MODE_RECORD_VERSION = exports.READ_COMMITTED_MODE_READ_CONSISTENCY = exports.READ_COMMITTED_MODE_DEFAULT = exports.ISOLATION_SERIALIZABLE = exports.ISOLATION_REPEATABLE_READ = exports.ISOLATION_READ_COMMITTED = exports.ISOLATION_READ_UNCOMMITTED = exports.QUERY_FLAG_NO_CACHE = exports.QUERY_FLAG_RETURN_SBLR = exports.QUERY_FLAG_INCLUDE_PLAN = exports.QUERY_FLAG_BINARY_RESULT = exports.QUERY_FLAG_NO_PORTAL = exports.QUERY_FLAG_DESCRIBE_ONLY = exports.FEATURE_CHECKSUMS = exports.FEATURE_2PC = exports.FEATURE_SAVEPOINTS = exports.FEATURE_BINARY_COPY = exports.FEATURE_PIPELINE = exports.FEATURE_BATCH = exports.FEATURE_QUERY_PLAN = exports.FEATURE_NOTIFICATIONS = exports.FEATURE_FEDERATION = exports.FEATURE_SBLR = exports.FEATURE_STREAMING = exports.FEATURE_COMPRESSION = exports.MSG_FLAG_CHECKSUM = exports.MSG_FLAG_ENCRYPTED = exports.MSG_FLAG_URGENT = exports.MSG_FLAG_FINAL = exports.MSG_FLAG_CONTINUED = exports.MSG_FLAG_COMPRESSED = exports.AuthMethod = exports.MessageType = exports.MAX_MESSAGE_SIZE = exports.HEADER_SIZE = exports.PROTOCOL_VERSION = exports.PROTOCOL_VERSION_MINOR = exports.PROTOCOL_VERSION_MAJOR = exports.PROTOCOL_MAGIC_BYTES = void 0;
exports.AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION = exports.AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN = exports.AUTH_PARAM_REQUIRE_CHANNEL_BINDING = exports.AUTH_PARAM_FORBIDDEN_METHODS = exports.AUTH_PARAM_REQUIRED_METHODS = exports.AUTH_PARAM_PROVIDER_PROFILE = exports.AUTH_PARAM_PAYLOAD_B64 = exports.AUTH_PARAM_PAYLOAD_JSON = exports.AUTH_PARAM_METHOD_PAYLOAD = exports.AUTH_PARAM_METHOD_ID = exports.SUB_TYPE_EVENT = exports.SUB_TYPE_QUERY = exports.SUB_TYPE_TABLE = exports.SUB_TYPE_CHANNEL = exports.STREAM_ACK = exports.STREAM_CANCEL = void 0;
exports.applyAuthPluginSelection = applyAuthPluginSelection;
exports.encodeMessage = encodeMessage;
exports.decodeHeader = decodeHeader;
exports.buildStartupPayload = buildStartupPayload;
exports.parseAuthRequest = parseAuthRequest;
exports.parseAuthContinue = parseAuthContinue;
exports.parseAuthOk = parseAuthOk;
exports.buildQueryPayload = buildQueryPayload;
exports.buildParsePayload = buildParsePayload;
exports.buildBindPayload = buildBindPayload;
exports.buildDescribePayload = buildDescribePayload;
exports.buildExecutePayload = buildExecutePayload;
exports.buildClosePayload = buildClosePayload;
exports.buildCancelPayload = buildCancelPayload;
exports.buildSblrExecutePayload = buildSblrExecutePayload;
exports.buildSubscribePayload = buildSubscribePayload;
exports.buildUnsubscribePayload = buildUnsubscribePayload;
exports.buildTxnBeginPayload = buildTxnBeginPayload;
exports.canonicalReadCommittedModeLabel = canonicalReadCommittedModeLabel;
exports.buildTxnCommitPayload = buildTxnCommitPayload;
exports.buildTxnRollbackPayload = buildTxnRollbackPayload;
exports.buildTxnSavepointPayload = buildTxnSavepointPayload;
exports.buildTxnReleasePayload = buildTxnReleasePayload;
exports.buildTxnRollbackToPayload = buildTxnRollbackToPayload;
exports.buildSetOptionPayload = buildSetOptionPayload;
exports.buildStreamControlPayload = buildStreamControlPayload;
exports.buildAttachCreatePayload = buildAttachCreatePayload;
exports.parseReady = parseReady;
exports.parseTxnStatus = parseTxnStatus;
exports.parseParameterStatus = parseParameterStatus;
exports.parseParameterDescription = parseParameterDescription;
exports.parseRowDescription = parseRowDescription;
exports.parseDataRow = parseDataRow;
exports.parseCommandComplete = parseCommandComplete;
exports.parseNotification = parseNotification;
exports.parseQueryPlan = parseQueryPlan;
exports.parseSblrCompiled = parseSblrCompiled;
exports.parseErrorMessage = parseErrorMessage;
const node_buffer_1 = require("node:buffer");
const types_1 = require("./types");
exports.PROTOCOL_MAGIC_BYTES = node_buffer_1.Buffer.from("SBWP");
exports.PROTOCOL_VERSION_MAJOR = 1;
exports.PROTOCOL_VERSION_MINOR = 1;
exports.PROTOCOL_VERSION = (exports.PROTOCOL_VERSION_MAJOR << 8) | exports.PROTOCOL_VERSION_MINOR;
exports.HEADER_SIZE = 40;
exports.MAX_MESSAGE_SIZE = 1024 * 1024 * 1024;
const P1_ROW_DESCRIPTION_HEADER_BYTES = 72;
const P1_CANONICAL_TYPE_REF_BYTES = 144;
var MessageType;
(function (MessageType) {
    MessageType[MessageType["STARTUP"] = 1] = "STARTUP";
    MessageType[MessageType["AUTH_RESPONSE"] = 2] = "AUTH_RESPONSE";
    MessageType[MessageType["QUERY"] = 3] = "QUERY";
    MessageType[MessageType["PARSE"] = 4] = "PARSE";
    MessageType[MessageType["BIND"] = 5] = "BIND";
    MessageType[MessageType["DESCRIBE"] = 6] = "DESCRIBE";
    MessageType[MessageType["EXECUTE"] = 7] = "EXECUTE";
    MessageType[MessageType["CLOSE"] = 8] = "CLOSE";
    MessageType[MessageType["SYNC"] = 9] = "SYNC";
    MessageType[MessageType["FLUSH"] = 10] = "FLUSH";
    MessageType[MessageType["CANCEL"] = 11] = "CANCEL";
    MessageType[MessageType["TERMINATE"] = 12] = "TERMINATE";
    MessageType[MessageType["COPY_DATA"] = 13] = "COPY_DATA";
    MessageType[MessageType["COPY_DONE"] = 14] = "COPY_DONE";
    MessageType[MessageType["COPY_FAIL"] = 15] = "COPY_FAIL";
    MessageType[MessageType["SBLR_EXECUTE"] = 16] = "SBLR_EXECUTE";
    MessageType[MessageType["SUBSCRIBE"] = 17] = "SUBSCRIBE";
    MessageType[MessageType["UNSUBSCRIBE"] = 18] = "UNSUBSCRIBE";
    MessageType[MessageType["FEDERATED_QUERY"] = 19] = "FEDERATED_QUERY";
    MessageType[MessageType["STREAM_CONTROL"] = 20] = "STREAM_CONTROL";
    MessageType[MessageType["TXN_BEGIN"] = 21] = "TXN_BEGIN";
    MessageType[MessageType["TXN_COMMIT"] = 22] = "TXN_COMMIT";
    MessageType[MessageType["TXN_ROLLBACK"] = 23] = "TXN_ROLLBACK";
    MessageType[MessageType["TXN_SAVEPOINT"] = 24] = "TXN_SAVEPOINT";
    MessageType[MessageType["TXN_RELEASE"] = 25] = "TXN_RELEASE";
    MessageType[MessageType["TXN_ROLLBACK_TO"] = 26] = "TXN_ROLLBACK_TO";
    MessageType[MessageType["PING"] = 27] = "PING";
    MessageType[MessageType["SET_OPTION"] = 28] = "SET_OPTION";
    MessageType[MessageType["CLUSTER_AUTH"] = 29] = "CLUSTER_AUTH";
    MessageType[MessageType["ATTACH_CREATE"] = 30] = "ATTACH_CREATE";
    MessageType[MessageType["ATTACH_DETACH"] = 31] = "ATTACH_DETACH";
    MessageType[MessageType["ATTACH_LIST"] = 32] = "ATTACH_LIST";
    MessageType[MessageType["AUTH_REQUEST"] = 64] = "AUTH_REQUEST";
    MessageType[MessageType["AUTH_OK"] = 65] = "AUTH_OK";
    MessageType[MessageType["AUTH_CONTINUE"] = 66] = "AUTH_CONTINUE";
    MessageType[MessageType["READY"] = 67] = "READY";
    MessageType[MessageType["ROW_DESCRIPTION"] = 68] = "ROW_DESCRIPTION";
    MessageType[MessageType["DATA_ROW"] = 69] = "DATA_ROW";
    MessageType[MessageType["COMMAND_COMPLETE"] = 70] = "COMMAND_COMPLETE";
    MessageType[MessageType["EMPTY_QUERY"] = 71] = "EMPTY_QUERY";
    MessageType[MessageType["ERROR"] = 72] = "ERROR";
    MessageType[MessageType["NOTICE"] = 73] = "NOTICE";
    MessageType[MessageType["PARSE_COMPLETE"] = 74] = "PARSE_COMPLETE";
    MessageType[MessageType["BIND_COMPLETE"] = 75] = "BIND_COMPLETE";
    MessageType[MessageType["CLOSE_COMPLETE"] = 76] = "CLOSE_COMPLETE";
    MessageType[MessageType["PORTAL_SUSPENDED"] = 77] = "PORTAL_SUSPENDED";
    MessageType[MessageType["NO_DATA"] = 78] = "NO_DATA";
    MessageType[MessageType["PARAMETER_STATUS"] = 79] = "PARAMETER_STATUS";
    MessageType[MessageType["PARAMETER_DESCRIPTION"] = 80] = "PARAMETER_DESCRIPTION";
    MessageType[MessageType["COPY_IN_RESPONSE"] = 81] = "COPY_IN_RESPONSE";
    MessageType[MessageType["COPY_OUT_RESPONSE"] = 82] = "COPY_OUT_RESPONSE";
    MessageType[MessageType["COPY_BOTH_RESPONSE"] = 83] = "COPY_BOTH_RESPONSE";
    MessageType[MessageType["NOTIFICATION"] = 84] = "NOTIFICATION";
    MessageType[MessageType["FUNCTION_RESULT"] = 85] = "FUNCTION_RESULT";
    MessageType[MessageType["NEGOTIATE_VERSION"] = 86] = "NEGOTIATE_VERSION";
    MessageType[MessageType["SBLR_COMPILED"] = 87] = "SBLR_COMPILED";
    MessageType[MessageType["QUERY_PLAN"] = 88] = "QUERY_PLAN";
    MessageType[MessageType["STREAM_READY"] = 89] = "STREAM_READY";
    MessageType[MessageType["STREAM_DATA"] = 90] = "STREAM_DATA";
    MessageType[MessageType["STREAM_END"] = 91] = "STREAM_END";
    MessageType[MessageType["TXN_STATUS"] = 92] = "TXN_STATUS";
    MessageType[MessageType["PONG"] = 93] = "PONG";
    MessageType[MessageType["CLUSTER_AUTH_OK"] = 94] = "CLUSTER_AUTH_OK";
    MessageType[MessageType["FEDERATED_RESULT"] = 95] = "FEDERATED_RESULT";
    MessageType[MessageType["HEARTBEAT"] = 128] = "HEARTBEAT";
    MessageType[MessageType["EXTENSION"] = 129] = "EXTENSION";
})(MessageType || (exports.MessageType = MessageType = {}));
var AuthMethod;
(function (AuthMethod) {
    AuthMethod[AuthMethod["OK"] = 0] = "OK";
    AuthMethod[AuthMethod["PASSWORD"] = 1] = "PASSWORD";
    AuthMethod[AuthMethod["MD5"] = 2] = "MD5";
    AuthMethod[AuthMethod["SCRAM_SHA_256"] = 3] = "SCRAM_SHA_256";
    AuthMethod[AuthMethod["SCRAM_SHA_512"] = 4] = "SCRAM_SHA_512";
    AuthMethod[AuthMethod["TOKEN"] = 5] = "TOKEN";
    AuthMethod[AuthMethod["PEER"] = 6] = "PEER";
    AuthMethod[AuthMethod["REATTACH"] = 7] = "REATTACH";
    AuthMethod[AuthMethod["CERTIFICATE"] = 8] = "CERTIFICATE";
    AuthMethod[AuthMethod["GSSAPI"] = 9] = "GSSAPI";
    AuthMethod[AuthMethod["SSPI"] = 10] = "SSPI";
    AuthMethod[AuthMethod["LDAP"] = 11] = "LDAP";
    AuthMethod[AuthMethod["SAML"] = 12] = "SAML";
    AuthMethod[AuthMethod["OIDC"] = 13] = "OIDC";
    AuthMethod[AuthMethod["MFA_TOTP"] = 14] = "MFA_TOTP";
    AuthMethod[AuthMethod["CLUSTER_PKI"] = 15] = "CLUSTER_PKI";
})(AuthMethod || (exports.AuthMethod = AuthMethod = {}));
exports.MSG_FLAG_COMPRESSED = 0x01;
exports.MSG_FLAG_CONTINUED = 0x02;
exports.MSG_FLAG_FINAL = 0x04;
exports.MSG_FLAG_URGENT = 0x08;
exports.MSG_FLAG_ENCRYPTED = 0x10;
exports.MSG_FLAG_CHECKSUM = 0x20;
exports.FEATURE_COMPRESSION = 1n << 0n;
exports.FEATURE_STREAMING = 1n << 1n;
exports.FEATURE_SBLR = 1n << 2n;
exports.FEATURE_FEDERATION = 1n << 3n;
exports.FEATURE_NOTIFICATIONS = 1n << 4n;
exports.FEATURE_QUERY_PLAN = 1n << 5n;
exports.FEATURE_BATCH = 1n << 6n;
exports.FEATURE_PIPELINE = 1n << 7n;
exports.FEATURE_BINARY_COPY = 1n << 8n;
exports.FEATURE_SAVEPOINTS = 1n << 9n;
exports.FEATURE_2PC = 1n << 10n;
exports.FEATURE_CHECKSUMS = 1n << 11n;
exports.QUERY_FLAG_DESCRIBE_ONLY = 0x01;
exports.QUERY_FLAG_NO_PORTAL = 0x02;
exports.QUERY_FLAG_BINARY_RESULT = 0x04;
exports.QUERY_FLAG_INCLUDE_PLAN = 0x08;
exports.QUERY_FLAG_RETURN_SBLR = 0x10;
exports.QUERY_FLAG_NO_CACHE = 0x20;
exports.ISOLATION_READ_UNCOMMITTED = 0;
exports.ISOLATION_READ_COMMITTED = 1;
exports.ISOLATION_REPEATABLE_READ = 2;
exports.ISOLATION_SERIALIZABLE = 3;
exports.READ_COMMITTED_MODE_DEFAULT = 0;
exports.READ_COMMITTED_MODE_READ_CONSISTENCY = 1;
exports.READ_COMMITTED_MODE_RECORD_VERSION = 2;
exports.READ_COMMITTED_MODE_NO_RECORD_VERSION = 3;
exports.TXN_FLAG_HAS_ISOLATION = 0x0001;
exports.TXN_FLAG_HAS_ACCESS = 0x0002;
exports.TXN_FLAG_HAS_DEFERRABLE = 0x0004;
exports.TXN_FLAG_HAS_WAIT = 0x0008;
exports.TXN_FLAG_HAS_TIMEOUT = 0x0010;
exports.TXN_FLAG_HAS_AUTOCOMMIT = 0x0020;
exports.TXN_FLAG_HAS_READ_COMMITTED_MODE = 0x0100;
exports.STREAM_START = 0;
exports.STREAM_PAUSE = 1;
exports.STREAM_RESUME = 2;
exports.STREAM_CANCEL = 3;
exports.STREAM_ACK = 4;
exports.SUB_TYPE_CHANNEL = 0;
exports.SUB_TYPE_TABLE = 1;
exports.SUB_TYPE_QUERY = 2;
exports.SUB_TYPE_EVENT = 3;
exports.AUTH_PARAM_METHOD_ID = "auth_method_id";
exports.AUTH_PARAM_METHOD_PAYLOAD = "auth_method_payload";
exports.AUTH_PARAM_PAYLOAD_JSON = "auth_payload_json";
exports.AUTH_PARAM_PAYLOAD_B64 = "auth_payload_b64";
exports.AUTH_PARAM_PROVIDER_PROFILE = "auth_provider_profile";
exports.AUTH_PARAM_REQUIRED_METHODS = "auth_required_methods";
exports.AUTH_PARAM_FORBIDDEN_METHODS = "auth_forbidden_methods";
exports.AUTH_PARAM_REQUIRE_CHANNEL_BINDING = "auth_require_channel_binding";
exports.AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN = "workload_identity_token";
exports.AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION = "proxy_principal_assertion";
function applyAuthPluginSelection(params, selection) {
    const methodId = (selection.methodId ?? "").trim();
    if (methodId && !methodId.startsWith("scratchbird.auth.")) {
        throw new Error("Invalid auth_method_id namespace");
    }
    if (methodId) {
        params[exports.AUTH_PARAM_METHOD_ID] = methodId;
    }
    if (selection.methodPayload) {
        params[exports.AUTH_PARAM_METHOD_PAYLOAD] = selection.methodPayload;
    }
    if (selection.payloadJson) {
        params[exports.AUTH_PARAM_PAYLOAD_JSON] = selection.payloadJson;
    }
    if (selection.payloadB64) {
        params[exports.AUTH_PARAM_PAYLOAD_B64] = selection.payloadB64;
    }
    if (selection.providerProfile) {
        params[exports.AUTH_PARAM_PROVIDER_PROFILE] = selection.providerProfile;
    }
    if (selection.requiredMethods) {
        params[exports.AUTH_PARAM_REQUIRED_METHODS] = selection.requiredMethods;
    }
    if (selection.forbiddenMethods) {
        params[exports.AUTH_PARAM_FORBIDDEN_METHODS] = selection.forbiddenMethods;
    }
    if (selection.requireChannelBinding) {
        params[exports.AUTH_PARAM_REQUIRE_CHANNEL_BINDING] = "1";
    }
    if (selection.workloadIdentityToken) {
        params[exports.AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN] = selection.workloadIdentityToken;
    }
    if (selection.proxyPrincipalAssertion) {
        params[exports.AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION] = selection.proxyPrincipalAssertion;
    }
}
function encodeMessage(header, payload) {
    const out = node_buffer_1.Buffer.alloc(exports.HEADER_SIZE + payload.length);
    exports.PROTOCOL_MAGIC_BYTES.copy(out, 0);
    out.writeUInt8(exports.PROTOCOL_VERSION_MAJOR, 4);
    out.writeUInt8(exports.PROTOCOL_VERSION_MINOR, 5);
    out.writeUInt8(header.type, 6);
    out.writeUInt8(header.flags ?? 0, 7);
    out.writeUInt32LE(payload.length, 8);
    out.writeUInt32LE(header.sequence ?? 0, 12);
    header.attachmentId.copy(out, 16);
    out.writeBigUInt64LE(header.txnId ?? 0n, 32);
    payload.copy(out, exports.HEADER_SIZE);
    return out;
}
function decodeHeader(data) {
    if (data.length !== exports.HEADER_SIZE) {
        throw new Error("Invalid header length");
    }
    if (!data.subarray(0, 4).equals(exports.PROTOCOL_MAGIC_BYTES)) {
        throw new Error("Invalid protocol magic");
    }
    const major = data.readUInt8(4);
    const minor = data.readUInt8(5);
    if (major !== exports.PROTOCOL_VERSION_MAJOR || minor !== exports.PROTOCOL_VERSION_MINOR) {
        throw new Error("Unsupported protocol version");
    }
    const type = data.readUInt8(6);
    const flags = data.readUInt8(7);
    const length = data.readUInt32LE(8);
    if (length > exports.MAX_MESSAGE_SIZE) {
        throw new Error("Payload too large");
    }
    const sequence = data.readUInt32LE(12);
    const attachmentId = data.subarray(16, 32);
    const txnId = data.readBigUInt64LE(32);
    return { type, flags, length, sequence, attachmentId, txnId };
}
function buildStartupPayload(features, params) {
    const paramBytes = buildP1ParamList(params);
    const payload = node_buffer_1.Buffer.alloc(88 + paramBytes.length);
    let offset = 0;
    payload.writeUInt16LE(exports.PROTOCOL_VERSION, offset);
    offset += 2;
    payload.writeUInt16LE(exports.PROTOCOL_VERSION, offset);
    offset += 2;
    payload.writeUInt32LE(0, offset);
    offset += 4;
    payload.writeBigUInt64LE(features, offset);
    offset += 8;
    payload.writeBigUInt64LE(0n, offset);
    offset += 8;
    payload.writeBigUInt64LE(0n, offset);
    offset += 8;
    payload.fill(0x11, offset, offset + 16);
    offset += 16;
    payload.fill(0, offset, offset + 32);
    offset += 32;
    const entries = Object.entries(params).sort(([left], [right]) => left.localeCompare(right));
    payload.writeUInt32LE(entries.length, offset);
    offset += 4;
    paramBytes.copy(payload, offset);
    offset += paramBytes.length;
    payload.writeUInt32LE(0, offset);
    return payload;
}
function buildP1ParamList(params) {
    const parts = [];
    const entries = Object.entries(params).sort(([left], [right]) => left.localeCompare(right));
    for (const [key, value] of entries) {
        const keyBytes = node_buffer_1.Buffer.from(key, "utf8");
        const valueBytes = node_buffer_1.Buffer.from(value, "utf8");
        const keyLength = node_buffer_1.Buffer.alloc(4);
        keyLength.writeUInt32LE(keyBytes.length, 0);
        const valueLength = node_buffer_1.Buffer.alloc(4);
        valueLength.writeUInt32LE(valueBytes.length, 0);
        parts.push(keyLength);
        parts.push(keyBytes);
        parts.push(node_buffer_1.Buffer.from([0x01, 0x00]));
        parts.push(valueLength);
        parts.push(valueBytes);
    }
    return node_buffer_1.Buffer.concat(parts);
}
function parseAuthRequest(payload) {
    if (payload.length < 4)
        throw new Error("Auth request truncated");
    const method = payload.readUInt8(0);
    const data = payload.subarray(4);
    return { method, data };
}
function parseAuthContinue(payload) {
    if (payload.length < 8)
        throw new Error("Auth continue truncated");
    const method = payload.readUInt8(0);
    const stage = payload.readUInt8(1);
    const dataLen = payload.readUInt32LE(4);
    if (8 + dataLen > payload.length)
        throw new Error("Auth continue truncated");
    return { method, stage, data: payload.subarray(8, 8 + dataLen) };
}
function parseAuthOk(payload) {
    if (payload.length < 20)
        throw new Error("Auth ok truncated");
    const sessionId = payload.subarray(0, 16);
    const infoLen = payload.readUInt32LE(16);
    if (20 + infoLen > payload.length)
        throw new Error("Auth ok truncated");
    return { sessionId, serverInfo: payload.subarray(20, 20 + infoLen) };
}
function buildQueryPayload(sql, flags, maxRows, timeoutMs) {
    const sqlBytes = node_buffer_1.Buffer.from(sql, "utf8");
    const payload = node_buffer_1.Buffer.alloc(12 + sqlBytes.length);
    payload.writeUInt32LE(flags, 0);
    payload.writeUInt32LE(maxRows, 4);
    payload.writeUInt32LE(timeoutMs, 8);
    sqlBytes.copy(payload, 12);
    return payload;
}
function buildParsePayload(statementName, sql, paramTypes) {
    const nameBytes = node_buffer_1.Buffer.from(statementName, "utf8");
    const sqlBytes = node_buffer_1.Buffer.from(sql, "utf8");
    const payload = node_buffer_1.Buffer.alloc(4 + nameBytes.length + 4 + sqlBytes.length + 2 + 2 + paramTypes.length * 4);
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
function buildBindPayload(portalName, statementName, params, resultFormats) {
    const portalBytes = node_buffer_1.Buffer.from(portalName, "utf8");
    const stmtBytes = node_buffer_1.Buffer.from(statementName, "utf8");
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
    const payload = node_buffer_1.Buffer.alloc(payloadLen);
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
        const data = param.data ?? node_buffer_1.Buffer.alloc(0);
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
function buildDescribePayload(describeType, name) {
    const nameBytes = node_buffer_1.Buffer.from(name, "utf8");
    const payload = node_buffer_1.Buffer.alloc(8 + nameBytes.length);
    payload.writeUInt8(describeType, 0);
    payload.writeUInt32LE(nameBytes.length, 4);
    nameBytes.copy(payload, 8);
    return payload;
}
function buildExecutePayload(portalName, maxRows) {
    const portalBytes = node_buffer_1.Buffer.from(portalName, "utf8");
    const payload = node_buffer_1.Buffer.alloc(4 + portalBytes.length + 4);
    payload.writeUInt32LE(portalBytes.length, 0);
    portalBytes.copy(payload, 4);
    payload.writeUInt32LE(maxRows, 4 + portalBytes.length);
    return payload;
}
function buildClosePayload(closeType, name) {
    const nameBytes = node_buffer_1.Buffer.from(name, "utf8");
    const payload = node_buffer_1.Buffer.alloc(8 + nameBytes.length);
    payload.writeUInt8(closeType, 0);
    payload.writeUInt32LE(nameBytes.length, 4);
    nameBytes.copy(payload, 8);
    return payload;
}
function buildCancelPayload(cancelType, targetSequence) {
    const payload = node_buffer_1.Buffer.alloc(8);
    payload.writeUInt32LE(cancelType, 0);
    payload.writeUInt32LE(targetSequence, 4);
    return payload;
}
function buildSblrExecutePayload(sblrHash, bytecode, params) {
    let payloadLen = 8 + 4 + 2 + 2 + bytecode.length;
    for (const param of params) {
        payloadLen += 4;
        if (!param.isNull && param.data) {
            payloadLen += param.data.length;
        }
    }
    const payload = node_buffer_1.Buffer.alloc(payloadLen);
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
        const data = param.data ?? node_buffer_1.Buffer.alloc(0);
        payload.writeUInt32LE(data.length, offset);
        offset += 4;
        data.copy(payload, offset);
        offset += data.length;
    }
    return payload;
}
function buildSubscribePayload(subscribeType, channel, filter) {
    const channelBytes = node_buffer_1.Buffer.from(channel, "utf8");
    const filterBytes = node_buffer_1.Buffer.from(filter, "utf8");
    const payload = node_buffer_1.Buffer.alloc(4 + 4 + channelBytes.length + 4 + filterBytes.length);
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
function buildUnsubscribePayload(channel) {
    const channelBytes = node_buffer_1.Buffer.from(channel, "utf8");
    const payload = node_buffer_1.Buffer.alloc(4 + channelBytes.length);
    payload.writeUInt32LE(channelBytes.length, 0);
    channelBytes.copy(payload, 4);
    return payload;
}
function buildTxnBeginPayload(flags, conflictAction, autocommitMode, isolationLevel, accessMode, deferrable, waitMode, timeoutMs, readCommittedMode = exports.READ_COMMITTED_MODE_DEFAULT) {
    const payload = node_buffer_1.Buffer.alloc(flags & exports.TXN_FLAG_HAS_READ_COMMITTED_MODE ? 16 : 12);
    payload.writeUInt16LE(flags, 0);
    payload.writeUInt8(conflictAction, 2);
    payload.writeUInt8(autocommitMode, 3);
    payload.writeUInt8(isolationLevel, 4);
    payload.writeUInt8(accessMode, 5);
    payload.writeUInt8(deferrable, 6);
    payload.writeUInt8(waitMode, 7);
    payload.writeUInt32LE(timeoutMs, 8);
    if (flags & exports.TXN_FLAG_HAS_READ_COMMITTED_MODE) {
        payload.writeUInt8(readCommittedMode, 12);
    }
    return payload;
}
function canonicalReadCommittedModeLabel(mode) {
    switch (mode) {
        case exports.READ_COMMITTED_MODE_DEFAULT:
            return "READ COMMITTED";
        case exports.READ_COMMITTED_MODE_READ_CONSISTENCY:
            return "READ COMMITTED READ CONSISTENCY";
        case exports.READ_COMMITTED_MODE_RECORD_VERSION:
            return "READ COMMITTED RECORD VERSION";
        case exports.READ_COMMITTED_MODE_NO_RECORD_VERSION:
            return "READ COMMITTED NO RECORD VERSION";
        default:
            return `UNKNOWN(${mode})`;
    }
}
function buildTxnCommitPayload(flags) {
    const payload = node_buffer_1.Buffer.alloc(4);
    payload.writeUInt8(flags, 0);
    return payload;
}
function buildTxnRollbackPayload(flags) {
    const payload = node_buffer_1.Buffer.alloc(4);
    payload.writeUInt8(flags, 0);
    return payload;
}
function buildTxnSavepointPayload(name) {
    const nameBytes = node_buffer_1.Buffer.from(name, "utf8");
    const payload = node_buffer_1.Buffer.alloc(4 + nameBytes.length);
    payload.writeUInt32LE(nameBytes.length, 0);
    nameBytes.copy(payload, 4);
    return payload;
}
function buildTxnReleasePayload(name) {
    return buildTxnSavepointPayload(name);
}
function buildTxnRollbackToPayload(name) {
    return buildTxnSavepointPayload(name);
}
function buildSetOptionPayload(name, value) {
    const nameBytes = node_buffer_1.Buffer.from(name, "utf8");
    const valueBytes = node_buffer_1.Buffer.from(value, "utf8");
    const payload = node_buffer_1.Buffer.alloc(4 + nameBytes.length + 4 + valueBytes.length);
    payload.writeUInt32LE(nameBytes.length, 0);
    nameBytes.copy(payload, 4);
    const offset = 4 + nameBytes.length;
    payload.writeUInt32LE(valueBytes.length, offset);
    valueBytes.copy(payload, offset + 4);
    return payload;
}
function buildStreamControlPayload(controlType, windowSize, timeoutMs) {
    const payload = node_buffer_1.Buffer.alloc(12);
    payload.writeUInt8(controlType, 0);
    payload.writeUInt32LE(windowSize, 4);
    payload.writeUInt32LE(timeoutMs, 8);
    return payload;
}
function buildAttachCreatePayload(mode, dbName) {
    const modeBytes = node_buffer_1.Buffer.from(mode, "utf8");
    const dbBytes = node_buffer_1.Buffer.from(dbName, "utf8");
    const payload = node_buffer_1.Buffer.alloc(4 + modeBytes.length + 4 + dbBytes.length);
    payload.writeUInt32LE(modeBytes.length, 0);
    modeBytes.copy(payload, 4);
    const offset = 4 + modeBytes.length;
    payload.writeUInt32LE(dbBytes.length, offset);
    dbBytes.copy(payload, offset + 4);
    return payload;
}
function parseReady(payload) {
    if (payload.length >= 76) {
        const statusByte = payload.readUInt8(56);
        if (statusByte === 0x49 ||
            statusByte === 0x54 ||
            statusByte === 0x45 ||
            statusByte === 0x52 ||
            statusByte === 0x41) {
            const txnId = payload.readBigUInt64LE(48);
            const status = statusByte === 0x54 || statusByte === 0x45 ? 1 : 0;
            return { status, txnId, visibility: txnId };
        }
    }
    if (payload.length < 20)
        throw new Error("Ready truncated");
    const status = payload.readUInt8(0);
    const txnId = payload.readBigUInt64LE(4);
    const visibility = payload.readBigUInt64LE(12);
    return { status, txnId, visibility };
}
function parseTxnStatus(payload) {
    if (payload.length < 12)
        throw new Error("Txn status truncated");
    const status = String.fromCharCode(payload.readUInt8(0));
    const txnId = payload.readBigUInt64LE(4);
    return { status, txnId };
}
function parseParameterStatus(payload) {
    if (payload.length < 8)
        throw new Error("Parameter status truncated");
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
function parseParameterDescription(payload) {
    if (isP1RowDescription(payload)) {
        const count = payload.readUInt32LE(68);
        let offset = P1_ROW_DESCRIPTION_HEADER_BYTES;
        const types = [];
        for (let i = 0; i < count; i++) {
            if (offset + 4 + 4 + 8 + 8 + P1_CANONICAL_TYPE_REF_BYTES + 4 + 5 > payload.length) {
                throw new Error("P1 parameter description truncated");
            }
            const typeOffset = offset + 4 + 4 + 8 + 8;
            types.push(oidFromCanonicalTypeRef(payload, typeOffset));
            offset = typeOffset + P1_CANONICAL_TYPE_REF_BYTES + 4;
            offset = readNullableText(payload, offset).offset;
        }
        return types;
    }
    if (payload.length < 4)
        throw new Error("Parameter description truncated");
    let offset = 0;
    const count = payload.readUInt16LE(offset);
    offset += 4;
    const types = [];
    for (let i = 0; i < count; i++) {
        if (offset + 4 > payload.length)
            throw new Error("Parameter description truncated");
        types.push(payload.readUInt32LE(offset));
        offset += 4;
    }
    return types;
}
function parseRowDescription(payload) {
    if (isP1RowDescription(payload))
        return parseP1RowDescription(payload);
    if (payload.length < 4)
        throw new Error("Row description truncated");
    let offset = 0;
    const count = payload.readUInt16LE(offset);
    offset += 4;
    const cols = [];
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
function isP1RowDescription(payload) {
    return (payload.length >= P1_ROW_DESCRIPTION_HEADER_BYTES &&
        payload.readUInt16LE(0) === 1 &&
        payload.readUInt8(3) === 1);
}
function parseP1RowDescription(payload) {
    const count = payload.readInt32LE(4);
    if (count < 0)
        throw new Error("P1 row description column count invalid");
    let offset = P1_ROW_DESCRIPTION_HEADER_BYTES;
    const cols = [];
    for (let i = 0; i < count; i++) {
        const fixedColumnBytes = 4 + 4 + 8 + P1_CANONICAL_TYPE_REF_BYTES + 56;
        if (offset + fixedColumnBytes > payload.length)
            throw new Error("P1 row description truncated");
        const ordinal = payload.readInt32LE(offset);
        offset += 4;
        offset += 1;
        const format = payload.readUInt8(offset) === 1 ? types_1.FORMAT_TEXT : types_1.FORMAT_BINARY;
        offset += 1;
        const nullable = payload.readUInt8(offset) === 1;
        offset += 1;
        offset += 1;
        offset += 8;
        const typeOid = oidFromCanonicalTypeRef(payload, offset);
        offset += P1_CANONICAL_TYPE_REF_BYTES;
        offset += 16 * 3;
        offset += 4;
        offset += 2;
        offset += 2;
        const text = readNullableText(payload, offset);
        offset = text.offset;
        const name = text.value || `column${i + 1}`;
        cols.push({
            name,
            tableOid: 0,
            columnIndex: ordinal === 0 ? i : ordinal - 1,
            typeOid,
            typeSize: typeSizeForOid(typeOid),
            typeModifier: -1,
            format,
            nullable,
        });
    }
    return cols;
}
function oidFromCanonicalTypeRef(payload, offset) {
    if (offset + 4 > payload.length)
        return types_1.OID_TEXT;
    const family = payload.readUInt16LE(offset);
    const code = payload.readUInt16LE(offset + 2);
    if (family === 1 && code === 1)
        return types_1.OID_BOOL;
    if (family === 2 && code === 3)
        return types_1.OID_INT4;
    if (family === 2 && code === 4)
        return types_1.OID_INT8;
    if (family === 4 && code === 1)
        return types_1.OID_NUMERIC;
    if (family === 6 && code === 2)
        return types_1.OID_FLOAT8;
    if (family === 8 && code === 1)
        return types_1.OID_TEXT;
    if (family === 9)
        return types_1.OID_BYTEA;
    if (family === 11) {
        if (code === 1)
            return types_1.OID_DATE;
        if (code === 2)
            return types_1.OID_TIME;
        return types_1.OID_TIMESTAMP;
    }
    if (family === 12)
        return types_1.OID_INTERVAL;
    if (family === 13)
        return types_1.OID_UUID;
    if (family === 19) {
        if (code === 3)
            return types_1.OID_MACADDR;
        return types_1.OID_INET;
    }
    if (family === 20)
        return types_1.OID_JSON;
    return types_1.OID_TEXT;
}
function typeSizeForOid(typeOid) {
    if (typeOid === types_1.OID_BOOL)
        return 1;
    if (typeOid === types_1.OID_INT4)
        return 4;
    if (typeOid === types_1.OID_INT8 || typeOid === types_1.OID_FLOAT8)
        return 8;
    if (typeOid === types_1.OID_UUID)
        return 16;
    return -1;
}
function readNullableText(payload, offset) {
    if (offset + 5 > payload.length)
        throw new Error("nullable text truncated");
    const tag = payload.readUInt8(offset);
    offset += 1;
    const length = payload.readInt32LE(offset);
    offset += 4;
    if (length < 0)
        throw new Error("nullable text length invalid");
    if (tag === 0)
        return { value: "", offset };
    if (offset + length > payload.length)
        throw new Error("nullable text truncated");
    return { value: payload.subarray(offset, offset + length).toString("utf8"), offset: offset + length };
}
function parseDataRow(payload, columnCount) {
    if (payload.length < 4)
        throw new Error("Row data truncated");
    let offset = 0;
    const count = payload.readUInt16LE(offset);
    offset += 2;
    const nullBytes = payload.readUInt16LE(offset);
    offset += 2;
    if (count < columnCount)
        throw new Error("Row data column count mismatch");
    const nullBitmap = payload.subarray(offset, offset + nullBytes);
    offset += nullBytes;
    const values = [];
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
        values.push({ data: node_buffer_1.Buffer.from(data) });
    }
    return values;
}
function parseCommandComplete(payload) {
    if (payload.length < 20)
        throw new Error("Command complete truncated");
    const commandType = payload.readUInt8(0);
    const rows = payload.readBigUInt64LE(4);
    const lastId = payload.readBigUInt64LE(12);
    const tagBytes = payload.subarray(20);
    const nullIdx = tagBytes.indexOf(0);
    const tag = (nullIdx >= 0 ? tagBytes.subarray(0, nullIdx) : tagBytes).toString("utf8");
    return { commandType, rows, lastId, tag };
}
function parseNotification(payload) {
    if (payload.length < 12)
        throw new Error("Notification truncated");
    let offset = 0;
    const processId = payload.readUInt32LE(offset);
    offset += 4;
    const channelLen = payload.readUInt32LE(offset);
    offset += 4;
    if (offset + channelLen + 4 > payload.length)
        throw new Error("Notification truncated");
    const channel = payload.subarray(offset, offset + channelLen).toString("utf8");
    offset += channelLen;
    const payloadLen = payload.readUInt32LE(offset);
    offset += 4;
    if (offset + payloadLen > payload.length)
        throw new Error("Notification truncated");
    const data = payload.subarray(offset, offset + payloadLen);
    offset += payloadLen;
    let changeType;
    let rowId;
    if (offset + 1 <= payload.length) {
        changeType = String.fromCharCode(payload[offset]);
        offset += 1;
        if (offset + 8 <= payload.length) {
            rowId = payload.readBigUInt64LE(offset);
        }
    }
    return { processId, channel, payload: data, changeType, rowId };
}
function parseQueryPlan(payload) {
    if (payload.length < 32)
        throw new Error("Query plan truncated");
    const format = payload.readUInt32LE(0);
    const planLength = payload.readUInt32LE(4);
    const planningTimeUs = payload.readBigUInt64LE(8);
    const estimatedRows = payload.readBigUInt64LE(16);
    const estimatedCost = payload.readBigUInt64LE(24);
    const planStart = 32;
    if (planStart + planLength > payload.length)
        throw new Error("Query plan truncated");
    const plan = payload.subarray(planStart, planStart + planLength);
    return { format, planningTimeUs, estimatedRows, estimatedCost, plan };
}
function parseSblrCompiled(payload) {
    if (payload.length < 16)
        throw new Error("SBLR compiled truncated");
    const hash = payload.readBigUInt64LE(0);
    const version = payload.readUInt32LE(8);
    const length = payload.readUInt32LE(12);
    if (16 + length > payload.length)
        throw new Error("SBLR compiled truncated");
    const bytecode = payload.subarray(16, 16 + length);
    return { hash, version, bytecode };
}
function parseErrorMessage(payload) {
    let offset = 0;
    let severity = "";
    let sqlState = "";
    let message = "";
    let detail = "";
    let hint = "";
    while (offset < payload.length) {
        const field = payload.readUInt8(offset);
        offset += 1;
        if (field === 0)
            break;
        const start = offset;
        while (offset < payload.length && payload[offset] !== 0)
            offset += 1;
        if (offset >= payload.length)
            break;
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
