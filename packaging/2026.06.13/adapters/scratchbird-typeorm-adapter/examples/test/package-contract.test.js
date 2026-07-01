// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const root = path.resolve(__dirname, "..");
const contract = JSON.parse(fs.readFileSync(path.join(root, "package_contract.json"), "utf8"));

test("package contract records TypeORM adapter release posture", () => {
  assert.equal(contract.component_id, "adaptor:scratchbird-typeorm-adapter");
  assert.equal(contract.status, "beta_2");
  assert.equal(contract.release_scope, "in_scope_required");
  assert.equal(contract.server_revalidation_required, true);
  assert.equal(contract.transaction_authority, "mga_engine");
  assert.equal(contract.delegation_posture.mode, "delegates_to_node");
  assert.equal(contract.delegation_posture.target_component, "driver:node");
  assert.equal(contract.dbeaver_exclusion.this_artifact_includes_dbeaver, false);
  assert.ok(contract.best_in_class_deltas.length >= 2);
  assert.equal(contract.artifact_verification.package_type, "npm_package");
});

test("package contract package_files are present and exclude DBeaver", () => {
  for (const relative of contract.package_files) {
    assert.ok(!relative.includes("dbeaver"), `unexpected DBeaver package file ${relative}`);
    assert.ok(fs.existsSync(path.join(root, relative)), `${relative} should exist`);
  }
});
