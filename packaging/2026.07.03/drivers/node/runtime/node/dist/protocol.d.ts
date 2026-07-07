import { Buffer } from "node:buffer";
export declare const PROTOCOL_MAGIC_BYTES: Buffer<ArrayBuffer>;
export declare const PROTOCOL_VERSION_MAJOR = 1;
export declare const PROTOCOL_VERSION_MINOR = 1;
export declare const PROTOCOL_VERSION: number;
export declare const HEADER_SIZE = 40;
export declare const MAX_MESSAGE_SIZE: number;
export declare enum MessageType {
    STARTUP = 1,
    AUTH_RESPONSE = 2,
    QUERY = 3,
    PARSE = 4,
    BIND = 5,
    DESCRIBE = 6,
    EXECUTE = 7,
    CLOSE = 8,
    SYNC = 9,
    FLUSH = 10,
    CANCEL = 11,
    TERMINATE = 12,
    COPY_DATA = 13,
    COPY_DONE = 14,
    COPY_FAIL = 15,
    SBLR_EXECUTE = 16,
    SUBSCRIBE = 17,
    UNSUBSCRIBE = 18,
    FEDERATED_QUERY = 19,
    STREAM_CONTROL = 20,
    TXN_BEGIN = 21,
    TXN_COMMIT = 22,
    TXN_ROLLBACK = 23,
    TXN_SAVEPOINT = 24,
    TXN_RELEASE = 25,
    TXN_ROLLBACK_TO = 26,
    PING = 27,
    SET_OPTION = 28,
    CLUSTER_AUTH = 29,
    ATTACH_CREATE = 30,
    ATTACH_DETACH = 31,
    ATTACH_LIST = 32,
    AUTH_REQUEST = 64,
    AUTH_OK = 65,
    AUTH_CONTINUE = 66,
    READY = 67,
    ROW_DESCRIPTION = 68,
    DATA_ROW = 69,
    COMMAND_COMPLETE = 70,
    EMPTY_QUERY = 71,
    ERROR = 72,
    NOTICE = 73,
    PARSE_COMPLETE = 74,
    BIND_COMPLETE = 75,
    CLOSE_COMPLETE = 76,
    PORTAL_SUSPENDED = 77,
    NO_DATA = 78,
    PARAMETER_STATUS = 79,
    PARAMETER_DESCRIPTION = 80,
    COPY_IN_RESPONSE = 81,
    COPY_OUT_RESPONSE = 82,
    COPY_BOTH_RESPONSE = 83,
    NOTIFICATION = 84,
    FUNCTION_RESULT = 85,
    NEGOTIATE_VERSION = 86,
    SBLR_COMPILED = 87,
    QUERY_PLAN = 88,
    STREAM_READY = 89,
    STREAM_DATA = 90,
    STREAM_END = 91,
    TXN_STATUS = 92,
    PONG = 93,
    CLUSTER_AUTH_OK = 94,
    FEDERATED_RESULT = 95,
    HEARTBEAT = 128,
    EXTENSION = 129
}
export declare enum AuthMethod {
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
    CLUSTER_PKI = 15
}
export declare const MSG_FLAG_COMPRESSED = 1;
export declare const MSG_FLAG_CONTINUED = 2;
export declare const MSG_FLAG_FINAL = 4;
export declare const MSG_FLAG_URGENT = 8;
export declare const MSG_FLAG_ENCRYPTED = 16;
export declare const MSG_FLAG_CHECKSUM = 32;
export declare const FEATURE_COMPRESSION: bigint;
export declare const FEATURE_STREAMING: bigint;
export declare const FEATURE_SBLR: bigint;
export declare const FEATURE_FEDERATION: bigint;
export declare const FEATURE_NOTIFICATIONS: bigint;
export declare const FEATURE_QUERY_PLAN: bigint;
export declare const FEATURE_BATCH: bigint;
export declare const FEATURE_PIPELINE: bigint;
export declare const FEATURE_BINARY_COPY: bigint;
export declare const FEATURE_SAVEPOINTS: bigint;
export declare const FEATURE_2PC: bigint;
export declare const FEATURE_CHECKSUMS: bigint;
export declare const QUERY_FLAG_DESCRIBE_ONLY = 1;
export declare const QUERY_FLAG_NO_PORTAL = 2;
export declare const QUERY_FLAG_BINARY_RESULT = 4;
export declare const QUERY_FLAG_INCLUDE_PLAN = 8;
export declare const QUERY_FLAG_RETURN_SBLR = 16;
export declare const QUERY_FLAG_NO_CACHE = 32;
export declare const ISOLATION_READ_UNCOMMITTED = 0;
export declare const ISOLATION_READ_COMMITTED = 1;
export declare const ISOLATION_REPEATABLE_READ = 2;
export declare const ISOLATION_SERIALIZABLE = 3;
export declare const READ_COMMITTED_MODE_DEFAULT = 0;
export declare const READ_COMMITTED_MODE_READ_CONSISTENCY = 1;
export declare const READ_COMMITTED_MODE_RECORD_VERSION = 2;
export declare const READ_COMMITTED_MODE_NO_RECORD_VERSION = 3;
export declare const TXN_FLAG_HAS_ISOLATION = 1;
export declare const TXN_FLAG_HAS_ACCESS = 2;
export declare const TXN_FLAG_HAS_DEFERRABLE = 4;
export declare const TXN_FLAG_HAS_WAIT = 8;
export declare const TXN_FLAG_HAS_TIMEOUT = 16;
export declare const TXN_FLAG_HAS_AUTOCOMMIT = 32;
export declare const TXN_FLAG_HAS_READ_COMMITTED_MODE = 256;
export declare const STREAM_START = 0;
export declare const STREAM_PAUSE = 1;
export declare const STREAM_RESUME = 2;
export declare const STREAM_CANCEL = 3;
export declare const STREAM_ACK = 4;
export declare const SUB_TYPE_CHANNEL = 0;
export declare const SUB_TYPE_TABLE = 1;
export declare const SUB_TYPE_QUERY = 2;
export declare const SUB_TYPE_EVENT = 3;
export declare const AUTH_PARAM_METHOD_ID = "auth_method_id";
export declare const AUTH_PARAM_METHOD_PAYLOAD = "auth_method_payload";
export declare const AUTH_PARAM_PAYLOAD_JSON = "auth_payload_json";
export declare const AUTH_PARAM_PAYLOAD_B64 = "auth_payload_b64";
export declare const AUTH_PARAM_PROVIDER_PROFILE = "auth_provider_profile";
export declare const AUTH_PARAM_REQUIRED_METHODS = "auth_required_methods";
export declare const AUTH_PARAM_FORBIDDEN_METHODS = "auth_forbidden_methods";
export declare const AUTH_PARAM_REQUIRE_CHANNEL_BINDING = "auth_require_channel_binding";
export declare const AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN = "workload_identity_token";
export declare const AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION = "proxy_principal_assertion";
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
export declare function applyAuthPluginSelection(params: Record<string, string>, selection: AuthPluginSelection): void;
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
export declare function encodeMessage(header: MessageHeader, payload: Buffer): Buffer;
export declare function decodeHeader(data: Buffer): MessageHeader;
export declare function buildStartupPayload(features: bigint, params: Record<string, string>): Buffer;
export declare function parseAuthRequest(payload: Buffer): {
    method: number;
    data: Buffer;
};
export declare function parseAuthContinue(payload: Buffer): {
    method: number;
    stage: number;
    data: Buffer;
};
export declare function parseAuthOk(payload: Buffer): {
    sessionId: Buffer;
    serverInfo: Buffer;
};
export declare function buildQueryPayload(sql: string, flags: number, maxRows: number, timeoutMs: number): Buffer;
export declare function buildParsePayload(statementName: string, sql: string, paramTypes: number[]): Buffer;
export interface ParamValue {
    format: number;
    data?: Buffer;
    isNull?: boolean;
}
export declare function buildBindPayload(portalName: string, statementName: string, params: ParamValue[], resultFormats: number[]): Buffer;
export declare function buildDescribePayload(describeType: number, name: string): Buffer;
export declare function buildExecutePayload(portalName: string, maxRows: number): Buffer;
export declare function buildClosePayload(closeType: number, name: string): Buffer;
export declare function buildCancelPayload(cancelType: number, targetSequence: number): Buffer;
export declare function buildSblrExecutePayload(sblrHash: bigint, bytecode: Buffer, params: ParamValue[]): Buffer;
export declare function buildSubscribePayload(subscribeType: number, channel: string, filter: string): Buffer;
export declare function buildUnsubscribePayload(channel: string): Buffer;
export declare function buildTxnBeginPayload(flags: number, conflictAction: number, autocommitMode: number, isolationLevel: number, accessMode: number, deferrable: number, waitMode: number, timeoutMs: number, readCommittedMode?: number): Buffer;
export declare function canonicalReadCommittedModeLabel(mode: number): string;
export declare function buildTxnCommitPayload(flags: number): Buffer;
export declare function buildTxnRollbackPayload(flags: number): Buffer;
export declare function buildTxnSavepointPayload(name: string): Buffer;
export declare function buildTxnReleasePayload(name: string): Buffer;
export declare function buildTxnRollbackToPayload(name: string): Buffer;
export declare function buildSetOptionPayload(name: string, value: string): Buffer;
export declare function buildStreamControlPayload(controlType: number, windowSize: number, timeoutMs: number): Buffer;
export declare function buildAttachCreatePayload(mode: string, dbName: string): Buffer;
export declare function parseReady(payload: Buffer): {
    status: number;
    txnId: bigint;
    visibility: bigint;
};
export declare function parseTxnStatus(payload: Buffer): {
    status: string;
    txnId: bigint;
};
export declare function parseParameterStatus(payload: Buffer): {
    name: string;
    value: string;
};
export declare function parseParameterDescription(payload: Buffer): number[];
export declare function parseRowDescription(payload: Buffer): ColumnInfo[];
export declare function parseDataRow(payload: Buffer, columnCount: number): ColumnValue[];
export declare function parseCommandComplete(payload: Buffer): {
    commandType: number;
    rows: bigint;
    lastId: bigint;
    tag: string;
};
export declare function parseNotification(payload: Buffer): NotificationMessage;
export declare function parseQueryPlan(payload: Buffer): QueryPlanMessage;
export declare function parseSblrCompiled(payload: Buffer): SblrCompiledMessage;
export declare function parseErrorMessage(payload: Buffer): {
    severity: string;
    sqlState: string;
    message: string;
    detail: string;
    hint: string;
};
