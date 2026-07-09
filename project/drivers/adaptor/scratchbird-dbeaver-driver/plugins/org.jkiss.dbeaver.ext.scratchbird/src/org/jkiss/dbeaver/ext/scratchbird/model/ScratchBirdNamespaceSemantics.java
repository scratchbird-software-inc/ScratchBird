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
import java.util.stream.Collectors;
import java.util.stream.IntStream;

public final class ScratchBirdNamespaceSemantics {

    public static final String MANAGEMENT_ROOT = ScratchBirdManagementSurfaceCatalog.ROOT_PATH;
    public static final String METRICS_ROOT = ScratchBirdManagementSurfaceCatalog.REPORT_BASE_PATH;

    private static final List<String> ROOT_DISPLAY_ORDER = List.of(
        MANAGEMENT_ROOT,
        "sys",
        "users",
        "app",
        "cluster",
        "emulated",
        "remote",
        "data"
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
        "alerts"
    );

    private static final Map<String, Integer> METRICS_DISPLAY_RANK = IntStream.range(0, METRICS_DISPLAY_ORDER.size())
        .boxed()
        .collect(Collectors.toUnmodifiableMap(METRICS_DISPLAY_ORDER::get, index -> index));

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
        return isManagementReportPath(path);
    }

    public static boolean isManagementPath(String path) {
        if (CommonUtils.isEmpty(path)) {
            return false;
        }
        return ScratchBirdManagementSurfaceCatalog.isManagementPath(path);
    }

    public static boolean isManagementReportPath(String path) {
        if (CommonUtils.isEmpty(path)) {
            return false;
        }
        String normalized = normalize(path);
        return normalized.equals(METRICS_ROOT) || normalized.startsWith(METRICS_ROOT + ".");
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
        return ROOT_DISPLAY_ORDER.size();
    }

    private static int getMetricsDisplayRank(@NotNull String path) {
        if (!isManagementReportPath(path)) {
            return 0;
        }
        String normalized = normalize(path);
        String prefix = METRICS_ROOT + ".";
        if (!normalized.startsWith(prefix)) {
            return -1;
        }
        String rest = normalized.substring(prefix.length());
        int nextSeparator = rest.indexOf('.');
        String branch = nextSeparator < 0 ? rest : rest.substring(0, nextSeparator);
        return METRICS_DISPLAY_RANK.getOrDefault(branch, METRICS_DISPLAY_ORDER.size());
    }

    @NotNull
    private static String normalize(@NotNull String path) {
        return path.toLowerCase(Locale.ENGLISH);
    }
}
