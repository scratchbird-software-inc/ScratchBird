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

import java.util.ArrayList;
import java.util.Collection;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

final class ScratchBirdSchemaTreeBuilder {

    private static final Comparator<Node> NODE_ORDER = (left, right) ->
        ScratchBirdNamespaceSemantics.comparePaths(left.getFullPath(), right.getFullPath());

    private ScratchBirdSchemaTreeBuilder() {
    }

    @NotNull
    static List<Node> build(@NotNull Collection<String> schemaPaths) {
        return build(schemaPaths, false);
    }

    @NotNull
    static List<Node> build(@NotNull Collection<String> schemaPaths, boolean includeClientReportBranches) {
        List<ScratchBirdCatalogObjectReference> references = new ArrayList<>();
        for (String fullPath : schemaPaths) {
            if (CommonUtils.isNotEmpty(fullPath)) {
                references.add(ScratchBirdCatalogObjectReference.syntheticSchema(fullPath));
            }
        }
        return buildFromCatalog(references, includeClientReportBranches);
    }

    @NotNull
    static List<Node> buildFromCatalog(@NotNull Collection<ScratchBirdCatalogObjectReference> catalogObjects) {
        return buildFromCatalog(catalogObjects, false);
    }

    @NotNull
    static List<Node> buildFromCatalog(
        @NotNull Collection<ScratchBirdCatalogObjectReference> catalogObjects,
        boolean includeClientReportBranches
    ) {
        Map<String, Node> nodesByPath = new LinkedHashMap<>();
        List<Node> roots = new ArrayList<>();
        Map<String, ScratchBirdCatalogObjectReference> resolvedCatalogObjects = inferMissingParentUuids(catalogObjects);

        for (ScratchBirdCatalogObjectReference reference : resolvedCatalogObjects.values()) {
            if (ScratchBirdNamespaceSemantics.isMetricsPath(reference.fullPath())) {
                continue;
            }
            addPath(nodesByPath, roots, reference);
        }

        if (includeClientReportBranches) {
            for (String metricsPath : ScratchBirdReportCatalog.metricTreePaths()) {
                addPath(nodesByPath, roots, ScratchBirdCatalogObjectReference.clientOnly(metricsPath, "REPORT"));
            }
        }

        roots.sort(NODE_ORDER);
        return roots;
    }

    @NotNull
    private static Map<String, ScratchBirdCatalogObjectReference> inferMissingParentUuids(
        @NotNull Collection<ScratchBirdCatalogObjectReference> catalogObjects
    ) {
        Map<String, ScratchBirdCatalogObjectReference> byPath = new LinkedHashMap<>();
        Map<String, String> uuidByPath = new LinkedHashMap<>();
        String databaseUuid = null;

        for (ScratchBirdCatalogObjectReference reference : catalogObjects) {
            if (CommonUtils.isEmpty(reference.fullPath())) {
                continue;
            }
            byPath.put(reference.fullPath(), reference);
            if (CommonUtils.isNotEmpty(reference.objectUuid())) {
                uuidByPath.put(reference.fullPath(), reference.objectUuid());
            }
            if (databaseUuid == null && CommonUtils.isNotEmpty(reference.databaseUuid())) {
                databaseUuid = reference.databaseUuid();
            }
        }

        Map<String, ScratchBirdCatalogObjectReference> resolved = new LinkedHashMap<>();
        for (ScratchBirdCatalogObjectReference reference : byPath.values()) {
            if (reference.clientOnly() || CommonUtils.isNotEmpty(reference.parentUuid())) {
                resolved.put(reference.fullPath(), reference);
                continue;
            }

            String parentPath = parentPath(reference.fullPath());
            String parentUuid = CommonUtils.isEmpty(parentPath) ? databaseUuid : uuidByPath.get(parentPath);
            resolved.put(reference.fullPath(), reference.withParentUuid(parentUuid));
        }
        return resolved;
    }

