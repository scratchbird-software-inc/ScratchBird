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
 */
package org.jkiss.dbeaver.ext.scratchbird.model;

import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Completion;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class ScratchBirdLanguageResourcePack {

    public static final String RESOURCE_IDENTITY = "sbsql.common_resource_pack.v1";
    public static final String DEFAULT_LANGUAGE_TAG = "en-US";

    private static final String RESOURCE_ROOT = "resources/sbsql-language-resource-pack/";
    private static final String SEED_PACK_REL =
        "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack";
    private static final Pattern UUID_VALUE = Pattern.compile(
        "\\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\b");

    private final boolean available;
    @NotNull
    private final String loadStatus;
    @NotNull
    private final String resourceIdentity;
    @NotNull
    private final String commonResourceHash;
    @NotNull
    private final List<String> supportedProfiles;
    @NotNull
    private final Set<String> keywordTokens;
    @NotNull
    private final List<String> canonicalCompletions;
    @NotNull
    private final Map<String, List<String>> localizedCompletionsByProfile;
    private final int registryRowCount;
    private final int predictiveStateCount;
    private final boolean hashVerificationPassed;
    @NotNull
    private final List<String> hashVerificationErrors;

    private ScratchBirdLanguageResourcePack(
        boolean available,
        @NotNull String loadStatus,
        @NotNull String resourceIdentity,
        @NotNull String commonResourceHash,
        @NotNull List<String> supportedProfiles,
        @NotNull Set<String> keywordTokens,
        @NotNull List<String> canonicalCompletions,
        @NotNull Map<String, List<String>> localizedCompletionsByProfile,
        int registryRowCount,
        int predictiveStateCount,
        boolean hashVerificationPassed,
        @NotNull List<String> hashVerificationErrors
    ) {
        this.available = available;
        this.loadStatus = loadStatus;
        this.resourceIdentity = resourceIdentity;
        this.commonResourceHash = commonResourceHash;
        this.supportedProfiles = List.copyOf(supportedProfiles);
        this.keywordTokens = Set.copyOf(keywordTokens);
        this.canonicalCompletions = List.copyOf(canonicalCompletions);
        this.localizedCompletionsByProfile = Map.copyOf(localizedCompletionsByProfile);
        this.registryRowCount = registryRowCount;
        this.predictiveStateCount = predictiveStateCount;
        this.hashVerificationPassed = hashVerificationPassed;
        this.hashVerificationErrors = List.copyOf(hashVerificationErrors);
    }

    @NotNull
    public static ScratchBirdLanguageResourcePack shared() {
        return Holder.INSTANCE;
    }

    @NotNull
    public static String defaultLanguageTag() {
        String property = System.getProperty("scratchbird.sbsql.languageProfile");
        if (property != null && !property.isBlank()) {
            return property.trim();
        }
        String env = System.getenv("SCRATCHBIRD_SBSQL_LANGUAGE_PROFILE");
        return env == null || env.isBlank() ? DEFAULT_LANGUAGE_TAG : env.trim();
    }

    public boolean available() {
        return available;
    }

    @NotNull
    public String loadStatus() {
        return loadStatus;
    }

    @NotNull
    public String resourceIdentity() {
        return resourceIdentity;
    }

    @NotNull
    public String commonResourceHash() {
        return commonResourceHash;
    }

    @NotNull
    public List<String> supportedProfiles() {
        return supportedProfiles;
    }

    public boolean supportsProfile(@Nullable String languageTag) {
        return supportedProfiles.contains(normalizeLanguageTag(languageTag));
    }

    public int registryRowCount() {
        return registryRowCount;
    }

    public int predictiveStateCount() {
        return predictiveStateCount;
    }

    public int canonicalCompletionCount() {
        return canonicalCompletions.size();
    }

    public int localizedCompletionCount(@Nullable String languageTag) {
        return completionsForProfile(languageTag).size();
    }

    public boolean hashVerificationPassed() {
        return hashVerificationPassed;
    }

    @NotNull
    public List<String> hashVerificationErrors() {
        return hashVerificationErrors;
    }

    @NotNull
    public Collection<String> keywordTokens() {
        return keywordTokens.stream().sorted(String.CASE_INSENSITIVE_ORDER).toList();
    }

    @NotNull
    public List<ScratchBirdV3Completion> completionCandidates(
        @NotNull String sql,
        int offset,
        @Nullable String languageTag
    ) {
        String prefix = activeTokenPrefix(sql, offset).toLowerCase(Locale.ROOT);
        if (!available || prefix.isBlank()) {
            return List.of();
        }

        LinkedHashMap<String, ScratchBirdV3Completion> completions = new LinkedHashMap<>();
        String profile = normalizeLanguageTag(languageTag);
        addMatchingCompletions(completions, completionsForProfile(profile), prefix, profile);
        if (!DEFAULT_LANGUAGE_TAG.equals(profile)) {
            addMatchingCompletions(completions, canonicalCompletions, prefix, DEFAULT_LANGUAGE_TAG + " fallback");
        }
        return completions.values().stream()
            .sorted(Comparator
                .comparingInt((ScratchBirdV3Completion completion) -> completion.label().length())
                .thenComparing(ScratchBirdV3Completion::label, String.CASE_INSENSITIVE_ORDER))
            .limit(250)
            .toList();
    }

    @NotNull
    private static ScratchBirdLanguageResourcePack load() {
        Optional<ResourceRoot> root = locateResourceRoot();
        if (root.isEmpty()) {
            return unavailable("shared SBsql language resource pack not found");
        }

        try {
            ResourceRoot resourceRoot = root.get();
            String manifest = resourceRoot.readString("manifest.sblrp.json");
            String phraseTable = resourceRoot.readString("resources/phrases/phrase-table.json");
            String sourceCorpus = resourceRoot.readString("resources/canonical/translation-source-corpus.jsonl");
            String predictive = resourceRoot.readString("resources/predictive/predictive-grammar.json");

            String identity = value(manifest, "resource_identity").orElse("");
            String commonHash = value(manifest, "common_resource_hash").orElse("");
            List<String> profiles = orderedDistinct(strings(manifest, "exact_tag"));
            int registryRows = integerValue(manifest, "registry_row_count").orElse(0);
            int predictiveStates = integerValue(predictive, "max_table_entries").orElse(0);

            LinkedHashSet<String> canonical = new LinkedHashSet<>();
            canonical.addAll(filteredStrings(phraseTable, "canonical_text"));
            canonical.addAll(sourceCorpusEntries(sourceCorpus));

            Map<String, List<String>> localizedByProfile = new LinkedHashMap<>();
            for (String profile : profiles) {
                String rel = "resources/languages/" + profile + "/language-profile.json";
                if (resourceRoot.exists(rel)) {
                    localizedByProfile.put(profile, localizedEntries(resourceRoot.readString(rel)));
                }
            }
            localizedByProfile.putIfAbsent(DEFAULT_LANGUAGE_TAG, List.copyOf(canonical));

            LinkedHashSet<String> keywordTokens = new LinkedHashSet<>();
            for (String label : canonical) {
                addKeywordTokens(keywordTokens, label);
            }
            for (List<String> localizedLabels : localizedByProfile.values()) {
                for (String label : localizedLabels) {
                    addKeywordTokens(keywordTokens, label);
                }
            }

            HashResult hashResult = verifyHashes(resourceRoot);
            boolean usable = RESOURCE_IDENTITY.equals(identity) && !canonical.isEmpty();
            String status = usable
                ? "loaded " + identity + " from " + resourceRoot.description()
                : "resource pack identity or canonical surface table is invalid";
            return new ScratchBirdLanguageResourcePack(
                usable,
                status,
                identity,
                commonHash,
                profiles,
                keywordTokens,
                orderedDistinct(canonical),
                localizedByProfile,
                registryRows,
                predictiveStates,
                hashResult.passed(),
                hashResult.errors());
        } catch (IOException | RuntimeException e) {
            return unavailable("failed loading shared SBsql language resource pack: " + e.getMessage());
        }
    }

    @NotNull
    private static ScratchBirdLanguageResourcePack unavailable(@NotNull String status) {
        return new ScratchBirdLanguageResourcePack(
            false,
            status,
            "",
            "",
            List.of(),
            Set.of(),
            List.of(),
            Map.of(),
            0,
            0,
            false,
            List.of(status));
    }

    @NotNull
    private List<String> completionsForProfile(@Nullable String languageTag) {
        String normalized = normalizeLanguageTag(languageTag);
        List<String> localized = localizedCompletionsByProfile.get(normalized);
        return localized == null || localized.isEmpty() ? canonicalCompletions : localized;
    }

    private static void addMatchingCompletions(
        @NotNull LinkedHashMap<String, ScratchBirdV3Completion> completions,
        @NotNull List<String> candidates,
        @NotNull String prefix,
        @NotNull String profile
    ) {
        for (String candidate : candidates) {
            if (candidate.toLowerCase(Locale.ROOT).startsWith(prefix)) {
                completions.putIfAbsent(
                    candidate.toUpperCase(Locale.ROOT),
                    new ScratchBirdV3Completion(candidate, "SBsql shared language resource " + profile));
            }
        }
    }

    @NotNull
    private static String normalizeLanguageTag(@Nullable String languageTag) {
        if (languageTag == null || languageTag.isBlank()) {
            return DEFAULT_LANGUAGE_TAG;
        }
        return Locale.forLanguageTag(languageTag.trim()).toLanguageTag();
    }

    @NotNull
    private static String activeTokenPrefix(@NotNull String sql, int offset) {
        int safeOffset = Math.max(0, Math.min(offset, sql.length()));
        int start = safeOffset;
        while (start > 0) {
            char character = sql.charAt(start - 1);
            if (!Character.isLetterOrDigit(character) && character != '_' && character != '-') {
                break;
            }
            start--;
        }
        return sql.substring(start, safeOffset).trim();
    }

    @NotNull
    private static Optional<ResourceRoot> locateResourceRoot() {
        Optional<ResourceRoot> configured = resourceRootFromPath(System.getProperty("scratchbird.sbsql.languageResourcePack"));
        if (configured.isPresent()) {
            return configured;
        }
        configured = resourceRootFromPath(System.getenv("SCRATCHBIRD_SBSQL_LANGUAGE_RESOURCE_PACK"));
        if (configured.isPresent()) {
            return configured;
        }

        ClassLoader loader = ScratchBirdLanguageResourcePack.class.getClassLoader();
        if (loader.getResource(RESOURCE_ROOT + "manifest.sblrp.json") != null) {
            return Optional.of(new ClasspathResourceRoot(loader));
        }

        Path start = Path.of(System.getProperty("user.dir", ".")).toAbsolutePath().normalize();
        for (Path current = start; current != null; current = current.getParent()) {
            for (String candidate : List.of(
                SEED_PACK_REL,
                "resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack",
                "resources/sbsql-language-resource-pack",
                "sbsql-language-resource-pack"
            )) {
                Optional<ResourceRoot> found = resourceRootFromPath(current.resolve(candidate).toString());
                if (found.isPresent()) {
                    return found;
                }
            }
        }

        return resourceRootFromPath(
            "/opt/ScratchBird/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack");
    }

    @NotNull
    private static Optional<ResourceRoot> resourceRootFromPath(@Nullable String value) {
        if (value == null || value.isBlank()) {
            return Optional.empty();
        }
        Path path = Path.of(value.trim()).toAbsolutePath().normalize();
        if (Files.isRegularFile(path.resolve("manifest.sblrp.json"))) {
            return Optional.of(new PathResourceRoot(path));
        }
        return Optional.empty();
    }

    @NotNull
    private static List<String> localizedEntries(@NotNull String languageProfileJson) {
        LinkedHashSet<String> labels = new LinkedHashSet<>();
        Matcher matcher = Pattern.compile("\\{[^{}]*\"localized_text\"\\s*:\\s*\"(?:\\\\.|[^\"\\\\])*\"[^{}]*}")
            .matcher(languageProfileJson);
        while (matcher.find()) {
            String object = matcher.group();
            String family = value(object, "source_family").orElse("");
            if (!"sbsql_surface".equals(family) && !"system_object".equals(family)) {
                continue;
            }
            value(object, "localized_text")
                .map(ScratchBirdLanguageResourcePack::normalizeLabel)
                .filter(ScratchBirdLanguageResourcePack::visibleCompletionLabel)
                .ifPresent(labels::add);
        }
        return orderedDistinct(labels);
    }

    @NotNull
    private static List<String> sourceCorpusEntries(@NotNull String jsonLines) {
        LinkedHashSet<String> labels = new LinkedHashSet<>();
        for (String line : jsonLines.split("\\R")) {
            String family = value(line, "source_family").orElse("");
            if (!"sbsql_surface".equals(family) && !"system_object".equals(family)) {
                continue;
            }
            value(line, "source_text")
                .map(ScratchBirdLanguageResourcePack::normalizeLabel)
                .filter(ScratchBirdLanguageResourcePack::visibleCompletionLabel)
                .ifPresent(labels::add);
        }
        return orderedDistinct(labels);
    }

    @NotNull
    private static List<String> filteredStrings(@NotNull String text, @NotNull String field) {
        return strings(text, field).stream()
            .map(ScratchBirdLanguageResourcePack::normalizeLabel)
            .filter(ScratchBirdLanguageResourcePack::visibleCompletionLabel)
            .toList();
    }

    private static boolean visibleCompletionLabel(@NotNull String value) {
        return !value.isBlank()
            && value.length() <= 120
            && !value.contains("SBSQL-")
            && !UUID_VALUE.matcher(value).find();
    }

    @NotNull
    private static String normalizeLabel(@NotNull String value) {
        return value.trim().replaceAll("\\s+", " ");
    }

    private static void addKeywordTokens(@NotNull Set<String> tokens, @NotNull String label) {
        for (String part : label.split("[^\\p{L}\\p{N}_]+")) {
            if (part.length() > 1 && !part.chars().allMatch(Character::isDigit)) {
                tokens.add(part.toUpperCase(Locale.ROOT));
            }
        }
    }

    @NotNull
    private static Optional<String> value(@NotNull String text, @NotNull String field) {
        Matcher matcher = fieldPattern(field).matcher(text);
        return matcher.find() ? Optional.of(unescapeJson(matcher.group(1))) : Optional.empty();
    }

    @NotNull
    private static List<String> strings(@NotNull String text, @NotNull String field) {
        Matcher matcher = fieldPattern(field).matcher(text);
        List<String> values = new ArrayList<>();
        while (matcher.find()) {
            values.add(unescapeJson(matcher.group(1)));
        }
        return values;
    }

    @NotNull
    private static Optional<Integer> integerValue(@NotNull String text, @NotNull String field) {
        Matcher matcher = Pattern.compile("\"" + Pattern.quote(field) + "\"\\s*:\\s*(\\d+)").matcher(text);
        return matcher.find() ? Optional.of(Integer.parseInt(matcher.group(1))) : Optional.empty();
    }

    @NotNull
    private static Pattern fieldPattern(@NotNull String field) {
        return Pattern.compile("\"" + Pattern.quote(field) + "\"\\s*:\\s*\"((?:\\\\.|[^\"\\\\])*)\"");
    }

    @NotNull
    private static String unescapeJson(@NotNull String text) {
        StringBuilder builder = new StringBuilder(text.length());
        for (int index = 0; index < text.length(); index++) {
            char character = text.charAt(index);
            if (character != '\\' || index + 1 >= text.length()) {
                builder.append(character);
                continue;
            }
            char escaped = text.charAt(++index);
            switch (escaped) {
                case '"' -> builder.append('"');
                case '\\' -> builder.append('\\');
                case '/' -> builder.append('/');
                case 'b' -> builder.append('\b');
                case 'f' -> builder.append('\f');
                case 'n' -> builder.append('\n');
                case 'r' -> builder.append('\r');
                case 't' -> builder.append('\t');
                case 'u' -> {
                    if (index + 4 < text.length()) {
                        builder.append((char) Integer.parseInt(text.substring(index + 1, index + 5), 16));
                        index += 4;
                    }
                }
                default -> builder.append(escaped);
            }
        }
        return builder.toString();
    }

    @NotNull
    private static List<String> orderedDistinct(@NotNull Collection<String> values) {
        return values.stream()
            .filter(value -> value != null && !value.isBlank())
            .distinct()
            .sorted(String.CASE_INSENSITIVE_ORDER)
            .toList();
    }

    @NotNull
    private static HashResult verifyHashes(@NotNull ResourceRoot root) throws IOException {
        if (!root.exists("hashes.sha256")) {
            return new HashResult(false, List.of("hashes.sha256 missing from language resource pack"));
        }
        List<String> errors = new ArrayList<>();
        String hashText = root.readString("hashes.sha256");
        for (String line : hashText.split("\\R")) {
            if (line.isBlank()) {
                continue;
            }
            String[] parts = line.trim().split("\\s+", 2);
            if (parts.length != 2 || !parts[0].startsWith("sha256:")) {
                errors.add("invalid hash row: " + line);
                continue;
            }
            String expected = parts[0].substring("sha256:".length());
            String rel = parts[1].trim();
            if (!root.exists(rel)) {
                errors.add("missing hashed resource: " + rel);
                continue;
            }
            String actual = sha256Hex(root.readBytes(rel));
            if (!expected.equalsIgnoreCase(actual)) {
                errors.add("hash mismatch for " + rel);
            }
        }
        return new HashResult(errors.isEmpty(), errors);
    }

    @NotNull
    private static String sha256Hex(byte[] bytes) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(bytes);
            StringBuilder builder = new StringBuilder(digest.length * 2);
            for (byte value : digest) {
                builder.append(String.format("%02x", value & 0xff));
            }
            return builder.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 unavailable", e);
        }
    }

    private interface ResourceRoot {
        boolean exists(@NotNull String rel);

        @NotNull
        byte[] readBytes(@NotNull String rel) throws IOException;

        @NotNull
        default String readString(@NotNull String rel) throws IOException {
            return new String(readBytes(rel), StandardCharsets.UTF_8);
        }

        @NotNull
        String description();
    }

    private record PathResourceRoot(@NotNull Path root) implements ResourceRoot {
        @Override
        public boolean exists(@NotNull String rel) {
            return Files.isRegularFile(root.resolve(rel));
        }

        @NotNull
        @Override
        public byte[] readBytes(@NotNull String rel) throws IOException {
            return Files.readAllBytes(root.resolve(rel));
        }

        @NotNull
        @Override
        public String description() {
            return root.toString();
        }
    }

    private record ClasspathResourceRoot(@NotNull ClassLoader loader) implements ResourceRoot {
        @Override
        public boolean exists(@NotNull String rel) {
            return loader.getResource(RESOURCE_ROOT + rel) != null;
        }

        @NotNull
        @Override
        public byte[] readBytes(@NotNull String rel) throws IOException {
            try (InputStream stream = loader.getResourceAsStream(RESOURCE_ROOT + rel)) {
                if (stream == null) {
                    throw new IOException("missing classpath resource: " + rel);
                }
                ByteArrayOutputStream output = new ByteArrayOutputStream();
                stream.transferTo(output);
                return output.toByteArray();
            }
        }

        @NotNull
        @Override
        public String description() {
            return "classpath:" + RESOURCE_ROOT;
        }
    }

    private record HashResult(boolean passed, @NotNull List<String> errors) {
    }

    private static final class Holder {
        private static final ScratchBirdLanguageResourcePack INSTANCE = ScratchBirdLanguageResourcePack.load();
    }
}
