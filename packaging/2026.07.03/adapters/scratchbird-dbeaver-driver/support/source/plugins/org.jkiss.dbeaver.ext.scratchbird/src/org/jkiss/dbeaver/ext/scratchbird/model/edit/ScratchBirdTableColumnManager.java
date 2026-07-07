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
import org.jkiss.dbeaver.ext.generic.edit.GenericTableColumnManager;
import org.jkiss.dbeaver.ext.generic.model.GenericTableBase;
import org.jkiss.dbeaver.ext.generic.model.GenericTableColumn;
import org.jkiss.dbeaver.model.DBPDataKind;
import org.jkiss.dbeaver.model.edit.DBECommandContext;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;
import org.jkiss.dbeaver.model.struct.DBSDataType;

import java.sql.Types;
import java.util.Map;

public class ScratchBirdTableColumnManager extends GenericTableColumnManager {

    @Override
    public boolean canCreateObject(@NotNull Object container) {
        if (!(container instanceof GenericTableBase tableBase) ||
            !ScratchBirdManagerSupport.isScratchBirdDataSource(tableBase.getDataSource())) {
            return super.canCreateObject(container);
        }
        return ScratchBirdManagerSupport.canCreateRegularSqlObject(tableBase.getContainer()) &&
            super.canCreateObject(container);
    }

    @Override
    public boolean canEditObject(@NotNull GenericTableColumn object) {
        if (!ScratchBirdManagerSupport.isScratchBirdDataSource(object.getDataSource())) {
            return super.canEditObject(object);
        }
        return ScratchBirdManagerSupport.canDeleteObject(object) &&
            super.canEditObject(object);
    }

    @Override
    public boolean canDeleteObject(@NotNull GenericTableColumn object) {
        if (!ScratchBirdManagerSupport.isScratchBirdDataSource(object.getDataSource())) {
            return super.canDeleteObject(object);
        }
        return ScratchBirdManagerSupport.canDeleteObject(object) &&
            super.canDeleteObject(object);
    }

    @Override
    protected GenericTableColumn createDatabaseObject(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBECommandContext context,
        Object container,
        Object copyFrom,
        @NotNull Map<String, Object> options
    ) throws DBException {
        if (!(container instanceof GenericTableBase tableBase) ||
            !ScratchBirdManagerSupport.isScratchBirdDataSource(tableBase.getDataSource()) ||
            !ScratchBirdManagerSupport.isNonRelational(tableBase)) {
            return super.createDatabaseObject(monitor, context, container, copyFrom, options);
        }

        DBSDataType columnType = findBestDataType(tableBase, "JSONB", "JSON", "VARIANT", "OBJECT", "VARCHAR");
        int typeId = columnType == null ? Types.OTHER : columnType.getTypeID();
        String typeName = columnType == null ? "JSONB" : columnType.getName();
        int columnSize = columnType != null && columnType.getDataKind() == DBPDataKind.STRING ? 100 : 0;

        GenericTableColumn column = tableBase.getDataSource().getMetaModel().createTableColumnImpl(
            monitor,
            null,
            tableBase,
            getNewColumnName(monitor, context, tableBase),
            typeName,
            typeId,
            typeId,
            -1,
            columnSize,
            columnSize,
            null,
            null,
            10,
            false,
            null,
            null,
            false,
            false
        );
        column.setPersisted(false);
        return column;
    }
}
