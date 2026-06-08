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

import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.stream.Collectors;
import java.util.stream.IntStream;

public final class ScratchBirdNamespaceSemantics {

    public static final String METRICS_ROOT = "metrics";

    private static final List<String> ROOT_DISPLAY_ORDER = List.of(
        "sys",
        "users",
        "cluster",
        "emulated",
        "remote",
        "data",
        METRICS_ROOT
    );

    private static final Map<String, Integer> ROOT_DISPLAY_RANK = IntStream.range(0, ROOT_DISPLAY_ORDER.size())
        .boxed()
        .collect(Collectors.toUnmodifiableMap(ROOT_DISPLAY_ORDER::get, index -> index));

    private static final List<String> METRICS_DISPLAY_ORDER = List.of(
        "health-scorecards",
        "workload-and-sql",
        "sessions-and-transactions",
        "locks-and-contention",
        "storage-buffer-cache",
        "mga-and-gc",
        "scheduler-and-jobs",
        "security-and-auth",
        "listener-and-parser",
        "cluster-and-replication",
        "admin-and-management",
        "alerts",
        "future-gated"
    );

    private static final Map<String, Integer> METRICS_DISPLAY_RANK = IntStream.range(0, METRICS_DISPLAY_ORDER.size())
        .boxed()
        .collect(Collectors.toUnmodifiableMap(METRICS_DISPLAY_ORDER::get, index -> index));

    private static final Set<String> MANAGEMENT_ROOTS = Set.of(
        "cluster",
        "connections",
        "emulated",
        "group",
        "local",
        "nosql",
        "remote"
    );

    private static final Comparator<String> PATH_COMPARATOR = Comparator
        .comparingInt(ScratchBirdNamespaceSemantics::getRootDisplayRank)
        .thenComparingInt(ScratchBirdNamespaceSemantics::getMetricsDisplayRank)
        .thenComparing(ScratchBirdNamespaceSemantics::normalize)
        .thenComparing(Comparator.naturalOrder());

    private ScratchBirdNamespaceSemantics() {
    }

    public static boolean isSystemPath(String path) {
        if (CommonUtils.isEmpty(path)) {
            return false;
        }
        String normalized = normalize(path);
        return normalized.equals("sys") || normalized.startsWith("sys.");
    }

    public static boolean isDomainPath(String path) {
        return normalize(path).equals("sys.domains");
    }

    public static boolean isMetricsPath(String path) {
        return normalize(getRootSegment(path)).equals(METRICS_ROOT);
    }

    public static boolean isManagementPath(String path) {
        if (CommonUtils.isEmpty(path)) {
            return false;
        }
        return MANAGEMENT_ROOTS.contains(normalize(getRootSegment(path)));
    }

    @NotNull
    public static String getRootSegment(@NotNull String path) {
        int separator = path.indexOf('.');
        return separator < 0 ? path : path.substring(0, separator);
    }

    public static int getPathDepth(@NotNull String path) {
        if (path.isEmpty()) {
            return 0;
        }
        int depth = 1;
        for (int i = 0; i < path.length(); i++) {
            if (path.charAt(i) == '.') {
                depth++;
            }
        }
        return depth;
    }

    public static int comparePaths(@NotNull String left, @NotNull String right) {
        return PATH_COMPARATOR.compare(left, right);
    }

    private static int getRootDisplayRank(@NotNull String path) {
        Integer canonicalRank = ROOT_DISPLAY_RANK.get(normalize(getRootSegment(path)));
        if (canonicalRank != null) {
            return canonicalRank;
        }
        if (isManagementPath(path)) {
            return ROOT_DISPLAY_ORDER.size() + 1;
        }
        return ROOT_DISPLAY_ORDER.size();
    }

    private static int getMetricsDisplayRank(@NotNull String path) {
        if (!isMetricsPath(path)) {
            return 0;
        }
        String normalized = normalize(path);
        int firstSeparator = normalized.indexOf('.');
        if (firstSeparator < 0) {
            return -1;
        }
        int nextSeparator = normalized.indexOf('.', firstSeparator + 1);
        String branch = nextSeparator < 0
            ? normalized.substring(firstSeparator + 1)
            : normalized.substring(firstSeparator + 1, nextSeparator);
        return METRICS_DISPLAY_RANK.getOrDefault(branch, METRICS_DISPLAY_ORDER.size());
    }

    @NotNull
    private static String normalize(@NotNull String path) {
        return path.toLowerCase(Locale.ENGLISH);
    }
}
