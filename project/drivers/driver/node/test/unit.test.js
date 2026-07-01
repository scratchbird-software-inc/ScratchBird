// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

const test = require("node:test");
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const {
  parseDsn,
  normalizeCallableQuery,
  normalizeQuery,
  decodeValue,
  OID_INT4,
  OID_SB_VECTOR,
  FORMAT_BINARY,
  Client,
  Pool,
  METADATA_TABLES_QUERY,
  METADATA_SCHEMAS_QUERY,
  METADATA_CONSTRAINTS_QUERY,
  METADATA_INDEX_COLUMNS_QUERY,
  METADATA_PRIMARY_KEYS_QUERY,
  METADATA_FOREIGN_KEYS_QUERY,
  METADATA_PROCEDURES_QUERY,
  METADATA_TABLE_PRIVILEGES_QUERY,
  METADATA_ROUTINES_QUERY,
  METADATA_TYPE_INFO_QUERY,
  filterMetadataRowsForCollectionFamily,
  filterMetadataRowsByRestrictions,
  resolveMetadataCollectionQuery,
  shapeMetadataRowsForCollection,
  buildMetadataSchemaTree,
  mapSqlState,
  READ_COMMITTED_MODE_READ_CONSISTENCY,
  canonicalReadCommittedModeLabel,
  ScratchbirdAuthError,
  ScratchbirdSyntaxError,
  ScratchbirdNotSupportedError,
  ScratchbirdConnectionError,
  ScratchbirdDataError,
  ScratchbirdError,
  CircuitBreaker,
  KeepaliveManager,
  LeakDetector,
  TelemetryCollector,
  betaDriverReadinessStatus,
  resolveLanguageProfile,
  validateAdvisoryCacheContext,
  validateLanguageResourceState,
  validatePreparedBundleReuse,
} = require("../dist/index.js");
const {
  AuthMethod,
  MessageType,
  QUERY_FLAG_BINARY_RESULT,
  applyAuthPluginSelection,
  buildQueryPayload,
} = require("../dist/protocol.js");
const packageMetadata = require("../package.json");

test("parseDsn supports uri", () => {
  const cfg = parseDsn("scratchbird://user:pass@localhost:3092/db?sslmode=require");
  assert.equal(cfg.host, "localhost");
  assert.equal(cfg.port, 3092);
  assert.equal(cfg.user, "user");
  assert.equal(cfg.password, "pass");
  assert.equal(cfg.database, "db");
  assert.equal(cfg.sslmode, "require");
});

test("beta readiness status matches manifest lane and authority boundary", () => {
  const status = betaDriverReadinessStatus();
  assert.equal(status.component_id, "driver:node");
  assert.equal(status.driver_package_uuid, "019e12a0-0008-7000-8000-000000000008");
  assert.equal(status.driver_status, "beta_2");
  assert.equal(status.release_bucket, "release_candidate");
  assert.equal(status.conformance_profile_ref, "driver_node_gate");
  assert.equal(status.authority_boundary.server_revalidation_required, true);
  assert.equal(status.authority_boundary.local_sblr_is_advisory, true);
  assert.equal(status.authority_boundary.local_uuid_cache_is_advisory, true);
  assert.equal(status.authority_boundary.transaction_finality_owner, "engine_mga_transaction_inventory");
  assert.ok(status.runtime_mapping.dsn_keys.includes("auth_method"));
});

test("advisory cache context refuses stale epochs and transaction reuse", () => {
  const current = {
    databaseUuid: "db-1",
    schemaEpoch: "schema-2",
    policyEpoch: "policy-2",
    languageEpoch: "lang-2",
    capabilityEpoch: "cap-2",
    principalUuid: "principal-1",
    roleSetHash: "role-a",
    groupSetHash: "group-a",
    transactionUuid: "txn-2",
  };
  let diag = validateAdvisoryCacheContext({ ...current, policyEpoch: "policy-1" }, current);
  assert.equal(diag.code, "SB_DRIVER_CACHE_POLICY_EPOCH_STALE");
  assert.equal(diag.sqlstate, "42501");

  diag = validateAdvisoryCacheContext({ ...current, transactionUuid: "txn-1" }, current);
  assert.equal(diag.code, "SB_DRIVER_CACHE_TRANSACTION_CONTEXT_MISMATCH");
  assert.equal(diag.sqlstate, "25001");
  assert.equal(validateAdvisoryCacheContext(current, current), undefined);
});

test("prepared bundle reuse requires server admission and matching context", () => {
  const current = {
    databaseUuid: "db-1",
    schemaEpoch: "schema-1",
    policyEpoch: "policy-1",
    principalUuid: "principal-1",
    transactionUuid: "txn-1",
  };
  let diag = validatePreparedBundleReuse({ ...current, serverAdmitted: false }, current);
  assert.equal(diag.code, "SB_DRIVER_SBLR_SERVER_ADMISSION_REQUIRED");

  diag = validatePreparedBundleReuse({ ...current, databaseUuid: "db-2", serverAdmitted: true }, current);
  assert.equal(diag.code, "SB_DRIVER_CACHE_DATABASE_MISMATCH");
});

test("language resources fall back to standard English and reject stale epochs", () => {
  let resolved = resolveLanguageProfile("fr_CA", { en_US: true });
  assert.equal(resolved.selected, "en_US");
  assert.equal(resolved.fallback, true);

  resolved = resolveLanguageProfile("fr_CA", { en_US: true, fr_CA: true });
  assert.equal(resolved.selected, "fr_CA");
  assert.equal(resolved.fallback, false);

  const diag = validateLanguageResourceState({
    locale: "fr_CA",
    schemaVersion: "1",
    contentHash: "sha256:abc",
    signature: "sig",
    epoch: "lang-1",
    expectedEpoch: "lang-2",
  });
  assert.equal(diag.code, "SB_DRIVER_LANGUAGE_RESOURCE_EPOCH_STALE");
});

test("package smoke reports MPL license", () => {
  assert.equal(packageMetadata.name, "scratchbird");
  assert.equal(packageMetadata.license, "MPL-2.0");
});

test("copy query payload can use binary-result framing", () => {
  const payload = buildQueryPayload("COPY users.public.t FROM STDIN", QUERY_FLAG_BINARY_RESULT, 0, 0);
  assert.equal(payload.readUInt32LE(0), QUERY_FLAG_BINARY_RESULT);
  assert.equal(payload.readUInt32LE(4), 0);
  assert.equal(payload.readUInt32LE(8), 0);
  assert.equal(payload.subarray(12).toString("utf8"), "COPY users.public.t FROM STDIN");
});

test("parseDsn supports key-value", () => {
  const cfg = parseDsn("host=127.0.0.1 port=3092 dbname=mydb user=me");
  assert.equal(cfg.host, "127.0.0.1");
  assert.equal(cfg.port, 3092);
  assert.equal(cfg.database, "mydb");
  assert.equal(cfg.user, "me");
});

test("parseDsn supports manager_proxy mode params", () => {
  const cfg = parseDsn("scratchbird://admin:secret@localhost:3092/mydb?front_door_mode=manager_proxy&manager_auth_token=token&manager_client_flags=7");
  assert.equal(cfg.frontDoorMode, "manager_proxy");
  assert.equal(cfg.managerAuthToken, "token");
  assert.equal(cfg.managerClientFlags, 7);
});

test("parseDsn supports metadataExpandSchemaParents aliases", () => {
  const fromUri = parseDsn("scratchbird://admin:secret@localhost:3092/mydb?metadata_expand_schema_parents=true");
  const fromKv = parseDsn("host=127.0.0.1 dbname=mydb user=me expandSchemaParents=1");
  assert.equal(fromUri.metadataExpandSchemaParents, true);
  assert.equal(fromKv.metadataExpandSchemaParents, true);
});

