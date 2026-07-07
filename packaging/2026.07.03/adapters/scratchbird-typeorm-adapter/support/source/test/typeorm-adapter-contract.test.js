// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

"use strict";

const assert = require("node:assert/strict");
const test = require("node:test");
const adapter = require("../lib/index");

test("guardrails reject unsupported options", () => {
  assert.throws(
    () =>
      adapter.normalizeTypeOrmOptions({
        url: "scratchbird://localhost:3092/main?sslmode=disable",
      }),
    /sslmode=disable is not supported/
  );

  assert.throws(
    () =>
      adapter.normalizeTypeOrmOptions({
        url: "scratchbird://localhost:3092/main?binaryTransfer=false",
      }),
    /binaryTransfer=false is not supported/
  );

  assert.throws(
    () =>
      adapter.normalizeTypeOrmOptions({
        url: "scratchbird://localhost:3092/main?compression=zstd",
      }),
    /compression=zstd is not supported/
  );
});

test("options normalize URL fields and extra parameters", () => {
  const options = adapter.normalizeTypeOrmOptions({
    url: "scratchbird://alice:pw@db.internal:3199/maindb?sslmode=require&binaryTransfer=true&currentSchema=tenant.analytics&frontDoorMode=manager_proxy&authMethodId=scratchbird.auth.jwt_oidc&managerAuthToken=manager-token",
    extra: {
      connectTimeout: "30",
      authToken: "opaque-token",
      authRequiredMethods: "TOKEN,SCRAM_SHA_512",
      authForbiddenMethods: "MD5",
      authRequireChannelBinding: "true",
      workloadIdentityToken: "workload-token",
      proxyPrincipalAssertion: "proxy-assertion",
      dormantId: "dormant-1",
      dormantReattachToken: "reattach-token",
      connectClientFlags: "readonly,analytics",
      fetchSize: "500",
    },
  });

  assert.equal(options.type, "scratchbird");
  assert.equal(options.host, "db.internal");
  assert.equal(options.port, 3199);
  assert.equal(options.database, "maindb");
  assert.equal(options.username, "alice");
  assert.equal(options.extra.sslmode, "require");
  assert.equal(options.extra.binary_transfer, "true");
  assert.equal(options.extra.connect_timeout, "30");
  assert.equal(options.extra.search_path, "tenant.analytics");
  assert.equal(options.extra.front_door_mode, "manager_proxy");
  assert.equal(options.extra.auth_method_id, "scratchbird.auth.jwt_oidc");
  assert.equal(options.extra.manager_auth_token, "manager-token");
  assert.equal(options.extra.auth_token, "opaque-token");
  assert.equal(options.extra.auth_required_methods, "TOKEN,SCRAM_SHA_512");
  assert.equal(options.extra.auth_forbidden_methods, "MD5");
  assert.equal(options.extra.auth_require_channel_binding, "true");
  assert.equal(options.extra.workload_identity_token, "workload-token");
  assert.equal(options.extra.proxy_principal_assertion, "proxy-assertion");
  assert.equal(options.extra.dormant_id, "dormant-1");
  assert.equal(options.extra.dormant_reattach_token, "reattach-token");
  assert.equal(options.extra.connect_client_flags, "readonly,analytics");
  assert.equal(options.extra.fetch_size, "500");
});

test("options reject invalid staged auth bootstrap values", () => {
  assert.throws(
    () =>
      adapter.normalizeTypeOrmOptions({
        extra: { frontDoorMode: "sidecar" },
      }),
    /front_door_mode must be direct or manager_proxy/
  );

  assert.throws(
    () =>
      adapter.normalizeTypeOrmOptions({
        extra: { authMethodId: "jwt_oidc" },
      }),
    /auth_method_id must start with scratchbird.auth\./
  );
});

test("type map supports scalar, array, and unknown fallback", () => {
  assert.deepEqual(adapter.mapScratchBirdTypeToTypeOrm("jsonb"), {
    typeormType: "jsonb",
    unsupported: false,
    isArray: false,
  });

  assert.deepEqual(adapter.mapScratchBirdTypeToTypeOrm("varchar[]"), {
    typeormType: "varchar",
    unsupported: false,
    isArray: true,
  });

  assert.deepEqual(adapter.mapScratchBirdTypeToTypeOrm("mystery_type"), {
    typeormType: "varchar",
    unsupported: true,
    isArray: false,
  });
});

test("metadata catalog generates entity schemas with nested relation", () => {
  const schemas = adapter.generateEntitySchemas({
    schemas: [
      {
        name: "sys",
        tables: [
          {
            name: "users",
            columns: [
              { name: "id", type: "bigint", nullable: false, identity: true },
              { name: "name", type: "varchar", nullable: false },
            ],
            primaryKey: ["id"],
          },
          {
            name: "posts",
            columns: [
              { name: "id", type: "bigint", nullable: false, identity: true },
              { name: "user_id", type: "bigint", nullable: false },
            ],
            primaryKey: ["id"],
            relations: [
              {
                name: "user",
                type: "many-to-one",
                targetSchema: "sys",
                targetTable: "users",
                joinColumn: "user_id",
                referencedColumn: "id",
              },
            ],
          },
        ],
      },
    ],
  });

  assert.equal(schemas.length, 2);

  const posts = schemas.find((schema) => schema.name === "sys_posts");
  assert.ok(posts);
  assert.equal(posts.columns.id.generated, "increment");
  assert.equal(posts.columns.id.primary, true);
  assert.equal(posts.relations.user.target, "sys_users");
  assert.equal(posts.relations.user.joinColumn.name, "user_id");
});

test("nested CRUD transaction plan includes savepoint lifecycle", () => {
  const plan = adapter.buildNestedCrudTransactionPlan({
    parentTable: "users",
    childTable: "posts",
    parentKey: "id",
    childFk: "user_id",
  });

  assert.deepEqual(plan.slice(0, 3), [
    "BEGIN",
    "INSERT INTO users (id) VALUES (:parent_id)",
    "SAVEPOINT after_parent_insert",
  ]);
  assert.equal(plan.at(-1), "COMMIT");
  assert.ok(plan.includes("ROLLBACK TO SAVEPOINT after_parent_insert"));
});
