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

import java.time.Instant;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

public final class ScratchBirdAuthorizedResolverCache {

    public static final String DIRECTION_PATH_TO_UUID = "path_to_uuid";
    public static final String DIRECTION_UUID_TO_PATH = "uuid_to_path";

    private final Map<Key, Entry> entries = new LinkedHashMap<>();

    public enum Status {
        AUTHORIZED,
        NOT_DISCLOSED,
        REFUSED
    }

    public record Key(
        @NotNull String direction,
        @NotNull String authorizationFingerprint,
        @NotNull String parentScope,
        @NotNull String objectClass,
        @NotNull String lookupMode,
        @NotNull String normalizedLookupKey,
        @NotNull String resolverSnapshotIdentity
    ) {
    }

    public record Entry(
        @NotNull Key key,
        @NotNull Status status,
        @Nullable ScratchBirdCatalogObjectReference reference,
        @NotNull ScratchBirdFeatureBoundaryStatus boundary,
        @NotNull String cachedAt
    ) {
    }

    @NotNull
    public synchronized Entry putAuthorizedReference(
        @NotNull ScratchBirdAuthorizationContext context,
        @NotNull ScratchBirdCatalogObjectReference reference,
        @NotNull String parentScope,
        @NotNull String lookupMode
    ) {
        ScratchBirdFeatureBoundaryStatus boundary = context.resolverBoundary();
        if (!context.canUseResolverCache()) {
            return refused(context, parentScope, reference.objectType(), lookupMode, reference.fullPath(), boundary);
        }
        Entry pathEntry = authorized(pathKey(context, parentScope, reference.objectType(), lookupMode, reference.fullPath()), reference);
        entries.put(pathEntry.key(), pathEntry);
        if (CommonUtils.isNotEmpty(reference.objectUuid())) {
            Entry uuidEntry = authorized(uuidKey(context, parentScope, reference.objectType(), lookupMode, reference.objectUuid()), reference);
            entries.put(uuidEntry.key(), uuidEntry);
        }
        return pathEntry;
    }

    @NotNull
    public synchronized Entry resolvePath(
        @NotNull ScratchBirdAuthorizationContext context,
        @NotNull String path,
        @NotNull String parentScope,
        @NotNull String objectClass,
        @NotNull String lookupMode
    ) {
        if (!context.canUseResolverCache()) {
            return refused(context, parentScope, objectClass, lookupMode, path, context.resolverBoundary());
        }
        Key key = pathKey(context, parentScope, objectClass, lookupMode, path);
        Entry entry = entries.get(key);
        return entry == null ? notDisclosed(key) : entry;
    }

    @NotNull
    public synchronized Entry resolveUuid(
        @NotNull ScratchBirdAuthorizationContext context,
        @NotNull String objectUuid,
        @NotNull String parentScope,
        @NotNull String objectClass,
        @NotNull String lookupMode
    ) {
        if (!context.canUseResolverCache()) {
            return refused(uuidKey(context, parentScope, objectClass, lookupMode, objectUuid), context.resolverBoundary());
        }
        Key key = uuidKey(context, parentScope, objectClass, lookupMode, objectUuid);
        Entry entry = entries.get(key);
        return entry == null ? notDisclosed(key) : entry;
    }

    public synchronized void invalidate() {
        entries.clear();
    }

    public synchronized int size() {
        return entries.size();
    }

    @NotNull
    private static Entry authorized(@NotNull Key key, @NotNull ScratchBirdCatalogObjectReference reference) {
        return new Entry(
            key,
            Status.AUTHORIZED,
            reference,
            ScratchBirdFeatureBoundaryStatus.available(
                "authorized_path_resolver",
                "Resolver entry came from an authorization-filtered server result for this session context.",
                "sys.catalog.object_resolver"),
            Instant.now().toString());
    }

    @NotNull
    private static Entry notDisclosed(@NotNull Key key) {
        return new Entry(
            key,
            Status.NOT_DISCLOSED,
            null,
            ScratchBirdFeatureBoundaryStatus.hiddenOrMissing(
                "authorized_path_resolver",
                "No authorized resolver entry is available; hidden and missing objects are intentionally indistinguishable.",
                "sys.catalog.object_resolver"),
            Instant.now().toString());
    }

    @NotNull
    private static Entry refused(
        @NotNull ScratchBirdAuthorizationContext context,
        @NotNull String parentScope,
        @NotNull String objectClass,
        @NotNull String lookupMode,
        @NotNull String lookupKey,
        @NotNull ScratchBirdFeatureBoundaryStatus boundary
    ) {
        return refused(pathKey(context, parentScope, objectClass, lookupMode, lookupKey), boundary);
    }

    @NotNull
    private static Entry refused(
        @NotNull Key key,
        @NotNull ScratchBirdFeatureBoundaryStatus boundary
    ) {
        return new Entry(
            key,
            Status.REFUSED,
            null,
            boundary,
            Instant.now().toString());
    }

    @NotNull
    private static Key pathKey(
        @NotNull ScratchBirdAuthorizationContext context,
        @NotNull String parentScope,
        @NotNull String objectClass,
        @NotNull String lookupMode,
        @NotNull String path
    ) {
        return key(context, DIRECTION_PATH_TO_UUID, parentScope, objectClass, lookupMode, path);
    }

    @NotNull
    private static Key uuidKey(
        @NotNull ScratchBirdAuthorizationContext context,
        @NotNull String parentScope,
        @NotNull String objectClass,
        @NotNull String lookupMode,
        @NotNull String objectUuid
    ) {
        return key(context, DIRECTION_UUID_TO_PATH, parentScope, objectClass, lookupMode, objectUuid);
    }

    @NotNull
    private static Key key(
        @NotNull ScratchBirdAuthorizationContext context,
        @NotNull String direction,
        @NotNull String parentScope,
        @NotNull String objectClass,
        @NotNull String lookupMode,
        @NotNull String lookupKey
    ) {
        return new Key(
            direction,
            context.fingerprint(),
            normalize(parentScope),
            normalize(objectClass),
            normalize(lookupMode),
            normalize(lookupKey),
            context.resolverSnapshotIdentity());
    }

    @NotNull
    private static String normalize(@NotNull String value) {
        return value.trim().toLowerCase(Locale.ROOT);
    }
}