test("parseDsn supports auth plugin and pinning params", () => {
  const cfg = parseDsn(
    "scratchbird://user:pass@localhost:3092/db"
      + "?connect_client_flags=257"
      + "&auth_method_id=scratchbird.auth.proxy_assertion"
      + "&auth_method_payload=opaque"
      + "&auth_payload_json=%7B%22subject%22%3A%22alice%22%7D"
      + "&auth_payload_b64=YWJj"
      + "&auth_provider_profile=corp_primary"
      + "&auth_required_methods=SCRAM_SHA_256%2CTOKEN"
      + "&auth_forbidden_methods=MD5"
      + "&auth_require_channel_binding=true"
      + "&workload_identity_token=jwt-token"
      + "&proxy_principal_assertion=signed-assertion",
  );
  assert.equal(cfg.connectClientFlags, 257);
  assert.equal(cfg.authMethodId, "scratchbird.auth.proxy_assertion");
  assert.equal(cfg.authMethodPayload, "opaque");
  assert.equal(cfg.authPayloadJson, "{\"subject\":\"alice\"}");
  assert.equal(cfg.authPayloadB64, "YWJj");
  assert.equal(cfg.authProviderProfile, "corp_primary");
  assert.equal(cfg.authRequiredMethods, "SCRAM_SHA_256,TOKEN");
  assert.equal(cfg.authForbiddenMethods, "MD5");
  assert.equal(cfg.authRequireChannelBinding, true);
  assert.equal(cfg.workloadIdentityToken, "jwt-token");
  assert.equal(cfg.proxyPrincipalAssertion, "signed-assertion");
});

test("parseDsn supports generic auth token param", () => {
  const cfg = parseDsn("scratchbird://user:pass@localhost:3092/db?auth_token=bearer-123");
  assert.equal(cfg.authToken, "bearer-123");
});

test("parseDsn supports dormant reattach params", () => {
  const cfg = parseDsn(
    "scratchbird://user:pass@localhost:3092/db"
      + "?dormant_id=00112233-4455-6677-8899-aabbccddeeff"
      + "&dormant_reattach_token=ffeeddcc-bbaa-9988-7766-554433221100",
  );
  assert.equal(cfg.dormantId, "00112233-4455-6677-8899-aabbccddeeff");
  assert.equal(cfg.dormantReattachToken, "ffeeddcc-bbaa-9988-7766-554433221100");
});

test("applyAuthPluginSelection sets extended params and rejects invalid namespace", () => {
  const params = {};
  applyAuthPluginSelection(params, {
    methodId: "scratchbird.auth.proxy_assertion",
    methodPayload: "opaque",
    payloadJson: "{\"subject\":\"alice\"}",
    payloadB64: "YWJj",
    providerProfile: "corp_primary",
    requiredMethods: "SCRAM_SHA_256,TOKEN",
    forbiddenMethods: "MD5",
    requireChannelBinding: true,
    workloadIdentityToken: "jwt-token",
    proxyPrincipalAssertion: "signed-assertion",
  });
  assert.equal(params.auth_method_id, "scratchbird.auth.proxy_assertion");
  assert.equal(params.auth_method_payload, "opaque");
  assert.equal(params.auth_payload_json, "{\"subject\":\"alice\"}");
  assert.equal(params.auth_payload_b64, "YWJj");
  assert.equal(params.auth_provider_profile, "corp_primary");
  assert.equal(params.auth_required_methods, "SCRAM_SHA_256,TOKEN");
  assert.equal(params.auth_forbidden_methods, "MD5");
  assert.equal(params.auth_require_channel_binding, "1");
  assert.equal(params.workload_identity_token, "jwt-token");
  assert.equal(params.proxy_principal_assertion, "signed-assertion");

  assert.throws(
    () => applyAuthPluginSelection({}, { methodId: "invalid.namespace" }),
    /Invalid auth_method_id namespace/,
  );
});

test("normalizeQuery rewrites positional", () => {
  const normalized = normalizeQuery("select ?", [1]);
  assert.equal(normalized.sql, "select $1");
  assert.deepEqual(normalized.params, [1]);
});

test("normalizeQuery rewrites named", () => {
  const normalized = normalizeQuery("select :a, @b", { a: 1, b: 2 });
  assert.equal(normalized.sql, "select $1, $2");
  assert.deepEqual(normalized.params, [1, 2]);
});

test("normalizeCallableQuery rewrites JDBC escape-call syntax", () => {
  const procedure = normalizeCallableQuery("{ call app.do_work(?, ?) }", [7, 9]);
  assert.equal(procedure.sql, "call app.do_work($1, $2)");
  assert.deepEqual(procedure.params, [7, 9]);

  const functionCall = normalizeCallableQuery("{ ? = call math.abs(?) }", [-3]);
  assert.equal(functionCall.sql, "select math.abs($1) as return_value");
  assert.deepEqual(functionCall.params, [-3]);
});

test("decodeValue decodes int4", () => {
  const buf = Buffer.alloc(4);
  buf.writeInt32LE(42, 0);
  assert.equal(decodeValue(OID_INT4, buf, FORMAT_BINARY), 42);
});

test("decodeValue decodes vector", () => {
  const vectorText = "[1, 2, 3]";
  const buf = Buffer.alloc(4 + vectorText.length);
  buf.writeUInt32LE(vectorText.length, 0);
  buf.write(vectorText, 4, "utf8");
  assert.deepEqual(decodeValue(OID_SB_VECTOR, buf, FORMAT_BINARY), [1, 2, 3]);
});

function makeAuthRequestPayload(method) {
  const payload = Buffer.alloc(4);
  payload.writeUInt8(method, 0);
  return payload;
}

function makeAuthContinuePayload(method, dataText) {
  const data = Buffer.from(dataText, "utf8");
  const payload = Buffer.alloc(8 + data.length);
  payload.writeUInt8(method, 0);
  payload.writeUInt8(1, 1);
  payload.writeUInt32LE(data.length, 4);
  data.copy(payload, 8);
  return payload;
}

function makeAuthOkPayload(serverInfo = "") {
  const info = Buffer.from(serverInfo, "utf8");
  const payload = Buffer.alloc(20 + info.length);
  Buffer.alloc(16).copy(payload, 0);
  payload.writeUInt32LE(info.length, 16);
  info.copy(payload, 20);
  return payload;
}

function parseScramAttributes(message) {
  const attrs = {};
  for (const part of message.split(",")) {
    const idx = part.indexOf("=");
    if (idx > 0) {
      attrs[part.slice(0, idx)] = part.slice(idx + 1);
    }
  }
  return attrs;
}

function buildScramServerVerifier({ algorithm, password, clientFirst, serverFirst }) {
  const clientFirstBare = clientFirst.slice(3);
  const parsed = parseScramAttributes(serverFirst);
  const salt = Buffer.from(parsed.s, "base64");
  const iterations = Number(parsed.i);
  const digestLength = algorithm === "sha512" ? 64 : 32;
  const saltedPassword = crypto.pbkdf2Sync(password, salt, iterations, digestLength, algorithm);
  const clientFinalWithoutProof = `c=biws,r=${parsed.r}`;
  const authMessage = `${clientFirstBare},${serverFirst},${clientFinalWithoutProof}`;
  const serverKey = crypto.createHmac(algorithm, saltedPassword).update("Server Key").digest();
  return crypto.createHmac(algorithm, serverKey).update(authMessage).digest("base64");
}

function makeReadyPayload(txnId, status = txnId === 0n ? 0 : 1) {
  const payload = Buffer.alloc(20);
  payload.writeUInt8(status, 0);
  payload.writeBigUInt64LE(txnId, 4);
  payload.writeBigUInt64LE(0n, 12);
  return payload;
}

function createMockProtocol(readyTxnIds = []) {
  const queue = readyTxnIds.map((entry) => ({
    header: { type: MessageType.READY },
    payload: typeof entry === "object"
      ? makeReadyPayload(entry.txnId ?? 0n, entry.status ?? ((entry.txnId ?? 0n) === 0n ? 0 : 1))
      : makeReadyPayload(entry),
  }));
  return createQueuedProtocol(queue);
}

function makeTxnStatusPayload(status, txnId) {
  const payload = Buffer.alloc(12);
  payload.writeUInt8(status.charCodeAt(0), 0);
  payload.writeBigUInt64LE(txnId, 4);
  return payload;
}

