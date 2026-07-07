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
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdValueProfile;
import org.jkiss.dbeaver.model.data.DBDContent;
import org.jkiss.dbeaver.model.exec.DBCException;
import org.jkiss.dbeaver.model.exec.DBCSession;
import org.jkiss.dbeaver.model.exec.jdbc.JDBCResultSet;
import org.jkiss.dbeaver.model.impl.jdbc.data.JDBCContentChars;
import org.jkiss.dbeaver.model.impl.jdbc.data.handlers.JDBCContentValueHandler;
import org.jkiss.dbeaver.model.struct.DBSTypedObject;

import java.sql.SQLException;

public class ScratchBirdStructuredContentValueHandler extends JDBCContentValueHandler {

    public static final ScratchBirdStructuredContentValueHandler INSTANCE = new ScratchBirdStructuredContentValueHandler();

    @NotNull
    @Override
    public String getValueContentType(@NotNull DBSTypedObject attribute) {
        return ScratchBirdValueProfile.fromTypedObject(attribute).contentTypeOrDefault();
    }

    @Override
    protected DBDContent fetchColumnValue(
        DBCSession session,
        JDBCResultSet resultSet,
        DBSTypedObject type,
        int index
    ) throws SQLException {
        return new ScratchBirdTypedContent(
            session.getExecutionContext(),
            resultSet.getString(index),
            getValueContentType(type));
    }

    @Override
    public DBDContent getValueFromObject(
        @NotNull DBCSession session,
        @NotNull DBSTypedObject type,
        @Nullable Object object,
        boolean copy,
        boolean validateValue
    ) throws DBCException {
        String contentType = getValueContentType(type);
        if (object == null) {
            return new ScratchBirdTypedContent(session.getExecutionContext(), null, contentType);
        }
        if (object instanceof ScratchBirdTypedContent typedContent) {
            return copy ? typedContent.cloneValue(session.getProgressMonitor()) : typedContent;
        }
        if (object instanceof JDBCContentChars chars) {
            Object rawValue = chars.getRawValue();
            return new ScratchBirdTypedContent(
                session.getExecutionContext(),
                rawValue == null ? null : rawValue.toString(),
                contentType);
        }
        if (object instanceof String stringValue) {
            return new ScratchBirdTypedContent(session.getExecutionContext(), stringValue, contentType);
        }
        Object rendered = ScratchBirdStructuredTextValueHandler.INSTANCE.getValueFromObject(
            session,
            type,
            object,
            copy,
            validateValue);
        return new ScratchBirdTypedContent(
            session.getExecutionContext(),
            rendered == null ? null : rendered.toString(),
            contentType);
    }
}
