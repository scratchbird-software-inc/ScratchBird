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
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Completion;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Parser;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

public final class ScratchBirdSqlPromptPlanner {

    private static final int STATUS_COMPLETION_LIMIT = 3;
    private static final List<String> PREFERRED_PROMPTS = List.of(
        "SCHEMA",
        "MANAGEMENT",
        "CLUSTER",
        "METRICS",
        "JOBS",
        "ROLES",
        "DATABASE",
        "TABLES",
        "VIEWS",
        "DOMAIN",
        "PROCEDURE",
        "FUNCTION",
        "TRIGGER"
    );
    private static final Set<String> DEPRIORITIZED_PROMPTS = Set.of(
        "ALL",
        "CHECK",
        "CHECKS",
        "COMMENT",
        "COMMENTS",
        "CREATE TABLE",
        "CURRENT SCHEMA",
        "CURRENT_SCHEMA",
        "DEPENDENCIES",
        "DEPENDENCY"
    );

    private ScratchBirdSqlPromptPlanner() {
    }

    @NotNull
    public static List<ScratchBirdV3Completion> completionCandidates(@NotNull String sql, int offset) {
        return distinctCompletions(ScratchBirdV3Parser.completionsAt(sql, clamp(offset, 0, sql.length())));
    }

    @NotNull
    public static String proposalInsertion(@NotNull String sql, int offset, @NotNull String label) {
        int safeOffset = clamp(offset, 0, sql.length());
        String prefix = tokenPrefix(sql, safeOffset);
        if (!prefix.isBlank() && prefix.equalsIgnoreCase(label)) {
            return "";
        }
        if (matchesPrefix(prefix, label)) {
            return label.substring(prefix.length());
        }
        return label;
    }

    @NotNull
    public static PromptPlan plan(@NotNull String sql, int offset) {
        int safeOffset = clamp(offset, 0, sql.length());
        if (sql.isBlank()) {
            return new PromptPlan(null, "ScratchBird v3 context: no SQL text at the current cursor.", safeOffset);
        }

        List<ScratchBirdV3Completion> completions = completionCandidates(sql, safeOffset);
        if (completions.isEmpty()) {
            return new PromptPlan(null, "ScratchBird v3 context: no parser completions for the current cursor.", safeOffset);
        }

        String inlineSuggestion = inlineSuggestion(sql, safeOffset, completions);
        String statusMessage = statusMessage(completions, inlineSuggestion != null);
        return new PromptPlan(inlineSuggestion, statusMessage, safeOffset);
    }

    @Nullable
    static String inlineSuggestion(
        @NotNull String sql,
        int offset,
        @NotNull List<ScratchBirdV3Completion> completions
    ) {
        if (!isTokenBoundary(sql, offset)) {
            return null;
        }
        String prefix = tokenPrefix(sql, offset);
        for (ScratchBirdV3Completion completion : completions) {
            if (matchesPrefix(prefix, completion.label())) {
                return completion.label();
            }
        }
        return prefix.isBlank() ? completions.getFirst().label() : null;
    }

    @NotNull
    static String statusMessage(@NotNull List<ScratchBirdV3Completion> completions, boolean hasInlineSuggestion) {
        List<String> labels = new ArrayList<>();
        for (int index = 0; index < completions.size() && index < STATUS_COMPLETION_LIMIT; index++) {
            labels.add(completions.get(index).label());
        }
        String message = "ScratchBird v3 context: " + String.join(", ", labels);
        if (completions.size() > STATUS_COMPLETION_LIMIT) {
            message += ", +" + (completions.size() - STATUS_COMPLETION_LIMIT) + " more";
        }
        if (hasInlineSuggestion) {
            message += ". Tab/Right accepts the inline prompt.";
        }
        return message;
    }

    @NotNull
    private static List<ScratchBirdV3Completion> distinctCompletions(@NotNull List<ScratchBirdV3Completion> completions) {
        Map<String, ScratchBirdV3Completion> distinct = new LinkedHashMap<>();
        for (ScratchBirdV3Completion completion : completions) {
            distinct.putIfAbsent(completion.label().toUpperCase(Locale.ROOT), completion);
        }
        return distinct.values().stream()
            .sorted(Comparator
                .comparingInt((ScratchBirdV3Completion completion) -> completionRank(completion.label()))
                .thenComparing(ScratchBirdV3Completion::label, String.CASE_INSENSITIVE_ORDER))
            .toList();
    }

    private static int completionRank(@NotNull String label) {
        String normalized = label.toUpperCase(Locale.ROOT);
        int preferredIndex = PREFERRED_PROMPTS.indexOf(normalized);
        if (preferredIndex >= 0) {
            return preferredIndex;
        }
        if (DEPRIORITIZED_PROMPTS.contains(normalized)) {
            return 10_000;
        }
        return 1_000;
    }

    private static boolean matchesPrefix(@NotNull String prefix, @NotNull String label) {
        if (prefix.isBlank()) {
            return false;
        }
        return label.length() > prefix.length() && label.regionMatches(true, 0, prefix, 0, prefix.length());
    }

    private static boolean isTokenBoundary(@NotNull String sql, int offset) {
        return offset >= sql.length() || !isCompletionTokenCharacter(sql.charAt(offset));
    }

    @NotNull
    private static String tokenPrefix(@NotNull String sql, int offset) {
        int start = clamp(offset, 0, sql.length());
        while (start > 0 && isCompletionTokenCharacter(sql.charAt(start - 1))) {
            start--;
        }
        return sql.substring(start, clamp(offset, start, sql.length()));
    }

    private static boolean isCompletionTokenCharacter(char character) {
        return Character.isLetterOrDigit(character) || character == '_' || character == '.';
    }

    private static int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }

    public record PromptPlan(
        @Nullable String inlineSuggestion,
        @Nullable String statusMessage,
        int caretOffset
    ) {
        @NotNull
        public static PromptPlan empty() {
            return new PromptPlan(null, null, 0);
        }
    }
}
