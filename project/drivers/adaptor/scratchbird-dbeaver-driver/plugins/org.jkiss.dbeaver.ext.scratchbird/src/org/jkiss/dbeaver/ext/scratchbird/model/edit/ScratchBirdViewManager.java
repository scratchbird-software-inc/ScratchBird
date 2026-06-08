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
import org.jkiss.dbeaver.ext.generic.GenericConstants;
import org.jkiss.dbeaver.ext.generic.edit.GenericViewManager;
import org.jkiss.dbeaver.ext.generic.model.GenericStructContainer;
import org.jkiss.dbeaver.ext.generic.model.GenericTableBase;
import org.jkiss.dbeaver.ext.generic.model.GenericView;
import org.jkiss.dbeaver.model.DBPEvaluationContext;
import org.jkiss.dbeaver.model.edit.DBECommandContext;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;

import java.util.Map;

public class ScratchBirdViewManager extends GenericViewManager {

    @Override
    public boolean canCreateObject(@NotNull Object container) {
        if (!ScratchBirdManagerSupport.isScratchBirdContainer(container)) {
            return super.canCreateObject(container);
        }
        return ScratchBirdManagerSupport.canCreateRegularSqlObject(container);
    }

    @Override
    public boolean canDeleteObject(@NotNull GenericTableBase object) {
        if (!ScratchBirdManagerSupport.isScratchBirdDataSource(object.getDataSource())) {
            return super.canDeleteObject(object);
        }
        return ScratchBirdManagerSupport.canDeleteObject(object);
    }

    @Override
    protected GenericTableBase createDatabaseObject(
        @NotNull DBRProgressMonitor monitor,
        @NotNull DBECommandContext context,
        Object container,
        Object copyFrom,
        @NotNull Map<String, Object> options
    ) {
        if (!(container instanceof GenericStructContainer structContainer) ||
            !ScratchBirdManagerSupport.isScratchBirdDataSource(structContainer.getDataSource())) {
            return super.createDatabaseObject(monitor, context, container, copyFrom, options);
        }

        String viewName = getNewChildName(monitor, structContainer, "new_view");
        GenericTableBase viewImpl = structContainer.getDataSource().getMetaModel().createTableOrViewImpl(
            structContainer,
            viewName,
            GenericConstants.TABLE_TYPE_VIEW,
            null);
        if (viewImpl instanceof GenericView view) {
            view.setObjectDefinitionText(
                "CREATE VIEW " + view.getFullyQualifiedName(DBPEvaluationContext.DDL) + " AS SELECT 1 AS sample\n");
        }
        return viewImpl;
    }
}
