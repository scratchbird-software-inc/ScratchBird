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
import org.jkiss.utils.CommonUtils;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;

public record ScratchBirdAuthorizationContext(
    @NotNull String serverIdentity,
    @NotNull String routeIdentity,
    @NotNull String databaseUuid,
    @NotNull String sessionUuid,
    @NotNull String authenticatedUserUuid,
    @NotNull List<String> effectiveRoleUuids,
    @NotNull List<String> effectiveGroupUuids,
    @NotNull String currentSchema,
    @NotNull String searchPath,
    @NotNull String exactLanguageProfileTag,
    @NotNull String databaseDefaultLanguageTag,
    @NotNull String catalogEpoch,
    @NotNull String grantEpoch,
    @NotNull String securityPolicyEpoch,
    @NotNull String descriptorEpoch,
    @NotNull String localizedNameEpoch,
    @NotNull String policyEpoch,
    @NotNull String resolverPolicy,
    @NotNull String resolverSnapshotIdentity,
    @NotNull String transactionContextId,
    @NotNull Set<String> capabilities,
    boolean serverAdmitted
) {
    public static final String CAPABILITY_SBLR_UUID_PASSTHROUGH = "sbsql.sblr_uuid_passthrough.v1";

    public ScratchBirdAuthorizationContext {
        serverIdentity = clean(serverIdentity);
        routeIdentity = clean(routeIdentity);
        databaseUuid = clean(databaseUuid);
        sessionUuid = clean(sessionUuid);
        authenticatedUserUuid = clean(authenticatedUserUuid);
        effectiveRoleUuids = sortedCopy(effectiveRoleUuids);
        effectiveGroupUuids = sortedCopy(effectiveGroupUuids);
        currentSchema = clean(currentSchema);
        searchPath = clean(searchPath);
        exactLanguageProfileTag = clean(exactLanguageProfileTag);
        databaseDefaultLanguageTag = clean(databaseDefaultLanguageTag);
        catalogEpoch = clean(catalogEpoch);
        grantEpoch = clean(grantEpoch);
        securityPolicyEpoch = clean(securityPolicyEpoch);
        descriptorEpoch = clean(descriptorEpoch);
        localizedNameEpoch = clean(localizedNameEpoch);
        policyEpoch = clean(policyEpoch);
        resolverPolicy = clean(resolverPolicy);
        resolverSnapshotIdentity = clean(resolverSnapshotIdentity);
        transactionContextId = clean(transactionContextId);
        capabilities = sortedSetCopy(capabilities);
    }

    @NotNull
    public static ScratchBirdAuthorizationContext unauthenticated(@NotNull String routeIdentity) {
        return builder()
            .serverIdentity("unknown")
            .routeIdentity(routeIdentity)
            .resolverPolicy("not_admitted")
            .build();
    }

    @NotNull
    public static Builder builder() {
        return new Builder();
    }

    public boolean canUseResolverCache() {
        return serverAdmitted &&
            isUuid(databaseUuid) &&
            isUuid(sessionUuid) &&
            isUuid(authenticatedUserUuid) &&
            CommonUtils.isNotEmpty(serverIdentity) &&
            CommonUtils.isNotEmpty(routeIdentity);
    }

    public boolean canSubmitSblrUuidPassThrough() {
        return canUseResolverCache() && capabilities.contains(CAPABILITY_SBLR_UUID_PASSTHROUGH);
    }

    public boolean hasCapability(@NotNull String capability) {
        return capabilities.contains(capability);
    }

    @NotNull
    public ScratchBirdFeatureBoundaryStatus resolverBoundary() {
        if (canUseResolverCache()) {
            return ScratchBirdFeatureBoundaryStatus.available(
                "authorized_path_resolver",
                "Resolver cache is bound to server-admitted session " + shortIdentity(sessionUuid) + ".",
                "sys.catalog.object_resolver");
        }
        return ScratchBirdFeatureBoundaryStatus.requiresServerAdmission(
            "authorized_path_resolver",
            "Resolver cache is unavailable until the server returns database, session, principal, role/group, epoch, and policy identity.",
            "sys.catalog.object_resolver");
    }

    @NotNull
    public ScratchBirdFeatureBoundaryStatus sblrPassThroughBoundary() {
        if (!serverAdmitted) {
            return ScratchBirdFeatureBoundaryStatus.requiresServerAdmission(
                "sblr_uuid_pass_through",
                "SBLR/UUID pass-through is refused before server authentication and capability negotiation.",
                "sbsql.sblr_uuid_passthrough.v1");
        }
        if (!hasCapability(CAPABILITY_SBLR_UUID_PASSTHROUGH)) {
            return ScratchBirdFeatureBoundaryStatus.unavailable(
                "sblr_uuid_pass_through",
                "Connected server session did not negotiate sbsql.sblr_uuid_passthrough.v1; use server-side SBsql text prepare when policy allows.",
                "sbsql.sblr_uuid_passthrough.v1");
        }
        return ScratchBirdFeatureBoundaryStatus.requiresServerAdmission(
            "sblr_uuid_pass_through",
            "Client-prepared SBLR/UUID payloads may be submitted only as untrusted claims and still require server revalidation.",
            "sbsql.sblr_uuid_passthrough.v1");
    }

    @NotNull
    public String cacheScopeKey() {
        return String.join("|",
            serverIdentity,
            routeIdentity,
            databaseUuid,
            sessionUuid,
            authenticatedUserUuid,
            String.join(",", effectiveRoleUuids),
            String.join(",", effectiveGroupUuids),
            currentSchema,
            searchPath,
            exactLanguageProfileTag,
            databaseDefaultLanguageTag,
            catalogEpoch,
            grantEpoch,
            securityPolicyEpoch,
            descriptorEpoch,
            localizedNameEpoch,
            policyEpoch,
            resolverPolicy,
            resolverSnapshotIdentity,
            transactionContextId);
    }

    @NotNull
    public String fingerprint() {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] hash = digest.digest(cacheScopeKey().getBytes(StandardCharsets.UTF_8));
            StringBuilder builder = new StringBuilder();
            for (int i = 0; i < Math.min(16, hash.length); i++) {
                builder.append(String.format(Locale.ROOT, "%02x", hash[i] & 0xff));
            }
            return builder.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 unavailable", e);
        }
    }

    @NotNull
    private static String clean(@NotNull String value) {
        return value.trim();
    }

    private static boolean isUuid(@NotNull String value) {
        if (value.isBlank()) {
            return false;
        }
        try {
            UUID.fromString(value);
            return true;
        } catch (IllegalArgumentException e) {
            return false;
        }
    }

    @NotNull
    private static List<String> sortedCopy(@NotNull List<String> values) {
        List<String> copy = new ArrayList<>();
        for (String value : values) {
            if (CommonUtils.isNotEmpty(value)) {
                copy.add(value.trim());
            }
        }
        Collections.sort(copy);
        return List.copyOf(copy);
    }

    @NotNull
    private static Set<String> sortedSetCopy(@NotNull Set<String> values) {
        List<String> copy = new ArrayList<>();
        for (String value : values) {
            if (CommonUtils.isNotEmpty(value)) {
                copy.add(value.trim());
            }
        }
        Collections.sort(copy);
        return Collections.unmodifiableSet(new LinkedHashSet<>(copy));
    }

    @NotNull
    private static String shortIdentity(@NotNull String value) {
        return value.length() <= 12 ? value : value.substring(0, 12);
    }

    public static final class Builder {
        private String serverIdentity = "";
        private String routeIdentity = "";
        private String databaseUuid = "";
        private String sessionUuid = "";
        private String authenticatedUserUuid = "";
        private List<String> effectiveRoleUuids = List.of();
        private List<String> effectiveGroupUuids = List.of();
        private String currentSchema = "";
        private String searchPath = "";
        private String exactLanguageProfileTag = "en-US";
        private String databaseDefaultLanguageTag = "en-US";
        private String catalogEpoch = "";
        private String grantEpoch = "";
        private String securityPolicyEpoch = "";
        private String descriptorEpoch = "";
        private String localizedNameEpoch = "";
        private String policyEpoch = "";
        private String resolverPolicy = "default";
        private String resolverSnapshotIdentity = "";
        private String transactionContextId = "";
        private Set<String> capabilities = Set.of();
        private boolean serverAdmitted;

        private Builder() {
        }

        @NotNull
        public Builder serverIdentity(@NotNull String value) {
            this.serverIdentity = value;
            return this;
        }

        @NotNull
        public Builder routeIdentity(@NotNull String value) {
            this.routeIdentity = value;
            return this;
        }

        @NotNull
        public Builder databaseUuid(@NotNull String value) {
            this.databaseUuid = value;
            return this;
        }

        @NotNull
        public Builder sessionUuid(@NotNull String value) {
            this.sessionUuid = value;
            return this;
        }

        @NotNull
        public Builder authenticatedUserUuid(@NotNull String value) {
            this.authenticatedUserUuid = value;
            return this;
        }

        @NotNull
        public Builder effectiveRoleUuids(@NotNull List<String> values) {
            this.effectiveRoleUuids = values;
            return this;
        }

        @NotNull
        public Builder effectiveGroupUuids(@NotNull List<String> values) {
            this.effectiveGroupUuids = values;
            return this;
        }

        @NotNull
        public Builder currentSchema(@NotNull String value) {
            this.currentSchema = value;
            return this;
        }

        @NotNull
        public Builder searchPath(@NotNull String value) {
            this.searchPath = value;
            return this;
        }

        @NotNull
        public Builder languageProfile(@NotNull String exactTag, @NotNull String defaultTag) {
            this.exactLanguageProfileTag = exactTag;
            this.databaseDefaultLanguageTag = defaultTag;
            return this;
        }

        @NotNull
        public Builder epochs(
            @NotNull String catalogEpoch,
            @NotNull String grantEpoch,
            @NotNull String securityPolicyEpoch,
            @NotNull String descriptorEpoch,
            @NotNull String localizedNameEpoch,
            @NotNull String policyEpoch
        ) {
            this.catalogEpoch = catalogEpoch;
            this.grantEpoch = grantEpoch;
            this.securityPolicyEpoch = securityPolicyEpoch;
            this.descriptorEpoch = descriptorEpoch;
            this.localizedNameEpoch = localizedNameEpoch;
            this.policyEpoch = policyEpoch;
            return this;
        }

        @NotNull
        public Builder resolverPolicy(@NotNull String value) {
            this.resolverPolicy = value;
            return this;
        }

        @NotNull
        public Builder resolverSnapshotIdentity(@NotNull String value) {
            this.resolverSnapshotIdentity = value;
            return this;
        }

        @NotNull
        public Builder transactionContextId(@NotNull String value) {
            this.transactionContextId = value;
            return this;
        }

        @NotNull
        public Builder capabilities(@NotNull Set<String> values) {
            this.capabilities = values;
            return this;
        }

        @NotNull
        public Builder serverAdmitted(boolean value) {
            this.serverAdmitted = value;
            return this;
        }

        @NotNull
        public ScratchBirdAuthorizationContext build() {
            return new ScratchBirdAuthorizationContext(
                serverIdentity,
                routeIdentity,
                databaseUuid,
                sessionUuid,
                authenticatedUserUuid,
                effectiveRoleUuids,
                effectiveGroupUuids,
                currentSchema,
                searchPath,
                exactLanguageProfileTag,
                databaseDefaultLanguageTag,
                catalogEpoch,
                grantEpoch,
                securityPolicyEpoch,
                descriptorEpoch,
                localizedNameEpoch,
                policyEpoch,
                resolverPolicy,
                resolverSnapshotIdentity,
                transactionContextId,
                capabilities,
                serverAdmitted);
        }
    }
}
