// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");

const {
  parseScratchbirdConnectionUrl,
  normalizeScratchbirdQueryParams,
  validatePrismaSchemaText,
  mapScratchBirdTypeToPrisma,
  generatePrismaSchemaFromMetadata,
} = require("../lib/index");

test("parseScratchbirdConnectionUrl accepts baseline secure URL", () => {
  const parsed = parseScratchbirdConnectionUrl(
    "scratchbird://alice:secret@db.local:3092/mydb?sslmode=require&binaryTransfer=true",
  );
  assert.equal(parsed.host, "db.local");
  assert.equal(parsed.port, 3092);
  assert.equal(parsed.database, "mydb");
  assert.equal(parsed.username, "alice");
  assert.equal(parsed.params.sslmode, "require");
  assert.equal(parsed.params.binary_transfer, "true");
});

test("parseScratchbirdConnectionUrl rejects insecure or unsupported flags", () => {
  assert.throws(
    () => parseScratchbirdConnectionUrl("scratchbird://db.local/mydb?sslmode=disable"),
    /sslmode=disable/,
  );
  assert.throws(
    () => parseScratchbirdConnectionUrl("scratchbird://db.local/mydb?binaryTransfer=false"),
    /binary_transfer=false/,
  );
  assert.throws(
    () => parseScratchbirdConnectionUrl("scratchbird://db.local/mydb?compression=zstd"),
    /compression=zstd/,
  );
});

test("normalizeScratchbirdQueryParams canonicalizes auth/bootstrap alias families", () => {
  const params = normalizeScratchbirdQueryParams({
    currentSchema: "tenant.analytics",
    searchPath: "ignored.later",
    applicationName: "prisma",
    connectTimeout: "7",
    socketTimeout: "12",
    binaryTransfer: "true",
    frontDoorMode: "manager_proxy",
    managerAuthToken: "manager-token",
    authToken: "opaque-token",
    authMethodId: "scratchbird.auth.jwt_oidc",
    authMethodPayload: "payload",
    authPayloadJson: "{\"sub\":\"alice\"}",
    authPayloadB64: "ZXhhbXBsZQ==",
    authProviderProfile: "entra",
    authRequiredMethods: "TOKEN,SCRAM_SHA_512",
    authForbiddenMethods: "MD5",
    authRequireChannelBinding: "true",
    workloadIdentityToken: "workload-token",
    proxyPrincipalAssertion: "proxy-assertion",
    dormantId: "dormant-1",
    dormantReattachToken: "reattach-token",
    connectClientFlags: "readonly,analytics",
    fetchSize: "500",
  });

  assert.equal(params.search_path, "tenant.analytics");
  assert.equal(params.application_name, "prisma");
  assert.equal(params.connect_timeout, "7");
  assert.equal(params.socket_timeout, "12");
  assert.equal(params.binary_transfer, "true");
  assert.equal(params.front_door_mode, "manager_proxy");
  assert.equal(params.manager_auth_token, "manager-token");
  assert.equal(params.auth_token, "opaque-token");
  assert.equal(params.auth_method_id, "scratchbird.auth.jwt_oidc");
  assert.equal(params.auth_method_payload, "payload");
  assert.equal(params.auth_payload_json, "{\"sub\":\"alice\"}");
  assert.equal(params.auth_payload_b64, "ZXhhbXBsZQ==");
  assert.equal(params.auth_provider_profile, "entra");
  assert.equal(params.auth_required_methods, "TOKEN,SCRAM_SHA_512");
  assert.equal(params.auth_forbidden_methods, "MD5");
  assert.equal(params.auth_require_channel_binding, "true");
  assert.equal(params.workload_identity_token, "workload-token");
  assert.equal(params.proxy_principal_assertion, "proxy-assertion");
  assert.equal(params.dormant_id, "dormant-1");
  assert.equal(params.dormant_reattach_token, "reattach-token");
  assert.equal(params.connect_client_flags, "readonly,analytics");
  assert.equal(params.fetch_size, "500");
});

test("normalizeScratchbirdQueryParams rejects invalid staged auth values", () => {
  assert.throws(
    () => normalizeScratchbirdQueryParams({ frontDoorMode: "sidecar" }),
    /front_door_mode must be direct or manager_proxy/,
  );
  assert.throws(
    () => normalizeScratchbirdQueryParams({ authMethodId: "jwt_oidc" }),
    /auth_method_id must start with scratchbird.auth\./,
  );
});

test("validatePrismaSchemaText enforces datasource + generator + env URL", () => {
  const valid = `
    datasource db {
      provider = "scratchbird"
      url      = env("DATABASE_URL")
    }

    generator client {
      provider = "prisma-client-js"
    }
  `;
  assert.equal(validatePrismaSchemaText(valid), true);

  assert.throws(() => validatePrismaSchemaText("generator client {}"), /datasource/);
  assert.throws(() => validatePrismaSchemaText("datasource db {}"), /generator/);
});

test("mapScratchBirdTypeToPrisma handles core, json, and arrays", () => {
  assert.deepEqual(mapScratchBirdTypeToPrisma("INTEGER"), {
    prismaType: "Int",
    nativeType: undefined,
    unsupported: false,
    isArray: false,
  });
  assert.deepEqual(mapScratchBirdTypeToPrisma("jsonb"), {
    prismaType: "Json",
    nativeType: undefined,
    unsupported: false,
    isArray: false,
  });
  assert.deepEqual(mapScratchBirdTypeToPrisma("varchar[]"), {
    prismaType: "String",
    nativeType: undefined,
    unsupported: false,
    isArray: true,
  });
});

test("generatePrismaSchemaFromMetadata emits deterministic models", () => {
  const schemaText = generatePrismaSchemaFromMetadata({
    database: "main_db",
    tables: [
      { schema_name: "users", table_name: "account" },
    ],
    columns: [
      {
        table_name: "account",
        column_name: "id",
        data_type_name: "INTEGER",
        is_nullable: 0,
        is_identity: 1,
        ordinal_position: 1,
      },
      {
        table_name: "account",
        column_name: "profile",
        data_type_name: "JSONB",
        is_nullable: 1,
        ordinal_position: 2,
      },
    ],
  });

  assert.match(schemaText, /provider = "scratchbird"/);
  assert.match(schemaText, /model UsersAccount/);
  assert.match(schemaText, /id Int @id @default\(autoincrement\(\)\)/);
  assert.match(schemaText, /profile Json\?/);
  assert.match(schemaText, /@@map\("account"\)/);
});
