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
package org.jkiss.dbeaver.ext.scratchbird.ui;

import org.eclipse.jface.fieldassist.ContentProposalAdapter;
import org.eclipse.jface.fieldassist.IContentProposal;
import org.eclipse.jface.fieldassist.IContentProposalProvider;
import org.eclipse.jface.text.BadLocationException;
import org.eclipse.jface.text.DocumentEvent;
import org.eclipse.jface.text.IDocument;
import org.eclipse.jface.text.IDocumentListener;
import org.eclipse.jface.text.Position;
import org.eclipse.jface.viewers.ISelectionChangedListener;
import org.eclipse.jface.viewers.ISelectionProvider;
import org.eclipse.jface.viewers.SelectionChangedEvent;
import org.eclipse.jface.text.source.Annotation;
import org.eclipse.jface.text.source.IAnnotationModel;
import org.eclipse.jface.text.source.IAnnotationModelExtension;
import org.eclipse.jface.action.IStatusLineManager;
import org.eclipse.swt.custom.StyledText;
import org.eclipse.swt.graphics.Point;
import org.eclipse.swt.widgets.Display;
import org.eclipse.swt.widgets.Shell;
import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdDataSource;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdSqlPromptPlanner;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Completion;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Diagnostic;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3ParseResult;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Parser;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3SourceSpan;
import org.jkiss.dbeaver.model.DBPDataSource;
import org.jkiss.dbeaver.model.DBPDataSourceContainer;
import org.jkiss.dbeaver.ui.ActionBars;
import org.jkiss.dbeaver.ui.contentassist.ContentAssistLabelProvider;
import org.jkiss.dbeaver.ui.contentassist.ContentProposalExt;
import org.jkiss.dbeaver.ui.editors.sql.SQLEditor;
import org.jkiss.dbeaver.ui.editors.sql.suggestion.SQLSuggestionTextPainter;
import org.jkiss.dbeaver.ui.editors.sql.addins.SQLEditorAddIn;

import java.io.PrintWriter;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;

public class ScratchBirdSqlEditorAddIn implements SQLEditorAddIn {

    public static final String ANNOTATION_TYPE = "org.jkiss.dbeaver.ext.scratchbird.ui.v3Problem";
    private static final int VALIDATION_DELAY_MS = 600;
    private static final int PROMPT_DELAY_MS = 120;
    private static final Map<SQLEditor, ScratchBirdSqlEditorAddIn> ACTIVE_EDITORS =
        Collections.synchronizedMap(new WeakHashMap<>());

    @Nullable
    private SQLEditor editor;
    @Nullable
    private IDocument document;
    @Nullable
    private ContentProposalAdapter proposalAdapter;
    private Annotation[] annotations = new Annotation[0];
    private int validationGeneration;
    private int promptGeneration;
    @Nullable
    private String lastStatusMessage;

    private final IDocumentListener documentListener = new IDocumentListener() {
        @Override
        public void documentAboutToBeChanged(DocumentEvent event) {
        }

        @Override
        public void documentChanged(DocumentEvent event) {
            scheduleValidation();
            schedulePrompt();
        }
    };

    private final ISelectionChangedListener selectionListener = new ISelectionChangedListener() {
        @Override
        public void selectionChanged(SelectionChangedEvent event) {
            schedulePrompt();
        }
    };

    @Override
    public void init(@NotNull SQLEditor editor) {
        if (!isScratchBirdEditor(editor)) {
            return;
        }
        this.editor = editor;
        this.document = editor.getDocument();
        this.proposalAdapter = createProposalAdapter(editor);
        ACTIVE_EDITORS.put(editor, this);
        if (document != null) {
            document.addDocumentListener(documentListener);
            scheduleValidation();
            schedulePrompt();
        }
        ISelectionProvider selectionProvider = editor.getSelectionProvider();
        if (selectionProvider != null) {
            selectionProvider.addSelectionChangedListener(selectionListener);
        }
    }

    @Override
    public void cleanup(@NotNull SQLEditor editor) {
        validationGeneration++;
        promptGeneration++;
        ACTIVE_EDITORS.remove(editor);
        if (document != null) {
            document.removeDocumentListener(documentListener);
        }
        ISelectionProvider selectionProvider = editor.getSelectionProvider();
        if (selectionProvider != null) {
            selectionProvider.removeSelectionChangedListener(selectionListener);
        }
        if (proposalAdapter != null) {
            proposalAdapter.closeProposalPopup();
            proposalAdapter.setEnabled(false);
        }
        clearAnnotations();
        clearPrompt();
        proposalAdapter = null;
        document = null;
        this.editor = null;
    }

    @Nullable
    @Override
    public PrintWriter getServerOutputConsumer() {
        return null;
    }

    public static boolean openParserCompletion(@NotNull SQLEditor editor) {
        ScratchBirdSqlEditorAddIn addIn = ACTIVE_EDITORS.get(editor);
        return addIn != null && addIn.openParserCompletionPopup();
    }

