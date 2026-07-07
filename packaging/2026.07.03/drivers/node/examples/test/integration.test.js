// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

const test = require("node:test");
const assert = require("node:assert/strict");
const { Client } = require("../dist/index.js");

async function connectClient(t) {
  const url = process.env.SCRATCHBIRD_NODE_URL;
  if (!url) {
    t.skip("SCRATCHBIRD_NODE_URL not set");
    return null;
  }
  const client = new Client(url);
  await client.connect();
  return client;
}

function isNotSupported(err) {
  if (!err || typeof err !== "object") {
    return false;
  }
  if (err.code === "0A000") {
    return true;
  }
  const message = String(err.message || "").toLowerCase();
  return message.includes("not supported");
}

test("connects and runs query", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    const res = await client.query("SELECT 1 as one");
    assert.equal(res.rows[0].one, 1);
  } finally {
    await client.end();
  }
});

test("prepared bind executes parameters", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    const res = await client.query("SELECT ?::INTEGER as value", [42]);
    assert.equal(res.rows[0].value, 42);
  } finally {
    await client.end();
  }
});

test("types fixture returns row", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    const res = await client.query("SELECT * FROM type_coverage");
    assert.ok(res.rows.length >= 1);
  } finally {
    await client.end();
  }
});

test("cancel query", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  const cancelSql = process.env.SCRATCHBIRD_NODE_CANCEL_SQL;
  if (!cancelSql) {
    t.skip("SCRATCHBIRD_NODE_CANCEL_SQL not set");
    await client.end();
    return;
  }
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 200);
  try {
    await assert.rejects(client.query(cancelSql, [], { signal: controller.signal }));
  } finally {
    clearTimeout(timer);
    await client.end();
  }
});

test("queryMulti returns independent result sets", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    let results;
    try {
      results = await client.queryMulti("SELECT 1 as first_value; SELECT 2 as second_value");
    } catch (err) {
      if (isNotSupported(err)) {
        t.skip(`queryMulti not supported by runtime: ${err.message}`);
        return;
      }
      throw err;
    }
    assert.equal(results.length, 2);
    assert.equal(results[0].rows[0].first_value, 1);
    assert.equal(results[1].rows[0].second_value, 2);
  } finally {
    await client.end();
  }
});

test("executeMulti on prepared statement returns independent result sets", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    await client.prepare("integration_multi_stmt", "SELECT ?::INTEGER as first_value; SELECT ?::INTEGER as second_value");
    let results;
    try {
      results = await client.executeMulti("integration_multi_stmt", [7, 9]);
    } catch (err) {
      if (isNotSupported(err)) {
        t.skip(`executeMulti not supported by runtime: ${err.message}`);
        return;
      }
      throw err;
    }
    assert.equal(results.length, 2);
    assert.equal(results[0].rows[0].first_value, 7);
    assert.equal(results[1].rows[0].second_value, 9);
  } finally {
    await client.end();
  }
});

test("queryBatch and executeBatch return batch summaries", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    const queryBatch = await client.queryBatch("SELECT ?::INTEGER as value", [[11], [22], [33]]);
    assert.equal(queryBatch.items.length, 3);
    assert.equal(queryBatch.totalRowCount, queryBatch.items.reduce((sum, item) => sum + item.rowCount, 0));

    await client.prepare("integration_batch_stmt", "SELECT ?::INTEGER as value");
    const execBatch = await client.executeBatch("integration_batch_stmt", [[101], [202]]);
    assert.equal(execBatch.items.length, 2);
    assert.equal(execBatch.totalRowCount, execBatch.items.reduce((sum, item) => sum + item.rowCount, 0));
  } finally {
    await client.end();
  }
});

test("call executes JDBC callable escape syntax", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    const result = await client.call("{ ? = call abs(?) }", [-3]);
    assert.ok(result.rows.length >= 1);
    const firstRow = result.rows[0];
    const value = firstRow.return_value ?? Object.values(firstRow)[0];
    assert.equal(Number(value), 3);
  } finally {
    await client.end();
  }
});

