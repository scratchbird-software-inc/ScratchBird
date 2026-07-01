// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import { Buffer } from "node:buffer";

export interface ClientConfig {
  host?: string;
  port?: number;
  frontDoorMode?: "direct" | "manager_proxy" | string;
  transportMode?: "inet_listener" | "local_ipc" | string;
  ipcMethod?: "unix" | string;
  ipcPath?: string;
  protocol?: string;
  parser?: string;
  dialect?: string;
  user?: string;
  password?: string;
  database?: string;
  schema?: string;
  metadataExpandSchemaParents?: boolean;
  ssl?: boolean | Record<string, any>;
  sslmode?: string;
  sslrootcert?: string;
  sslcert?: string;
  sslkey?: string;
  sslpassword?: string;
  connectTimeoutMs?: number;
  socketTimeoutMs?: number;
  applicationName?: string;
  role?: string;
  binaryTransfer?: boolean;
  compression?: "zstd" | "off";
  managerAuthToken?: string;
  managerUsername?: string;
  managerDatabase?: string;
  managerConnectionProfile?: string;
  managerClientIntent?: string;
  managerClientFlags?: number;
  managerAuthFastPath?: boolean;
  connectClientFlags?: number;
  authToken?: string;
  authMethodId?: string;
  authMethodPayload?: string;
  authPayloadJson?: string;
  authPayloadB64?: string;
  authProviderProfile?: string;
  authRequiredMethods?: string;
  authForbiddenMethods?: string;
  authRequireChannelBinding?: boolean;
  workloadIdentityToken?: string;
  proxyPrincipalAssertion?: string;
  dormantId?: string;
  dormantReattachToken?: string;
}

export type BootstrapIngressMode = "direct" | "manager_proxy";

export type BootstrapAuthMethod =
  | "PASSWORD"
  | "MD5"
  | "SCRAM_SHA_256"
  | "SCRAM_SHA_512"
  | "TOKEN"
  | "PEER"
  | "REATTACH";

export interface AuthMethodSurface {
  wireMethod: BootstrapAuthMethod;
  pluginMethodId: string | null;
  executableLocally: boolean;
  brokerRequired: boolean;
}

export interface AuthProbeResult {
  reachable: boolean;
  ingressMode: BootstrapIngressMode;
  resolvedHost: string;
  resolvedPort: number;
  admittedMethods: AuthMethodSurface[];
  requiredMethod: BootstrapAuthMethod | null;
  requiredPluginMethodId: string | null;
  allowedTransportMask: number | null;
  additionalContinuationPossible: boolean;
}

export interface ResolvedAuthContext {
  ingressMode: BootstrapIngressMode;
  resolvedAuthMethod: BootstrapAuthMethod | null;
  resolvedAuthPluginId: string | null;
  managerAuthenticated: boolean;
  attached: boolean;
}

export interface FieldDef {
  name: string;
  dataType: string;
  format: "text" | "binary";
  nullable: boolean;
  typeOid?: number;
  typeModifier?: number;
}

export interface QueryResult<T = any> {
  rows: T[];
  rowCount: number;
  fields: FieldDef[];
  command: string;
  lastId: bigint | null;
}

export interface BatchItemResult {
  index: number;
  rowCount: number;
  fields: FieldDef[];
  command: string;
  lastId: bigint | null;
}

export interface BatchResult {
  items: BatchItemResult[];
  totalRowCount: number;
}

export interface ParamValue {
  format: number;
  data?: Buffer;
  isNull?: boolean;
}

export const FORMAT_TEXT = 0;
export const FORMAT_BINARY = 1;

export const OID_BOOL = 16;
export const OID_BYTEA = 17;
export const OID_CHAR = 18;
export const OID_INT8 = 20;
export const OID_INT2 = 21;
export const OID_INT4 = 23;
export const OID_TEXT = 25;
export const OID_JSON = 114;
export const OID_XML = 142;
export const OID_POINT = 600;
export const OID_LSEG = 601;
export const OID_PATH = 602;
export const OID_BOX = 603;
export const OID_POLYGON = 604;
export const OID_LINE = 628;
export const OID_FLOAT4 = 700;
export const OID_FLOAT8 = 701;
export const OID_CIRCLE = 718;
export const OID_MONEY = 790;
export const OID_MACADDR = 829;
export const OID_CIDR = 650;
export const OID_INET = 869;
export const OID_MACADDR8 = 774;
export const OID_BPCHAR = 1042;
export const OID_VARCHAR = 1043;
export const OID_DATE = 1082;
export const OID_TIME = 1083;
export const OID_TIMESTAMP = 1114;
export const OID_TIMESTAMPTZ = 1184;
export const OID_INTERVAL = 1186;
export const OID_TIMETZ = 1266;
export const OID_NUMERIC = 1700;
export const OID_UUID = 2950;
export const OID_JSONB = 3802;
export const OID_RECORD = 2249;
export const OID_INT4RANGE = 3904;
export const OID_NUMRANGE = 3906;
export const OID_TSRANGE = 3908;
export const OID_TSTZRANGE = 3910;
export const OID_DATERANGE = 3912;
export const OID_INT8RANGE = 3926;
export const OID_TSVECTOR = 3614;
export const OID_TSQUERY = 3615;
export const OID_SB_VECTOR = 16386;

const RANGE_EMPTY = 0x01;
const RANGE_LB_INC = 0x02;
const RANGE_UB_INC = 0x04;
const RANGE_LB_INF = 0x08;
const RANGE_UB_INF = 0x10;

export class ScratchbirdJsonb {
  raw: Buffer;
  value?: any;
  constructor(raw: Buffer, value?: any) {
    this.raw = raw;
    this.value = value;
  }
}

export class ScratchbirdJson {
  raw: Buffer;
  value?: any;
  constructor(raw: Buffer, value?: any) {
    this.raw = raw;
    this.value = value;
  }
}

export class ScratchbirdGeometry {
  wkb: Buffer;
  srid?: number;
  wkt?: string;
  constructor(wkb: Buffer, opts?: { srid?: number; wkt?: string }) {
    this.wkb = wkb;
    this.srid = opts?.srid;
    this.wkt = opts?.wkt;
  }
}

export class ScratchbirdRange<T> {
  lower?: T;
  upper?: T;
  lowerInclusive = false;
  upperInclusive = false;
  lowerInfinite = false;
  upperInfinite = false;
  empty = false;
  rangeOid?: number;

