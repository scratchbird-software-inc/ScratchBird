// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * DBeaver - Universal Database Manager
 * Copyright (C) 2010-2026 DBeaver Corp and others
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.jkiss.dbeaver.ext.scratchbird.model;

import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.model.struct.DBSTypedObject;
import org.jkiss.dbeaver.utils.MimeTypes;

import java.util.Locale;
import java.util.Set;

public record ScratchBirdValueProfile(
    @NotNull String declaredTypeName,
    @NotNull String typeKey,
    @NotNull Family family,
    @NotNull HandlerRoute handlerRoute,
    boolean explicitTextRoundTrip,
    @Nullable String contentType,
    @NotNull String canonicalTextForm
) {

    private static final Set<String> UUID_TYPES = Set.of("UUID", "UUID_V7");
    private static final Set<String> JSON_TYPES = Set.of("JSON", "JSONB", "JSONPATH", "VARIANT");
    private static final Set<String> XML_TYPES = Set.of("XML", "SQLXML");
    private static final Set<String> VECTOR_TYPES = Set.of("VECTOR");
    private static final Set<String> GEOMETRY_TYPES = Set.of(
        "GEOMETRY",
        "POINT",
        "LINESTRING",
        "POLYGON",
        "MULTIPOINT",
        "MULTILINESTRING",
        "MULTIPOLYGON",
        "GEOMETRYCOLLECTION");
    private static final Set<String> RANGE_TYPES = Set.of(
        "RANGE",
        "RANGE_DATE",
        "RANGE_INT4",
        "RANGE_INT8",
        "RANGE_NUM",
        "RANGE_TS",
        "RANGE_TSTZ");
    private static final Set<String> NETWORK_TYPES = Set.of("INET", "CIDR", "MACADDR", "MACADDR8");
    private static final Set<String> FULLTEXT_TYPES = Set.of("TSVECTOR", "TSQUERY");
    private static final Set<String> BINARY_TYPES = Set.of("BYTEA", "BINARY", "VARBINARY", "BLOB", "LOB_REF", "HASH256");
    private static final Set<String> COMPOSITE_TYPES = Set.of("COMPOSITE", "ROW");
    private static final Set<String> ENUM_TYPES = Set.of("ENUM", "SET");
    private static final Set<String> TEXT_TYPES = Set.of("CHAR", "NCHAR", "VARCHAR", "NVARCHAR", "TEXT", "STRING", "CSTRING");
    private static final Set<String> BOOLEAN_TYPES = Set.of("BOOLEAN", "BOOL");

    public enum Family {
        NUMERIC("numeric"),
        BOOLEAN("boolean"),
        TEXT("text"),
        UUID("uuid"),
        JSON("json/document"),
        XML("xml"),
        VECTOR("vector"),
        GEOMETRY("geometry"),
        RANGE("range"),
        NETWORK("network"),
        FULLTEXT("full-text"),
        INTERVAL("interval"),
        MONEY("money"),
        BINARY("binary/content"),
        COMPOSITE("composite/row"),
        ENUM_SET("enum/set"),
        TEMPORAL("temporal"),
        OTHER("other");

        private final String label;

        Family(@NotNull String label) {
            this.label = label;
        }

        @NotNull
        public String label() {
            return label;
        }
    }

    public enum HandlerRoute {
        STANDARD("standard scalar"),
        UUID("uuid"),
        STRUCTURED_TEXT("structured text"),
        STRUCTURED_CONTENT("structured content"),
        BINARY_CONTENT("binary content");

        private final String label;

        HandlerRoute(@NotNull String label) {
            this.label = label;
        }

        @NotNull
        public String label() {
            return label;
        }
    }

    @NotNull
    public static ScratchBirdValueProfile fromTypedObject(@NotNull DBSTypedObject typedObject) {
        return fromTypeName(typedObject.getTypeName());
    }

    @NotNull
    public static ScratchBirdValueProfile fromTypeName(@Nullable String typeName) {
        String declared = sanitizeDeclaredTypeName(typeName);
        String key = normalizeTypeKey(declared);

        if (UUID_TYPES.contains(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.UUID,
                HandlerRoute.UUID,
                true,
                null,
                "canonical hyphenated UUID text");
        }
        if (JSON_TYPES.contains(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.JSON,
                HandlerRoute.STRUCTURED_CONTENT,
                true,
                MimeTypes.APPLICATION_JSON,
                "canonical reference-compatible document text");
        }
        if (XML_TYPES.contains(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.XML,
                HandlerRoute.STRUCTURED_CONTENT,
                true,
                MimeTypes.TEXT_XML,
                "canonical XML text");
        }
        if (VECTOR_TYPES.contains(key) || (key.startsWith("VECTOR") && !"TSVECTOR".equals(key))) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.VECTOR,
                HandlerRoute.STRUCTURED_TEXT,
                true,
                MimeTypes.TEXT_PLAIN,
                "deterministic bracketed vector text");
        }
        if (GEOMETRY_TYPES.contains(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.GEOMETRY,
                HandlerRoute.STRUCTURED_TEXT,
                true,
                MimeTypes.TEXT_PLAIN,
                "canonical geo wrapper text");
        }
        if (RANGE_TYPES.contains(key) || key.startsWith("RANGE") || key.contains("MULTIRANGE")) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.RANGE,
                HandlerRoute.STRUCTURED_TEXT,
                true,
                MimeTypes.TEXT_PLAIN,
                "canonical reference-compatible range text");
        }
        if (NETWORK_TYPES.contains(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.NETWORK,
                HandlerRoute.STRUCTURED_TEXT,
                true,
                MimeTypes.TEXT_PLAIN,
                "canonical network address text");
        }
        if (FULLTEXT_TYPES.contains(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.FULLTEXT,
                HandlerRoute.STRUCTURED_TEXT,
                true,
                MimeTypes.TEXT_PLAIN,
                "canonical full-text payload text");
        }
        if ("INTERVAL".equals(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.INTERVAL,
                HandlerRoute.STRUCTURED_TEXT,
                true,
                MimeTypes.TEXT_PLAIN,
                "canonical unit-locked interval text");
        }
        if ("MONEY".equals(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.MONEY,
                HandlerRoute.STRUCTURED_TEXT,
                true,
                MimeTypes.TEXT_PLAIN,
                "canonical money text");
        }
        if (BINARY_TYPES.contains(key) || key.endsWith("_BYTES") || key.contains("BLOB")) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.BINARY,
                HandlerRoute.BINARY_CONTENT,
                true,
                MimeTypes.OCTET_STREAM,
                "lower-case hex prefixed with 0x");
        }
        if (COMPOSITE_TYPES.contains(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.COMPOSITE,
                HandlerRoute.STRUCTURED_CONTENT,
                true,
                MimeTypes.TEXT_PLAIN,
                "stable reference-compatible composite text");
        }
        if (ENUM_TYPES.contains(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.ENUM_SET,
                HandlerRoute.STRUCTURED_TEXT,
                true,
                MimeTypes.TEXT_PLAIN,
                "stored label text subject to domain normalization");
        }
        if (BOOLEAN_TYPES.contains(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.BOOLEAN,
                HandlerRoute.STANDARD,
                true,
                null,
                "TRUE or FALSE");
        }
        if (isNumericType(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.NUMERIC,
                HandlerRoute.STANDARD,
                true,
                null,
                "canonical decimal text");
        }
        if (isTemporalType(key)) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.TEMPORAL,
                HandlerRoute.STANDARD,
                true,
                null,
                "ISO-8601 or RFC-3339 compatible text");
        }
        if (TEXT_TYPES.contains(key) || key.endsWith("TEXT") || key.endsWith("STRING")) {
            return new ScratchBirdValueProfile(
                declared,
                key,
                Family.TEXT,
                HandlerRoute.STANDARD,
                false,
                MimeTypes.TEXT_PLAIN,
                "stored text exactly as written");
        }
        return new ScratchBirdValueProfile(
            declared,
            key,
            Family.OTHER,
            HandlerRoute.STANDARD,
            false,
            null,
            "driver-standard display text");
    }

    @NotNull
    public String familyLabel() {
        return family.label();
    }

    @NotNull
    public String handlerRouteLabel() {
        return handlerRoute.label();
    }

    @NotNull
    public String contentTypeOrDefault() {
        return contentType == null ? MimeTypes.TEXT_PLAIN : contentType;
    }

    private static boolean isNumericType(@NotNull String key) {
        return key.contains("INT") || key.contains("DECIMAL") || key.contains("NUMERIC") ||
            key.contains("REAL") || key.contains("DOUBLE") || key.contains("FLOAT") ||
            key.contains("SERIAL") || key.startsWith("NUMBER") || key.equals("BIGNUM") ||
            key.equals("BFLOAT16");
    }

    private static boolean isTemporalType(@NotNull String key) {
        return key.equals("DATE") || key.equals("TIME") || key.startsWith("TIMESTAMP");
    }

    @NotNull
    private static String sanitizeDeclaredTypeName(@Nullable String typeName) {
        return typeName == null || typeName.isBlank() ? "TEXT" : typeName.trim();
    }

    @NotNull
    private static String normalizeTypeKey(@NotNull String declaredTypeName) {
        String normalized = declaredTypeName.toUpperCase(Locale.ROOT).replaceAll("\\s+", " ");
        int paren = normalized.indexOf('(');
        if (paren >= 0) {
            normalized = normalized.substring(0, paren).trim();
        }
        if (normalized.endsWith("[]")) {
            normalized = normalized.substring(0, normalized.length() - 2).trim();
        }
        return normalized;
    }
}