    private static void addPath(
        @NotNull Map<String, Node> nodesByPath,
        @NotNull List<Node> roots,
        @NotNull ScratchBirdCatalogObjectReference reference
    ) {
        Node parent = null;
        StringBuilder pathBuilder = new StringBuilder();
        String fullPath = reference.fullPath();
        List<String> segments = splitPath(fullPath);
        for (int segmentIndex = 0; segmentIndex < segments.size(); segmentIndex++) {
            String segment = segments.get(segmentIndex);
            if (pathBuilder.length() > 0) {
                pathBuilder.append('.');
            }
            pathBuilder.append(segment);
            String currentPath = pathBuilder.toString();

            Node node = nodesByPath.get(currentPath);
            if (node == null) {
                node = new Node(segment, currentPath);
                nodesByPath.put(currentPath, node);
                if (parent == null) {
                    roots.add(node);
                } else {
                    parent.addChild(node);
                }
            }

            boolean terminal = segmentIndex == segments.size() - 1;
            if (terminal) {
                node.mark(reference, true);
            } else {
                node.mark(intermediateReference(currentPath), false);
            }
            parent = node;
        }
    }

    @NotNull
    private static ScratchBirdCatalogObjectReference intermediateReference(@NotNull String currentPath) {
        if (ScratchBirdNamespaceSemantics.isMetricsPath(currentPath)) {
            return ScratchBirdCatalogObjectReference.clientOnly(currentPath, "REPORT_GROUP");
        }
        return ScratchBirdCatalogObjectReference.syntheticSchema(currentPath);
    }

    @NotNull
    private static String parentPath(@NotNull String fullPath) {
        int separator = fullPath.lastIndexOf('.');
        return separator < 0 ? "" : fullPath.substring(0, separator);
    }

    @NotNull
    private static List<String> splitPath(@NotNull String fullPath) {
        List<String> segments = new ArrayList<>();
        for (String segment : fullPath.split("\\.")) {
            if (!segment.isEmpty()) {
                segments.add(segment);
            }
        }
        return segments;
    }

    static final class Node {
        @NotNull
        private final String name;
        @NotNull
        private final String fullPath;
        @NotNull
        private final Map<String, Node> children = new LinkedHashMap<>();
        private boolean terminal;
        private boolean catalogBacked;
        @NotNull
        private ScratchBirdObjectPath objectPath;
        @NotNull
        private String objectType = "SCHEMA";

        private Node(@NotNull String name, @NotNull String fullPath) {
            this.name = name;
            this.fullPath = fullPath;
            this.objectPath = ScratchBirdObjectPath.fromDisplayPath(fullPath, false);
        }

        @NotNull
        String getName() {
            return name;
        }

        @NotNull
        String getFullPath() {
            return fullPath;
        }

        @NotNull
        Collection<Node> getChildren() {
            List<Node> sortedChildren = new ArrayList<>(children.values());
            sortedChildren.sort(NODE_ORDER);
            return sortedChildren;
        }

        boolean isTerminal() {
            return terminal;
        }

        boolean isCatalogBacked() {
            return catalogBacked;
        }

        boolean isClientOnly() {
            return !catalogBacked && ScratchBirdNamespaceSemantics.isMetricsPath(fullPath);
        }

        @NotNull
        ScratchBirdObjectPath getObjectPath() {
            return objectPath;
        }

        @NotNull
        String getObjectType() {
            return objectType;
        }

        void mark(@NotNull ScratchBirdCatalogObjectReference reference, boolean terminal) {
            this.catalogBacked = this.catalogBacked || reference.catalogBacked();
            this.terminal = this.terminal || terminal;
            if (terminal || !objectPath.hasCatalogIdentity()) {
                this.objectPath = ScratchBirdObjectPath.fromCatalogReference(reference);
                this.objectType = reference.objectType();
            }
        }

        void addChild(@NotNull Node child) {
            children.putIfAbsent(child.getName(), child);
        }
    }
}
