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
import org.jkiss.dbeaver.ext.generic.edit.GenericForeignKeyManager;
import org.jkiss.dbeaver.ext.generic.model.GenericTableBase;
import org.jkiss.dbeaver.ext.generic.model.GenericTableColumn;
import org.jkiss.dbeaver.ext.generic.model.GenericTableForeignKey;
import org.jkiss.dbeaver.ext.generic.model.GenericTableForeignKeyColumnTable;
import org.jkiss.dbeaver.ext.generic.model.GenericUniqueKey;
import org.jkiss.dbeaver.model.edit.DBECommandContext;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.dbeaver.model.struct.rdb.DBSForeignKeyDeferability;
import org.jkiss.dbeaver.model.struct.rdb.DBSForeignKeyModifyRule;

import java.util.Map;

public class ScratchBirdForeignKeyManager extends GenericForeignKeyManager {

    @Override
    public boolean canCreateObject(@NotNull Object container) {
        if (!(container instanceof GenericTableBase tableBase) ||
            !ScratchBirdManagerSupport.isScratchBirdDataSource(tableBase.getDataSource())) {
            return super.canCreateObject(container);
        }
        return ScratchBirdManagerSupport.canCreateForeignKey(container);
    }

    @Override
    public boolean canEditObject(@NotNull GenericTableForeignKey object) {
        if (!ScratchBirdManagerSupport.isScratchBirdDataSource(object.getDataSource())) {
            return super.canEditObject(object);
        }
        return ScratchBirdManagerSupport.canDeleteObject(object) &&
            super.canEditObject(object);
    }

    @Override
    public boolean canDeleteObject(@NotNull GenericTableForeignKey object) {
        if (!ScratchBirdManagerSupport.isScratchBirdDataSource(object.getDataSource())) {
            return super.canDeleteObject(object);
        }
        return ScratchBirdManagerSupport.canDeleteObject(object) &&
            super.canDeleteObject(object);
    }

    @Override
    protected GenericTableForeignKey createDatabaseObject(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBECommandContext context,
        Object container,
        Object from,
        @NotNull Map<String, Object> options
    ) {
        if (!(container instanceof GenericTableBase tableBase) ||
            !ScratchBirdManagerSupport.isScratchBirdDataSource(tableBase.getDataSource())) {
            return super.createDatabaseObject(monitor, context, container, from, options);
        }

        GenericTableColumn ownColumn = null;
        GenericUniqueKey referencedConstraint = null;
        GenericTableColumn referencedColumn = null;
        try {
            ownColumn = ScratchBirdManagerSupport.firstTableColumn(monitor, tableBase);
            referencedConstraint = ScratchBirdManagerSupport.defaultReferenceConstraint(monitor, tableBase);
            referencedColumn = ScratchBirdManagerSupport.firstConstraintColumn(monitor, referencedConstraint);
        } catch (Exception ignored) {
            // Keep the draft foreign key creatable even when live metadata is incomplete.
        }

        GenericTableForeignKey foreignKey = tableBase.getDataSource().getMetaModel().createTableForeignKeyImpl(
            tableBase,
            getNewChildName(monitor, tableBase, "new_fk"),
            null,
            referencedConstraint,
            DBSForeignKeyModifyRule.NO_ACTION,
            DBSForeignKeyModifyRule.NO_ACTION,
            DBSForeignKeyDeferability.NOT_DEFERRABLE,
            false);
        foreignKey.setReferencedConstraint(referencedConstraint);
        if (ownColumn != null && referencedColumn != null) {
            foreignKey.addColumn(new GenericTableForeignKeyColumnTable(foreignKey, ownColumn, 1, referencedColumn));
        }
        return foreignKey;
    }
}
