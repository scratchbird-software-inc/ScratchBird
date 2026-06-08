// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

"use strict";

function safeIdentifier(value, label) {
  if (!value || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(value)) {
    throw new Error(`${label} must be a SQL identifier`);
  }
  return value;
}

function buildNestedCrudTransactionPlan(options) {
  const cfg = options || {};
  const parentTable = safeIdentifier(cfg.parentTable || "parent_table", "parentTable");
  const childTable = safeIdentifier(cfg.childTable || "child_table", "childTable");
  const parentKey = safeIdentifier(cfg.parentKey || "id", "parentKey");
  const childFk = safeIdentifier(cfg.childFk || "parent_id", "childFk");

  return [
    "BEGIN",
    `INSERT INTO ${parentTable} (${parentKey}) VALUES (:parent_id)`,
    "SAVEPOINT after_parent_insert",
    `INSERT INTO ${childTable} (${childFk}, payload) VALUES (:parent_id, :payload)`,
    `UPDATE ${childTable} SET payload = :payload_updated WHERE ${childFk} = :parent_id`,
    `DELETE FROM ${childTable} WHERE ${childFk} = :parent_id AND :delete_child = 1`,
    "ROLLBACK TO SAVEPOINT after_parent_insert",
    "RELEASE SAVEPOINT after_parent_insert",
    "COMMIT",
  ];
}

module.exports = {
  buildNestedCrudTransactionPlan,
};