function makeParameterStatusPayload(name, value) {
  const nameBuffer = Buffer.from(name, "utf8");
  const valueBuffer = Buffer.from(value, "utf8");
  const payload = Buffer.alloc(8 + nameBuffer.length + valueBuffer.length);
  payload.writeUInt32LE(nameBuffer.length, 0);
  nameBuffer.copy(payload, 4);
  payload.writeUInt32LE(valueBuffer.length, 4 + nameBuffer.length);
  valueBuffer.copy(payload, 8 + nameBuffer.length);
  return payload;
}

function makeErrorPayload(fields) {
  const parts = [];
  for (const [tag, value] of Object.entries(fields)) {
    parts.push(Buffer.from(tag, "ascii"));
    parts.push(Buffer.from(String(value), "utf8"));
    parts.push(Buffer.from([0]));
  }
  parts.push(Buffer.from([0]));
  return Buffer.concat(parts);
}

function createQueuedProtocol(queueEntries = []) {
  const queue = [...queueEntries];
  const sent = [];
  let txnId = 0n;
  return {
    sent,
    async sendMessage(type, payload, flags, forceZero) {
      sent.push({ type, payload, flags, forceZero });
      return sent.length;
    },
    async recv() {
      if (!queue.length) {
        throw new Error("mock receive queue exhausted");
      }
      return queue.shift();
    },
    setTxnId(nextTxnId) {
      txnId = nextTxnId;
    },
    getTxnId() {
      return txnId;
    },
    setAttachment(_id, nextTxnId) {
      txnId = nextTxnId;
    },
    close() {},
  };
}

function makeReadyMessage(txnId) {
  return {
    header: { type: MessageType.READY },
    payload: makeReadyPayload(txnId),
  };
}

function createManagerProbeProtocol(frames) {
  const sent = [];
  const queue = [...frames];
  return {
    sent,
    async connect() {},
    async sendManagerFrame(type, payload) {
      sent.push({ type, payload });
    },
    async recvManagerFrame() {
      if (!queue.length) {
        throw new Error("manager frame queue exhausted");
      }
      return queue.shift();
    },
    close() {},
  };
}

function makeCommandCompletePayload(tag, rows, lastId = 0n, commandType = 0) {
  const tagBuffer = Buffer.from(tag, "utf8");
  const payload = Buffer.alloc(20 + tagBuffer.length + 1);
  payload.writeUInt8(commandType, 0);
  payload.writeBigUInt64LE(rows, 4);
  payload.writeBigUInt64LE(lastId, 12);
  tagBuffer.copy(payload, 20);
  payload[payload.length - 1] = 0;
  return payload;
}

function parseSqlFromParsePayload(payload) {
  const nameLen = payload.readUInt32LE(0);
  const sqlLenOffset = 4 + nameLen;
  const sqlLen = payload.readUInt32LE(sqlLenOffset);
  return payload.subarray(sqlLenOffset + 4, sqlLenOffset + 4 + sqlLen).toString("utf8");
}

function parseSqlFromQueryPayload(payload) {
  const sqlWithTerminator = payload.subarray(12).toString("utf8");
  return sqlWithTerminator.endsWith("\u0000")
    ? sqlWithTerminator.slice(0, -1)
    : sqlWithTerminator;
}

test("probeAuthSurface reports direct auth negotiation requirements", async () => {
  const client = new Client({ user: "alice", database: "db" });
  client.protocol = {
    async connect() {},
    async sendMessage() {},
    async recv() {
      return {
        header: { type: MessageType.AUTH_REQUEST },
        payload: makeAuthRequestPayload(AuthMethod.SCRAM_SHA_512),
      };
    },
    close() {},
  };

  const probe = await client.probeAuthSurface();
  assert.equal(probe.reachable, true);
  assert.equal(probe.ingressMode, "direct");
  assert.equal(probe.requiredMethod, "SCRAM_SHA_512");
  assert.equal(probe.requiredPluginMethodId, "scratchbird.auth.scram_sha_512");
  assert.equal(probe.admittedMethods.length, 1);
  assert.equal(probe.admittedMethods[0].wireMethod, "SCRAM_SHA_512");
  assert.equal(probe.admittedMethods[0].executableLocally, true);
});

test("probeAuthSurface reports manager proxy token bootstrap", async () => {
  const client = new Client({
    user: "alice",
    database: "db",
    frontDoorMode: "manager_proxy",
  });
  client.protocol = createManagerProbeProtocol([
    { type: 0x64, payload: Buffer.alloc(0) },
    { type: 0x12, payload: Buffer.alloc(0) },
  ]);

  const probe = await client.probeAuthSurface();
  assert.equal(probe.ingressMode, "manager_proxy");
  assert.equal(probe.requiredMethod, "TOKEN");
  assert.equal(probe.additionalContinuationPossible, true);
});

test("connect negotiates SCRAM-SHA-512 and records resolved auth context", async () => {
  const client = new Client({ user: "alice", password: "secret", database: "db" });
  const sent = [];
  let stage = 0;
  let clientFirst = "";
  let serverFirst = "";
  const protocol = {
    sent,
    async connect() {},
    async sendMessage(type, payload, flags, forceZero) {
      sent.push({ type, payload, flags, forceZero });
      if (type === MessageType.AUTH_RESPONSE && stage === 1) {
        clientFirst = payload.toString("utf8");
      }
      return sent.length;
    },
    async recv() {
      if (stage === 0) {
        stage += 1;
        return {
          header: { type: MessageType.AUTH_REQUEST, attachmentId: Buffer.alloc(16), txnId: 0n },
          payload: makeAuthRequestPayload(AuthMethod.SCRAM_SHA_512),
        };
      }
      if (stage === 1) {
        const parsed = parseScramAttributes(clientFirst.slice(3));
        serverFirst = `r=${parsed.r}server,s=${Buffer.from("salt512", "utf8").toString("base64")},i=4096`;
        stage += 1;
        return {
          header: { type: MessageType.AUTH_CONTINUE, attachmentId: Buffer.alloc(16), txnId: 0n },
          payload: makeAuthContinuePayload(AuthMethod.SCRAM_SHA_512, serverFirst),
        };
      }
      if (stage === 2) {
        stage += 1;
        return {
          header: { type: MessageType.AUTH_OK, attachmentId: Buffer.alloc(16), txnId: 0n },
          payload: makeAuthOkPayload(
            `v=${buildScramServerVerifier({
              algorithm: "sha512",
              password: "secret",
              clientFirst,
              serverFirst,
            })}`,
          ),
        };
      }
      return makeReadyMessage(0n);
    },
    setTxnId() {},
    setAttachment() {},
    close() {},
  };
  client.protocol = protocol;
  client.applySchema = async () => {};

  await client.connect();
  const authResponses = sent.filter((entry) => entry.type === MessageType.AUTH_RESPONSE);
  assert.equal(authResponses.length, 2);
  assert.equal(client.getResolvedAuthContext().resolvedAuthMethod, "SCRAM_SHA_512");
  assert.equal(client.getResolvedAuthContext().resolvedAuthPluginId, "scratchbird.auth.scram_sha_512");
});

test("connect sends TOKEN auth payload and records resolved auth context", async () => {
  const client = new Client({
    user: "alice",
    database: "db",
    authToken: "jwt-token",
  });
  const protocol = createQueuedProtocol([
    {
      header: { type: MessageType.AUTH_REQUEST, attachmentId: Buffer.alloc(16), txnId: 0n },
      payload: makeAuthRequestPayload(AuthMethod.TOKEN),
    },
    {
      header: { type: MessageType.AUTH_OK, attachmentId: Buffer.alloc(16), txnId: 0n },
      payload: makeAuthOkPayload(),
    },
    makeReadyMessage(0n),
  ]);
  client.protocol = protocol;
  client.protocol.connect = async () => {};
  client.protocol.close = () => {};
  client.applySchema = async () => {};

  await client.connect();
  const authResponses = protocol.sent.filter((entry) => entry.type === MessageType.AUTH_RESPONSE);
  assert.equal(authResponses.length, 1);
  assert.equal(authResponses[0].payload.toString("utf8"), "jwt-token");
  assert.deepEqual(client.getResolvedAuthContext(), {
    ingressMode: "direct",
    resolvedAuthMethod: "TOKEN",
    resolvedAuthPluginId: "scratchbird.auth.authkey_token",
    managerAuthenticated: false,
    attached: true,
  });
});

