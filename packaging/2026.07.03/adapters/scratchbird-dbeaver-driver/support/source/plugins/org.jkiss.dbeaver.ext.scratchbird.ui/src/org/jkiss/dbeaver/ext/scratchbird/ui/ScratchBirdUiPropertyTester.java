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
package org.jkiss.dbeaver.ext.scratchbird.ui;

import org.eclipse.core.expressions.PropertyTester;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDataSource;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdNavigatorActionRegistry;
import org.jkiss.dbeaver.model.DBPDataSource;
import org.jkiss.dbeaver.model.DBPDataSourceContainer;
import org.jkiss.dbeaver.ui.editors.sql.SQLEditor;

public class ScratchBirdUiPropertyTester extends PropertyTester {

    @Override
    public boolean test(Object receiver, String property, Object[] args, Object expectedValue) {
        return switch (property) {
            case "canManage" -> ScratchBirdSelectionUtils.isScratchBirdObject(receiver);
            case "canOpen" -> ScratchBirdSelectionUtils.supportsAction(receiver, ScratchBirdNavigatorActionRegistry.Action.OPEN);
            case "canNew" -> ScratchBirdSelectionUtils.supportsAction(receiver, ScratchBirdNavigatorActionRegistry.Action.NEW);
            case "canAlter" -> ScratchBirdSelectionUtils.supportsAction(receiver, ScratchBirdNavigatorActionRegistry.Action.ALTER);
            case "canDelete" -> ScratchBirdSelectionUtils.supportsAction(receiver, ScratchBirdNavigatorActionRegistry.Action.DELETE);
            case "canTasks" -> ScratchBirdSelectionUtils.supportsAction(receiver, ScratchBirdNavigatorActionRegistry.Action.TASKS);
            case "canReports" -> ScratchBirdSelectionUtils.supportsAction(receiver, ScratchBirdNavigatorActionRegistry.Action.REPORTS);
            case "canSourceStatus" -> ScratchBirdSelectionUtils.supportsAction(receiver, ScratchBirdNavigatorActionRegistry.Action.SOURCE_STATUS);
            case "canValidateSql" -> isScratchBirdSqlEditor(receiver);
            default -> false;
        };
    }

    private static boolean isScratchBirdSqlEditor(Object receiver) {
        if (!(receiver instanceof SQLEditor editor)) {
            return false;
        }
        DBPDataSource dataSource = editor.getDataSource();
        if (dataSource instanceof ScratchBirdDataSource) {
            return true;
        }
        DBPDataSourceContainer container = editor.getDataSourceContainer();
        return container != null && "scratchbird_jdbc".equalsIgnoreCase(container.getDriver().getId());
    }
}
