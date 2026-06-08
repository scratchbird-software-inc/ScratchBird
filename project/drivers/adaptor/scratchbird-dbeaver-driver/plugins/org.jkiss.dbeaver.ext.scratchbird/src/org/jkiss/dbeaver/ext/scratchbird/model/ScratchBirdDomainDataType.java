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
import org.jkiss.dbeaver.ext.generic.model.GenericDataType;
import org.jkiss.dbeaver.ext.generic.model.GenericStructContainer;
import org.jkiss.dbeaver.model.DBPSaveableObject;
import org.jkiss.dbeaver.model.meta.Property;

import java.sql.Types;

public class ScratchBirdDomainDataType extends GenericDataType implements DBPSaveableObject {

    private boolean persisted;
    @NotNull
    private String baseTypeName;
    private boolean required;
    @Nullable
    private String defaultExpression;

    public ScratchBirdDomainDataType(
        @NotNull GenericStructContainer owner,
        @NotNull String name,
        @NotNull String baseTypeName
    ) {
        super(owner, Types.VARCHAR, name, null, false, true, 255, 0, 0);
        this.persisted = false;
        this.baseTypeName = baseTypeName;
    }

    @Override
    public boolean isPersisted() {
        return persisted;
    }

    @Override
    public void setPersisted(boolean persisted) {
        this.persisted = persisted;
    }

    @NotNull
    @Property(viewable = true, editable = true, order = 30)
    public String getBaseTypeName() {
        return baseTypeName;
    }

    public void setBaseTypeName(@NotNull String baseTypeName) {
        this.baseTypeName = baseTypeName;
    }

    @Property(viewable = true, editable = true, order = 31)
    public boolean isRequired() {
        return required;
    }

    public void setRequired(boolean required) {
        this.required = required;
    }

    @Nullable
    @Property(viewable = true, editable = true, order = 32)
    public String getDefaultExpression() {
        return defaultExpression;
    }

    public void setDefaultExpression(@Nullable String defaultExpression) {
        this.defaultExpression = defaultExpression;
    }
}