  constructor(init?: Partial<ScratchbirdRange<T>>) {
    if (!init) return;
    Object.assign(this, init);
  }
}

export interface ScratchbirdCompositeField {
  oid: number;
  value?: any;
  raw?: Buffer | null;
}

export class ScratchbirdComposite {
  typeOid: number;
  fields: ScratchbirdCompositeField[];
  constructor(fields: ScratchbirdCompositeField[], typeOid: number = OID_RECORD) {
    this.fields = fields;
    this.typeOid = typeOid;
  }
}

export class ScratchbirdInterval {
  months: number;
  days: number;
  micros: number;
  constructor(micros: number, days = 0, months = 0) {
    this.micros = micros;
    this.days = days;
    this.months = months;
  }
}

export class ScratchbirdDate {
  value: Date;
  constructor(value: Date) {
    this.value = value;
  }
}

export class ScratchbirdTime {
  micros: number;
  constructor(micros: number) {
    this.micros = micros;
  }
}

export class ScratchbirdTimestamp {
  value: Date;
  constructor(value: Date) {
    this.value = value;
  }
}

export class ScratchbirdTimestampTZ {
  value: Date;
  constructor(value: Date) {
    this.value = value;
  }
}

export class ScratchbirdDecimal {
  value: string;
  constructor(value: string) {
    this.value = value;
  }
}

export class ScratchbirdMoney {
  cents: bigint;
  constructor(cents: bigint) {
    this.cents = cents;
  }
}

export class ScratchbirdRaw {
  oid: number;
  data: Buffer;
  constructor(oid: number, data: Buffer) {
    this.oid = oid;
    this.data = data;
  }
}

export class ScratchbirdTypedValue {
  oid: number;
  value: unknown;
  constructor(oid: number, value: unknown) {
    this.oid = oid;
    this.value = value;
  }
}

const uuidRegex = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

export function oidToString(oid: number): string {
  switch (oid) {
    case OID_BOOL:
      return "boolean";
    case OID_INT2:
      return "int2";
    case OID_INT4:
      return "int4";
    case OID_INT8:
      return "int8";
    case OID_FLOAT4:
      return "float4";
    case OID_FLOAT8:
      return "float8";
    case OID_NUMERIC:
      return "numeric";
    case OID_MONEY:
      return "money";
    case OID_TEXT:
      return "text";
    case OID_VARCHAR:
      return "varchar";
    case OID_CHAR:
    case OID_BPCHAR:
      return "char";
    case OID_BYTEA:
      return "bytea";
    case OID_DATE:
      return "date";
    case OID_TIME:
      return "time";
    case OID_TIMESTAMP:
      return "timestamp";
    case OID_TIMESTAMPTZ:
      return "timestamptz";
    case OID_INTERVAL:
      return "interval";
    case OID_UUID:
      return "uuid";
    case OID_JSON:
      return "json";
    case OID_JSONB:
      return "jsonb";
    case OID_XML:
      return "xml";
    case OID_INET:
      return "inet";
    case OID_CIDR:
      return "cidr";
    case OID_MACADDR:
      return "macaddr";
    case OID_MACADDR8:
      return "macaddr8";
    case OID_TSVECTOR:
      return "tsvector";
    case OID_TSQUERY:
      return "tsquery";
    case OID_INT4RANGE:
      return "int4range";
    case OID_INT8RANGE:
      return "int8range";
    case OID_NUMRANGE:
      return "numrange";
    case OID_TSRANGE:
      return "tsrange";
    case OID_TSTZRANGE:
      return "tstzrange";
    case OID_DATERANGE:
      return "daterange";
    case OID_SB_VECTOR:
      return "vector";
    default:
      return "unknown";
  }
}

