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
import org.eclipse.jface.dialogs.IDialogConstants;
import org.eclipse.jface.dialogs.MessageDialog;
import org.eclipse.jface.dialogs.TitleAreaDialog;
import org.eclipse.jface.text.IDocument;
import org.eclipse.jface.text.ITextSelection;
import org.eclipse.jface.viewers.ISelection;
import org.eclipse.jface.viewers.ISelectionProvider;
import org.eclipse.swt.SWT;
import org.eclipse.swt.dnd.Clipboard;
import org.eclipse.swt.dnd.TextTransfer;
import org.eclipse.swt.dnd.Transfer;
import org.eclipse.swt.layout.GridData;
import org.eclipse.swt.layout.GridLayout;
import org.eclipse.swt.widgets.Button;
import org.eclipse.swt.widgets.Composite;
import org.eclipse.swt.widgets.Control;
import org.eclipse.swt.widgets.Shell;
import org.eclipse.swt.widgets.Text;
import org.eclipse.ui.handlers.HandlerUtil;
import org.jkiss.code.NotNull;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDataSource;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdValidationBridge;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Completion;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Parser;
import org.jkiss.dbeaver.model.DBPDataSource;
import org.jkiss.dbeaver.model.DBPDataSourceContainer;
import org.jkiss.dbeaver.model.sql.SQLScriptElement;
import org.jkiss.dbeaver.ui.editors.sql.SQLEditor;
import org.jkiss.dbeaver.utils.RuntimeUtils;

import java.util.List;

public class ScratchBirdValidateSqlHandler extends AbstractHandler {

    private static final int COPY_REPORT_ID = IDialogConstants.CLIENT_ID + 10;
    private static final int MAX_COMPLETIONS = 40;

    @Override
    public Object execute(ExecutionEvent event) throws ExecutionException {
        Shell shell = HandlerUtil.getActiveShell(event);
        SQLEditor editor = RuntimeUtils.getObjectAdapter(HandlerUtil.getActiveEditor(event), SQLEditor.class);
        if (editor == null) {
            return null;
        }
        if (!isScratchBirdEditor(editor)) {
            MessageDialog.openInformation(shell, "ScratchBird", "Open a ScratchBird SQL editor first.");
            return null;
        }

        ValidationTarget target = extractTarget(editor);
        if (target.sql().isBlank()) {
            MessageDialog.openInformation(shell, "ScratchBird v3 SQL Validation", "No SQL text is available to validate.");
            return null;
        }

        List<String> diagnostics = ScratchBirdValidationBridge.diagnosticsFor(target.sql());
        List<ScratchBirdV3Completion> completions = ScratchBirdV3Parser.completionsAt(target.sql(), target.caretOffset());
        new ScratchBirdSqlValidationDialog(shell, target, diagnostics, completions).open();
        return null;
    }

    private static boolean isScratchBirdEditor(@NotNull SQLEditor editor) {
        DBPDataSource dataSource = editor.getDataSource();
        if (dataSource instanceof ScratchBirdDataSource) {
            return true;
        }
        DBPDataSourceContainer container = editor.getDataSourceContainer();
        return container != null && "scratchbird_jdbc".equalsIgnoreCase(container.getDriver().getId());
    }

    @NotNull
    private static ValidationTarget extractTarget(@NotNull SQLEditor editor) {
        ITextSelection textSelection = getTextSelection(editor);
        if (textSelection != null && textSelection.getLength() > 0) {
            return new ValidationTarget("Selection", textSelection.getText(), textSelection.getText().length());
        }

        SQLScriptElement activeQuery = editor.extractActiveQuery();
        if (activeQuery != null && activeQuery.getLength() > 0) {
            int caretOffset = activeQuery.getText().length();
            if (textSelection != null) {
                caretOffset = textSelection.getOffset() - activeQuery.getOffset();
            }
            return new ValidationTarget(
                "Active statement",
                activeQuery.getText(),
                clamp(caretOffset, 0, activeQuery.getText().length()));
        }

        IDocument document = editor.getDocument();
        if (document == null) {
            return new ValidationTarget("Editor document", "", 0);
        }
        int caretOffset = textSelection == null ? document.getLength() : textSelection.getOffset();
        return new ValidationTarget("Editor document", document.get(), clamp(caretOffset, 0, document.getLength()));
    }

    private static ITextSelection getTextSelection(@NotNull SQLEditor editor) {
        ISelectionProvider provider = editor.getSelectionProvider();
        if (provider == null) {
            return null;
        }
        ISelection selection = provider.getSelection();
        return selection instanceof ITextSelection textSelection ? textSelection : null;
    }

