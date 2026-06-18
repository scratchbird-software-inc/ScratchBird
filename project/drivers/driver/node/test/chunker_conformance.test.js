// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// Cross-driver statement-chunker conformance. Runs the Node driver's
// splitTopLevelStatements over the shared oracle fixture and asserts it
// reproduces `expected` exactly for every case. Mirrors the Python reference
// verifier at tests/conformance/drivers/chunker_conformance/verify_python_reference.py.

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const { splitTopLevelStatements } = require("../dist/sql.js");

const CASES_PATH = path.resolve(
  __dirname,
  "../../../../tests/conformance/drivers/chunker_conformance/cases.json",
);

const fixture = JSON.parse(fs.readFileSync(CASES_PATH, "utf8"));

for (const testCase of fixture.cases) {
  test(`chunker conformance: ${testCase.name}`, () => {
    const actual = splitTopLevelStatements(testCase.input);
    assert.deepEqual(actual, testCase.expected);
  });
}
