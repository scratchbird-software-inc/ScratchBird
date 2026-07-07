// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

"use strict";

const crypto = require("node:crypto");
const { validatePrismaSchemaText } = require("./connection-url");
const { generatePrismaSchemaFromMetadata } = require("./schema-generator");

function _normalizeMigrationName(name) {
  const raw = String(name || "init").trim();
  const replaced = raw.replace(/[^A-Za-z0-9_]/g, "_");
  if (!replaced) {
    return "init";
  }
  return replaced;
}

function _schemaFingerprint(schemaText) {
  return crypto.createHash("sha256").update(schemaText).digest("hex").slice(0, 12);
}

function buildDeterministicMigrationPlan(schemaText, migrationName) {
  validatePrismaSchemaText(schemaText);

  const normalizedName = _normalizeMigrationName(migrationName);
  const fingerprint = _schemaFingerprint(schemaText);
  const name = `${normalizedName}_${fingerprint}`;

  return {
    migrationName: name,
    migrationDirectory: `prisma/migrations/${name}`,
    migrationFile: `prisma/migrations/${name}/migration.sql`,
    checksum: fingerprint,
    sqlTemplate: [
      "-- ScratchBird Prisma migration contract template",
      `-- name: ${name}`,
      "-- Fill in generated SQL statements from Prisma migration engine.",
    ].join("\n"),
  };
}

function runReflectionRoundTripContract(metadataInput) {
  const schemaText = generatePrismaSchemaFromMetadata(metadataInput);
  validatePrismaSchemaText(schemaText);

  const modelCount = (schemaText.match(/^model\s+/gm) || []).length;
  const checksum = _schemaFingerprint(schemaText);

  return {
    schemaText,
    modelCount,
    checksum,
  };
}

module.exports = {
  buildDeterministicMigrationPlan,
  runReflectionRoundTripContract,
};
