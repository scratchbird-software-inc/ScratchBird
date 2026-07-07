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
import org.jkiss.dbeaver.ext.generic.model.GenericSchema;
import org.jkiss.dbeaver.ext.generic.model.GenericStructContainer;
import org.jkiss.dbeaver.ext.generic.model.GenericView;
import org.jkiss.dbeaver.model.DBPEvaluationContext;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCResultSet;
import org.jkiss.dbeaver.model.meta.Property;
import org.jkiss.utils.CommonUtils;

public class ScratchBirdView extends GenericView {

    @Nullable
    private final ScratchBirdObjectPath objectPath;

    public ScratchBirdView(
        @NotNull GenericStructContainer container,
        @Nullable String tableName,
        @Nullable String tableType,
        @Nullable JDBCResultSet dbResult
    ) {
        super(container, tableName, tableType, dbResult);
        this.objectPath = null;
    }

    public ScratchBirdView(
        @NotNull GenericStructContainer container,
        @Nullable String tableName,
        @Nullable String tableType,
        @Nullable JDBCResultSet dbResult,
        @Nullable ScratchBirdObjectPath objectPath
    ) {
        super(container, tableName, tableType, dbResult);
        this.objectPath = objectPath;
    }

    @Override
    public @NotNull String getFullyQualifiedName(@NotNull DBPEvaluationContext context) {
        if (objectPath != null && CommonUtils.isNotEmpty(objectPath.authorityPath())) {
            return ScratchBirdQualifiedNames.qualifyAuthorityPath(objectPath.authorityPath());
        }
        return ScratchBirdQualifiedNames.qualify((ScratchBirdDataSource) getDataSource(), getSchema(), this);
    }

    @NotNull
    @Property(viewable = true, order = 21)
    public String getAuthorityPath() {
        if (objectPath != null && CommonUtils.isNotEmpty(objectPath.authorityPath())) {
            return objectPath.authorityPath();
        }
        GenericSchema schema = getSchema();
        return ScratchBirdQualifiedNames.joinPath(schema == null ? null : schema.getName(), getName());
    }

    @NotNull
    @Property(viewable = true, order = 22)
    public String getAuthoritySchemaPath() {
        return ScratchBirdQualifiedNames.parentPath(getAuthorityPath());
    }

    @Nullable
    @Property(viewable = true, optional = true, order = 23)
    public String getDatabaseUuid() {
        return objectPath == null ? null : objectPath.databaseUuid();
    }

    @Nullable
    @Property(viewable = true, optional = true, order = 24)
    public String getObjectUuid() {
        return objectPath == null ? null : objectPath.objectUuid();
    }

    @Nullable
    @Property(viewable = true, optional = true, order = 25)
    public String getParentUuid() {
        return objectPath == null ? null : objectPath.parentUuid();
    }

    @NotNull
    @Property(viewable = true, order = 26)
    public String getIdentityStatus() {
        return objectPath == null
            ? "server UUID metadata not published by current JDBC metadata surface"
            : objectPath.identityStatus();
    }

    @Override
    public boolean isSystem() {
        return false;
    }
}
