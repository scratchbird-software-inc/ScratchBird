// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

export { Client, Pool } from "./client";
export type {
  AuthMethodSurface,
  AuthProbeResult,
  BatchItemResult,
  BatchResult,
  BootstrapAuthMethod,
  BootstrapIngressMode,
  ClientConfig,
  FieldDef,
  ParamValue,
  QueryResult,
  ResolvedAuthContext,
} from "./types";
export {
  FORMAT_TEXT,
  FORMAT_BINARY,
  OID_BOOL,
  OID_BYTEA,
  OID_CHAR,
  OID_BPCHAR,
  OID_INT8,
  OID_INT2,
  OID_INT4,
  OID_TEXT,
  OID_JSON,
  OID_XML,
  OID_POINT,
  OID_LSEG,
  OID_PATH,
  OID_BOX,
  OID_POLYGON,
  OID_LINE,
  OID_CIRCLE,
  OID_FLOAT4,
  OID_FLOAT8,
  OID_MONEY,
  OID_MACADDR,
  OID_MACADDR8,
  OID_CIDR,
  OID_INET,
  OID_VARCHAR,
  OID_DATE,
  OID_TIME,
  OID_TIMESTAMP,
  OID_TIMESTAMPTZ,
  OID_INTERVAL,
  OID_NUMERIC,
  OID_UUID,
  OID_JSONB,
  OID_INT4RANGE,
  OID_NUMRANGE,
  OID_TSRANGE,
  OID_TSTZRANGE,
  OID_DATERANGE,
  OID_INT8RANGE,
  OID_TSVECTOR,
  OID_TSQUERY,
  OID_SB_VECTOR,
  ScratchbirdJsonb,
  ScratchbirdJson,
  ScratchbirdGeometry,
  ScratchbirdRange,
  ScratchbirdInterval,
  ScratchbirdDate,
  ScratchbirdTime,
  ScratchbirdTimestamp,
  ScratchbirdTimestampTZ,
  ScratchbirdDecimal,
  ScratchbirdMoney,
  ScratchbirdRaw,
  ScratchbirdTypedValue,
  encodeParam,
  decodeValue,
  oidToString,
} from "./types";
export { parseDsn } from "./dsn";
export { normalizeCallableQuery, normalizeCallableSql, normalizeQuery } from "./sql";
export {
  READ_COMMITTED_MODE_DEFAULT,
  READ_COMMITTED_MODE_READ_CONSISTENCY,
  READ_COMMITTED_MODE_RECORD_VERSION,
  READ_COMMITTED_MODE_NO_RECORD_VERSION,
  canonicalReadCommittedModeLabel,
} from "./protocol";
export * from "./metadata";
export * from "./errors";
export * from "./circuit_breaker";
export * from "./keepalive";
export * from "./leak_detector";
export * from "./telemetry";
