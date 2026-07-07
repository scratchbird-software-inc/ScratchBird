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
import org.jkiss.dbeaver.DBException;
import org.jkiss.dbeaver.ext.generic.model.GenericDataSource;
import org.jkiss.dbeaver.ext.generic.model.meta.GenericMetaModel;
import org.jkiss.dbeaver.model.DBPDataSourceContainer;
import org.jkiss.dbeaver.model.meta.Association;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.dbeaver.model.sql.SQLDialect;
import org.jkiss.dbeaver.model.struct.DBSObject;

import java.util.Collection;
import java.util.Collections;

public class ScratchBirdDataSource extends GenericDataSource {

    @NotNull
    private final ScratchBirdSessionScope sessionScope;
    private ScratchBirdCatalog syntheticRootCatalog;

    public ScratchBirdDataSource(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBPDataSourceContainer container,
        @NotNull GenericMetaModel metaModel,
        @NotNull SQLDialect dialect
    ) throws DBException {
        super(monitor, container, metaModel, dialect);
        this.sessionScope = ScratchBirdSessionScope.forConnection(container.getId());
    }

    @NotNull
    public ScratchBirdSessionScope getScratchBirdSessionScope() {
        return sessionScope;
    }

    @Association
    public synchronized Collection<ScratchBirdSchemaNode> getSchemaTree(@NotNull DBRProgressMonitor monitor) throws DBException {
        ScratchBirdCatalog rootCatalog = getOrCreateSyntheticRootCatalog();
        Collection<ScratchBirdSchemaNode> schemaTree = rootCatalog.getSchemaTree(monitor);
        return schemaTree == null ? Collections.emptyList() : schemaTree;
    }

    @Override
    public DBSObject refreshObject(@NotNull DBRProgressMonitor monitor) throws DBException {
        syntheticRootCatalog = null;
        return super.refreshObject(monitor);
    }

    @NotNull
    private ScratchBirdCatalog getOrCreateSyntheticRootCatalog() {
        if (syntheticRootCatalog == null) {
            syntheticRootCatalog = new ScratchBirdCatalog(this, getContainer().getActualConnectionConfiguration().getDatabaseName());
        }
        return syntheticRootCatalog;
    }
}