test("connect fails closed when PEER auth is admitted", async () => {
  const client = new Client({ user: "alice", database: "db" });
  const protocol = createQueuedProtocol([
    {
      header: { type: MessageType.AUTH_REQUEST, attachmentId: Buffer.alloc(16), txnId: 0n },
      payload: makeAuthRequestPayload(AuthMethod.PEER),
    },
  ]);
  client.protocol = protocol;
  client.protocol.connect = async () => {};
  client.protocol.close = () => {};

  await assert.rejects(
    () => client.connect(),
    (err) => err instanceof ScratchbirdNotSupportedError && err.code === "0A000",
  );
});

test("transaction lifecycle restarts implicit boundaries and still enforces begin-before-commit semantics", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createMockProtocol([42n, 43n, 0n]);
  client.connected = true;
  client.protocol = protocol;

  await assert.rejects(() => client.commitTransaction(), (err) => err && err.code === "25000");

  await client.beginTransaction();
  await client.beginTransaction();
  await client.commitTransaction();

  assert.deepEqual(
    protocol.sent.map((entry) => entry.type),
    [MessageType.TXN_BEGIN, MessageType.TXN_BEGIN, MessageType.TXN_COMMIT],
  );
});

test("prepared transaction helpers emit canonical control SQL", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createMockProtocol([0n, 0n, 0n]);
  client.connected = true;
  client.protocol = protocol;

  await client.prepareTransaction("gid'alpha");
  await client.commitPrepared("gid'alpha");
  await client.rollbackPrepared("gid'alpha");

  assert.deepEqual(
    protocol.sent.map((entry) => entry.type),
    [MessageType.QUERY, MessageType.QUERY, MessageType.QUERY],
  );
  assert.equal(parseSqlFromQueryPayload(protocol.sent[0].payload), "PREPARE TRANSACTION 'gid''alpha'");
  assert.equal(parseSqlFromQueryPayload(protocol.sent[1].payload), "COMMIT PREPARED 'gid''alpha'");
  assert.equal(parseSqlFromQueryPayload(protocol.sent[2].payload), "ROLLBACK PREPARED 'gid''alpha'");
});

test("dormant helpers expose the explicit native token flow", async () => {
  const client = new Client({ user: "me", database: "db" });
  assert.equal(client.supportsPreparedTransactions(), true);
  assert.equal(client.supportsDormantReattach(), true);
});

test("detachToDormant returns engine-issued identifiers", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createQueuedProtocol([
    {
      header: { type: MessageType.PARAMETER_STATUS },
      payload: makeParameterStatusPayload("dormant_id", "00112233-4455-6677-8899-aabbccddeeff"),
    },
    {
      header: { type: MessageType.PARAMETER_STATUS },
      payload: makeParameterStatusPayload("dormant_reattach_token", "ffeeddcc-bbaa-9988-7766-554433221100"),
    },
    makeReadyMessage(0n),
  ]);
  client.connected = true;
  client.protocol = protocol;

  const detached = await client.detachToDormant();

  assert.equal(detached.dormantId, "00112233-4455-6677-8899-aabbccddeeff");
  assert.equal(detached.reattachToken, "ffeeddcc-bbaa-9988-7766-554433221100");
  assert.deepEqual(protocol.sent.map((entry) => entry.type), [MessageType.ATTACH_DETACH]);
});

test("detachToDormant rejects missing identifiers", async () => {
  const client = new Client({ user: "me", database: "db" });
  client.connected = true;
  client.protocol = createMockProtocol([0n]);

  await assert.rejects(
    () => client.detachToDormant(),
    (err) => err && err.code === "08006",
  );
});

test("reattachDormant reconnects with explicit startup params", async () => {
  const client = new Client({ user: "me", database: "db", schema: "analytics.dev" });
  const observed = [];
  client.connected = true;
  client.protocol.close = () => {};
  client.cleanupResilience = () => {};
  client.clearAbandonedSessionState = () => {};
  client.connect = async function connectWithCapture() {
    observed.push({
      dormantId: this.config.dormantId,
      dormantReattachToken: this.config.dormantReattachToken,
      skipSchemaApplyOnce: this.skipSchemaApplyOnce,
    });
  };

  await client.reattachDormant(
    "00112233-4455-6677-8899-aabbccddeeff",
    "ffeeddcc-bbaa-9988-7766-554433221100",
  );

  assert.deepEqual(observed, [
    {
      dormantId: "00112233-4455-6677-8899-aabbccddeeff",
      dormantReattachToken: "ffeeddcc-bbaa-9988-7766-554433221100",
      skipSchemaApplyOnce: true,
    },
  ]);
  assert.equal(client.config.dormantId, undefined);
  assert.equal(client.config.dormantReattachToken, undefined);
  assert.equal(client.skipSchemaApplyOnce, false);
});

test("reattachDormant validates token requirements", async () => {
  const client = new Client({ user: "me", database: "db" });
  client.connected = true;

  await assert.rejects(
    () => client.reattachDormant("not-a-uuid", "ffeeddcc-bbaa-9988-7766-554433221100"),
    (err) => err && err.code === "42601",
  );
  await assert.rejects(
    () => client.reattachDormant("00112233-4455-6677-8899-aabbccddeeff"),
    (err) => err && err.code === "42601",
  );
});

test("portal resume requires explicit suspended state", async () => {
  const client = new Client({ user: "me", database: "db" });
  client.protocol = createMockProtocol([]);

  await assert.rejects(
    () => client.resumePortal(1),
    (err) => err && err.code === "55000",
  );
});

test("beginTransaction encodes readCommittedMode and documents its canonical meaning", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createMockProtocol([42n]);
  client.connected = true;
  client.protocol = protocol;

  await client.beginTransaction({
    readCommittedMode: READ_COMMITTED_MODE_READ_CONSISTENCY,
    timeoutMs: 25,
  });

  const payload = protocol.sent[0].payload;
  assert.equal(canonicalReadCommittedModeLabel(READ_COMMITTED_MODE_READ_CONSISTENCY), "READ COMMITTED READ CONSISTENCY");
  assert.equal(payload.length, 16);
  assert.equal(payload.readUInt16LE(0), 0x0111);
  assert.equal(payload.readUInt8(4), 1);
  assert.equal(payload.readUInt32LE(8), 25);
  assert.equal(payload.readUInt8(12), READ_COMMITTED_MODE_READ_CONSISTENCY);
});

test("beginTransaction rejects readCommittedMode with snapshot aliases", async () => {
  const client = new Client({ user: "me", database: "db" });
  client.connected = true;
  client.protocol = createMockProtocol();

  await assert.rejects(
    () =>
      client.beginTransaction({
        isolationLevel: 2,
        readCommittedMode: READ_COMMITTED_MODE_READ_CONSISTENCY,
      }),
    (err) =>
      err instanceof ScratchbirdNotSupportedError &&
      err.code === "0A000" &&
      /READ COMMITTED isolation alias/.test(err.message),
  );
});

test("savepoint flows require an active transaction and a non-empty name", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createMockProtocol([88n, 88n, 88n, 88n, 0n]);
  client.connected = true;
  client.protocol = protocol;

  await assert.rejects(() => client.savepoint("sp"), (err) => err && err.code === "25000");

  await client.beginTransaction();
  await assert.rejects(() => client.savepoint("   "), (err) => err && err.code === "42601");
  await client.savepoint("sp");
  await client.releaseSavepoint("sp");
  await client.rollbackToSavepoint("sp");
  await client.rollbackTransaction();

  assert.deepEqual(
    protocol.sent.map((entry) => entry.type),
    [MessageType.TXN_BEGIN, MessageType.TXN_SAVEPOINT, MessageType.TXN_RELEASE, MessageType.TXN_ROLLBACK_TO, MessageType.TXN_ROLLBACK],
  );
});

