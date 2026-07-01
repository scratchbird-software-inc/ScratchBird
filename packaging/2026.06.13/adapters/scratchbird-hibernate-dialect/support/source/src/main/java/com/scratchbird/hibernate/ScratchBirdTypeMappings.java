// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.hibernate;

import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

public final class ScratchBirdTypeMappings {
  private static final Map<String, String> TYPE_MAP;

  static {
    LinkedHashMap<String, String> map = new LinkedHashMap<>();
    map.put("BOOLEAN", "boolean");
    map.put("SMALLINT", "short");
    map.put("INTEGER", "integer");
    map.put("INT", "integer");
    map.put("BIGINT", "long");
    map.put("REAL", "float");
    map.put("FLOAT", "float");
    map.put("DOUBLE", "double");
    map.put("DOUBLE PRECISION", "double");
    map.put("NUMERIC", "big_decimal");
    map.put("DECIMAL", "big_decimal");
    map.put("CHAR", "string");
    map.put("VARCHAR", "string");
    map.put("TEXT", "text");
    map.put("DATE", "date");
    map.put("TIME", "time");
    map.put("TIMESTAMP", "timestamp");
    map.put("TIMESTAMPTZ", "timestamp_utc");
    map.put("TIMESTAMP WITH TIME ZONE", "timestamp_utc");
    map.put("UUID", "uuid-char");
    map.put("JSON", "json");
    map.put("JSONB", "json");
    map.put("BYTEA", "binary");
    map.put("BLOB", "materialized_blob");
    map.put("VECTOR", "string");
    map.put("GEOMETRY", "binary");
    TYPE_MAP = Collections.unmodifiableMap(map);
  }

  private ScratchBirdTypeMappings() {}

  public static String normalizeTypeName(String typeName) {
    if (typeName == null) {
      return "";
    }
    String normalized = typeName.trim().toUpperCase(Locale.ROOT).replaceAll("\\s+", " ");
    int paren = normalized.indexOf('(');
    if (paren >= 0) {
      normalized = normalized.substring(0, paren).trim();
    }
    return normalized;
  }

  public static String mapToHibernateType(String scratchBirdType) {
    String normalized = normalizeTypeName(scratchBirdType);
    if (normalized.endsWith("[]")) {
      return "string";
    }
    return TYPE_MAP.getOrDefault(normalized, "string");
  }

  public static boolean isKnownType(String scratchBirdType) {
    return TYPE_MAP.containsKey(normalizeTypeName(scratchBirdType));
  }

  public static Map<String, String> defaultHibernateTypes() {
    return TYPE_MAP;
  }
}
