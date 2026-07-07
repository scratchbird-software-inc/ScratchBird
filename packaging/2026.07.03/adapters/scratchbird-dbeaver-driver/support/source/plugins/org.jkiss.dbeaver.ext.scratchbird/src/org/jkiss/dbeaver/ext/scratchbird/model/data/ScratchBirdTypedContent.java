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
package org.jkiss.dbeaver.ext.scratchbird.model.data;

import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.model.exec.DBCExecutionContext;
import org.jkiss.dbeaver.model.impl.jdbc.data.JDBCContentChars;
import org.jkiss.dbeaver.model.runtime.DBRProgressMonitor;

public class ScratchBirdTypedContent extends JDBCContentChars {

    @NotNull
    private final String contentType;

    public ScratchBirdTypedContent(
        @NotNull DBCExecutionContext executionContext,
        @Nullable String data,
        @NotNull String contentType
    ) {
        super(executionContext, data);
        this.contentType = contentType;
    }

    private ScratchBirdTypedContent(@NotNull ScratchBirdTypedContent copyFrom) {
        super(copyFrom);
        this.contentType = copyFrom.contentType;
    }

    @NotNull
    @Override
    public String getContentType() {
        return contentType;
    }

    @Override
    public ScratchBirdTypedContent cloneValue(DBRProgressMonitor monitor) {
        return new ScratchBirdTypedContent(this);
    }
}
