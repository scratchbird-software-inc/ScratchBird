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

import java.util.List;

public final class ScratchBirdMetricSourceResolver {

    private ScratchBirdMetricSourceResolver() {
    }

    @NotNull
    public static ScratchBirdRefusalModel sourceStatus(@NotNull ScratchBirdReportDefinition report) {
        if (report.futureGated()) {
            return ScratchBirdFeatureBoundaryStatus.unavailable(
                "report:" + report.id(),
                "Future-gated report: backing sys view or management surface is not yet published.",
                String.join(", ", report.sourceSurfaces())).toRefusalModel();
        }
        if (requiresRawHistogram(report.sourceSurfaces())) {
            return ScratchBirdFeatureBoundaryStatus.requiresServerAdmission(
                "report:" + report.id(),
                "Report requires raw histogram family samples; do not derive percentiles from aggregate metrics only.",
                String.join(", ", report.sourceSurfaces())).toRefusalModel();
        }
        return ScratchBirdFeatureBoundaryStatus.requiresServerAdmission(
            "report:" + report.id(),
            "Report may run when the listed server surfaces are visible to the connected principal.",
            String.join(", ", report.sourceSurfaces())).toRefusalModel();
    }

    private static boolean requiresRawHistogram(@NotNull List<String> sources) {
        for (String source : sources) {
            if (source.endsWith("_*")) {
                return true;
            }
        }
        return false;
    }
}