    private boolean openParserCompletionPopup() {
        SQLEditor currentEditor = editor;
        IDocument currentDocument = document;
        ContentProposalAdapter currentProposalAdapter = proposalAdapter;
        if (currentEditor == null || currentDocument == null || currentProposalAdapter == null) {
            return false;
        }
        if (!isScratchBirdEditor(currentEditor) || currentProposalAdapter.getControl().isDisposed()) {
            return false;
        }
        if (ScratchBirdSqlPromptPlanner.completionCandidates(
            currentDocument.get(),
            currentCaretOffset(currentEditor, currentDocument)).isEmpty()) {
            return false;
        }
        currentProposalAdapter.openProposalPopup();
        return currentProposalAdapter.isProposalPopupOpen();
    }

    private void scheduleValidation() {
        SQLEditor currentEditor = editor;
        IDocument currentDocument = document;
        if (currentEditor == null || currentDocument == null) {
            return;
        }

        int generation = ++validationGeneration;
        Display display = getDisplay(currentEditor);
        if (display == null || display.isDisposed()) {
            return;
        }
        display.timerExec(VALIDATION_DELAY_MS, () -> {
            if (generation == validationGeneration) {
                validateNow();
            }
        });
    }

    private void schedulePrompt() {
        SQLEditor currentEditor = editor;
        IDocument currentDocument = document;
        if (currentEditor == null || currentDocument == null) {
            return;
        }

        int generation = ++promptGeneration;
        Display display = getDisplay(currentEditor);
        if (display == null || display.isDisposed()) {
            return;
        }
        display.timerExec(PROMPT_DELAY_MS, () -> {
            if (generation == promptGeneration) {
                updatePromptNow();
            }
        });
    }

    private void validateNow() {
        SQLEditor currentEditor = editor;
        IDocument currentDocument = document;
        if (currentEditor == null || currentDocument == null || !isScratchBirdEditor(currentEditor)) {
            clearAnnotations();
            clearPrompt();
            return;
        }

        String sql = currentDocument.get();
        if (sql.isBlank()) {
            clearAnnotations();
            clearPrompt();
            return;
        }

        ScratchBirdV3ParseResult result = ScratchBirdV3Parser.parse(sql);
        Map<Annotation, Position> newAnnotations = new LinkedHashMap<>();
        for (ScratchBirdV3Diagnostic diagnostic : result.diagnostics()) {
            newAnnotations.put(
                new ScratchBirdV3ProblemAnnotation(diagnostic.format()),
                positionFor(currentDocument, diagnostic.span()));
        }
        replaceAnnotations(newAnnotations);
    }

    private void updatePromptNow() {
        SQLEditor currentEditor = editor;
        IDocument currentDocument = document;
        if (currentEditor == null || currentDocument == null || !isScratchBirdEditor(currentEditor)) {
            clearPrompt();
            return;
        }

        ScratchBirdSqlPromptPlanner.PromptPlan promptPlan = ScratchBirdSqlPromptPlanner.plan(
            currentDocument.get(),
            currentCaretOffset(currentEditor, currentDocument));
        applyPrompt(currentEditor, promptPlan);
    }

    private void clearPrompt() {
        SQLEditor currentEditor = editor;
        if (currentEditor == null) {
            lastStatusMessage = null;
            return;
        }
        applyPrompt(currentEditor, ScratchBirdSqlPromptPlanner.PromptPlan.empty());
    }

    private void applyPrompt(@NotNull SQLEditor currentEditor, @NotNull ScratchBirdSqlPromptPlanner.PromptPlan promptPlan) {
        SQLSuggestionTextPainter suggestionTextPainter = currentEditor.getSuggestionTextPainter();
        if (suggestionTextPainter != null) {
            if (promptPlan.inlineSuggestion() == null || promptPlan.inlineSuggestion().isBlank()) {
                suggestionTextPainter.removeHint();
            } else {
                suggestionTextPainter.showHint(promptPlan.inlineSuggestion(), promptPlan.caretOffset());
            }
        }
        updateStatusLine(currentEditor, promptPlan.statusMessage());
    }

    private void updateStatusLine(@NotNull SQLEditor currentEditor, @Nullable String statusMessage) {
        IStatusLineManager statusLineManager = ActionBars.extractStatusLineManager(currentEditor.getEditorSite());
        if (statusLineManager == null) {
            lastStatusMessage = statusMessage;
            return;
        }
        if (statusMessage != null && statusMessage.equals(lastStatusMessage)) {
            return;
        }
        statusLineManager.setMessage(statusMessage);
        lastStatusMessage = statusMessage;
    }

    private void clearAnnotations() {
        replaceAnnotations(Map.of());
    }