test("READY keeps the session transaction-active and preserves the reported txn id", () => {
  const client = new Client({ user: "me", database: "db" });
  client.applyRuntimeReadyState(1, 77n);

  assert.equal(client.transactionActive, true);
  assert.equal(client.protocol.getTxnId(), 77n);
});

test("TXN_STATUS updates can mark and clear active native boundaries", () => {
  const client = new Client({ user: "me", database: "db" });

  const activeHandled = client.handleAsyncMessage({
    header: { type: MessageType.TXN_STATUS },
    payload: makeTxnStatusPayload("T", 91n),
  });
  assert.equal(activeHandled, true);
  assert.equal(client.transactionActive, true);
  assert.equal(client.protocol.getTxnId(), 91n);

  const idleHandled = client.handleAsyncMessage({
    header: { type: MessageType.TXN_STATUS },
    payload: makeTxnStatusPayload("I", 0n),
  });
  assert.equal(idleHandled, true);
  assert.equal(client.transactionActive, false);
  assert.equal(client.protocol.getTxnId(), 0n);
});

test("drainUntilReady consumes replacement READY after ERROR before throwing", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createQueuedProtocol([
    {
      header: { type: MessageType.ERROR },
      payload: makeErrorPayload({ C: "HY000", M: "statement refused" }),
    },
    makeReadyMessage(123n),
  ]);
  client.protocol = protocol;

  await assert.rejects(() => client.drainUntilReady(), /statement refused/);
  assert.equal(protocol.getTxnId(), 123n);
  assert.equal(client.transactionActive, true);
});

test("autocommit toggle drives implicit begin and commit", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createMockProtocol([{ txnId: 0n, status: 1 }]);
  client.connected = true;
  client.protocol = protocol;
  client.transactionActive = true;
  client.collectResults = async () => ({ rows: [], rowCount: 0, fields: [], command: "SELECT", lastId: null });

  assert.equal(client.getAutoCommit(), true);
  await client.setAutoCommit(false);
  await client.query("select 1");
  assert.equal(client.getAutoCommit(), false);
  assert.equal(client.transactionActive, true);

  await client.setAutoCommit(true);
  assert.equal(client.getAutoCommit(), true);
  assert.equal(client.transactionActive, true);
  assert.deepEqual(
    protocol.sent.map((entry) => entry.type),
    [MessageType.QUERY, MessageType.TXN_COMMIT],
  );
});

test("setSessionSchema null resets to users.public", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createMockProtocol([0n]);
  client.connected = true;
  client.protocol = protocol;

  await client.setSessionSchema(null);
  assert.equal(client.getSessionSchema(), null);
  assert.equal(client.config.schema, undefined);
  assert.equal(protocol.sent.length, 1);
  assert.equal(protocol.sent[0].type, MessageType.QUERY);
  assert.equal(parseSqlFromQueryPayload(protocol.sent[0].payload), 'SET SCHEMA "users.public"');
});

test("setSessionSchema applies schema statement on connected clients", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createMockProtocol([0n]);
  client.connected = true;
  client.protocol = protocol;

  await client.setSessionSchema("analytics.dev");
  assert.equal(client.getSessionSchema(), "analytics.dev");
  assert.equal(client.config.schema, "analytics.dev");
  assert.equal(protocol.sent.length, 1);
  assert.equal(protocol.sent[0].type, MessageType.QUERY);
  assert.equal(parseSqlFromQueryPayload(protocol.sent[0].payload), 'SET SCHEMA "analytics.dev"');
});

test("extended query path rewrites named parameters and sends parse/bind/execute/sync", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createMockProtocol();
  client.connected = true;
  client.protocol = protocol;
  client.describeStatement = async () => 1;
  client.collectResults = async () => ({ rows: [], rowCount: 0, fields: [], command: "SELECT", lastId: null });

  await client.query("select :value", { value: 7 });

  assert.deepEqual(
    protocol.sent.map((entry) => entry.type),
    [MessageType.PARSE, MessageType.BIND, MessageType.EXECUTE, MessageType.SYNC],
  );
  assert.equal(parseSqlFromParsePayload(protocol.sent[0].payload), "select $1");
});

test("prepared execute path sends bind/execute/sync and nativeSQL normalizes aliases", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createMockProtocol();
  client.connected = true;
  client.protocol = protocol;
  client.collectResults = async () => ({ rows: [], rowCount: 0, fields: [], command: "SELECT", lastId: null });
  client.prepared.set("stmt", { sql: "select $1, $2", paramCount: 2, namedOrder: ["a", "b"] });

  const normalized = client.nativeSQL("select :a, @b", { a: 1, b: 2 });
  await client.execute("stmt", { a: 1, b: 2 });

  assert.equal(normalized, "select $1, $2");
  assert.deepEqual(
    protocol.sent.map((entry) => entry.type),
    [MessageType.BIND, MessageType.EXECUTE, MessageType.SYNC],
  );
});

test("prepare supports statement name reuse and refreshes cached SQL", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createMockProtocol();
  client.connected = true;
  client.protocol = protocol;
  client.describeStatement = async () => 0;

  await client.prepare("reuse_stmt", "select 1 as value");
  await client.prepare("reuse_stmt", "select 2 as value");

  assert.equal(client.prepared.get("reuse_stmt").sql, "select 2 as value");
  assert.deepEqual(
    protocol.sent.map((entry) => entry.type),
    [MessageType.PARSE, MessageType.PARSE],
  );
});

test("prepare rewrites placeholders so prepared execute can reuse positional parameters", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createMockProtocol();
  client.connected = true;
  client.protocol = protocol;
  client.describeStatement = async () => 2;
  client.collectResults = async () => ({ rows: [], rowCount: 0, fields: [], command: "SELECT", lastId: null });

  await client.prepare("multi_stmt", "select ? as first_value; select ? as second_value");
  assert.equal(parseSqlFromParsePayload(protocol.sent[0].payload), "select $1 as first_value; select $2 as second_value");

  protocol.sent.length = 0;
  await client.execute("multi_stmt", [7, 9]);

  assert.deepEqual(
    protocol.sent.map((entry) => entry.type),
    [MessageType.BIND, MessageType.EXECUTE, MessageType.SYNC],
  );
});

test("prepare preserves named placeholder order for prepared execution", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createMockProtocol();
  client.connected = true;
  client.protocol = protocol;
  client.describeStatement = async () => 2;
  client.collectResults = async () => ({ rows: [], rowCount: 0, fields: [], command: "SELECT", lastId: null });

  await client.prepare("named_stmt", "select :first_value, @second_value");
  assert.equal(parseSqlFromParsePayload(protocol.sent[0].payload), "select $1, $2");

  protocol.sent.length = 0;
  await client.execute("named_stmt", { second_value: 9, first_value: 7 });

  assert.deepEqual(
    protocol.sent.map((entry) => entry.type),
    [MessageType.BIND, MessageType.EXECUTE, MessageType.SYNC],
  );
});

test("connect clears abandoned prepared and session-bound state on same client reuse", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = client.protocol;
  let closes = 0;

  protocol.close = () => {
    closes++;
  };
  protocol.connect = async () => {};
  client.handshake = async () => {};
  client.applySchema = async () => {};
  client.keepaliveManager.start = () => {};
  client.keepaliveManager.register = () => ({
    needsValidation() {
      return false;
    },
    markActive() {},
  });
  client.leakDetector.start = () => {};
  client.leakDetector.checkout = () => ({
    release() {},
  });

  client.connected = true;
  client.transactionActive = true;
  client.prepared.set("stmt", { sql: "select 1", paramCount: 0 });
  client.parameters = { attachment_id: "stale", current_txn_id: "77" };
  client.lastPlan = { source: "stale" };
  client.lastSblr = { source: "stale" };

  await client.connect();

  assert.equal(closes, 1);
  assert.equal(client.connected, true);
  assert.equal(client.transactionActive, false);
  assert.equal(client.prepared.size, 0);
  assert.deepEqual(client.parameters, {});
  assert.equal(client.getLastPlan(), undefined);
  assert.equal(client.getLastSblr(), undefined);
  await assert.rejects(() => client.execute("stmt"), /Unknown prepared statement: stmt/);
});

