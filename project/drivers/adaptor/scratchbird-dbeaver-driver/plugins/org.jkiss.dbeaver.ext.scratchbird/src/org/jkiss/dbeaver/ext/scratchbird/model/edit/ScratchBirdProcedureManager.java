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
import org.jkiss.dbeaver.DBException;
import org.jkiss.dbeaver.ext.generic.edit.GenericProcedureManager;
import org.jkiss.dbeaver.ext.generic.model.GenericProcedure;
import org.jkiss.dbeaver.ext.generic.model.GenericStructContainer;
import org.jkiss.dbeaver.model.edit.DBECommandContext;
import org.jkiss.dbeaver.model.edit.DBEPersistAction;
import org.jkiss.dbeaver.model.exec.DBCException;
import org.jkiss.dbeaver.model.exec.DBCExecutionContext;
import org.jkiss.dbeaver.model.impl.edit.SQLDatabasePersistAction;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.dbeaver.model.struct.rdb.DBSProcedureType;

import java.util.List;
import java.util.Map;

public class ScratchBirdProcedureManager extends GenericProcedureManager {

    @Override
    public boolean canCreateObject(@NotNull Object container) {
        if (!ScratchBirdManagerSupport.isScratchBirdContainer(container)) {
            return super.canCreateObject(container);
        }
        return ScratchBirdManagerSupport.canCreateStoredCode(container);
    }

    @Override
    public boolean canDeleteObject(@NotNull GenericProcedure object) {
        if (!ScratchBirdManagerSupport.isScratchBirdDataSource(object.getDataSource())) {
            return super.canDeleteObject(object);
        }
        return ScratchBirdManagerSupport.canDeleteObject(object);
    }

    @Override
    protected String getBaseObjectName() {
        return "new_procedure";
    }

    @Override
    protected GenericProcedure createDatabaseObject(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBECommandContext context,
        Object container,
        Object from,
        @NotNull Map<String, Object> options
    ) throws DBException {
        if (!(container instanceof GenericStructContainer structContainer) ||
            !ScratchBirdManagerSupport.isScratchBirdDataSource(structContainer.getDataSource())) {
            return super.createDatabaseObject(monitor, context, container, from, options);
        }

        GenericProcedure procedure = new GenericProcedure(
            structContainer,
            getNewChildName(monitor, structContainer, getBaseObjectName()),
            null,
            DBSProcedureType.PROCEDURE,
            null,
            false);
        procedure.setSource(ScratchBirdManagerSupport.defaultProcedureSource(procedure));
        return procedure;
    }

    @Override
    protected void addObjectCreateActions(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBCExecutionContext executionContext,
        @NotNull List<DBEPersistAction> actions,
        @NotNull ObjectCreateCommand command,
        @NotNull Map<String, Object> options
    ) throws DBCException {
        if (!ScratchBirdManagerSupport.isScratchBirdDataSource(command.getObject().getDataSource())) {
            super.addObjectCreateActions(monitor, executionContext, actions, command, options);
            return;
        }

        try {
            actions.add(new SQLDatabasePersistAction(
                "Create procedure",
                command.getObject().getObjectDefinitionText(monitor, options)));
        } catch (DBException e) {
            throw new DBCException("Unable to generate ScratchBird procedure DDL", e);
        }
    }
}