export function encodeParam(value: any): { param: ParamValue; oid: number } {
  if (value === null || value === undefined) {
    return { param: { isNull: true, format: FORMAT_BINARY }, oid: 0 };
  }
  if (value instanceof ScratchbirdRaw) {
    return { param: { data: Buffer.from(value.data), format: FORMAT_BINARY }, oid: value.oid };
  }
  if (value instanceof ScratchbirdTypedValue) {
    return encodeTypedValue(value);
  }
  if (value instanceof ScratchbirdJsonb) {
    let raw = value.raw;
    if ((!raw || raw.length === 0) && value.value !== undefined) {
      raw = Buffer.from(JSON.stringify(value.value), "utf8");
    }
    if (!raw || raw.length === 0) {
      throw new Error("JSONB requires raw payload");
    }
    return { param: { data: encodeLengthPrefixed(raw), format: FORMAT_BINARY }, oid: OID_JSONB };
  }
  if (value instanceof ScratchbirdJson) {
    let raw = value.raw;
    if ((!raw || raw.length === 0) && value.value !== undefined) {
      raw = Buffer.from(JSON.stringify(value.value), "utf8");
    }
    if (!raw) {
      throw new Error("JSON requires raw payload");
    }
    return { param: { data: encodeLengthPrefixed(raw), format: FORMAT_BINARY }, oid: OID_JSON };
  }
  if (value instanceof ScratchbirdComposite) {
    const encoded = encodeComposite(value);
    return { param: { data: encoded.data, format: FORMAT_BINARY }, oid: encoded.oid };
  }
  if (value instanceof ScratchbirdGeometry) {
    if (!value.wkb || value.wkb.length === 0) {
      throw new Error("geometry requires WKB payload");
    }
    return { param: { data: encodeLengthPrefixed(value.wkb), format: FORMAT_BINARY }, oid: OID_POINT };
  }
  if (value instanceof ScratchbirdRange) {
    const encoded = encodeRange(value);
    return { param: { data: encoded.data, format: FORMAT_BINARY }, oid: encoded.oid };
  }
  if (value instanceof ScratchbirdDate) {
    return { param: { data: encodeDate(value.value), format: FORMAT_BINARY }, oid: OID_DATE };
  }
  if (value instanceof ScratchbirdTime) {
    return { param: { data: encodeTimeMicros(value.micros), format: FORMAT_BINARY }, oid: OID_TIME };
  }
  if (value instanceof ScratchbirdTimestamp) {
    return { param: { data: encodeTimestamp(value.value), format: FORMAT_BINARY }, oid: OID_TIMESTAMP };
  }
  if (value instanceof ScratchbirdTimestampTZ) {
    return { param: { data: encodeTimestamp(value.value), format: FORMAT_BINARY }, oid: OID_TIMESTAMPTZ };
  }
  if (value instanceof ScratchbirdInterval) {
    return { param: { data: encodeInterval(value), format: FORMAT_BINARY }, oid: OID_INTERVAL };
  }
  if (value instanceof ScratchbirdDecimal) {
    return { param: { data: encodeLengthPrefixed(Buffer.from(value.value, "utf8")), format: FORMAT_BINARY }, oid: OID_NUMERIC };
  }
  if (value instanceof ScratchbirdMoney) {
    return { param: { data: encodeInt64(value.cents), format: FORMAT_BINARY }, oid: OID_MONEY };
  }
  if (typeof value === "boolean") {
    return { param: { data: Buffer.from([value ? 1 : 0]), format: FORMAT_BINARY }, oid: OID_BOOL };
  }
  if (typeof value === "bigint") {
    return { param: { data: encodeInt64(value), format: FORMAT_BINARY }, oid: OID_INT8 };
  }
  if (typeof value === "number") {
    if (!Number.isFinite(value)) {
      throw new Error("numeric value must be finite");
    }
    if (Number.isInteger(value)) {
      if (value >= -2147483648 && value <= 2147483647) {
        return { param: { data: encodeInt32(value), format: FORMAT_BINARY }, oid: OID_INT4 };
      }
      if (Number.isSafeInteger(value)) {
        return { param: { data: encodeInt64(BigInt(value)), format: FORMAT_BINARY }, oid: OID_INT8 };
      }
      throw new Error("integer out of range for int64");
    }
    return { param: { data: encodeFloat64(value), format: FORMAT_BINARY }, oid: OID_FLOAT8 };
  }
  if (value instanceof Date) {
    return { param: { data: encodeTimestamp(value), format: FORMAT_BINARY }, oid: OID_TIMESTAMPTZ };
  }
  if (value instanceof Buffer) {
    return { param: { data: encodeLengthPrefixed(value), format: FORMAT_BINARY }, oid: OID_BYTEA };
  }
  if (value instanceof Uint8Array) {
    return { param: { data: encodeLengthPrefixed(Buffer.from(value)), format: FORMAT_BINARY }, oid: OID_BYTEA };
  }
  if (value instanceof Float32Array || value instanceof Float64Array) {
    return { param: { data: encodeLengthPrefixed(Buffer.from(formatVectorLiteral(Array.from(value)))), format: FORMAT_BINARY }, oid: OID_SB_VECTOR };
  }
  if (Array.isArray(value)) {
    if (value.length > 0 && value.every((item) => typeof item === "number")) {
      return { param: { data: encodeLengthPrefixed(Buffer.from(formatVectorLiteral(value))), format: FORMAT_BINARY }, oid: OID_SB_VECTOR };
    }
    return { param: { data: encodeLengthPrefixed(Buffer.from(formatArrayLiteral(value), "utf8")), format: FORMAT_BINARY }, oid: 0 };
  }
  if (typeof value === "string") {
    if (uuidRegex.test(value)) {
      return { param: { data: Buffer.from(value.replace(/-/g, ""), "hex"), format: FORMAT_BINARY }, oid: OID_UUID };
    }
    return { param: { data: encodeLengthPrefixed(Buffer.from(value, "utf8")), format: FORMAT_BINARY }, oid: OID_TEXT };
  }
  if (isIntervalObject(value)) {
    return { param: { data: encodeInterval(value), format: FORMAT_BINARY }, oid: OID_INTERVAL };
  }
  if (typeof value === "object") {
    return { param: { data: encodeLengthPrefixed(Buffer.from(JSON.stringify(value), "utf8")), format: FORMAT_BINARY }, oid: OID_JSON };
  }
  throw new Error("unsupported parameter type");
}

export function decodeValue(typeOid: number, data: Buffer | null, format: number): any {
  if (data === null) {
    return null;
  }
  if (typeOid === 0) {
    if (format === FORMAT_TEXT) {
      return parseUnknownText(decodeTextValue(data));
    }
    return decodeUnknownBinary(data);
  }
  if (format === FORMAT_TEXT) {
    try {
      return decodeTextTypedValue(typeOid, data);
    } catch {
      return decodeTextValue(data);
    }
  }
  return decodeBinaryValue(typeOid, data);
}

function decodeBinaryValue(typeOid: number, data: Buffer): any {
  const textFallback = maybeDecodeBinaryTextValue(typeOid, data);
  if (textFallback !== undefined) {
    return textFallback;
  }
  switch (typeOid) {
    case OID_BOOL:
      return data.length > 0 && data[0] === 1;
    case OID_INT2:
      return data.readInt16LE(0);
    case OID_INT4:
      return data.readInt32LE(0);
    case OID_INT8: {
      const value = data.readBigInt64LE(0);
      if (value >= BigInt(Number.MIN_SAFE_INTEGER) && value <= BigInt(Number.MAX_SAFE_INTEGER)) {
        return Number(value);
      }
      return value;
    }
    case OID_FLOAT4:
      return data.readFloatLE(0);
    case OID_FLOAT8:
      return data.readDoubleLE(0);
    case OID_NUMERIC:
      return stripLengthPrefix(data).toString("utf8");
    case OID_MONEY:
      return moneyToString(data.readBigInt64LE(0));
    case OID_TEXT:
    case OID_VARCHAR:
    case OID_CHAR:
    case OID_BPCHAR:
    case OID_JSON:
    case OID_XML:
    case OID_TSVECTOR:
    case OID_TSQUERY:
      return stripLengthPrefix(data).toString("utf8");
    case OID_JSONB:
      return new ScratchbirdJsonb(Buffer.from(stripLengthPrefix(data)));
    case OID_BYTEA:
      return Buffer.from(stripLengthPrefix(data));
    case OID_DATE:
      return decodeDate(data);
    case OID_TIME:
      return decodeTime(data);
    case OID_TIMESTAMP:
      return decodeTimestamp(data);
    case OID_TIMESTAMPTZ:
      return decodeTimestamp(data);
    case OID_INTERVAL:
      return decodeInterval(data);
    case OID_UUID:
      return bytesToUuid(data);
    case OID_INET:
    case OID_CIDR:
    case OID_MACADDR:
    case OID_MACADDR8:
      return stripLengthPrefix(data).toString("utf8");
    case OID_INT4RANGE:
    case OID_INT8RANGE:
    case OID_NUMRANGE:
    case OID_TSRANGE:
    case OID_TSTZRANGE:
    case OID_DATERANGE:
      return decodeRange(typeOid, data);
    case OID_SB_VECTOR:
      return parseVectorLiteral(stripLengthPrefix(data).toString("utf8"));
    case OID_POINT:
    case OID_LSEG:
    case OID_PATH:
    case OID_BOX:
    case OID_POLYGON:
    case OID_LINE:
    case OID_CIRCLE:
      return new ScratchbirdGeometry(Buffer.from(stripLengthPrefix(data)));
    case OID_RECORD:
      return decodeComposite(data);
    default:
      return Buffer.from(data);
  }
}