    private void replaceAnnotations(@NotNull Map<Annotation, Position> newAnnotations) {
        SQLEditor currentEditor = editor;
        if (currentEditor == null) {
            annotations = new Annotation[0];
            return;
        }

        IAnnotationModel annotationModel = currentEditor.getAnnotationModel();
        if (annotationModel == null) {
            annotations = new Annotation[0];
            return;
        }

        Annotation[] previousAnnotations = annotations;
        annotations = newAnnotations.keySet().toArray(Annotation[]::new);
        if (annotationModel instanceof IAnnotationModelExtension extension) {
            extension.replaceAnnotations(previousAnnotations, newAnnotations);
            return;
        }

        for (Annotation annotation : previousAnnotations) {
            annotationModel.removeAnnotation(annotation);
        }
        newAnnotations.forEach(annotationModel::addAnnotation);
    }

    @NotNull
    private static Position positionFor(@NotNull IDocument document, @Nullable ScratchBirdV3SourceSpan span) {
        int documentLength = document.getLength();
        if (documentLength == 0) {
            return new Position(0, 0);
        }

        int offset = span == null ? 0 : clamp(span.start().offset(), 0, documentLength);
        int length = span == null ? 1 : span.length();
        if (length <= 0) {
            length = inferTokenLength(document, offset);
        }
        if (offset >= documentLength) {
            return new Position(documentLength - 1, 1);
        }
        return new Position(offset, clamp(length, 1, documentLength - offset));
    }

    private static int inferTokenLength(@NotNull IDocument document, int offset) {
        int documentLength = document.getLength();
        if (documentLength == 0 || offset >= documentLength) {
            return 1;
        }

        int length = 0;
        for (int index = Math.max(0, offset); index < documentLength; index++) {
            try {
                char character = document.getChar(index);
                if (!Character.isLetterOrDigit(character) && character != '_' && character != '$' && character != '.') {
                    break;
                }
            } catch (BadLocationException e) {
                break;
            }
            length++;
        }
        return Math.max(1, length);
    }

    private static boolean isScratchBirdEditor(@NotNull SQLEditor editor) {
        DBPDataSource dataSource = editor.getDataSource();
        if (dataSource instanceof ScratchBirdDataSource) {
            return true;
        }
        DBPDataSourceContainer container = editor.getDataSourceContainer();
        return container != null && "scratchbird_jdbc".equalsIgnoreCase(container.getDriver().getId());
    }

    @Nullable
    private static Display getDisplay(@NotNull SQLEditor editor) {
        if (editor.getSite() == null) {
            return null;
        }
        Shell shell = editor.getSite().getShell();
        return shell == null ? null : shell.getDisplay();
    }

    private static int currentCaretOffset(@NotNull SQLEditor editor, @NotNull IDocument document) {
        ISelectionProvider selectionProvider = editor.getSelectionProvider();
        if (selectionProvider != null && selectionProvider.getSelection() instanceof org.eclipse.jface.text.ITextSelection textSelection) {
            return clamp(textSelection.getOffset(), 0, document.getLength());
        }
        return document.getLength();
    }

    private static int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }

    @Nullable
    private ContentProposalAdapter createProposalAdapter(@NotNull SQLEditor editor) {
        if (editor.getTextViewer() == null) {
            return null;
        }
        StyledText textWidget = editor.getTextViewer().getTextWidget();
        if (textWidget == null || textWidget.isDisposed()) {
            return null;
        }
        ContentProposalAdapter adapter = new ContentProposalAdapter(
            textWidget,
            new ScratchBirdProposalContentAdapter(),
            new ScratchBirdProposalProvider(),
            null,
            new char[0]);
        adapter.setProposalAcceptanceStyle(ContentProposalAdapter.PROPOSAL_INSERT);
        adapter.setLabelProvider(new ContentAssistLabelProvider());
        adapter.setPopupSize(new Point(420, 240));
        return adapter;
    }

    private final class ScratchBirdProposalProvider implements IContentProposalProvider {
        @Override
        public IContentProposal[] getProposals(String contents, int position) {
            List<ScratchBirdV3Completion> completions = ScratchBirdSqlPromptPlanner.completionCandidates(contents, position);
            if (completions.isEmpty()) {
                return new IContentProposal[0];
            }
            return completions.stream()
                .map(completion -> proposalFor(contents, position, completion))
                .filter(proposal -> proposal != null)
                .toArray(IContentProposal[]::new);
        }

        @Nullable
        private IContentProposal proposalFor(
            @NotNull String contents,
            int position,
            @NotNull ScratchBirdV3Completion completion
        ) {
            String insertion = ScratchBirdSqlPromptPlanner.proposalInsertion(contents, position, completion.label());
            if (insertion.isEmpty()) {
                return null;
            }
            return new ContentProposalExt(
                insertion,
                completion.label(),
                completion.detail(),
                insertion.length());
        }
    }

    private static final class ScratchBirdProposalContentAdapter extends org.jkiss.dbeaver.ui.contentassist.StyledTextContentAdapter {
    }

    private static final class ScratchBirdV3ProblemAnnotation extends Annotation {
        private ScratchBirdV3ProblemAnnotation(@NotNull String message) {
            super(ANNOTATION_TYPE, false, message);
        }
    }
}
