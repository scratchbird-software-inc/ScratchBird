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

import org.jkiss.dbeaver.Log;
import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.model.DBPDataSource;
import org.jkiss.dbeaver.model.struct.DBSObject;
import org.jkiss.dbeaver.runtime.DBWorkbench;
import org.jkiss.dbeaver.utils.GeneralUtils;
import org.jkiss.utils.CommonUtils;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.time.Instant;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Base64;
import java.util.Deque;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

public final class ScratchBirdProbeHistory {

    private static final Log log = Log.getLog(ScratchBirdProbeHistory.class);
    private static final int MAX_SCOPE_ENTRIES = 24;
    private static final String STORE_FILE_NAME = "probe-history-v1.tsv";
    private static final String STORE_VERSION = "v1";
    private static final Object IO_LOCK = new Object();
    private static final Map<String, Deque<HistoryEntry>> HISTORY = new ConcurrentHashMap<>();
    private static boolean loaded;

    public enum EntryKind {
        AUTHZ_PROBE("Authz probe"),
        LIVE_PROBE("Live probe"),
        TASK_PREVIEW("Task preview"),
        TASK_VALIDATE("Task validate"),
        TASK_EXECUTE("Task execute");

        @NotNull
        private final String label;

        EntryKind(@NotNull String label) {
            this.label = label;
        }

        @NotNull
        public String label() {
            return label;
        }
    }

    public record HistoryEntry(
        @NotNull String recordedAt,
        @NotNull String scopeKey,
        @NotNull String targetPath,
        @NotNull String formId,
        @NotNull String formName,
        @NotNull String taskId,
        @NotNull String taskTitle,
        @NotNull EntryKind kind,
        @NotNull String label,
        @NotNull String authority,
        @NotNull ScratchBirdRefusalModel status,
        boolean surrogate,
        int statementCount,
        @NotNull String commandText,
        @NotNull String previewText
    ) {
        @NotNull
        public List<String> summaryLines() {
            List<String> lines = new ArrayList<>();
            lines.add("Recorded at: " + recordedAt);
            lines.add("Scope: " + targetPath);
            lines.add("Kind: " + kind.label());
            lines.add("Status: " + status.kind() + ": " + status.message());
            lines.add("Form: " + formId + " - " + formName);
            if (!taskId.isBlank()) {
                lines.add("Task: " + taskId + " - " + taskTitle);
            }
            lines.add("Statements: " + statementCount);
            lines.add("Authority: " + authority);
            lines.add("Surrogate: " + surrogate);
            return List.copyOf(lines);
        }

        @NotNull
        public String displayLabel() {
            StringBuilder builder = new StringBuilder();
            builder.append(recordedAt)
                .append(" | ")
                .append(kind.label())
                .append(" | ")
                .append(status.kind());
            if (!taskId.isBlank()) {
                builder.append(" | ").append(taskId);
            }
            return builder.toString();
        }
    }

    private ScratchBirdProbeHistory() {
    }

    @NotNull
    public static String scopeKey(@NotNull DBSObject targetObject, @NotNull String targetPath) {
        DBPDataSource dataSource = targetObject.getDataSource();
        String dataSourceId = dataSource != null && dataSource.getContainer() != null ?
            dataSource.getContainer().getId() :
            "detached";
        return dataSourceId + "::" + normalize(targetPath);
    }

    @NotNull
    public static String storeLocationText() {
        return storeFile().toAbsolutePath().toString();
    }

    @NotNull
    public static HistoryEntry recordLiveProbe(
        @NotNull String scopeKey,
        @NotNull String targetPath,
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdLiveProbe.ProbeResult result
    ) {
        synchronized (IO_LOCK) {
            ensureLoadedLocked();
            HistoryEntry entry = new HistoryEntry(
                Instant.now().toString(),
                scopeKey,
                targetPath,
                form.id(),
                form.name(),
                "",
                "",
                EntryKind.LIVE_PROBE,
                result.plan().label(),
                result.plan().authority(),
                result.status(),
                result.plan().surrogate(),
                result.statementResults().size(),
                result.plan().commandText(),
                result.previewText());
            addNewestLocked(scopeKey, entry);
            persistLocked();
            return entry;
        }
    }