function decodeTextTypedValue(typeOid: number, data: Buffer): any {
  const text = decodeTextValue(data);
  const stripped = text.trim();
  switch (typeOid) {
    case OID_BOOL:
      if (!/^(t|true|1|f|false|0)$/i.test(stripped)) {
        throw new Error("invalid boolean text payload");
      }
      return /^(t|true|1)$/i.test(stripped);
    case OID_INT2:
    case OID_INT4: {
      if (!/^[+-]?\d+$/.test(stripped)) {
        throw new Error("invalid integer text payload");
      }
      const parsed = Number.parseInt(stripped, 10);
      if (Number.isNaN(parsed)) {
        throw new Error("invalid integer text payload");
      }
      return parsed;
    }
    case OID_INT8: {
      if (!/^[+-]?\d+$/.test(stripped)) {
        throw new Error("invalid bigint text payload");
      }
      try {
        const parsed = BigInt(stripped);
        if (parsed >= BigInt(Number.MIN_SAFE_INTEGER) && parsed <= BigInt(Number.MAX_SAFE_INTEGER)) {
          return Number(parsed);
        }
        return parsed;
      } catch {
        throw new Error("invalid bigint text payload");
      }
    }
    case OID_FLOAT4:
    case OID_FLOAT8: {
      if (!/^[+-]?(?:\d+\.?\d*|\d*\.?\d+)(?:[eE][+-]?\d+)?$/.test(stripped)) {
        throw new Error("invalid floating text payload");
      }
      const parsed = Number(stripped);
      if (Number.isNaN(parsed)) {
        throw new Error("invalid floating text payload");
      }
      return parsed;
    }
    case OID_NUMERIC:
    case OID_MONEY:
    case OID_TEXT:
    case OID_VARCHAR:
    case OID_CHAR:
    case OID_BPCHAR:
    case OID_JSON:
    case OID_XML:
    case OID_TSVECTOR:
    case OID_TSQUERY:
    case OID_INET:
    case OID_CIDR:
    case OID_MACADDR:
    case OID_MACADDR8:
      return text;
    case OID_JSONB:
      return new ScratchbirdJsonb(Buffer.from(text, "utf8"));
    case OID_BYTEA:
      return decodeTextByteaValue(stripped);
    case OID_DATE:
      return new Date(`${stripped}T00:00:00.000Z`);
    case OID_TIME:
      return decodeTimeText(stripped);
    case OID_TIMESTAMP:
      return decodeTimestampText(stripped, false);
    case OID_TIMESTAMPTZ:
      return decodeTimestampText(stripped, true);
    case OID_UUID:
      return stripped;
    case OID_SB_VECTOR:
      return parseVectorLiteral(stripped);
    default:
      return parseUnknownText(text);
  }
}

function maybeDecodeBinaryTextValue(typeOid: number, data: Buffer): any {
  switch (typeOid) {
    case OID_NUMERIC:
    case OID_TEXT:
    case OID_VARCHAR:
    case OID_CHAR:
    case OID_BPCHAR:
    case OID_JSON:
    case OID_XML:
    case OID_TSVECTOR:
    case OID_TSQUERY:
    case OID_JSONB:
    case OID_BYTEA:
    case OID_DATE:
    case OID_TIME:
    case OID_TIMESTAMP:
    case OID_TIMESTAMPTZ:
    case OID_UUID:
    case OID_SB_VECTOR:
    case OID_INET:
    case OID_CIDR:
    case OID_MACADDR:
    case OID_MACADDR8:
      break;
    default:
      return undefined;
  }
  const candidates: Buffer[] = [];
  const stripped = stripTrailingNulls(data);
  if (stripped.length > 0 && looksLikeText(stripped)) {
    candidates.push(stripped);
  }
  if (data.length >= 4) {
    const maybePrefixed = stripLengthPrefix(data);
    if (maybePrefixed.length > 0 && maybePrefixed.length !== data.length && looksLikeText(maybePrefixed)) {
      candidates.push(maybePrefixed);
    }
  }
  for (const candidate of candidates) {
    try {
      return decodeTextTypedValue(typeOid, candidate);
    } catch {
      // Fall through to the regular binary decoder on malformed text fallbacks.
    }
  }
  return undefined;
}

