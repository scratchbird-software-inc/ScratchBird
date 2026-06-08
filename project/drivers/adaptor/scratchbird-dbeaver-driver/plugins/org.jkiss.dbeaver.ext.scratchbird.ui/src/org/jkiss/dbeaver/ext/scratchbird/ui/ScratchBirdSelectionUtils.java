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

import org.eclipse.jface.viewers.ISelection;
import org.eclipse.jface.viewers.IStructuredSelection;
import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDataSource;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdNavigatorActionRegistry;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdObjectFormContext;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdSchemaNode;
import org.jkiss.dbeaver.model.DBPDataSource;
import org.jkiss.dbeaver.model.navigator.DBNDatabaseNode;
import org.jkiss.dbeaver.model.navigator.DBNNode;
import org.jkiss.dbeaver.model.struct.DBSObject;

import java.util.List;

public final class ScratchBirdSelectionUtils {

    private ScratchBirdSelectionUtils() {
    }

    @Nullable
    public static DBSObject getSelectedObject(@Nullable ISelection selection) {
        if (selection instanceof IStructuredSelection structuredSelection && structuredSelection.size() == 1) {
            return getDatabaseObject(structuredSelection.getFirstElement());
        }
        return null;
    }

    @Nullable
    public static DBSObject getDatabaseObject(@Nullable Object element) {
        if (element instanceof DBSObject object) {
            return object;
        }
        if (element instanceof DBNDatabaseNode databaseNode) {
            return databaseNode.getObject();
        }
        if (element instanceof DBNNode node && node.getParentNode() instanceof DBNDatabaseNode databaseNode) {
            return databaseNode.getObject();
        }
        return null;
    }

    public static boolean isScratchBirdObject(@Nullable Object element) {
        DBSObject object = getDatabaseObject(element);
        if (object instanceof ScratchBirdSchemaNode) {
            return true;
        }
        if (object != null) {
            DBPDataSource dataSource = object.getDataSource();
            return dataSource instanceof ScratchBirdDataSource;
        }
        return false;
    }

    public static boolean supportsAction(@Nullable Object element, @NotNull ScratchBirdNavigatorActionRegistry.Action action) {
        DBSObject object = getDatabaseObject(element);
        if (object instanceof ScratchBirdSchemaNode schemaNode) {
            List<ScratchBirdNavigatorActionRegistry.Action> actions = ScratchBirdNavigatorActionRegistry.actionsForPath(
                schemaNode.getFullPath(),
                schemaNode.isClientOnly(),
                schemaNode.isCatalogBacked());
            return actions.contains(action);
        }
        return isScratchBirdObject(object);
    }

    @NotNull
    public static String displayPath(@NotNull DBSObject object) {
        return ScratchBirdObjectFormContext.displayPath(object);
    }
}
