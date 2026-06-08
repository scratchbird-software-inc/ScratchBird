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
import org.jkiss.dbeaver.ext.generic.edit.GenericPrimaryKeyManager;
import org.jkiss.dbeaver.ext.generic.model.GenericTableBase;
import org.jkiss.dbeaver.ext.generic.model.GenericUniqueKey;
import org.jkiss.dbeaver.model.edit.DBECommandContext;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;

import java.util.Map;

public class ScratchBirdConstraintManager extends GenericPrimaryKeyManager {

    @Override
    public boolean canCreateObject(@NotNull Object container) {
        if (!(container instanceof GenericTableBase tableBase) ||
            !ScratchBirdManagerSupport.isScratchBirdDataSource(tableBase.getDataSource())) {
            return super.canCreateObject(container);
        }
        return ScratchBirdManagerSupport.canCreateConstraint(container);
    }

    @Override
    public boolean canEditObject(@NotNull GenericUniqueKey object) {
        if (!ScratchBirdManagerSupport.isScratchBirdDataSource(object.getDataSource())) {
            return super.canEditObject(object);
        }
        return ScratchBirdManagerSupport.canDeleteObject(object) &&
            super.canEditObject(object);
    }

    @Override
    public boolean canDeleteObject(@NotNull GenericUniqueKey object) {
        if (!ScratchBirdManagerSupport.isScratchBirdDataSource(object.getDataSource())) {
            return super.canDeleteObject(object);
        }
        return ScratchBirdManagerSupport.canDeleteObject(object) &&
            super.canDeleteObject(object);
    }

    @Override
    protected GenericUniqueKey createDatabaseObject(
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

        return new ScratchBirdDraftCheckConstraint(
            tableBase,
            getNewChildName(monitor, tableBase, "new_constraint"),
            null,
            "1 = 1");
    }
}