function encodeTypedValue(typed: ScratchbirdTypedValue): { param: ParamValue; oid: number } {
  const oid = typed.oid;
  const value = typed.value;
  if (value === null || value === undefined) {
    return { param: { isNull: true, format: FORMAT_BINARY }, oid };
  }
  switch (oid) {
    case OID_BOOL:
      if (typeof value !== "boolean") {
        throw new Error("typed boolean requires boolean value");
      }
      return { param: { data: Buffer.from([value ? 1 : 0]), format: FORMAT_BINARY }, oid };
    case OID_INT2: {
      if (typeof value !== "number" || !Number.isInteger(value)) {
        throw new Error("typed int2 requires integer value");
      }
      if (value < -32768 || value > 32767) {
        throw new Error("typed int2 out of range");
      }
      return { param: { data: encodeInt16(value), format: FORMAT_BINARY }, oid };
    }
    case OID_INT4: {
      if (typeof value !== "number" || !Number.isInteger(value)) {
        throw new Error("typed int4 requires integer value");
      }
      if (value < -2147483648 || value > 2147483647) {
        throw new Error("typed int4 out of range");
      }
      return { param: { data: encodeInt32(value), format: FORMAT_BINARY }, oid };
    }
    case OID_INT8:
      return { param: { data: encodeInt64(toBigInt(value, "int8")), format: FORMAT_BINARY }, oid };
    case OID_FLOAT4:
      return { param: { data: encodeFloat32(toFiniteNumber(value, "float4")), format: FORMAT_BINARY }, oid };
    case OID_FLOAT8:
      return { param: { data: encodeFloat64(toFiniteNumber(value, "float8")), format: FORMAT_BINARY }, oid };
    case OID_NUMERIC:
      return { param: { data: encodeLengthPrefixed(Buffer.from(String(value), "utf8")), format: FORMAT_BINARY }, oid };
    case OID_MONEY:
      return { param: { data: encodeInt64(toBigInt(value, "money")), format: FORMAT_BINARY }, oid };
    case OID_UUID:
      return { param: { data: encodeUuid(value), format: FORMAT_BINARY }, oid };
    case OID_JSON:
      return { param: { data: encodeLengthPrefixed(encodeJsonPayload(value)), format: FORMAT_BINARY }, oid };
    case OID_JSONB:
      return { param: { data: encodeLengthPrefixed(encodeJsonPayload(value)), format: FORMAT_BINARY }, oid };
    case OID_BYTEA:
      return { param: { data: encodeLengthPrefixed(toBuffer(value, "bytea")), format: FORMAT_BINARY }, oid };
    case OID_DATE:
      return { param: { data: encodeDate(toDate(value, "date")), format: FORMAT_BINARY }, oid };
    case OID_TIME: {
      const micros =
        value instanceof ScratchbirdTime
          ? value.micros
          : typeof value === "number"
            ? value
            : Number.NaN;
      if (!Number.isFinite(micros)) {
        throw new Error("typed time requires microsecond number or ScratchbirdTime");
      }
      return { param: { data: encodeTimeMicros(micros), format: FORMAT_BINARY }, oid };
    }
    case OID_TIMESTAMP:
    case OID_TIMESTAMPTZ:
      return { param: { data: encodeTimestamp(toDate(value, oid === OID_TIMESTAMP ? "timestamp" : "timestamptz")), format: FORMAT_BINARY }, oid };
    case OID_INTERVAL:
      if (value instanceof ScratchbirdInterval || isIntervalObject(value)) {
        return { param: { data: encodeInterval(value), format: FORMAT_BINARY }, oid };
      }
      throw new Error("typed interval requires ScratchbirdInterval or interval object");
    case OID_SB_VECTOR:
      return { param: { data: encodeLengthPrefixed(Buffer.from(formatVectorLiteral(toNumericArray(value)), "utf8")), format: FORMAT_BINARY }, oid };
    case OID_INT4RANGE:
    case OID_INT8RANGE:
    case OID_NUMRANGE:
    case OID_TSRANGE:
    case OID_TSTZRANGE:
    case OID_DATERANGE: {
      if (!(value instanceof ScratchbirdRange)) {
        throw new Error("typed range requires ScratchbirdRange value");
      }
      const encoded = encodeRange(new ScratchbirdRange({ ...value, rangeOid: oid }));
      return { param: { data: encoded.data, format: FORMAT_BINARY }, oid };
    }
    case OID_RECORD: {
      if (!(value instanceof ScratchbirdComposite)) {
        throw new Error("typed record requires ScratchbirdComposite value");
      }
      const encoded = encodeComposite(new ScratchbirdComposite(value.fields, OID_RECORD));
      return { param: { data: encoded.data, format: FORMAT_BINARY }, oid: OID_RECORD };
    }
    case OID_POINT:
    case OID_LSEG:
    case OID_PATH:
    case OID_BOX:
    case OID_POLYGON:
    case OID_LINE:
    case OID_CIRCLE: {
      if (!(value instanceof ScratchbirdGeometry)) {
        throw new Error("typed geometry requires ScratchbirdGeometry value");
      }
      if (!value.wkb || value.wkb.length === 0) {
        throw new Error("typed geometry requires WKB payload");
      }
      return { param: { data: encodeLengthPrefixed(value.wkb), format: FORMAT_BINARY }, oid };
    }
    case OID_TEXT:
    case OID_CHAR:
    case OID_BPCHAR:
    case OID_VARCHAR:
    case OID_XML:
    case OID_INET:
    case OID_CIDR:
    case OID_MACADDR:
    case OID_MACADDR8:
    case OID_TSVECTOR:
    case OID_TSQUERY:
      if (typeof value !== "string") {
        throw new Error(`typed ${oidToString(oid)} requires string value`);
      }
      return { param: { data: encodeLengthPrefixed(Buffer.from(value, "utf8")), format: FORMAT_BINARY }, oid };
    default:
      throw new Error(`typed OID ${oid} is not supported for parameter encoding`);
  }
}

function encodeComposite(value: ScratchbirdComposite): { data: Buffer; oid: number } {
  const chunks: Buffer[] = [];
  const fields = value.fields ?? [];
  const header = Buffer.alloc(4);
  header.writeInt32LE(fields.length, 0);
  chunks.push(header);

  for (const field of fields) {
    let oid = field.oid ?? 0;
    let data: Buffer | null = null;
    if (field.raw !== undefined) {
      data = field.raw;
    } else if (field.value !== undefined) {
      const encoded = encodeParam(field.value);
      if (!oid) {
        oid = encoded.oid;
      }
      if (encoded.param.isNull) {
        data = null;
      } else {
        data = encoded.param.data ?? Buffer.alloc(0);
      }
    }

    if (!oid) {
      throw new Error("composite field OID is required");
    }
    const oidBuf = Buffer.alloc(4);
    oidBuf.writeUInt32LE(oid, 0);
    const lenBuf = Buffer.alloc(4);
    if (data === null) {
      lenBuf.writeInt32LE(-1, 0);
      chunks.push(oidBuf, lenBuf);
      continue;
    }
    lenBuf.writeInt32LE(data.length, 0);
    chunks.push(oidBuf, lenBuf, data);
  }

  const typeOid = value.typeOid || OID_RECORD;
  return { data: Buffer.concat(chunks), oid: typeOid };
}

function decodeComposite(data: Buffer): ScratchbirdComposite {
  if (data.length < 4) {
    return new ScratchbirdComposite([]);
  }
  const count = data.readInt32LE(0);
  let offset = 4;
  const fields: ScratchbirdCompositeField[] = [];
  for (let i = 0; i < count; i++) {
    if (offset + 8 > data.length) break;
    const oid = data.readUInt32LE(offset);
    offset += 4;
    const length = data.readInt32LE(offset);
    offset += 4;
    if (length < 0) {
      fields.push({ oid, value: null, raw: null });
      continue;
    }
    if (offset + length > data.length) {
      break;
    }
    const raw = data.subarray(offset, offset + length);
    offset += length;
    const value = decodeBinaryValue(oid, raw);
    fields.push({ oid, value, raw: Buffer.from(raw) });
  }
  return new ScratchbirdComposite(fields);
}

