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
import org.jkiss.dbeaver.ext.generic.model.GenericDataType;
import org.jkiss.dbeaver.ext.generic.model.GenericStructContainer;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDomainDataType;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdNamespaceSemantics;
import org.jkiss.dbeaver.model.DBPDataSource;
import org.jkiss.dbeaver.model.edit.DBECommandContext;
import org.jkiss.dbeaver.model.edit.DBEPersistAction;
import org.jkiss.dbeaver.model.exec.DBCExecutionContext;
import org.jkiss.dbeaver.model.impl.edit.SQLDatabasePersistAction;
import org.jkiss.dbeaver.model.impl.sql.edit.SQLObjectEditor;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.dbeaver.model.struct.DBSObject;
import org.jkiss.dbeaver.model.struct.cache.DBSObjectCache;
import org.jkiss.utils.CommonUtils;

import java.util.List;
import java.util.Map;

public class ScratchBirdDomainManager extends SQLObjectEditor<GenericDataType, GenericStructContainer> {

    @Override
    public long getMakerOptions(@NotNull DBPDataSource dataSource) {
        return FEATURE_EDITOR_ON_CREATE;
    }

    @Override
    public boolean canCreateObject(@NotNull Object container) {
        return ScratchBirdManagerSupport.canCreateDomain(container);
    }

    @Override
    public boolean canEditObject(@NotNull GenericDataType object) {
        if (!ScratchBirdManagerSupport.isScratchBirdDataSource(object.getDataSource())) {
            return false;
        }
        String path = ScratchBirdManagerSupport.objectPath(object);
        return path != null && ScratchBirdNamespaceSemantics.isDomainPath(path);
    }

    @Override
    public boolean canDeleteObject(@NotNull GenericDataType object) {
        return canEditObject(object) && ScratchBirdManagerSupport.canDeleteObject(object);
    }

    @Nullable
    @Override
    public DBSObjectCache<? extends DBSObject, GenericDataType> getObjectsCache(GenericDataType object) {
        return null;
    }

    @Override
    protected String getBaseObjectName() {
        return "new_domain";
    }

    @Override
    protected GenericDataType createDatabaseObject(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBECommandContext context,
        Object container,
        Object copyFrom,
        @NotNull Map<String, Object> options
    ) throws DBException {
        if (!(container instanceof GenericStructContainer structContainer)) {
            throw new DBException("ScratchBird domains can only be created inside sys.domains.");
        }
        return new ScratchBirdDomainDataType(
            structContainer,
            getNewChildName(monitor, structContainer, getBaseObjectName()),
            "VARCHAR(255)");
    }

    @Override
    protected void addObjectCreateActions(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBCExecutionContext executionContext,
        @NotNull List<DBEPersistAction> actions,
        @NotNull ObjectCreateCommand command,
        @NotNull Map<String, Object> options
    ) {
        GenericDataType domain = command.getObject();
        String baseTypeName = domain instanceof ScratchBirdDomainDataType scratchBirdDomain
            ? scratchBirdDomain.getBaseTypeName()
            : "VARCHAR(255)";
        StringBuilder ddl = new StringBuilder(128)
            .append("CREATE DOMAIN ")
            .append(ScratchBirdManagerSupport.qualifiedName(domain))
            .append(" AS ")
            .append(baseTypeName);

        if (domain instanceof ScratchBirdDomainDataType scratchBirdDomain) {
            if (scratchBirdDomain.isRequired()) {
                ddl.append(" NOT NULL");
            }
            if (!CommonUtils.isEmpty(scratchBirdDomain.getDefaultExpression())) {
                ddl.append(" DEFAULT ").append(scratchBirdDomain.getDefaultExpression());
            }
        }

        actions.add(new SQLDatabasePersistAction("Create domain", ddl.toString()));
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
            "Drop domain",
            "DROP DOMAIN " + ScratchBirdManagerSupport.qualifiedName(command.getObject())));
    }
}
