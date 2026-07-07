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

public final class ScratchBirdNavigatorActionRegistry {

    public enum Action {
        OPEN,
        NEW,
        PROPERTIES,
        ALTER,
        DELETE,
        TASKS,
        REPORTS,
        REFRESH,
        SOURCE_STATUS
    }

    private ScratchBirdNavigatorActionRegistry() {
    }

    @NotNull
    public static List<Action> actionsFor(@NotNull ScratchBirdSchemaTreeBuilder.Node node) {
        return actionsForPath(node.getFullPath(), node.isClientOnly(), node.isCatalogBacked());
    }

    @NotNull
    public static List<Action> actionsForPath(@NotNull String fullPath, boolean clientOnly, boolean catalogBacked) {
        if (clientOnly || ScratchBirdNamespaceSemantics.isMetricsPath(fullPath)) {
            return List.of(
                Action.OPEN,
                Action.PROPERTIES,
                Action.REPORTS,
                Action.REFRESH,
                Action.SOURCE_STATUS);
        }
        if (ScratchBirdNamespaceSemantics.isDomainPath(fullPath)) {
            return List.of(
                Action.NEW,
                Action.PROPERTIES,
                Action.ALTER,
                Action.DELETE,
                Action.REFRESH);
        }
        if (!catalogBacked) {
            return List.of(Action.PROPERTIES, Action.REFRESH, Action.SOURCE_STATUS);
        }
        return ScratchBirdBranchProfile.forPath(fullPath).actions();
    }
}
