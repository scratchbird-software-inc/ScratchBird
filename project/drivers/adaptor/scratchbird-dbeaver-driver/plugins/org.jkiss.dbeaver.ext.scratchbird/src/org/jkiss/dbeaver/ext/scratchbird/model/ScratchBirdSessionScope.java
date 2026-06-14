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

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;

public final class ScratchBirdSessionScope {

    @NotNull
    private final String connectionScopeId;
    @NotNull
    private final ScratchBirdAuthorizedResolverCache resolverCache = new ScratchBirdAuthorizedResolverCache();
    @NotNull
    private final Map<String, ScratchBirdActionAdmission> actionAdmissions = new LinkedHashMap<>();
    @NotNull
    private ScratchBirdAuthorizationContext authorizationContext;

    private ScratchBirdSessionScope(@NotNull String connectionScopeId) {
        this.connectionScopeId = connectionScopeId;
        this.authorizationContext = ScratchBirdAuthorizationContext.unauthenticated(connectionScopeId);
    }

    @NotNull
    public static ScratchBirdSessionScope forConnection(@NotNull String connectionScopeId) {
        return new ScratchBirdSessionScope(connectionScopeId);
    }

    @NotNull
    public static ScratchBirdSessionScope forConnection(
        @NotNull String connectionScopeId,
        @NotNull String principalIdentity,
        @NotNull String roleIdentity,
        @NotNull String groupIdentity,
        @NotNull String languageTag
    ) {
        ScratchBirdSessionScope scope = new ScratchBirdSessionScope(connectionScopeId);
        String cacheBasis = String.join("|", connectionScopeId, principalIdentity, roleIdentity, groupIdentity, languageTag);
        scope.authorizationContext = ScratchBirdAuthorizationContext.builder()
            .serverIdentity("preview-server:" + stableUuid("server:" + connectionScopeId))
            .routeIdentity(connectionScopeId)
            .databaseUuid(stableUuid("database:" + connectionScopeId))
            .sessionUuid(stableUuid("session:" + cacheBasis))
            .authenticatedUserUuid(stableUuid("principal:" + principalIdentity))
            .effectiveRoleUuids(List.of(stableUuid("role:" + roleIdentity)))
            .effectiveGroupUuids(List.of(stableUuid("group:" + groupIdentity)))
            .languageProfile(languageTag, "en-US")
            .resolverPolicy("preview_static_only")
            .resolverSnapshotIdentity("preview:" + sha256(cacheBasis).substring(0, 24))
            .transactionContextId("preview-txn:" + sha256("txn:" + cacheBasis).substring(0, 24))
            .serverAdmitted(false)
            .build();
        return scope;
    }

    @NotNull
    public static ScratchBirdSessionScope anonymousPreviewScope(@NotNull String targetPath) {
        String targetHash = sha256("anonymous-preview:" + targetPath);
        return forConnection(
            "preview:" + targetHash.substring(0, 24),
            "anonymous-preview-principal",
            "anonymous-preview-role",
            "anonymous-preview-group",
            "en-US");
    }

    @NotNull
    public synchronized String connectionScopeId() {
        return connectionScopeId;
    }

    @NotNull
    public synchronized ScratchBirdAuthorizationContext authorizationContext() {
        return authorizationContext;
    }

    public synchronized void updateAuthorizationContext(@NotNull ScratchBirdAuthorizationContext context) {
        String previousFingerprint = authorizationContext.fingerprint();
        authorizationContext = context;
        if (!previousFingerprint.equals(context.fingerprint())) {
            invalidateCaches();
        }
    }

    @NotNull
    public ScratchBirdAuthorizedResolverCache resolverCache() {
        return resolverCache;
    }

    public synchronized void invalidateCaches() {
        resolverCache.invalidate();
        actionAdmissions.clear();
    }

    @NotNull
    public synchronized ScratchBirdActionAdmission recordActionAdmission(@NotNull ScratchBirdActionAdmission admission) {
        actionAdmissions.put(admission.requestUuid(), admission);
        return admission;
    }

    @Nullable
    public synchronized ScratchBirdActionAdmission actionAdmission(@NotNull String requestUuid) {
        return actionAdmissions.get(requestUuid);
    }

    @NotNull
    public synchronized List<ScratchBirdActionAdmission> actionAdmissions() {
        return List.copyOf(actionAdmissions.values());
    }

    public synchronized boolean cacheCompatibleWith(@NotNull ScratchBirdSessionScope other) {
        return authorizationContext.cacheScopeKey().equals(other.authorizationContext.cacheScopeKey());
    }

    @NotNull
    public synchronized String transactionScopeKey() {
        if (authorizationContext.transactionContextId().isBlank()) {
            return "no-server-transaction:" + authorizationContext.fingerprint();
        }
        return authorizationContext.transactionContextId();
    }

    @NotNull
    public synchronized List<String> summaryLines() {
        return List.of(
            "Connection scope: " + connectionScopeId,
            "Authorization fingerprint: " + authorizationContext.fingerprint(),
            "Resolver boundary: " + authorizationContext.resolverBoundary().summary(),
            "Transaction scope: " + transactionScopeKey(),
            "Isolation rule: cache keys include connection, server identity, database UUID, principal UUID, role/group UUIDs, language, catalog/grant/security/descriptor/localized epochs, resolver snapshot, and transaction context.",
            "Server admission: " + authorizationContext.serverAdmitted());
    }

    @NotNull
    public static String sha256(@NotNull String value) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] hash = digest.digest(value.getBytes(StandardCharsets.UTF_8));
            StringBuilder builder = new StringBuilder();
            for (byte b : hash) {
                builder.append(String.format(Locale.ROOT, "%02x", b & 0xff));
            }
            return builder.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 unavailable", e);
        }
    }

    @NotNull
    private static String stableUuid(@NotNull String value) {
        return UUID.nameUUIDFromBytes(value.getBytes(StandardCharsets.UTF_8)).toString();
    }
}
