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
import org.jkiss.utils.CommonUtils;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class ScratchBirdSecurityRedactor {

    public static final String REDACTED_TOKEN = "<redacted>";

    private static final Set<String> SENSITIVE_PROPERTY_NAMES = Set.of(
        "password",
        "sslpassword",
        "sslkey",
        "manager_auth_token",
        "auth_token",
        "auth_method_payload",
        "auth_payload_json",
        "auth_payload_b64",
        "workload_identity_token",
        "proxy_principal_assertion",
        "dormant_reattach_token",
        "channel_binding_token",
        "private_key"
    );

    private static final Pattern SENSITIVE_ASSIGNMENT = Pattern.compile(
        "(?i)\\b([A-Za-z0-9_.-]*(?:password|token|secret|credential|assertion|private[_-]?key|sslkey|channel[_-]?binding)[A-Za-z0-9_.-]*)\\s*([:=])\\s*([^\\s,;]+)"
    );
    private static final Pattern SENSITIVE_JSON_FIELD = Pattern.compile(
        "(?i)(\"[^\"]*(?:password|token|secret|credential|assertion|private[_-]?key|sslkey|channel[_-]?binding)[^\"]*\"\\s*:\\s*\")([^\"]*)(\")"
    );
    private static final Pattern AUTHORIZATION_HEADER = Pattern.compile(
        "(?i)\\b(Authorization\\s*:\\s*(?:Bearer|Basic)\\s+)([^\\s,;]+)"
    );

    private ScratchBirdSecurityRedactor() {
    }

    public static boolean isSensitiveProperty(@Nullable String propertyName) {
        if (CommonUtils.isEmpty(propertyName)) {
            return false;
        }
        String normalized = normalizePropertyName(propertyName);
        if (SENSITIVE_PROPERTY_NAMES.contains(normalized)) {
            return true;
        }
        return normalized.endsWith("_token") ||
            normalized.endsWith("_assertion") ||
            normalized.endsWith("_credential") ||
            normalized.endsWith("_secret") ||
            normalized.contains("password") ||
            normalized.contains("private_key");
    }

    @NotNull
    public static String redactPropertyValue(@NotNull String propertyName, @Nullable Object value) {
        if (value == null) {
            return "";
        }
        if (isSensitiveProperty(propertyName)) {
            return REDACTED_TOKEN;
        }
        return redactEvidenceText(String.valueOf(value));
    }

    @Nullable
    public static String sanitizeDefaultValue(@Nullable String propertyName, @Nullable String value) {
        if (value == null) {
            return null;
        }
        if (isSensitiveProperty(propertyName)) {
            return null;
        }
        return redactEvidenceText(value);
    }

    @Nullable
    public static String sanitizeDescription(@Nullable String propertyName, @Nullable String description) {
        if (!isSensitiveProperty(propertyName)) {
            return description == null ? null : redactEvidenceText(description);
        }
        String prefix = CommonUtils.isEmpty(description) ? "" : redactEvidenceText(description) + " ";
        return prefix + "ScratchBird treats this value as secret evidence; logs, history, packets, and support bundles must render it as " + REDACTED_TOKEN + ".";
    }

    @NotNull
    public static Map<String, String> redactProperties(@NotNull Map<String, ?> properties) {
        Map<String, String> redacted = new LinkedHashMap<>();
        for (Map.Entry<String, ?> entry : properties.entrySet()) {
            redacted.put(entry.getKey(), redactPropertyValue(entry.getKey(), entry.getValue()));
        }
        return redacted;
    }

    @NotNull
    public static String redactEvidenceText(@Nullable String value) {
        if (value == null || value.isBlank()) {
            return "";
        }
        String redacted = replaceSensitiveAssignments(value);
        redacted = replacePattern(redacted, SENSITIVE_JSON_FIELD, "$1" + REDACTED_TOKEN + "$3");
        redacted = replacePattern(redacted, AUTHORIZATION_HEADER, "$1" + REDACTED_TOKEN);
        return redacted;
    }

    @NotNull
    public static String hashForAudit(@Nullable String value) {
        if (value == null) {
            return "sha256:null";
        }
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] hash = digest.digest(value.getBytes(StandardCharsets.UTF_8));
            StringBuilder builder = new StringBuilder("sha256:");
            for (int i = 0; i < Math.min(12, hash.length); i++) {
                builder.append(String.format(Locale.ROOT, "%02x", hash[i] & 0xff));
            }
            return builder.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 unavailable", e);
        }
    }

    @NotNull
    private static String replaceSensitiveAssignments(@NotNull String value) {
        Matcher matcher = SENSITIVE_ASSIGNMENT.matcher(value);
        StringBuilder builder = new StringBuilder();
        while (matcher.find()) {
            String key = matcher.group(1);
            String separator = matcher.group(2);
            matcher.appendReplacement(builder, Matcher.quoteReplacement(key + separator + REDACTED_TOKEN));
        }
        matcher.appendTail(builder);
        return builder.toString();
    }

    @NotNull
    private static String replacePattern(
        @NotNull String value,
        @NotNull Pattern pattern,
        @NotNull String replacement
    ) {
        return pattern.matcher(value).replaceAll(replacement);
    }

    @NotNull
    private static String normalizePropertyName(@NotNull String propertyName) {
        return propertyName.trim()
            .replace('-', '_')
            .toLowerCase(Locale.ROOT);
    }
}
