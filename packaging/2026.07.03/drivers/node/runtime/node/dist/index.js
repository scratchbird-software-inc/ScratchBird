"use strict";
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __exportStar = (this && this.__exportStar) || function(m, exports) {
    for (var p in m) if (p !== "default" && !Object.prototype.hasOwnProperty.call(exports, p)) __createBinding(exports, m, p);
};
Object.defineProperty(exports, "__esModule", { value: true });
exports.ScratchbirdRange = exports.ScratchbirdGeometry = exports.ScratchbirdJson = exports.ScratchbirdJsonb = exports.OID_SB_VECTOR = exports.OID_TSQUERY = exports.OID_TSVECTOR = exports.OID_INT8RANGE = exports.OID_DATERANGE = exports.OID_TSTZRANGE = exports.OID_TSRANGE = exports.OID_NUMRANGE = exports.OID_INT4RANGE = exports.OID_JSONB = exports.OID_UUID = exports.OID_NUMERIC = exports.OID_INTERVAL = exports.OID_TIMESTAMPTZ = exports.OID_TIMESTAMP = exports.OID_TIME = exports.OID_DATE = exports.OID_VARCHAR = exports.OID_INET = exports.OID_CIDR = exports.OID_MACADDR8 = exports.OID_MACADDR = exports.OID_MONEY = exports.OID_FLOAT8 = exports.OID_FLOAT4 = exports.OID_CIRCLE = exports.OID_LINE = exports.OID_POLYGON = exports.OID_BOX = exports.OID_PATH = exports.OID_LSEG = exports.OID_POINT = exports.OID_XML = exports.OID_JSON = exports.OID_TEXT = exports.OID_INT4 = exports.OID_INT2 = exports.OID_INT8 = exports.OID_BPCHAR = exports.OID_CHAR = exports.OID_BYTEA = exports.OID_BOOL = exports.FORMAT_BINARY = exports.FORMAT_TEXT = exports.Pool = exports.Client = void 0;
exports.canonicalReadCommittedModeLabel = exports.READ_COMMITTED_MODE_NO_RECORD_VERSION = exports.READ_COMMITTED_MODE_RECORD_VERSION = exports.READ_COMMITTED_MODE_READ_CONSISTENCY = exports.READ_COMMITTED_MODE_DEFAULT = exports.normalizeQuery = exports.normalizeCallableSql = exports.normalizeCallableQuery = exports.parseDsn = exports.oidToString = exports.decodeValue = exports.encodeParam = exports.ScratchbirdTypedValue = exports.ScratchbirdRaw = exports.ScratchbirdMoney = exports.ScratchbirdDecimal = exports.ScratchbirdTimestampTZ = exports.ScratchbirdTimestamp = exports.ScratchbirdTime = exports.ScratchbirdDate = exports.ScratchbirdInterval = void 0;
var client_1 = require("./client");
Object.defineProperty(exports, "Client", { enumerable: true, get: function () { return client_1.Client; } });
Object.defineProperty(exports, "Pool", { enumerable: true, get: function () { return client_1.Pool; } });
var types_1 = require("./types");
Object.defineProperty(exports, "FORMAT_TEXT", { enumerable: true, get: function () { return types_1.FORMAT_TEXT; } });
Object.defineProperty(exports, "FORMAT_BINARY", { enumerable: true, get: function () { return types_1.FORMAT_BINARY; } });
Object.defineProperty(exports, "OID_BOOL", { enumerable: true, get: function () { return types_1.OID_BOOL; } });
Object.defineProperty(exports, "OID_BYTEA", { enumerable: true, get: function () { return types_1.OID_BYTEA; } });
Object.defineProperty(exports, "OID_CHAR", { enumerable: true, get: function () { return types_1.OID_CHAR; } });
Object.defineProperty(exports, "OID_BPCHAR", { enumerable: true, get: function () { return types_1.OID_BPCHAR; } });
Object.defineProperty(exports, "OID_INT8", { enumerable: true, get: function () { return types_1.OID_INT8; } });
Object.defineProperty(exports, "OID_INT2", { enumerable: true, get: function () { return types_1.OID_INT2; } });
Object.defineProperty(exports, "OID_INT4", { enumerable: true, get: function () { return types_1.OID_INT4; } });
Object.defineProperty(exports, "OID_TEXT", { enumerable: true, get: function () { return types_1.OID_TEXT; } });
Object.defineProperty(exports, "OID_JSON", { enumerable: true, get: function () { return types_1.OID_JSON; } });
Object.defineProperty(exports, "OID_XML", { enumerable: true, get: function () { return types_1.OID_XML; } });
Object.defineProperty(exports, "OID_POINT", { enumerable: true, get: function () { return types_1.OID_POINT; } });
Object.defineProperty(exports, "OID_LSEG", { enumerable: true, get: function () { return types_1.OID_LSEG; } });
Object.defineProperty(exports, "OID_PATH", { enumerable: true, get: function () { return types_1.OID_PATH; } });
Object.defineProperty(exports, "OID_BOX", { enumerable: true, get: function () { return types_1.OID_BOX; } });
Object.defineProperty(exports, "OID_POLYGON", { enumerable: true, get: function () { return types_1.OID_POLYGON; } });
Object.defineProperty(exports, "OID_LINE", { enumerable: true, get: function () { return types_1.OID_LINE; } });
Object.defineProperty(exports, "OID_CIRCLE", { enumerable: true, get: function () { return types_1.OID_CIRCLE; } });
Object.defineProperty(exports, "OID_FLOAT4", { enumerable: true, get: function () { return types_1.OID_FLOAT4; } });
Object.defineProperty(exports, "OID_FLOAT8", { enumerable: true, get: function () { return types_1.OID_FLOAT8; } });
Object.defineProperty(exports, "OID_MONEY", { enumerable: true, get: function () { return types_1.OID_MONEY; } });
Object.defineProperty(exports, "OID_MACADDR", { enumerable: true, get: function () { return types_1.OID_MACADDR; } });
Object.defineProperty(exports, "OID_MACADDR8", { enumerable: true, get: function () { return types_1.OID_MACADDR8; } });
Object.defineProperty(exports, "OID_CIDR", { enumerable: true, get: function () { return types_1.OID_CIDR; } });
Object.defineProperty(exports, "OID_INET", { enumerable: true, get: function () { return types_1.OID_INET; } });
Object.defineProperty(exports, "OID_VARCHAR", { enumerable: true, get: function () { return types_1.OID_VARCHAR; } });
Object.defineProperty(exports, "OID_DATE", { enumerable: true, get: function () { return types_1.OID_DATE; } });
Object.defineProperty(exports, "OID_TIME", { enumerable: true, get: function () { return types_1.OID_TIME; } });
Object.defineProperty(exports, "OID_TIMESTAMP", { enumerable: true, get: function () { return types_1.OID_TIMESTAMP; } });
Object.defineProperty(exports, "OID_TIMESTAMPTZ", { enumerable: true, get: function () { return types_1.OID_TIMESTAMPTZ; } });
Object.defineProperty(exports, "OID_INTERVAL", { enumerable: true, get: function () { return types_1.OID_INTERVAL; } });
Object.defineProperty(exports, "OID_NUMERIC", { enumerable: true, get: function () { return types_1.OID_NUMERIC; } });
Object.defineProperty(exports, "OID_UUID", { enumerable: true, get: function () { return types_1.OID_UUID; } });
Object.defineProperty(exports, "OID_JSONB", { enumerable: true, get: function () { return types_1.OID_JSONB; } });
Object.defineProperty(exports, "OID_INT4RANGE", { enumerable: true, get: function () { return types_1.OID_INT4RANGE; } });
Object.defineProperty(exports, "OID_NUMRANGE", { enumerable: true, get: function () { return types_1.OID_NUMRANGE; } });
Object.defineProperty(exports, "OID_TSRANGE", { enumerable: true, get: function () { return types_1.OID_TSRANGE; } });
Object.defineProperty(exports, "OID_TSTZRANGE", { enumerable: true, get: function () { return types_1.OID_TSTZRANGE; } });
Object.defineProperty(exports, "OID_DATERANGE", { enumerable: true, get: function () { return types_1.OID_DATERANGE; } });
Object.defineProperty(exports, "OID_INT8RANGE", { enumerable: true, get: function () { return types_1.OID_INT8RANGE; } });
Object.defineProperty(exports, "OID_TSVECTOR", { enumerable: true, get: function () { return types_1.OID_TSVECTOR; } });
Object.defineProperty(exports, "OID_TSQUERY", { enumerable: true, get: function () { return types_1.OID_TSQUERY; } });
Object.defineProperty(exports, "OID_SB_VECTOR", { enumerable: true, get: function () { return types_1.OID_SB_VECTOR; } });
Object.defineProperty(exports, "ScratchbirdJsonb", { enumerable: true, get: function () { return types_1.ScratchbirdJsonb; } });
Object.defineProperty(exports, "ScratchbirdJson", { enumerable: true, get: function () { return types_1.ScratchbirdJson; } });
Object.defineProperty(exports, "ScratchbirdGeometry", { enumerable: true, get: function () { return types_1.ScratchbirdGeometry; } });
Object.defineProperty(exports, "ScratchbirdRange", { enumerable: true, get: function () { return types_1.ScratchbirdRange; } });
Object.defineProperty(exports, "ScratchbirdInterval", { enumerable: true, get: function () { return types_1.ScratchbirdInterval; } });
Object.defineProperty(exports, "ScratchbirdDate", { enumerable: true, get: function () { return types_1.ScratchbirdDate; } });
Object.defineProperty(exports, "ScratchbirdTime", { enumerable: true, get: function () { return types_1.ScratchbirdTime; } });
Object.defineProperty(exports, "ScratchbirdTimestamp", { enumerable: true, get: function () { return types_1.ScratchbirdTimestamp; } });
Object.defineProperty(exports, "ScratchbirdTimestampTZ", { enumerable: true, get: function () { return types_1.ScratchbirdTimestampTZ; } });
Object.defineProperty(exports, "ScratchbirdDecimal", { enumerable: true, get: function () { return types_1.ScratchbirdDecimal; } });
Object.defineProperty(exports, "ScratchbirdMoney", { enumerable: true, get: function () { return types_1.ScratchbirdMoney; } });
Object.defineProperty(exports, "ScratchbirdRaw", { enumerable: true, get: function () { return types_1.ScratchbirdRaw; } });
Object.defineProperty(exports, "ScratchbirdTypedValue", { enumerable: true, get: function () { return types_1.ScratchbirdTypedValue; } });
Object.defineProperty(exports, "encodeParam", { enumerable: true, get: function () { return types_1.encodeParam; } });
Object.defineProperty(exports, "decodeValue", { enumerable: true, get: function () { return types_1.decodeValue; } });
Object.defineProperty(exports, "oidToString", { enumerable: true, get: function () { return types_1.oidToString; } });
var dsn_1 = require("./dsn");
Object.defineProperty(exports, "parseDsn", { enumerable: true, get: function () { return dsn_1.parseDsn; } });
var sql_1 = require("./sql");
Object.defineProperty(exports, "normalizeCallableQuery", { enumerable: true, get: function () { return sql_1.normalizeCallableQuery; } });
Object.defineProperty(exports, "normalizeCallableSql", { enumerable: true, get: function () { return sql_1.normalizeCallableSql; } });
Object.defineProperty(exports, "normalizeQuery", { enumerable: true, get: function () { return sql_1.normalizeQuery; } });
var protocol_1 = require("./protocol");
Object.defineProperty(exports, "READ_COMMITTED_MODE_DEFAULT", { enumerable: true, get: function () { return protocol_1.READ_COMMITTED_MODE_DEFAULT; } });
Object.defineProperty(exports, "READ_COMMITTED_MODE_READ_CONSISTENCY", { enumerable: true, get: function () { return protocol_1.READ_COMMITTED_MODE_READ_CONSISTENCY; } });
Object.defineProperty(exports, "READ_COMMITTED_MODE_RECORD_VERSION", { enumerable: true, get: function () { return protocol_1.READ_COMMITTED_MODE_RECORD_VERSION; } });
Object.defineProperty(exports, "READ_COMMITTED_MODE_NO_RECORD_VERSION", { enumerable: true, get: function () { return protocol_1.READ_COMMITTED_MODE_NO_RECORD_VERSION; } });
Object.defineProperty(exports, "canonicalReadCommittedModeLabel", { enumerable: true, get: function () { return protocol_1.canonicalReadCommittedModeLabel; } });
__exportStar(require("./metadata"), exports);
__exportStar(require("./errors"), exports);
__exportStar(require("./readiness"), exports);
__exportStar(require("./circuit_breaker"), exports);
__exportStar(require("./keepalive"), exports);
__exportStar(require("./leak_detector"), exports);
__exportStar(require("./telemetry"), exports);