test("executeWithGeneratedKeys returns key list", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    let keys;
    try {
      keys = await client.executeWithGeneratedKeys("SELECT 1");
    } catch (err) {
      if (isNotSupported(err)) {
        t.skip(`executeWithGeneratedKeys not supported by runtime: ${err.message}`);
        return;
      }
      throw err;
    }
    assert.ok(Array.isArray(keys));
    assert.ok(keys.every((value) => typeof value === "bigint" && value >= 0n));
  } finally {
    await client.end();
  }
});

test("autocommit toggle drives implicit transaction lifecycle", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    assert.equal(client.getAutoCommit(), true);
    await client.setAutoCommit(false);
    assert.equal(client.getAutoCommit(), false);
    await client.query("SELECT 1 as one");
    await client.commitTransaction();
    await client.setAutoCommit(true);
    assert.equal(client.getAutoCommit(), true);
  } finally {
    await client.end();
  }
});

test("savepoint lifecycle works in explicit transaction mode", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    await client.beginTransaction();
    try {
      await client.savepoint("integration_sp");
      await client.query("SELECT 1 as value");
      await client.rollbackToSavepoint("integration_sp");
      await client.releaseSavepoint("integration_sp");
      await client.commitTransaction();
    } catch (err) {
      if (isNotSupported(err)) {
        t.skip(`transaction savepoint flow not supported by runtime: ${err.message}`);
        return;
      }
      throw err;
    }
  } finally {
    await client.end();
  }
});

test("queryStream paginates with maxRows and yields complete stream", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    const sql = "SELECT schema_name FROM sys.schemas ORDER BY schema_name";
    const baseline = await client.query(sql);
    const expectedValues = baseline.rows.map((row) => String(row.schema_name));
    if (expectedValues.length < 2) {
      t.skip("stream paging fixture returned fewer than two rows");
      return;
    }
    const values = [];
    try {
      const stream = await client.queryStream(sql, [], {
        maxRows: 1,
      });
      for await (const row of stream) {
        values.push(String(row.schema_name));
      }
    } catch (err) {
      if (isNotSupported(err)) {
        t.skip(`queryStream paging not supported by runtime: ${err.message}`);
        return;
      }
      throw err;
    }
    assert.deepEqual(values, expectedValues);
  } finally {
    await client.end();
  }
});

test("metadata helpers provide JDBC-compatible alias columns", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    let tables;
    try {
      tables = await client.queryMetadata("tables");
    } catch (err) {
      if (isNotSupported(err)) {
        t.skip(`metadata helpers not supported by runtime: ${err.message}`);
        return;
      }
      throw err;
    }
    if (!Array.isArray(tables.rows) || tables.rows.length === 0) {
      t.skip("metadata tables collection returned no rows");
      return;
    }
    const first = tables.rows[0];
    assert.ok(Object.prototype.hasOwnProperty.call(first, "TABLE_CAT"));
    assert.ok(Object.prototype.hasOwnProperty.call(first, "TABLE_SCHEM"));
    assert.ok(Object.prototype.hasOwnProperty.call(first, "TABLE_NAME"));
  } finally {
    await client.end();
  }
});

test("metadata convenience wrappers execute routine families", async (t) => {
  const client = await connectClient(t);
  if (!client) return;
  try {
    let procedures;
    let functions;
    let routines;
    try {
      procedures = await client.procedures(undefined, "users");
      functions = await client.functions(undefined, "users");
      routines = await client.routines(undefined, "users");
    } catch (err) {
      if (isNotSupported(err)) {
        t.skip(`routine metadata helpers not supported by runtime: ${err.message}`);
        return;
      }
      throw err;
    }
    assert.ok(Array.isArray(procedures.rows));
    assert.ok(Array.isArray(functions.rows));
    assert.ok(Array.isArray(routines.rows));
  } finally {
    await client.end();
  }
});
