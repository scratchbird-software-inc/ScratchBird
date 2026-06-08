// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

"use strict";

const { mapScratchBirdTypeToTypeOrm } = require("./type-map");

function normalizeIdentifier(identifier) {
  return String(identifier || "")
    .trim()
    .replace(/[^a-zA-Z0-9_]/g, "_");
}

function buildEntityName(schemaName, tableName) {
  return `${normalizeIdentifier(schemaName)}_${normalizeIdentifier(tableName)}`;
}

function toTypeOrmColumns(table) {
  const primaryKeySet = new Set(table.primaryKey || []);
  const columns = {};

  for (const column of table.columns || []) {
    const mapped = mapScratchBirdTypeToTypeOrm(column.type);
    const definition = {
      type: mapped.typeormType,
      nullable: column.nullable !== false,
    };

    if (primaryKeySet.has(column.name)) {
      definition.primary = true;
    }
    if (column.identity) {
      definition.generated = "increment";
    }
    if (column.default !== undefined && column.default !== null) {
      definition.default = column.default;
    }
    if (mapped.isArray) {
      definition.array = true;
    }

    columns[column.name] = definition;
  }

  return columns;
}

function toTypeOrmRelations(table, schemaName) {
  const relations = {};

  for (const relation of table.relations || []) {
    const target = buildEntityName(relation.targetSchema || schemaName, relation.targetTable);
    const relationType = relation.type || "many-to-one";
    const inverseSide = relation.inverseSide || undefined;

    relations[relation.name] = {
      type: relationType,
      target,
      inverseSide,
      joinColumn: relation.joinColumn
        ? {
            name: relation.joinColumn,
            referencedColumnName: relation.referencedColumn || "id",
          }
        : undefined,
    };
  }

  return relations;
}

function generateEntitySchemas(metadataCatalog) {
  if (!metadataCatalog || !Array.isArray(metadataCatalog.schemas)) {
    throw new Error("metadataCatalog.schemas is required");
  }

  const entities = [];
  for (const schema of metadataCatalog.schemas) {
    for (const table of schema.tables || []) {
      entities.push({
        name: buildEntityName(schema.name, table.name),
        tableName: table.name,
        schema: schema.name,
        columns: toTypeOrmColumns(table),
        relations: toTypeOrmRelations(table, schema.name),
      });
    }
  }
  return entities;
}

module.exports = {
  buildEntityName,
  generateEntitySchemas,
};