    @NotNull
    public static HistoryEntry recordAuthorizationProbe(
        @NotNull String scopeKey,
        @NotNull String targetPath,
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdLiveProbe.ProbeResult result
    ) {
        synchronized (IO_LOCK) {
            ensureLoadedLocked();
            HistoryEntry entry = new HistoryEntry(
                Instant.now().toString(),
                scopeKey,
                targetPath,
                form.id(),
                form.name(),
                "",
                "",
                EntryKind.AUTHZ_PROBE,
                result.plan().label(),
                result.plan().authority(),
                result.status(),
                result.plan().surrogate(),
                result.statementResults().size(),
                result.plan().commandText(),
                result.previewText());
            addNewestLocked(scopeKey, entry);
            persistLocked();
            return entry;
        }
    }

    @NotNull
    public static HistoryEntry recordTaskProbe(
        @NotNull String scopeKey,
        @NotNull String targetPath,
        @NotNull ScratchBirdFormDefinition form,
        @NotNull ScratchBirdTaskDefinition taskDefinition,
        @NotNull ScratchBirdLiveProbe.TaskProbePhase phase,
        @NotNull ScratchBirdLiveProbe.ProbeResult result
    ) {
        synchronized (IO_LOCK) {
            ensureLoadedLocked();
            EntryKind kind = switch (phase) {
                case PREVIEW -> EntryKind.TASK_PREVIEW;
                case VALIDATE -> EntryKind.TASK_VALIDATE;
                case EXECUTE -> EntryKind.TASK_EXECUTE;
            };
            HistoryEntry entry = new HistoryEntry(
                Instant.now().toString(),
                scopeKey,
                targetPath,
                form.id(),
                form.name(),
                taskDefinition.id(),
                taskDefinition.title(),
                kind,
                result.plan().label(),
                result.plan().authority(),
                result.status(),
                result.plan().surrogate(),
                result.statementResults().size(),
                result.plan().commandText(),
                result.previewText());
            addNewestLocked(scopeKey, entry);
            persistLocked();
            return entry;
        }
    }

    @NotNull
    public static List<HistoryEntry> historyFor(@NotNull String scopeKey) {
        synchronized (IO_LOCK) {
            ensureLoadedLocked();
            Deque<HistoryEntry> entries = HISTORY.get(scopeKey);
            if (entries == null || entries.isEmpty()) {
                return List.of();
            }
            synchronized (entries) {
                return List.copyOf(entries);
            }
        }
    }

    public static void clear(@NotNull String scopeKey) {
        synchronized (IO_LOCK) {
            ensureLoadedLocked();
            HISTORY.remove(scopeKey);
            persistLocked();
        }
    }

    static void clearAll() {
        synchronized (IO_LOCK) {
            HISTORY.clear();
            loaded = true;
            deleteStoreLocked();
        }
    }

    static void resetCache() {
        synchronized (IO_LOCK) {
            HISTORY.clear();
            loaded = false;
        }
    }

    @NotNull
    static Path storeFileForTests() {
        return storeFile();
    }

    private static void ensureLoadedLocked() {
        if (loaded) {
            return;
        }
        loadLocked();
        loaded = true;
    }

    private static void addNewestLocked(@NotNull String scopeKey, @NotNull HistoryEntry entry) {
        Deque<HistoryEntry> entries = HISTORY.computeIfAbsent(scopeKey, key -> new ArrayDeque<>());
        synchronized (entries) {
            entries.addFirst(entry);
            while (entries.size() > MAX_SCOPE_ENTRIES) {
                entries.removeLast();
            }
        }
    }

    private static void addLoadedLocked(@NotNull HistoryEntry entry) {
        Deque<HistoryEntry> entries = HISTORY.computeIfAbsent(entry.scopeKey(), key -> new ArrayDeque<>());
        synchronized (entries) {
            entries.addLast(entry);
            while (entries.size() > MAX_SCOPE_ENTRIES) {
                entries.removeLast();
            }
        }
    }