function decodeTextValue(data: Buffer): string {
  if (data.length >= 4) {
    const length = data.readUInt32LE(0);
    if (length <= data.length - 4) {
      return data.subarray(4, 4 + length).toString("utf8");
    }
  }
  return data.toString("utf8");
}

function decodeUnknownBinary(data: Buffer): any {
  if (data.length >= 4) {
    const length = data.readUInt32LE(0);
    if (length === data.length - 4) {
      const text = data.subarray(4).toString("utf8");
      if (looksLikeArrayLiteral(text)) {
        return parseArrayLiteral(text);
      }
      return parseUnknownText(text);
    }
  }
  const trimmed = stripTrailingNulls(data);
  if (trimmed.length > 0 && looksLikeText(trimmed)) {
    const text = trimmed.toString("utf8");
    if (looksLikeArrayLiteral(text)) {
      return parseArrayLiteral(text);
    }
    return parseUnknownText(text);
  }
  switch (data.length) {
    case 1:
      return data[0];
    case 2:
      return data.readInt16LE(0);
    case 4:
      return data.readInt32LE(0);
    case 8:
      return data.readBigInt64LE(0);
    case 16:
      return bytesToUuid(data);
    default:
      return Buffer.from(data);
  }
}

function parseUnknownText(text: string): any {
  const trimmed = text.trim();
  if (trimmed === "") {
    return text;
  }
  if (looksLikeArrayLiteral(trimmed)) {
    return parseArrayLiteral(trimmed);
  }
  if (/^(true|false)$/i.test(trimmed)) {
    return trimmed.toLowerCase() === "true";
  }
  if (/^[+-]?\d+$/.test(trimmed)) {
    const asNum = Number(trimmed);
    if (Number.isSafeInteger(asNum) && String(asNum) === trimmed) {
      return asNum;
    }
    try {
      return BigInt(trimmed);
    } catch {
      return trimmed;
    }
  }
  if (/^[+-]?(?:\d+\.?\d*|\d*\.?\d+)(?:[eE][+-]?\d+)?$/.test(trimmed)) {
    const asNum = Number(trimmed);
    if (!Number.isNaN(asNum)) {
      return asNum;
    }
  }
  return text;
}

function stripTrailingNulls(data: Buffer): Buffer {
  let end = data.length;
  while (end > 0 && data[end - 1] === 0) {
    end -= 1;
  }
  return data.subarray(0, end);
}

function decodeTextByteaValue(text: string): Buffer {
  if (/^(\\x|0x)/i.test(text)) {
    const hex = text.slice(2);
    return /^[0-9a-f]*$/i.test(hex) ? Buffer.from(hex, "hex") : Buffer.from(text, "utf8");
  }
  if (/^[0-9a-f]+$/i.test(text) && text.length % 2 === 0) {
    return Buffer.from(text, "hex");
  }
  return Buffer.from(text, "utf8");
}

function decodeTimeText(text: string): Date {
  return new Date(`2000-01-01T${normalizeTemporalText(text)}`);
}

function decodeTimestampText(text: string, forceUtc: boolean): Date {
  const normalized = normalizeTemporalText(text);
  if (forceUtc) {
    return new Date(normalized.includes("+") || /z$/i.test(normalized) ? normalized : `${normalized}Z`);
  }
  return new Date(normalized.replace(" ", "T"));
}

function normalizeTemporalText(text: string): string {
  let normalized = text.trim();
  if (normalized.includes(" ") && !normalized.includes("T")) {
    normalized = normalized.replace(" ", "T");
  }
  if (/z$/i.test(normalized)) {
    normalized = `${normalized.slice(0, -1)}+00:00`;
  }
  if (/[+-]\d{2}$/.test(normalized)) {
    normalized = `${normalized}:00`;
  }
  return normalized;
}

function looksLikeText(data: Buffer): boolean {
  for (const byte of data) {
    if (byte === 0x09 || byte === 0x0a || byte === 0x0d) {
      continue;
    }
    if (byte < 0x20 || byte > 0x7e) {
      return false;
    }
  }
  return true;
}

function looksLikeArrayLiteral(text: string): boolean {
  return text.startsWith("{") && text.endsWith("}");
}

function encodeLengthPrefixed(data: Buffer): Buffer {
  const out = Buffer.alloc(4 + data.length);
  out.writeUInt32LE(data.length, 0);
  data.copy(out, 4);
  return out;
}

function stripLengthPrefix(data: Buffer): Buffer {
  if (data.length < 4) {
    return data;
  }
  const length = data.readUInt32LE(0);
  if (length <= data.length - 4) {
    return data.subarray(4, 4 + length);
  }
  return data;
}

function toFiniteNumber(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new Error(`typed ${label} requires finite numeric value`);
  }
  return value;
}

function toBigInt(value: unknown, label: string): bigint {
  if (typeof value === "bigint") {
    return value;
  }
  if (typeof value === "number") {
    if (!Number.isSafeInteger(value)) {
      throw new Error(`typed ${label} requires safe integer value`);
    }
    return BigInt(value);
  }
  if (typeof value === "string" && /^[+-]?\d+$/.test(value.trim())) {
    return BigInt(value.trim());
  }
  throw new Error(`typed ${label} requires integer-compatible value`);
}

function toDate(value: unknown, label: string): Date {
  if (value instanceof Date) {
    return value;
  }
  if (value instanceof ScratchbirdDate) {
    return value.value;
  }
  if (value instanceof ScratchbirdTimestamp) {
    return value.value;
  }
  if (value instanceof ScratchbirdTimestampTZ) {
    return value.value;
  }
  throw new Error(`typed ${label} requires Date-compatible value`);
}

function toBuffer(value: unknown, label: string): Buffer {
  if (Buffer.isBuffer(value)) {
    return value;
  }
  if (value instanceof Uint8Array) {
    return Buffer.from(value);
  }
  throw new Error(`typed ${label} requires Buffer or Uint8Array value`);
}

function toNumericArray(value: unknown): number[] {
  if (value instanceof Float32Array || value instanceof Float64Array) {
    return Array.from(value);
  }
  if (Array.isArray(value) && value.every((item) => typeof item === "number" && Number.isFinite(item))) {
    return value;
  }
  throw new Error("typed vector requires numeric array value");
}

