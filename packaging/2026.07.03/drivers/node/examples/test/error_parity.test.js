// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

const test = require("node:test");
const assert = require("node:assert/strict");

const protocol = require("../dist/protocol.js");
const {
  Client,
  ScratchbirdError,
  ScratchbirdIntegrityError,
  ScratchbirdConnectionError,
  retryScopeForSqlState,
  isRetryableSqlState,
} = require("../dist/index.js");

function errorPayload(fields) {
  const parts = [];
  for (const [tag, value] of Object.entries(fields)) {
    parts.push(Buffer.from(tag, "ascii"));
    parts.push(Buffer.from(value, "utf8"));
    parts.push(Buffer.from([0]));
  }
  parts.push(Buffer.from([0]));
  return Buffer.concat(parts);
}

test("raiseProtocolError maps SQLSTATE class and composes DETAIL/HINT text", () => {
  const client = new Client({ user: "me", database: "db" });
  const payload = errorPayload({
    S: "ERROR",
    C: "23505",
    M: "duplicate key value violates unique constraint",
    D: "Key (id)=(1) already exists.",
    H: "Use a new id.",
  });

  const err = client.raiseProtocolError(payload);
  assert.ok(err instanceof ScratchbirdIntegrityError);
  assert.equal(err.code, "23505");
  assert.equal(err.detail, "Key (id)=(1) already exists.");
  assert.equal(err.hint, "Use a new id.");
  assert.equal(
    err.message,
    "duplicate key value violates unique constraint\nDETAIL: Key (id)=(1) already exists.\nHINT: Use a new id.",
  );
});

test("raiseProtocolError falls back to query failed when message is empty", () => {
  const client = new Client({ user: "me", database: "db" });
  const payload = errorPayload({ S: "ERROR", C: "08006" });

  const err = client.raiseProtocolError(payload);
  assert.ok(err instanceof ScratchbirdConnectionError);
  assert.equal(err.code, "08006");
  assert.equal(err.message, "query failed");
});

test("raiseProtocolError falls back to generic query failed when parser throws", () => {
  const client = new Client({ user: "me", database: "db" });
  const original = protocol.parseErrorMessage;
  protocol.parseErrorMessage = () => {
    throw new Error("bad payload");
  };

  try {
    const err = client.raiseProtocolError(Buffer.from([1, 2, 3]));
    assert.ok(err instanceof ScratchbirdError);
    assert.equal(err.message, "query failed");
    assert.equal(err.code, undefined);
  } finally {
    protocol.parseErrorMessage = original;
  }
});

test("retryScopeForSqlState classifies reconnect and statement-only retry boundaries", () => {
  assert.equal(retryScopeForSqlState("40001"), "statement");
  assert.equal(retryScopeForSqlState("40P01"), "statement");
  assert.equal(retryScopeForSqlState("08006"), "reconnect");
  assert.equal(retryScopeForSqlState("57014"), "none");
  assert.equal(retryScopeForSqlState(undefined), "none");
});

test("isRetryableSqlState only allows fresh-boundary retry scopes", () => {
  assert.equal(isRetryableSqlState("40001"), true);
  assert.equal(isRetryableSqlState("08003"), true);
  assert.equal(isRetryableSqlState("57014"), false);
  assert.equal(isRetryableSqlState(""), false);
});
