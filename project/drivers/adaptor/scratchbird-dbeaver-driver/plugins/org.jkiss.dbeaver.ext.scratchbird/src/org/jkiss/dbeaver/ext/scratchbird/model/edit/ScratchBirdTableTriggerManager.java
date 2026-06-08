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
import org.jkiss.dbeaver.ext.generic.edit.GenericTriggerManager;
import org.jkiss.dbeaver.ext.generic.model.GenericTableBase;
import org.jkiss.dbeaver.ext.generic.model.GenericTableTrigger;
import org.jkiss.dbeaver.model.edit.DBECommandContext;
import org.jkiss.dbeaver.model.edit.DBEPersistAction;
import org.jkiss.dbeaver.model.exec.DBCExecutionContext;
import org.jkiss.dbeaver.model.impl.edit.SQLDatabasePersistAction;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;

import java.util.Collections;
import java.util.List;
import java.util.Map;

public class ScratchBirdTableTriggerManager extends GenericTriggerManager<GenericTableTrigger> {

    @Override
    public boolean canCreateObject(@NotNull Object container) {
        if (!(container instanceof GenericTableBase tableBase) ||
            !ScratchBirdManagerSupport.isScratchBirdDataSource(tableBase.getDataSource())) {
            return super.canCreateObject(container);
        }
        return ScratchBirdManagerSupport.canCreateTableTrigger(container);
    }

    @Override
    public boolean canDeleteObject(@NotNull GenericTableTrigger object) {
        if (!ScratchBirdManagerSupport.isScratchBirdDataSource(object.getDataSource())) {
            return super.canDeleteObject(object);
        }
        return ScratchBirdManagerSupport.canDeleteObject(object);
    }

    @Override
    protected GenericTableTrigger createDatabaseObject(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBECommandContext context,
        Object container,
        Object copyFrom,
        @NotNull Map<String, Object> options
    ) throws DBException {
        if (!(container instanceof GenericTableBase tableBase) ||
            !ScratchBirdManagerSupport.isScratchBirdDataSource(tableBase.getDataSource())) {
            return super.createDatabaseObject(monitor, context, container, copyFrom, options);
        }

        ScratchBirdDraftTableTrigger trigger = new ScratchBirdDraftTableTrigger(
            tableBase,
            getNewChildName(monitor, tableBase, "new_trigger"),
            null);
        trigger.setSource(ScratchBirdManagerSupport.defaultTableTriggerSource(trigger));
        return trigger;
    }

    @Override
    protected void createOrReplaceTriggerQuery(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBCExecutionContext executionContext,
        @NotNull List<DBEPersistAction> actions,
        @NotNull GenericTableTrigger trigger,
        boolean create
    ) {
        try {
            actions.add(new SQLDatabasePersistAction(
                create ? "Create trigger" : "Replace trigger",
                trigger.getObjectDefinitionText(monitor, Collections.emptyMap())));
        } catch (DBException e) {
            throw new IllegalStateException("Unable to generate ScratchBird trigger DDL", e);
        }
    }
}