test("nativeSQL and nativeCallableSQL wrap normalization failures as syntax errors", () => {
  const client = new Client({ user: "me", database: "db" });

  assert.throws(
    () => client.nativeSQL("select ?", []),
    (err) => err instanceof ScratchbirdSyntaxError && err.code === "07001",
  );

  assert.throws(
    () => client.nativeCallableSQL("{ ? = call abs( }", []),
    (err) => err instanceof ScratchbirdSyntaxError && err.code === "07001",
  );
});

test("queryMulti returns independent result sets and preserves generated keys", async () => {
  const client = new Client({ user: "me", database: "db" });
  const protocol = createQueuedProtocol([
    {
      header: { type: MessageType.COMMAND_COMPLETE },
      payload: makeCommandCompletePayload("INSERT", 1n, 101n),
    },
    makeReadyMessage(0n),
    {
      header: { type: MessageType.COMMAND_COMPLETE },
      payload: makeCommandCompletePayload("INSERT", 1n, 202n),
    },
    makeReadyMessage(0n),
  ]);
  client.connected = true;
  client.protocol = protocol;

  const results = await client.queryMulti("insert into t values (1); insert into t values (2)");
  assert.equal(results.length, 2);
  assert.equal(results[0].command, "INSERT");
  assert.equal(results[0].lastId, 101n);
  assert.equal(results[1].lastId, 202n);
});

test("queryBatch aggregates per-item command summaries", async () => {
  const client = new Client({ user: "me", database: "db" });
  client.connected = true;
  client.query = async (_sql, params) => ({
    rows: [],
    rowCount: 1,
    fields: [],
    command: "INSERT",
    lastId: BigInt(Array.isArray(params) ? params[0] : 0),
  });

  const batch = await client.queryBatch("insert into t(id) values (?)", [[11], [22], [33]]);
  assert.equal(batch.items.length, 3);
  assert.equal(batch.totalRowCount, 3);
  assert.deepEqual(
    batch.items.map((item) => item.lastId),
    [11n, 22n, 33n],
  );
});

test("queryBatch and executeBatch reject empty batch parameters", async () => {
  const client = new Client({ user: "me", database: "db" });
  client.connected = true;
  client.prepared.set("stmt", { sql: "select ?::integer", paramCount: 1 });

  await assert.rejects(
    () => client.queryBatch("select ?::integer", []),
    (err) => err instanceof ScratchbirdSyntaxError && err.code === "07001",
  );
  await assert.rejects(
    () => client.executeBatch("stmt", []),
    (err) => err instanceof ScratchbirdSyntaxError && err.code === "07001",
  );
});

test("executeWithGeneratedKeys returns non-zero generated keys", async () => {
  const client = new Client({ user: "me", database: "db" });
  client.connected = true;
  client.executeQueryMulti = async () => [
    { rows: [], rowCount: 1, fields: [], command: "INSERT", lastId: 0n },
    { rows: [], rowCount: 1, fields: [], command: "INSERT", lastId: 101n },
    { rows: [], rowCount: 1, fields: [], command: "INSERT", lastId: null },
    { rows: [], rowCount: 1, fields: [], command: "INSERT", lastId: 202n },
  ];

  const keys = await client.executeWithGeneratedKeys("insert into t values (1); insert into t values (2)");
  assert.deepEqual(keys, [101n, 202n]);
});

test("call rewrites escape syntax and delegates through executeQuery", async () => {
  const client = new Client({ user: "me", database: "db" });
  client.connected = true;
  let observedSql = null;
  let observedParams = null;
  client.executeQuery = async (sql, params) => {
    observedSql = sql;
    observedParams = params;
    return { rows: [{ return_value: 3 }], rowCount: 1, fields: [], command: "SELECT", lastId: null };
  };

  const result = await client.call("{ ? = call math.abs(?) }", [-3]);
  assert.equal(observedSql, "select math.abs($1) as return_value");
  assert.deepEqual(observedParams, [-3]);
  assert.equal(result.rows[0].return_value, 3);
});

function findTreeNode(nodes, path) {
  for (const node of nodes) {
    if (node.path === path) {
      return node;
    }
  }
  return null;
}

test("metadata query resolver supports collection aliases", () => {
  assert.equal(resolveMetadataCollectionQuery(), METADATA_TABLES_QUERY);
  assert.equal(resolveMetadataCollectionQuery("schemas"), METADATA_SCHEMAS_QUERY);
  assert.equal(resolveMetadataCollectionQuery("indexcolumns"), METADATA_INDEX_COLUMNS_QUERY);
  assert.equal(resolveMetadataCollectionQuery("constraints"), METADATA_CONSTRAINTS_QUERY);
  assert.equal(resolveMetadataCollectionQuery("pk"), METADATA_PRIMARY_KEYS_QUERY);
  assert.equal(resolveMetadataCollectionQuery("foreign_keys"), METADATA_FOREIGN_KEYS_QUERY);
  assert.equal(resolveMetadataCollectionQuery("procedures"), METADATA_PROCEDURES_QUERY);
  assert.equal(resolveMetadataCollectionQuery("table_privileges"), METADATA_TABLE_PRIVILEGES_QUERY);
  assert.equal(resolveMetadataCollectionQuery("routines"), METADATA_ROUTINES_QUERY);
  assert.equal(resolveMetadataCollectionQuery("types"), METADATA_TYPE_INFO_QUERY);
  assert.throws(() => resolveMetadataCollectionQuery("no_such_metadata"), /not supported/);
});

test("metadata family filter narrows view-backed collections client-side", () => {
  assert.deepEqual(
    filterMetadataRowsForCollectionFamily(
      [
        { CONSTRAINT_NAME: "pk_events", CONSTRAINT_TYPE: "PRIMARY KEY" },
        { CONSTRAINT_NAME: "fk_events_users", CONSTRAINT_TYPE: "FOREIGN KEY" },
      ],
      "primary_keys",
    ).map((row) => row.CONSTRAINT_NAME),
    ["pk_events"],
  );
  assert.deepEqual(
    filterMetadataRowsForCollectionFamily(
      [
        { ROUTINE_NAME: "upsert_event", ROUTINE_TYPE: "PROCEDURE" },
        { ROUTINE_NAME: "event_count", ROUTINE_TYPE: "FUNCTION" },
      ],
      "functions",
    ).map((row) => row.ROUTINE_NAME),
    ["event_count"],
  );
});

test("getSchema routes metadata collections and rejects unsupported collections", async () => {
  const client = new Client({ user: "me", database: "db", metadataExpandSchemaParents: false });
  client.connected = true;
  const issuedSql = [];
  client.query = async (sql) => {
    issuedSql.push(sql);
    return { rows: [], rowCount: 0, fields: [], command: "SELECT", lastId: null };
  };

  await client.getSchema("index_columns");
  await client.getSchema("foreign_keys");
  await client.getSchema("table_privileges");
  await client.getSchema("routines");
  await assert.rejects(() => client.getSchema("privileges_not_supported"), (err) => err && err.code === "0A000");

  assert.deepEqual(issuedSql, [METADATA_INDEX_COLUMNS_QUERY, METADATA_FOREIGN_KEYS_QUERY, METADATA_TABLE_PRIVILEGES_QUERY, METADATA_ROUTINES_QUERY]);
});