function encodeUuid(value: unknown): Buffer {
  if (Buffer.isBuffer(value)) {
    if (value.length !== 16) {
      throw new Error("typed uuid Buffer must be 16 bytes");
    }
    return Buffer.from(value);
  }
  if (typeof value === "string" && uuidRegex.test(value)) {
    return Buffer.from(value.replace(/-/g, ""), "hex");
  }
  throw new Error("typed uuid requires UUID string or 16-byte Buffer");
}

function encodeJsonPayload(value: unknown): Buffer {
  if (Buffer.isBuffer(value)) {
    return value;
  }
  if (typeof value === "string") {
    return Buffer.from(value, "utf8");
  }
  return Buffer.from(JSON.stringify(value), "utf8");
}

function encodeInt16(value: number): Buffer {
  const out = Buffer.alloc(2);
  out.writeInt16LE(value, 0);
  return out;
}

function encodeInt32(value: number): Buffer {
  const out = Buffer.alloc(4);
  out.writeInt32LE(value, 0);
  return out;
}

function encodeInt64(value: bigint): Buffer {
  const out = Buffer.alloc(8);
  out.writeBigInt64LE(value, 0);
  return out;
}

function encodeFloat64(value: number): Buffer {
  const out = Buffer.alloc(8);
  out.writeDoubleLE(value, 0);
  return out;
}

function encodeFloat32(value: number): Buffer {
  const out = Buffer.alloc(4);
  out.writeFloatLE(value, 0);
  return out;
}

function encodeDate(value: Date): Buffer {
  const base = Date.UTC(2000, 0, 1);
  const days = Math.trunc((value.getTime() - base) / 86400000);
  const out = Buffer.alloc(4);
  out.writeInt32LE(days, 0);
  return out;
}

function encodeTimeMicros(micros: number): Buffer {
  const out = Buffer.alloc(8);
  out.writeBigInt64LE(BigInt(micros), 0);
  return out;
}

function encodeTimestamp(value: Date): Buffer {
  const base = Date.UTC(2000, 0, 1);
  const micros = BigInt(value.getTime() - base) * 1000n;
  const out = Buffer.alloc(8);
  out.writeBigInt64LE(micros, 0);
  return out;
}

function encodeInterval(interval: { micros: number; days?: number; months?: number }): Buffer {
  const out = Buffer.alloc(16);
  out.writeBigInt64LE(BigInt(interval.micros), 0);
  out.writeInt32LE(interval.days ?? 0, 8);
  out.writeInt32LE(interval.months ?? 0, 12);
  return out;
}

function decodeDate(data: Buffer): Date {
  if (data.length < 4) {
    return new Date(0);
  }
  const days = data.readInt32LE(0);
  const base = Date.UTC(2000, 0, 1);
  const millis = base + days * 86400000;
  return new Date(millis);
}

function decodeTime(data: Buffer): Date {
  if (data.length < 8) {
    return new Date(Date.UTC(2000, 0, 1));
  }
  const micros = data.readBigInt64LE(0);
  const base = Date.UTC(2000, 0, 1);
  const millis = Number(micros / 1000n);
  return new Date(base + millis);
}

function decodeTimestamp(data: Buffer): Date {
  if (data.length < 8) {
    return new Date(0);
  }
  const micros = data.readBigInt64LE(0);
  const base = Date.UTC(2000, 0, 1);
  const millis = Number(micros / 1000n);
  return new Date(base + millis);
}

function decodeInterval(data: Buffer): { months: number; days: number; micros: number } {
  if (data.length < 16) {
    return { months: 0, days: 0, micros: 0 };
  }
  const micros = Number(data.readBigInt64LE(0));
  const days = data.readInt32LE(8);
  const months = data.readInt32LE(12);
  return { months, days, micros };
}

function moneyToString(cents: bigint): string {
  const negative = cents < 0n;
  const abs = negative ? -cents : cents;
  const units = abs / 100n;
  const fraction = abs % 100n;
  const value = `${units.toString()}.${fraction.toString().padStart(2, "0")}`;
  return negative ? `-${value}` : value;
}