    private static int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }

    private record ValidationTarget(@NotNull String scope, @NotNull String sql, int caretOffset) {
    }

    private static class ScratchBirdSqlValidationDialog extends TitleAreaDialog {

        @NotNull
        private final ValidationTarget target;
        @NotNull
        private final List<String> diagnostics;
        @NotNull
        private final List<ScratchBirdV3Completion> completions;
        private String reportText;

        ScratchBirdSqlValidationDialog(
            Shell parentShell,
            @NotNull ValidationTarget target,
            @NotNull List<String> diagnostics,
            @NotNull List<ScratchBirdV3Completion> completions
        ) {
            super(parentShell);
            this.target = target;
            this.diagnostics = diagnostics;
            this.completions = completions;
        }

        @Override
        public void create() {
            super.create();
            setTitle("ScratchBird v3 SQL Validation");
            setMessage("Local Java v3 parser diagnostics are advisory; execution and permissions remain server-authoritative.");
        }

        @Override
        protected Control createDialogArea(Composite parent) {
            Composite area = (Composite) super.createDialogArea(parent);
            Composite container = new Composite(area, SWT.NONE);
            container.setLayoutData(new GridData(SWT.FILL, SWT.FILL, true, true));
            container.setLayout(new GridLayout(1, false));

            reportText = buildReport();
            Text report = new Text(container, SWT.BORDER | SWT.MULTI | SWT.READ_ONLY | SWT.V_SCROLL | SWT.H_SCROLL);
            report.setText(reportText);
            GridData data = new GridData(SWT.FILL, SWT.FILL, true, true);
            data.widthHint = 760;
            data.heightHint = 460;
            report.setLayoutData(data);
            return area;
        }

        @Override
        protected void createButtonsForButtonBar(Composite parent) {
            Button copyReport = createButton(parent, COPY_REPORT_ID, "Copy Report", false);
            copyReport.setToolTipText("Copy the ScratchBird v3 validation report.");
            createButton(parent, IDialogConstants.OK_ID, IDialogConstants.OK_LABEL, true);
        }

        @Override
        protected void buttonPressed(int buttonId) {
            if (buttonId == COPY_REPORT_ID) {
                copyToClipboard(reportText == null ? buildReport() : reportText);
                setMessage("ScratchBird v3 validation report copied to clipboard.");
                return;
            }
            super.buttonPressed(buttonId);
        }

        @NotNull
        private String buildReport() {
            StringBuilder report = new StringBuilder();
            report.append("ScratchBird v3 SQL Validation\n");
            appendLine(report, "Scope", target.scope());
            appendLine(report, "Caret offset", Integer.toString(target.caretOffset()));
            appendSection(report, "Statement inventory", ScratchBirdValidationBridge.statementSummaryFor(target.sql()));
            appendSection(report, "Diagnostics", diagnostics);
            appendSection(report, "Lint hints", ScratchBirdValidationBridge.lintHintsFor(target.sql(), target.caretOffset()));
            appendSection(report, "Context hints", ScratchBirdValidationBridge.contextHintsFor(target.sql(), target.caretOffset()));
            appendSection(report, "Server probe hints", ScratchBirdValidationBridge.serverProbeHintsFor(target.sql(), target.caretOffset()));
            appendSection(report, "Form hints", ScratchBirdValidationBridge.formHintsFor(target.sql(), target.caretOffset()));
            appendCompletions(report);
            report.append("SQL:\n");
            report.append(target.sql()).append('\n');
            return report.toString();
        }

        private void appendCompletions(@NotNull StringBuilder report) {
            report.append("Context completions:\n");
            if (completions.isEmpty()) {
                report.append("- -\n");
                return;
            }
            int count = Math.min(completions.size(), MAX_COMPLETIONS);
            for (int index = 0; index < count; index++) {
                ScratchBirdV3Completion completion = completions.get(index);
                report.append("- ").append(completion.label());
                if (!completion.detail().isBlank()) {
                    report.append(" - ").append(completion.detail());
                }
                report.append('\n');
            }
            if (completions.size() > MAX_COMPLETIONS) {
                report.append("- ... ").append(completions.size() - MAX_COMPLETIONS).append(" more\n");
            }
        }

        private static void appendSection(
            @NotNull StringBuilder report,
            @NotNull String label,
            @NotNull List<String> values
        ) {
            report.append(label).append(":\n");
            if (values.isEmpty()) {
                report.append("- -\n");
                return;
            }
            for (String value : values) {
                report.append("- ").append(value).append('\n');
            }
        }

        private static void appendLine(
            @NotNull StringBuilder report,
            @NotNull String label,
            @NotNull String value
        ) {
            report.append(label).append(": ").append(value).append('\n');
        }

        private void copyToClipboard(@NotNull String text) {
            Clipboard clipboard = new Clipboard(getShell().getDisplay());
            try {
                clipboard.setContents(new Object[]{text}, new Transfer[]{TextTransfer.getInstance()});
            } finally {
                clipboard.dispose();
            }
        }
    }
}