    private static void loadLocked() {
        Path storeFile = storeFile();
        if (!Files.exists(storeFile)) {
            return;
        }
        try (BufferedReader reader = Files.newBufferedReader(storeFile, StandardCharsets.UTF_8)) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (line.isBlank() || line.startsWith("#")) {
                    continue;
                }
                HistoryEntry entry = deserialize(line);
                if (entry != null) {
                    addLoadedLocked(entry);
                }
            }
        } catch (IOException e) {
            log.warn("Error reading ScratchBird probe history from " + storeFile, e);
        }
    }

    private static void persistLocked() {
        Path storeFile = storeFile();
        try {
            Files.createDirectories(storeFile.getParent());
            Path tempFile = storeFile.resolveSibling(storeFile.getFileName() + ".tmp");
            try (BufferedWriter writer = Files.newBufferedWriter(tempFile, StandardCharsets.UTF_8)) {
                writer.write("# ScratchBird probe history " + STORE_VERSION);
                writer.newLine();
                for (Map.Entry<String, Deque<HistoryEntry>> scopeEntry : HISTORY.entrySet().stream()
                    .sorted(Map.Entry.comparingByKey())
                    .toList()) {
                    for (HistoryEntry entry : scopeEntry.getValue()) {
                        writer.write(serialize(entry));
                        writer.newLine();
                    }
                }
            }
            try {
                Files.move(tempFile, storeFile, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            } catch (IOException e) {
                Files.move(tempFile, storeFile, StandardCopyOption.REPLACE_EXISTING);
            }
        } catch (IOException e) {
            log.warn("Error writing ScratchBird probe history to " + storeFile, e);
        }
    }

    private static void deleteStoreLocked() {
        Path storeFile = storeFile();
        try {
            Files.deleteIfExists(storeFile);
        } catch (IOException e) {
            log.warn("Error deleting ScratchBird probe history store " + storeFile, e);
        }
    }

    @NotNull
    private static Path storeFile() {
        if (DBWorkbench.isPlatformStarted()) {
            try {
                return GeneralUtils.getMetadataFolder()
                    .resolve("scratchbird")
                    .resolve(STORE_FILE_NAME);
            } catch (RuntimeException e) {
                log.debug("Falling back to user-home ScratchBird probe history store", e);
            }
        }
        String userHome = System.getProperty("user.home");
        if (!CommonUtils.isEmpty(userHome)) {
            return Path.of(userHome)
                .resolve(".dbeaver-scratchbird")
                .resolve(STORE_FILE_NAME);
        }
        return Path.of(System.getProperty("java.io.tmpdir"))
            .resolve("dbeaver-scratchbird")
            .resolve(STORE_FILE_NAME);
    }

    @NotNull
    private static String serialize(@NotNull HistoryEntry entry) {
        return String.join("\t",
            STORE_VERSION,
            encode(entry.recordedAt()),
            encode(entry.scopeKey()),
            encode(entry.targetPath()),
            encode(entry.formId()),
            encode(entry.formName()),
            encode(entry.taskId()),
            encode(entry.taskTitle()),
            entry.kind().name(),
            encode(entry.label()),
            encode(entry.authority()),
            entry.status().kind().name(),
            encode(entry.status().message()),
            encode(CommonUtils.notEmpty(entry.status().sourceSurface())),
            Boolean.toString(entry.surrogate()),
            Integer.toString(entry.statementCount()),
            encode(entry.commandText()),
            encode(entry.previewText()));
    }

    @Nullable
    private static HistoryEntry deserialize(@NotNull String line) {
        String[] parts = line.split("\t", -1);
        if (parts.length != 18 || !STORE_VERSION.equals(parts[0])) {
            return null;
        }
        try {
            String sourceSurface = decode(parts[13]);
            return new HistoryEntry(
                decode(parts[1]),
                decode(parts[2]),
                decode(parts[3]),
                decode(parts[4]),
                decode(parts[5]),
                decode(parts[6]),
                decode(parts[7]),
                EntryKind.valueOf(parts[8]),
                decode(parts[9]),
                decode(parts[10]),
                new ScratchBirdRefusalModel(
                    ScratchBirdRefusalModel.Kind.valueOf(parts[11]),
                    decode(parts[12]),
                    sourceSurface.isBlank() ? null : sourceSurface),
                Boolean.parseBoolean(parts[14]),
                Integer.parseInt(parts[15]),
                decode(parts[16]),
                decode(parts[17]));
        } catch (RuntimeException e) {
            log.warn("Error parsing ScratchBird probe history line", e);
            return null;
        }
    }

    @NotNull
    private static String encode(@NotNull String value) {
        return Base64.getEncoder().encodeToString(value.getBytes(StandardCharsets.UTF_8));
    }

    @NotNull
    private static String decode(@NotNull String value) {
        return new String(Base64.getDecoder().decode(value), StandardCharsets.UTF_8);
    }

    @NotNull
    private static String normalize(@NotNull String value) {
        return value.toLowerCase(Locale.ENGLISH);
    }
}