test("metadata convenience wrappers forward collection-specific restrictions", async () => {
  const client = new Client({ user: "me", database: "db" });
  client.connected = true;
  const captured = [];
  client.getSchema = async (collection, restrictions) => {
    captured.push({ collection, restrictions });
    return { rows: [], rowCount: 0, fields: [], command: "SELECT", lastId: null };
  };

  await client.procedures("main", "users", "upsert_event");
  await client.functions("main", "users", "event_count");
  await client.routines("main", "users", "event_count");
  await client.tablePrivileges("main", "users", "events");
  await client.columnPrivileges("main", "users", "events", "event_id");
  await client.typeInfo("INTEGER");

  assert.deepEqual(captured, [
    { collection: "procedures", restrictions: { catalog: "main", schema: "users", procedure: "upsert_event" } },
    { collection: "functions", restrictions: { catalog: "main", schema: "users", function: "event_count" } },
    { collection: "routines", restrictions: { catalog: "main", schema: "users", routine: "event_count" } },
    { collection: "table_privileges", restrictions: { catalog: "main", schema: "users", table: "events" } },
    { collection: "column_privileges", restrictions: { catalog: "main", schema: "users", table: "events", column: "event_id" } },
    { collection: "type_info", restrictions: { type: "INTEGER" } },
  ]);
});

test("queryMetadata shapes JDBC-style aliases and applies catalog/schema restrictions", async () => {
  const client = new Client({ user: "me", database: "main_catalog" });
  client.connected = true;
  client.query = async () => ({
    rows: [
      { table_id: 1, schema_name: "sys", table_name: "sessions", table_type: "SYSTEM VIEW", owner_id: 10 },
      { table_id: 2, schema_name: "users", table_name: "events", table_type: "TABLE", owner_id: 10 },
    ],
    rowCount: 2,
    fields: [],
    command: "SELECT",
    lastId: null,
  });

  const rows = await client.queryMetadata("tables", { catalog: "main_catalog", schema: "users" });
  assert.equal(rows.rowCount, 1);
  assert.equal(rows.rows[0].TABLE_CAT, "main_catalog");
  assert.equal(rows.rows[0].TABLE_SCHEM, "users");
  assert.equal(rows.rows[0].TABLE_NAME, "events");
  assert.equal(rows.rows[0].TABLE_TYPE, "TABLE");
});

test("queryMetadata applies client-side family filters for view-backed metadata", async () => {
  const client = new Client({ user: "me", database: "main" });
  client.connected = true;
  client.query = async (sql) => ({
    rows: sql === METADATA_CONSTRAINTS_QUERY
      ? [
          { CONSTRAINT_NAME: "pk_events", CONSTRAINT_TYPE: "PRIMARY KEY", TABLE_SCHEMA: "users", TABLE_NAME: "events" },
          { CONSTRAINT_NAME: "fk_events_users", CONSTRAINT_TYPE: "FOREIGN KEY", TABLE_SCHEMA: "users", TABLE_NAME: "events" },
        ]
      : [
          { ROUTINE_NAME: "upsert_event", ROUTINE_TYPE: "PROCEDURE", ROUTINE_SCHEMA: "users" },
          { ROUTINE_NAME: "event_count", ROUTINE_TYPE: "FUNCTION", ROUTINE_SCHEMA: "users" },
        ],
    rowCount: 2,
    fields: [],
    command: "SELECT",
    lastId: null,
  });

  const primaryKeys = await client.queryMetadata("primary_keys");
  const functions = await client.queryMetadata("functions");

  assert.equal(primaryKeys.rows.length, 1);
  assert.equal(primaryKeys.rows[0].CONSTRAINT_NAME, "pk_events");
  assert.equal(functions.rows.length, 1);
  assert.equal(functions.rows[0].ROUTINE_NAME, "event_count");
});

test("shapeMetadataRowsForCollection enriches rows for DDL/editor compatibility fields", () => {
  const columns = shapeMetadataRowsForCollection(
    [
      {
        schema_name: "users",
        table_name: "events",
        column_name: "event_id",
        data_type_id: 23,
        data_type_name: "int4",
        ordinal_position: 1,
        is_nullable: false,
        default_value: null,
      },
    ],
    "columns",
    { database: "main_catalog" },
  );
  assert.equal(columns[0].TABLE_CAT, "main_catalog");
  assert.equal(columns[0].TABLE_SCHEM, "users");
  assert.equal(columns[0].TABLE_NAME, "events");
  assert.equal(columns[0].COLUMN_NAME, "event_id");
  assert.equal(columns[0].DATA_TYPE, 23);
  assert.equal(columns[0].TYPE_NAME, "int4");
  assert.equal(columns[0].ORDINAL_POSITION, 1);
  assert.equal(columns[0].IS_NULLABLE, "NO");
});

test("filterMetadataRowsByRestrictions supports aliases, null matching, and unknown-key ignore", () => {
  const rows = [
    { schema_name: "sys", table_name: "events", owner_id: null },
    { schema_name: "users", table_name: "events", owner_id: null },
    { schema_name: "users", table_name: "profiles", owner_id: 7 },
  ];

  let filtered = filterMetadataRowsByRestrictions(rows, { schema: "users", table: "events" }, "tables");
  assert.deepEqual(filtered, [{ schema_name: "users", table_name: "events", owner_id: null }]);

  filtered = filterMetadataRowsByRestrictions(rows, { owner_id: "null", missing_filter: "ignored" }, "tables");
  assert.equal(filtered.length, 2);
  assert.equal(filtered[0].schema_name, "sys");
  assert.equal(filtered[1].schema_name, "users");
});

test("getSchema returns synthetic catalogs without issuing SQL", async () => {
  const client = new Client({ user: "me", database: "main" });
  client.connected = true;
  let queryInvoked = false;
  client.query = async () => {
    queryInvoked = true;
    return { rows: [], rowCount: 0, fields: [], command: "SELECT", lastId: null };
  };

  const catalogs = await client.getSchema("catalogs");
  assert.equal(queryInvoked, false);
  assert.equal(catalogs.rowCount, 1);
  assert.equal(catalogs.rows[0].catalog_name, "main");
  assert.equal(catalogs.rows[0].TABLE_CAT, "main");
  assert.equal(catalogs.rows[0].table_catalog, "main");
});

test("getSchema applies restrictions before schema parent expansion", async () => {
  const client = new Client({ user: "me", database: "db", metadataExpandSchemaParents: true });
  client.connected = true;
  client.query = async () => ({
    rows: [
      { schema_name: "users.alice.dev" },
      { schema_name: "sys.admin" },
    ],
    rowCount: 2,
    fields: [],
    command: "SELECT",
    lastId: null,
  });

  const schemas = await client.getSchema("schemas", { schema_name: "users.alice.dev" });
  assert.deepEqual(
    schemas.rows.map((row) => row.schema_name),
    ["users", "users.alice", "users.alice.dev"],
  );
});

test("getSchema expands schema parents when metadataExpandSchemaParents is enabled", async () => {
  const client = new Client({ user: "me", database: "db", metadataExpandSchemaParents: true });
  client.connected = true;
  client.query = async () => ({
    rows: [
      { schema_id: 1, schema_name: "users.alice.dev", owner_id: 7, default_tablespace_id: 3 },
      { schema_id: 2, schema_name: "sys", owner_id: 7, default_tablespace_id: 3 },
      { schema_id: 3, schema_name: "users.bob.dev", owner_id: 7, default_tablespace_id: 3 },
      { schema_id: 4, schema_name: "users.bob.dev", owner_id: 7, default_tablespace_id: 3 },
    ],
    rowCount: 4,
    fields: [],
    command: "SELECT",
    lastId: null,
  });

  const schemas = await client.getSchema("schemas");

  assert.equal(schemas.rowCount, 6);
  assert.deepEqual(
    schemas.rows.map((row) => row.schema_name),
    ["users", "users.alice", "users.alice.dev", "sys", "users.bob", "users.bob.dev"],
  );
  assert.equal(schemas.rows[0].schema_id, null);
  assert.equal(schemas.rows[2].schema_id, 1);
});

