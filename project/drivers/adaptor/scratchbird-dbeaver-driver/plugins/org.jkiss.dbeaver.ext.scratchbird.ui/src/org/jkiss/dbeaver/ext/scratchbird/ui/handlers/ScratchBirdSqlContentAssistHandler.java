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
package org.jkiss.dbeaver.ext.scratchbird.ui.handlers;

import org.eclipse.core.commands.AbstractHandler;
import org.eclipse.core.commands.ExecutionEvent;
import org.eclipse.core.commands.ExecutionException;
import org.eclipse.jface.text.source.SourceViewer;
import org.eclipse.ui.handlers.HandlerUtil;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDataSource;
import org.jkiss.dbeaver.ext.scratchbird.ui.ScratchBirdSqlEditorAddIn;
import org.jkiss.dbeaver.model.DBPDataSource;
import org.jkiss.dbeaver.model.DBPDataSourceContainer;
import org.jkiss.dbeaver.ui.editors.sql.SQLEditor;
import org.jkiss.dbeaver.utils.RuntimeUtils;

public class ScratchBirdSqlContentAssistHandler extends AbstractHandler {

    @Override
    public Object execute(ExecutionEvent event) throws ExecutionException {
        SQLEditor editor = RuntimeUtils.getObjectAdapter(HandlerUtil.getActiveEditor(event), SQLEditor.class);
        if (editor == null || !isScratchBirdEditor(editor)) {
            return null;
        }
        if (!ScratchBirdSqlEditorAddIn.openParserCompletion(editor) && editor.getTextViewer() != null) {
            editor.getTextViewer().doOperation(SourceViewer.CONTENTASSIST_PROPOSALS);
        }
        return null;
    }

    private static boolean isScratchBirdEditor(SQLEditor editor) {
        DBPDataSource dataSource = editor.getDataSource();
        if (dataSource instanceof ScratchBirdDataSource) {
            return true;
        }
        DBPDataSourceContainer container = editor.getDataSourceContainer();
        return container != null && "scratchbird_jdbc".equalsIgnoreCase(container.getDriver().getId());
    }
}
