// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.hibernate;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.StringJoiner;

public final class ScratchBirdJdbcMetadataMapper {

  public record ColumnMetadata(
      String schema,
      String table,
      String column,
      String typeName,
      boolean nullable,
      boolean identity,
      String defaultValue) {}

  public Map<String, Object> toHibernateColumnDefinition(ColumnMetadata columnMetadata) {
    Objects.requireNonNull(columnMetadata, "columnMetadata is required");

    Map<String, Object> result = new LinkedHashMap<>();
    result.put("schema", columnMetadata.schema());
    result.put("table", columnMetadata.table());
    result.put("column", columnMetadata.column());
    result.put("hibernateType", ScratchBirdTypeMappings.mapToHibernateType(columnMetadata.typeName()));
    result.put("nullable", columnMetadata.nullable());
    result.put("identity", columnMetadata.identity());
    result.put("default", columnMetadata.defaultValue());
    return result;
  }

  public String generatePrimaryKeyConstraint(String tableName, List<String> columns) {
    if (tableName == null || tableName.isBlank()) {
      throw new IllegalArgumentException("tableName is required");
    }
    if (columns == null || columns.isEmpty()) {
      throw new IllegalArgumentException("columns are required");
    }
    return "alter table " + tableName + " add primary key (" + String.join(", ", columns) + ")";
  }

  public String generateForeignKeyConstraint(
      String tableName,
      String constraintName,
      List<String> columns,
      String targetTable,
      List<String> targetColumns) {
    if (columns == null || columns.isEmpty()) {
      throw new IllegalArgumentException("columns are required");
    }
    if (targetColumns == null || targetColumns.isEmpty()) {
      throw new IllegalArgumentException("targetColumns are required");
    }
    if (columns.size() != targetColumns.size()) {
      throw new IllegalArgumentException("columns and targetColumns must have same size");
    }

    StringJoiner source = new StringJoiner(", ");
    StringJoiner target = new StringJoiner(", ");
    columns.forEach(source::add);
    targetColumns.forEach(target::add);

    return "alter table "
        + tableName
        + " add constraint "
        + constraintName
        + " foreign key ("
        + source
        + ") references "
        + targetTable
        + " ("
        + target
        + ")";
  }
}