test("buildMetadataSchemaTree preserves recursive ancestry and per-parent uniqueness", () => {
  const tree = buildMetadataSchemaTree(
    [
      { schema_name: "users.alice.dev" },
      { schema_name: "users.alice.prod" },
      { schema_name: "users.bob.dev" },
      { schema_name: "users.bob.dev" },
      { schema_name: "analytics.dev" },
      { schema_name: "analytics.prod" },
    ],
    { database: "main" },
  );

  assert.equal(tree.database, "main");
  assert.deepEqual(
    tree.schemas.map((node) => node.path),
    ["users", "analytics"],
  );

  const users = findTreeNode(tree.schemas, "users");
  assert.ok(users);
  assert.equal(users.terminal, false);

  const alice = findTreeNode(users.children, "users.alice");
  const bob = findTreeNode(users.children, "users.bob");
  assert.ok(alice);
  assert.ok(bob);
  assert.deepEqual(
    alice.children.map((node) => node.path),
    ["users.alice.dev", "users.alice.prod"],
  );
  assert.deepEqual(
    bob.children.map((node) => node.path),
    ["users.bob.dev"],
  );

  const aliceDev = findTreeNode(alice.children, "users.alice.dev");
  const bobDev = findTreeNode(bob.children, "users.bob.dev");
  assert.ok(aliceDev);
  assert.ok(bobDev);
  assert.equal(aliceDev.terminal, true);
  assert.equal(bobDev.terminal, true);
});

test("getSchemaTree builds metadata-only tree from schema rows", async () => {
  const client = new Client({ user: "me", database: "db_main" });
  client.connected = true;
  client.getSchema = async () => ({
    rows: [{ schema_name: "sys" }, { schema_name: "users.alice.dev" }, { schema_name: "users.bob.dev" }],
    rowCount: 3,
    fields: [],
    command: "SELECT",
    lastId: null,
  });

  const tree = await client.getSchemaTree();
  assert.equal(tree.database, "db_main");
  assert.deepEqual(
    tree.schemas.map((node) => node.path),
    ["sys", "users"],
  );

  const users = findTreeNode(tree.schemas, "users");
  assert.ok(users);
  assert.deepEqual(
    users.children.map((node) => node.path),
    ["users.alice", "users.bob"],
  );
});

test("mapSqlState resolves typed driver errors for known classes", () => {
  assert.equal(mapSqlState("42P01"), ScratchbirdSyntaxError);
  assert.equal(mapSqlState("08006"), ScratchbirdConnectionError);
  assert.equal(mapSqlState("08ZZZ"), ScratchbirdConnectionError);
  assert.equal(mapSqlState("22ZZZ"), ScratchbirdDataError);
  assert.equal(mapSqlState("99999"), ScratchbirdError);
  assert.equal(mapSqlState(undefined), ScratchbirdError);
});

test("pool query returns leased clients and releases them to idle queue", async () => {
  const originalConnect = Client.prototype.connect;
  const originalQuery = Client.prototype.query;
  const originalEnd = Client.prototype.end;
  try {
    let connects = 0;
    let ends = 0;
    Client.prototype.connect = async function mockConnect() {
      this.connected = true;
      connects++;
    };
    Client.prototype.query = async function mockQuery() {
      return { rows: [{ ok: 1 }], rowCount: 1, fields: [], command: "SELECT", lastId: null };
    };
    Client.prototype.end = async function mockEnd() {
      this.connected = false;
      ends++;
    };

    const pool = new Pool({ user: "me", database: "db", max: 1, idleTimeoutMs: 5 });
    const first = await pool.query("select 1");
    const second = await pool.query("select 1");
    assert.equal(first.rows[0].ok, 1);
    assert.equal(second.rows[0].ok, 1);
    assert.equal(connects, 1, "expected pooled client reuse");

    await new Promise((resolve) => setTimeout(resolve, 10));
    await pool.query("select 1");
    await pool.end();
    assert.ok(ends >= 1);
  } finally {
    Client.prototype.connect = originalConnect;
    Client.prototype.query = originalQuery;
    Client.prototype.end = originalEnd;
  }
});

test("circuit breaker transitions through open and half-open gates", () => {
  const originalNow = Date.now;
  let now = 1_000;
  Date.now = () => now;
  try {
    const breaker = new CircuitBreaker({
      failureThreshold: 2,
      recoveryTimeoutMs: 50,
      successThreshold: 2,
      halfOpenMaxRequests: 2,
    }, "node-unit");

    assert.equal(breaker.getState(), "CLOSED");
    breaker.recordFailure();
    assert.equal(breaker.getState(), "CLOSED");
    breaker.recordFailure();
    assert.equal(breaker.getState(), "OPEN");
    assert.equal(breaker.allowRequest(), false);

    now += 75;
    assert.equal(breaker.allowRequest(), true);
    assert.equal(breaker.getState(), "HALF_OPEN");
    assert.equal(breaker.allowRequest(), true);
    assert.equal(breaker.allowRequest(), false, "half-open request cap should be enforced");

    breaker.recordSuccess();
    assert.equal(breaker.getState(), "HALF_OPEN");
    breaker.recordSuccess();
    assert.equal(breaker.getState(), "CLOSED");
  } finally {
    Date.now = originalNow;
  }
});

test("keepalive manager validates idle trackers and keeps unhealthy trackers stale", async () => {
  const originalNow = Date.now;
  let now = 0;
  Date.now = () => now;
  try {
    const manager = new KeepaliveManager({
      intervalMs: 1_000,
      maxIdleBeforeCheckMs: 10,
      validationTimeoutMs: 10,
    });
    let healthyPings = 0;
    let unhealthyPings = 0;

    const healthy = manager.register("healthy", async () => {
      healthyPings += 1;
      return true;
    });
    const unhealthy = manager.register("unhealthy", async () => {
      unhealthyPings += 1;
      return false;
    });

    now = 11;
    await manager.checkConnections();
    assert.equal(healthyPings, 1);
    assert.equal(unhealthyPings, 1);
    assert.equal(healthy.needsValidation(), false, "healthy tracker should be refreshed");
    assert.equal(unhealthy.needsValidation(), true, "unhealthy tracker should remain stale");

    manager.unregister("healthy");
    now = 25;
    await manager.checkConnections();
    assert.equal(healthyPings, 1, "unregistered tracker should not be pinged");
    assert.equal(unhealthyPings, 2);
  } finally {
    Date.now = originalNow;
  }
});

test("leak detector reports potential leaks and guard release is idempotent", () => {
  const originalNow = Date.now;
  let now = 5_000;
  Date.now = () => now;
  try {
    const detector = new LeakDetector({ thresholdMs: 20, captureStackTrace: true, checkIntervalMs: 100 });
    const guard = detector.checkout("conn-1", { lane: "node" });

    assert.equal(detector.activeCount(), 1);
    assert.equal(detector.stats().potentialLeaks, 0);

    const checkoutInfo = detector.checkouts.get("conn-1");
    assert.ok(checkoutInfo?.stackTrace, "stack trace should be captured when enabled");

    now += 25;
    assert.equal(detector.stats().potentialLeaks, 1);

    guard.release();
    guard.release();
    assert.equal(detector.activeCount(), 0);
  } finally {
    Date.now = originalNow;
  }
});

test("telemetry collector tracks metrics and slow query attributes", () => {
  const originalNow = Date.now;
  let now = 10_000;
  Date.now = () => now;
  try {
    const collector = new TelemetryCollector({ slowQueryThresholdMs: 50 });
    const span = collector.startSpan("query");
    assert.ok(span);

    span.withAttribute(
      "db.statement",
      TelemetryCollector.sanitizeQuery("select * from t where api_key = 'secret'"),
    );

    now += 60;
    collector.endSpan(span, false);

    const metrics = collector.metrics();
    assert.equal(metrics.totalQueries, 1);
    assert.equal(metrics.failedQueries, 1);
    assert.equal(metrics.successfulQueries, 0);
    assert.equal(metrics.operationMetrics.query.count, 1);
    assert.equal(metrics.operationMetrics.query.errorCount, 1);

    const slowQueries = collector.slowQueryLog();
    assert.equal(slowQueries.length, 1);
    assert.equal(slowQueries[0].spanName, "query");
    assert.equal(slowQueries[0].attributes["db.statement"], "select * from t where api_key = '?'");

    const prometheus = collector.exportPrometheusMetrics();
    assert.match(prometheus, /scratchbird_queries_total 1/);
  } finally {
    Date.now = originalNow;
  }
});