function bytesToUuid(buf: Buffer): string {
  const hex = buf.toString("hex");
  if (hex.length !== 32) {
    return hex;
  }
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

function encodeRange(range: ScratchbirdRange<any>): { data: Buffer; oid: number } {
  const oid = resolveRangeOid(range);
  const flags =
    (range.empty ? RANGE_EMPTY : 0) |
    (range.lowerInclusive ? RANGE_LB_INC : 0) |
    (range.upperInclusive ? RANGE_UB_INC : 0) |
    (range.lowerInfinite ? RANGE_LB_INF : 0) |
    (range.upperInfinite ? RANGE_UB_INF : 0);

  const parts: Buffer[] = [Buffer.from([flags, 0, 0, 0])];
  if (!range.empty && !range.lowerInfinite) {
    const bound = encodeRangeBound(oid, range.lower);
    parts.push(encodeInt32(bound.length));
    parts.push(bound);
  }
  if (!range.empty && !range.upperInfinite) {
    const bound = encodeRangeBound(oid, range.upper);
    parts.push(encodeInt32(bound.length));
    parts.push(bound);
  }
  return { data: Buffer.concat(parts), oid };
}

function resolveRangeOid(range: ScratchbirdRange<any>): number {
  if (range.rangeOid) {
    return range.rangeOid;
  }
  const sample = range.lower ?? range.upper;
  if (sample === undefined || sample === null) {
    throw new Error("range type cannot be inferred from empty bounds");
  }
  if (sample instanceof ScratchbirdDate) {
    return OID_DATERANGE;
  }
  if (sample instanceof ScratchbirdTimestamp) {
    return OID_TSRANGE;
  }
  if (sample instanceof ScratchbirdTimestampTZ || sample instanceof Date) {
    return OID_TSTZRANGE;
  }
  if (sample instanceof ScratchbirdDecimal) {
    return OID_NUMRANGE;
  }
  if (typeof sample === "bigint") {
    return OID_INT8RANGE;
  }
  if (typeof sample === "number") {
    return sample >= -2147483648 && sample <= 2147483647 ? OID_INT4RANGE : OID_INT8RANGE;
  }
  throw new Error("unsupported range bound type");
}

function encodeRangeBound(rangeOid: number, value: any): Buffer {
  switch (rangeOid) {
    case OID_INT4RANGE: {
      if (typeof value !== "number") throw new Error("int4range requires number bounds");
      return encodeInt32(value);
    }
    case OID_INT8RANGE: {
      if (typeof value === "number") {
        if (!Number.isSafeInteger(value)) throw new Error("int8range requires safe integer bounds");
        return encodeInt64(BigInt(value));
      }
      if (typeof value === "bigint") {
        return encodeInt64(value);
      }
      throw new Error("int8range requires bigint bounds");
    }
    case OID_NUMRANGE: {
      if (value instanceof ScratchbirdDecimal) {
        return encodeLengthPrefixed(Buffer.from(value.value, "utf8"));
      }
      if (typeof value === "string") {
        return encodeLengthPrefixed(Buffer.from(value, "utf8"));
      }
      throw new Error("numrange requires decimal bounds");
    }
    case OID_DATERANGE: {
      if (value instanceof ScratchbirdDate) {
        return encodeDate(value.value);
      }
      if (value instanceof Date) {
        return encodeDate(value);
      }
      throw new Error("daterange requires date bounds");
    }
    case OID_TSRANGE: {
      if (value instanceof ScratchbirdTimestamp) {
        return encodeTimestamp(value.value);
      }
      if (value instanceof Date) {
        return encodeTimestamp(value);
      }
      throw new Error("tsrange requires timestamp bounds");
    }
    case OID_TSTZRANGE: {
      if (value instanceof ScratchbirdTimestampTZ) {
        return encodeTimestamp(value.value);
      }
      if (value instanceof Date) {
        return encodeTimestamp(value);
      }
      throw new Error("tstzrange requires timestamptz bounds");
    }
    default:
      throw new Error("unsupported range type");
  }
}

function decodeRange(rangeOid: number, data: Buffer): ScratchbirdRange<any> {
  if (data.length < 4) {
    return new ScratchbirdRange();
  }
  const flags = data[0];
  let offset = 4;
  const range = new ScratchbirdRange<any>({
    empty: (flags & RANGE_EMPTY) !== 0,
    lowerInclusive: (flags & RANGE_LB_INC) !== 0,
    upperInclusive: (flags & RANGE_UB_INC) !== 0,
    lowerInfinite: (flags & RANGE_LB_INF) !== 0,
    upperInfinite: (flags & RANGE_UB_INF) !== 0,
    rangeOid,
  });
  if (range.empty) {
    return range;
  }
  if (!range.lowerInfinite) {
    if (offset + 4 > data.length) {
      return range;
    }
    const length = data.readInt32LE(offset);
    offset += 4;
    if (offset + length > data.length) {
      return range;
    }
    const bound = data.subarray(offset, offset + length);
    offset += length;
    range.lower = decodeRangeBound(rangeOid, bound);
  }
  if (!range.upperInfinite) {
    if (offset + 4 > data.length) {
      return range;
    }
    const length = data.readInt32LE(offset);
    offset += 4;
    if (offset + length > data.length) {
      return range;
    }
    const bound = data.subarray(offset, offset + length);
    range.upper = decodeRangeBound(rangeOid, bound);
  }
  return range;
}

function decodeRangeBound(rangeOid: number, data: Buffer): any {
  switch (rangeOid) {
    case OID_INT4RANGE:
      return data.length >= 4 ? data.readInt32LE(0) : 0;
    case OID_INT8RANGE:
      return data.length >= 8 ? data.readBigInt64LE(0) : 0n;
    case OID_NUMRANGE:
      return stripLengthPrefix(data).toString("utf8");
    case OID_DATERANGE:
      return decodeDate(data);
    case OID_TSRANGE:
    case OID_TSTZRANGE:
      return decodeTimestamp(data);
    default:
      return null;
  }
}

function isIntervalObject(value: any): value is { micros: number; days?: number; months?: number } {
  return value && typeof value === "object" && typeof value.micros === "number";
}

function formatArrayLiteral(values: any[]): string {
  const items = values.map((value) => formatArrayItem(value));
  return `{${items.join(",")}}`;
}

function formatArrayItem(value: any): string {
  if (value === null || value === undefined) {
    return "NULL";
  }
  if (Array.isArray(value)) {
    return formatArrayLiteral(value);
  }
  if (typeof value === "string") {
    return `"${value.replace(/"/g, "\\\"")}"`;
  }
  if (typeof value === "boolean") {
    return value ? "true" : "false";
  }
  return String(value);
}

function parseArrayLiteral(text: string): any[] {
  let trimmed = text.trim();
  if (trimmed === "{}" || trimmed === "") {
    return [];
  }
  if (trimmed.startsWith("{") && trimmed.endsWith("}")) {
    trimmed = trimmed.slice(1, -1);
  }
  return splitArrayItems(trimmed);
}

function splitArrayItems(text: string): any[] {
  const items: any[] = [];
  let depth = 0;
  let buf = "";
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ch === "{") {
      depth++;
      buf += ch;
    } else if (ch === "}") {
      depth = Math.max(0, depth - 1);
      buf += ch;
    } else if (ch === "," && depth === 0) {
      items.push(parseArrayItem(buf));
      buf = "";
    } else {
      buf += ch;
    }
  }
  if (buf.length || text.length) {
    items.push(parseArrayItem(buf));
  }
  return items;
}

function parseArrayItem(raw: string): any {
  const token = raw.trim();
  if (token === "") {
    return "";
  }
  if (token.toUpperCase() === "NULL") {
    return null;
  }
  if (token.startsWith("{") && token.endsWith("}")) {
    return parseArrayLiteral(token);
  }
  if (token.startsWith("[") && token.endsWith("]")) {
    return parseVectorLiteral(token);
  }
  if (token === "true" || token === "false") {
    return token === "true";
  }
  const num = Number(token);
  if (!Number.isNaN(num)) {
    return num;
  }
  return token;
}

function parseVectorLiteral(text: string): number[] {
  let trimmed = text.trim();
  if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
    trimmed = trimmed.slice(1, -1);
  }
  if (!trimmed) {
    return [];
  }
  return trimmed
    .split(",")
    .map((part) => Number(part.trim()))
    .filter((val) => !Number.isNaN(val));
}

function formatVectorLiteral(values: number[]): string {
  const parts = values.map((value) => Number.isFinite(value) ? String(value) : "0");
  return `[${parts.join(",")}]`;
}

export function decodeArrayLiteral(text: string): any[] {
  return parseArrayLiteral(text);
}
