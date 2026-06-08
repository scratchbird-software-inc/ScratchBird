// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

"use strict";

const { mapScratchBirdTypeToPrisma } = require("./type-map");

function _toArray(value) {
  return Array.isArray(value) ? value : [];
}

function _safeIdentifier(name) {
  const raw = String(name || "field").trim();
  const replaced = raw.replace(/[^A-Za-z0-9_]/g, "_");
  if (!replaced) {
    return "field";
  }
  if (/^[0-9]/.test(replaced)) {
    return `f_${replaced}`;
  }
  return replaced;
}

function _toPascalCase(name) {
  const ident = _safeIdentifier(name);
  return ident
    .split(/[_\s]+/)
    .filter(Boolean)
    .map((token) => token[0].toUpperCase() + token.slice(1))
    .join("") || "Model";
}

function _renderField(column) {
  const fieldName = _safeIdentifier(column.column_name);
  const mapped = mapScratchBirdTypeToPrisma(column.data_type_name);
  const nullable = column.is_nullable === true || column.is_nullable === 1;
  const isIdentity = column.is_identity === true || column.is_identity === 1;

  let typeDecl = mapped.prismaType;
  if (mapped.isArray) {
    typeDecl += "[]";
  }
  if (nullable) {
    typeDecl += "?";
  }

  const attrs = [];
  if (fieldName === "id" || isIdentity) {
    attrs.push("@id");
  }
  if (isIdentity) {
    attrs.push("@default(autoincrement())");
  }
  if (mapped.nativeType) {
    attrs.push(`@db.${mapped.nativeType}`);
  }

  const comment = mapped.unsupported
    ? " // mapped as String (unsupported native ScratchBird type for Prisma)"
    : "";
  const attrText = attrs.length ? ` ${attrs.join(" ")}` : "";
  return `  ${fieldName} ${typeDecl}${attrText}${comment}`;
}

function generatePrismaSchemaFromMetadata(input) {
  const database = input && input.database ? String(input.database) : "scratchbird";
  const tables = _toArray(input && input.tables);
  const columns = _toArray(input && input.columns);

  const columnsByTable = new Map();
  for (const column of columns) {
    const tableName = String(column.table_name || "");
    if (!columnsByTable.has(tableName)) {
      columnsByTable.set(tableName, []);
    }
    columnsByTable.get(tableName).push(column);
  }

  const models = [];
  for (const table of tables) {
    const tableName = String(table.table_name || "");
    const schemaName = String(table.schema_name || "public");
    const modelName = _toPascalCase(`${schemaName}_${tableName}`);

    const modelLines = [
      `model ${modelName} {`,
    ];

    const tableColumns = (columnsByTable.get(tableName) || [])
      .slice()
      .sort((a, b) => Number(a.ordinal_position || 0) - Number(b.ordinal_position || 0));

    if (tableColumns.length === 0) {
      modelLines.push("  id Int @id");
    } else {
      for (const column of tableColumns) {
        modelLines.push(_renderField(column));
      }
    }

    modelLines.push(`  @@map(\"${tableName}\")`);
    modelLines.push("}");
    models.push(modelLines.join("\n"));
  }

  const header = [
    `// Generated from ScratchBird metadata for database: ${database}`,
    "datasource db {",
    "  provider = \"scratchbird\"",
    "  url      = env(\"DATABASE_URL\")",
    "}",
    "",
    "generator client {",
    "  provider = \"prisma-client-js\"",
    "}",
  ].join("\n");

  if (models.length === 0) {
    return `${header}\n`;
  }
  return `${header}\n\n${models.join("\n\n")}\n`;
}

module.exports = {
  generatePrismaSchemaFromMetadata,
};
