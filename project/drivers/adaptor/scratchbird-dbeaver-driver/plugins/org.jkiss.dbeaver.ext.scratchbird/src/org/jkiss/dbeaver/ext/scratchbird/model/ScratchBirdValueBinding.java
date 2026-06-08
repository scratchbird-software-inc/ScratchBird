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

import java.math.BigDecimal;
import java.math.BigInteger;
import java.util.Locale;

public final class ScratchBirdValueBinding {

    private ScratchBirdValueBinding() {
    }

    @NotNull
    public static String toSqlLiteral(@Nullable Object value, @NotNull String typeName) {
        if (value == null) {
            return "NULL";
        }
        ScratchBirdValueProfile profile = ScratchBirdValueProfile.fromTypeName(typeName);
        if (profile.family() == ScratchBirdValueProfile.Family.BOOLEAN && value instanceof Boolean booleanValue) {
            return booleanValue ? "TRUE" : "FALSE";
        }
        if (profile.family() == ScratchBirdValueProfile.Family.NUMERIC && isNumericValue(value)) {
            return String.valueOf(value);
        }
        String textValue = canonicalTextValue(value, profile);
        if (profile.explicitTextRoundTrip() && profile.family() != ScratchBirdValueProfile.Family.TEXT) {
            return "CAST('" + escapeSingleQuotes(textValue) + "' AS " + profile.declaredTypeName() + ")";
        }
        return "'" + escapeSingleQuotes(textValue) + "'";
    }

    @NotNull
    public static String exampleLiteralForType(@NotNull String typeName) {
        ScratchBirdValueProfile profile = ScratchBirdValueProfile.fromTypeName(typeName);
        return switch (profile.family()) {
            case BOOLEAN -> "TRUE";
            case NUMERIC -> "42";
            case UUID -> "CAST('123e4567-e89b-12d3-a456-426614174000' AS " + profile.declaredTypeName() + ")";
            case JSON -> "CAST('{\"sample\":true}' AS " + profile.declaredTypeName() + ")";
            case XML -> "CAST('<sample />' AS " + profile.declaredTypeName() + ")";
            case VECTOR -> "CAST('[1, 2, 3]' AS " + profile.declaredTypeName() + ")";
            case GEOMETRY -> "CAST('POINT(1 2)' AS " + profile.declaredTypeName() + ")";
            case RANGE -> "CAST('[1,10)' AS " + profile.declaredTypeName() + ")";
            case NETWORK -> "CAST('192.0.2.0/24' AS " + profile.declaredTypeName() + ")";
            case FULLTEXT -> "CAST('alpha beta' AS " + profile.declaredTypeName() + ")";
            case INTERVAL -> "CAST('15 minutes' AS " + profile.declaredTypeName() + ")";
            case MONEY -> "CAST('19.99' AS " + profile.declaredTypeName() + ")";
            case BINARY -> "CAST('0xdeadbeef' AS " + profile.declaredTypeName() + ")";
            case COMPOSITE -> "CAST('(sample)' AS " + profile.declaredTypeName() + ")";
            case ENUM_SET -> "CAST('sample' AS " + profile.declaredTypeName() + ")";
            case TEMPORAL -> "CAST('2026-04-23T12:00:00Z' AS " + profile.declaredTypeName() + ")";
            case TEXT, OTHER -> "'sample'";
        };
    }

    @NotNull
    public static String canonicalTextHint(@NotNull String typeName) {
        return ScratchBirdValueProfile.fromTypeName(typeName).canonicalTextForm();
    }

    @NotNull
    private static String canonicalTextValue(@Nullable Object value, @NotNull ScratchBirdValueProfile profile) {
        if (value == null) {
            return "NULL";
        }
        if (value instanceof byte[] bytes) {
            return bytesToHex(bytes);
        }
        if (value instanceof Boolean booleanValue) {
            return booleanValue ? "TRUE" : "FALSE";
        }
        if (isNumericValue(value)) {
            return String.valueOf(value);
        }
        String textValue = String.valueOf(value);
        if (profile.family() == ScratchBirdValueProfile.Family.BINARY && !textValue.startsWith("0x") && !textValue.startsWith("0X")) {
            return "0x" + textValue.toLowerCase(Locale.ROOT);
        }
        return textValue;
    }

    private static boolean isNumericValue(@Nullable Object value) {
        return value instanceof Byte || value instanceof Short || value instanceof Integer || value instanceof Long ||
            value instanceof Float || value instanceof Double || value instanceof BigInteger || value instanceof BigDecimal;
    }

    @NotNull
    private static String bytesToHex(byte[] bytes) {
        StringBuilder builder = new StringBuilder("0x");
        for (byte value : bytes) {
            builder.append(Character.forDigit((value >>> 4) & 0xF, 16));
            builder.append(Character.forDigit(value & 0xF, 16));
        }
        return builder.toString();
    }

    @NotNull
    private static String escapeSingleQuotes(@NotNull String value) {
        return value.replace("'", "''");
    }
}
