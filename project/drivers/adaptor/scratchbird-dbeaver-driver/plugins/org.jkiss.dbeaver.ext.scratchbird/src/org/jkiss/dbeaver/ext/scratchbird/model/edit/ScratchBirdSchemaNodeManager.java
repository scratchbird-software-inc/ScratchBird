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
package org.jkiss.dbeaver.ext.scratchbird.model.edit;

import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.DBException;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdCatalog;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdNamespaceSemantics;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdObjectPath;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdSchemaNode;
import org.jkiss.dbeaver.model.DBPDataSource;
import org.jkiss.dbeaver.model.DBUtils;
import org.jkiss.dbeaver.model.edit.DBECommandContext;
import org.jkiss.dbeaver.model.edit.DBEPersistAction;
import org.jkiss.dbeaver.model.exec.DBCExecutionContext;
import org.jkiss.dbeaver.model.impl.edit.SQLDatabasePersistAction;
import org.jkiss.dbeaver.model.impl.sql.edit.SQLObjectEditor;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.dbeaver.model.struct.DBSObject;
import org.jkiss.dbeaver.model.struct.cache.DBSObjectCache;

import java.util.List;
import java.util.Map;

public class ScratchBirdSchemaNodeManager extends SQLObjectEditor<ScratchBirdSchemaNode, DBSObject> {

    @Override
    public long getMakerOptions(@NotNull DBPDataSource dataSource) {
        return FEATURE_SAVE_IMMEDIATELY | FEATURE_EDITOR_ON_CREATE;
    }

    @Nullable
    @Override
    public DBSObjectCache<? extends DBSObject, ScratchBirdSchemaNode> getObjectsCache(ScratchBirdSchemaNode object) {
        return null;
    }

    @Override
    public boolean canCreateObject(@NotNull Object container) {
        if (container instanceof ScratchBirdSchemaNode schemaNode) {
            return schemaNode.isCatalogBacked() && !schemaNode.isClientOnly() && !schemaNode.isDomainBranch();
        }
        return container instanceof ScratchBirdCatalog;
    }

    @Override
    public boolean canDeleteObject(@NotNull ScratchBirdSchemaNode object) {
        return object.isCatalogBacked() &&
            !object.isClientOnly() &&
            !object.isScratchBirdSystemPath() &&
            !isCanonicalRoot(object.getFullPath());
    }

    @Override
    protected ScratchBirdSchemaNode createDatabaseObject(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBECommandContext context,
        Object container,
        Object copyFrom,
        @NotNull Map<String, Object> options
    ) throws DBException {
        ScratchBirdCatalog catalog;
        ScratchBirdSchemaNode parentNode = null;
        String parentPath = "";

        if (container instanceof ScratchBirdCatalog scratchBirdCatalog) {
            catalog = scratchBirdCatalog;
        } else if (container instanceof ScratchBirdSchemaNode scratchBirdSchemaNode &&
            scratchBirdSchemaNode.getCatalog() instanceof ScratchBirdCatalog scratchBirdCatalog) {
            catalog = scratchBirdCatalog;
            parentNode = scratchBirdSchemaNode;
            parentPath = scratchBirdSchemaNode.getFullPath();
        } else {
            throw new DBException("ScratchBird schema branches can only be created under ScratchBird catalog/schema nodes.");
        }

        String objectName = "new_branch";
        String fullPath = parentPath.isEmpty() ? objectName : parentPath + "." + objectName;
        return new ScratchBirdSchemaNode(
            catalog.getDataSource(),
            catalog,
            parentNode,
            objectName,
            fullPath,
            true,
            false,
            ScratchBirdObjectPath.fromDisplayPath(fullPath, false),
            "SCHEMA");
    }

    @Override
    protected void addObjectCreateActions(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBCExecutionContext executionContext,
        @NotNull List<DBEPersistAction> actions,
        @NotNull ObjectCreateCommand command,
        @NotNull Map<String, Object> options
    ) {
        actions.add(new SQLDatabasePersistAction(
            "Create ScratchBird schema branch",
            "CREATE SCHEMA " + quotePath(command.getObject())));
    }

    @Override
    protected void addObjectDeleteActions(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBCExecutionContext executionContext,
        @NotNull List<DBEPersistAction> actions,
        @NotNull ObjectDeleteCommand command,
        @NotNull Map<String, Object> options
    ) {
        actions.add(new SQLDatabasePersistAction(
            "Drop ScratchBird schema branch",
            "DROP SCHEMA " + quotePath(command.getObject())));
    }

    @NotNull
    private static String quotePath(@NotNull ScratchBirdSchemaNode object) {
        StringBuilder builder = new StringBuilder();
        for (String segment : object.getFullPath().split("\\.")) {
            if (segment.isEmpty()) {
                continue;
            }
            if (builder.length() > 0) {
                builder.append('.');
            }
            builder.append(DBUtils.getQuotedIdentifier(object.getDataSource(), segment));
        }
        return builder.toString();
    }

    private static boolean isCanonicalRoot(@NotNull String fullPath) {
        return ScratchBirdNamespaceSemantics.getPathDepth(fullPath) == 1 &&
            switch (fullPath) {
                case "sys", "users", "cluster", "emulated", "remote", "data" -> true;
                default -> false;
            };
    }
}
