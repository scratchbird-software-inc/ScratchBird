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
import org.jkiss.dbeaver.ext.generic.edit.GenericTableManager;
import org.jkiss.dbeaver.ext.generic.model.GenericTableBase;

public class ScratchBirdTableManager extends GenericTableManager {

    @Override
    public boolean canCreateObject(@NotNull Object container) {
        if (!ScratchBirdManagerSupport.isScratchBirdContainer(container)) {
            return super.canCreateObject(container);
        }
        return ScratchBirdManagerSupport.canCreateRegularSqlObject(container) &&
            super.canCreateObject(container);
    }

    @Override
    public boolean canDeleteObject(@NotNull GenericTableBase object) {
        if (!ScratchBirdManagerSupport.isScratchBirdDataSource(object.getDataSource())) {
            return super.canDeleteObject(object);
        }
        return ScratchBirdManagerSupport.canDeleteObject(object) &&
            super.canDeleteObject(object);
    }
}
