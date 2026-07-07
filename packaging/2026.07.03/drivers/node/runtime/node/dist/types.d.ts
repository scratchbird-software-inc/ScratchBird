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
export type BootstrapAuthMethod = "PASSWORD" | "MD5" | "SCRAM_SHA_256" | "SCRAM_SHA_512" | "TOKEN" | "PEER" | "REATTACH";
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
export declare const FORMAT_TEXT = 0;
export declare const FORMAT_BINARY = 1;
export declare const OID_BOOL = 16;
export declare const OID_BYTEA = 17;
export declare const OID_CHAR = 18;
export declare const OID_INT8 = 20;
export declare const OID_INT2 = 21;
export declare const OID_INT4 = 23;
export declare const OID_TEXT = 25;
export declare const OID_JSON = 114;
export declare const OID_XML = 142;
export declare const OID_POINT = 600;
export declare const OID_LSEG = 601;
export declare const OID_PATH = 602;
export declare const OID_BOX = 603;
export declare const OID_POLYGON = 604;
export declare const OID_LINE = 628;
export declare const OID_FLOAT4 = 700;
export declare const OID_FLOAT8 = 701;
export declare const OID_CIRCLE = 718;
export declare const OID_MONEY = 790;
export declare const OID_MACADDR = 829;
export declare const OID_CIDR = 650;
export declare const OID_INET = 869;
export declare const OID_MACADDR8 = 774;
export declare const OID_BPCHAR = 1042;
export declare const OID_VARCHAR = 1043;
export declare const OID_DATE = 1082;
export declare const OID_TIME = 1083;
export declare const OID_TIMESTAMP = 1114;
export declare const OID_TIMESTAMPTZ = 1184;
export declare const OID_INTERVAL = 1186;
export declare const OID_TIMETZ = 1266;
export declare const OID_NUMERIC = 1700;
export declare const OID_UUID = 2950;
export declare const OID_JSONB = 3802;
export declare const OID_RECORD = 2249;
export declare const OID_INT4RANGE = 3904;
export declare const OID_NUMRANGE = 3906;
export declare const OID_TSRANGE = 3908;
export declare const OID_TSTZRANGE = 3910;
export declare const OID_DATERANGE = 3912;
export declare const OID_INT8RANGE = 3926;
export declare const OID_TSVECTOR = 3614;
export declare const OID_TSQUERY = 3615;
export declare const OID_SB_VECTOR = 16386;
export declare class ScratchbirdJsonb {
    raw: Buffer;
    value?: any;
    constructor(raw: Buffer, value?: any);
}
export declare class ScratchbirdJson {
    raw: Buffer;
    value?: any;
    constructor(raw: Buffer, value?: any);
}
export declare class ScratchbirdGeometry {
    wkb: Buffer;
    srid?: number;
    wkt?: string;
    constructor(wkb: Buffer, opts?: {
        srid?: number;
        wkt?: string;
    });
}
export declare class ScratchbirdRange<T> {
    lower?: T;
    upper?: T;
    lowerInclusive: boolean;
    upperInclusive: boolean;
    lowerInfinite: boolean;
    upperInfinite: boolean;
    empty: boolean;
    rangeOid?: number;
    constructor(init?: Partial<ScratchbirdRange<T>>);
}
export interface ScratchbirdCompositeField {
    oid: number;
    value?: any;
    raw?: Buffer | null;
}
export declare class ScratchbirdComposite {
    typeOid: number;
    fields: ScratchbirdCompositeField[];
    constructor(fields: ScratchbirdCompositeField[], typeOid?: number);
}
export declare class ScratchbirdInterval {
    months: number;
    days: number;
    micros: number;
    constructor(micros: number, days?: number, months?: number);
}
export declare class ScratchbirdDate {
    value: Date;
    constructor(value: Date);
}
export declare class ScratchbirdTime {
    micros: number;
    constructor(micros: number);
}
export declare class ScratchbirdTimestamp {
    value: Date;
    constructor(value: Date);
}
export declare class ScratchbirdTimestampTZ {
    value: Date;
    constructor(value: Date);
}
export declare class ScratchbirdDecimal {
    value: string;
    constructor(value: string);
}
export declare class ScratchbirdMoney {
    cents: bigint;
    constructor(cents: bigint);
}
export declare class ScratchbirdRaw {
    oid: number;
    data: Buffer;
    constructor(oid: number, data: Buffer);
}
export declare class ScratchbirdTypedValue {
    oid: number;
    value: unknown;
    constructor(oid: number, value: unknown);
}
export declare function oidToString(oid: number): string;
export declare function encodeParam(value: any): {
    param: ParamValue;
    oid: number;
};
export declare function decodeValue(typeOid: number, data: Buffer | null, format: number): any;
export declare function decodeArrayLiteral(text: string): any[];
