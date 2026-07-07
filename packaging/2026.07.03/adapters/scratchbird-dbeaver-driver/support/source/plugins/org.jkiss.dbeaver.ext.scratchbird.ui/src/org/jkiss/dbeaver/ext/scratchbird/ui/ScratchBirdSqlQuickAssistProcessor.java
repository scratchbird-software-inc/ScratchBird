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

import org.eclipse.jface.text.BadLocationException;
import org.eclipse.jface.text.IDocument;
import org.eclipse.jface.text.contentassist.CompletionProposal;
import org.eclipse.jface.text.contentassist.ICompletionProposal;
import org.eclipse.jface.text.quickassist.IQuickAssistInvocationContext;
import org.eclipse.jface.text.quickassist.IQuickAssistProcessor;
import org.eclipse.jface.text.source.Annotation;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Completion;
import org.jkiss.dbeaver.ext.scratchbird.parser.v3.ScratchBirdV3Parser;
import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public class ScratchBirdSqlQuickAssistProcessor implements IQuickAssistProcessor {

    private static final ICompletionProposal[] NO_PROPOSALS = new ICompletionProposal[0];

    @Nullable
    private String errorMessage;

    @Override
    public boolean canFix(@NotNull Annotation annotation) {
        return ScratchBirdSqlEditorAddIn.ANNOTATION_TYPE.equals(annotation.getType());
    }

    @Override
    public boolean canAssist(@NotNull IQuickAssistInvocationContext invocationContext) {
        return !proposalsFor(invocationContext).isEmpty();
    }

    @NotNull
    @Override
    public ICompletionProposal[] computeQuickAssistProposals(@NotNull IQuickAssistInvocationContext invocationContext) {
        List<ICompletionProposal> proposals = proposalsFor(invocationContext);
        return proposals.isEmpty() ? NO_PROPOSALS : proposals.toArray(ICompletionProposal[]::new);
    }

    @Nullable
    @Override
    public String getErrorMessage() {
        return errorMessage;
    }

    @NotNull
    private List<ICompletionProposal> proposalsFor(@NotNull IQuickAssistInvocationContext invocationContext) {
        errorMessage = null;
        if (invocationContext.getSourceViewer() == null || invocationContext.getSourceViewer().getDocument() == null) {
            return List.of();
        }

        IDocument document = invocationContext.getSourceViewer().getDocument();
        try {
            List<ICompletionProposal> proposals = new ArrayList<>();
            StatementLead lead = findStatementLead(document, invocationContext.getOffset());
            if (lead != null) {
                proposals.addAll(proposalsForLead(lead));
            }
            addContextCompletions(document, invocationContext.getOffset(), proposals);
            return proposals;
        } catch (BadLocationException e) {
            errorMessage = e.getMessage();
            return List.of();
        }
    }

    @NotNull
    private static List<ICompletionProposal> proposalsForLead(@NotNull StatementLead lead) {
        List<ICompletionProposal> proposals = new ArrayList<>();
        if ("DESC".equalsIgnoreCase(lead.text())) {
            proposals.add(replaceLead(
                lead,
                "DESCRIBE",
                "Replace removed DESC alias with DESCRIBE",
                "ScratchBird v3 keeps DESCRIBE as the native inspection statement and rejects the removed DESC alias."));
        }
        if ("EXEC".equalsIgnoreCase(lead.text())) {
            proposals.add(replaceLead(
                lead,
                "EXECUTE",
                "Replace removed EXEC alias with EXECUTE",
                "ScratchBird v3 keeps EXECUTE as the native statement and rejects the removed EXEC alias."));
        }
        if ("DO".equalsIgnoreCase(lead.text())) {
            String replacement = "EXECUTE BLOCK AS BEGIN\n    \nEND";
            proposals.add(new CompletionProposal(
                replacement,
                lead.offset(),
                lead.length(),
                "EXECUTE BLOCK AS BEGIN\n    ".length(),
                null,
                "Replace removed DO alias with EXECUTE BLOCK skeleton",
                null,
                "ScratchBird v3 uses EXECUTE BLOCK for anonymous PSQL blocks and rejects the removed DO alias."));
        }
        if ("VACUUM".equalsIgnoreCase(lead.text())) {
            proposals.add(replaceLead(
                lead,
                "SWEEP DATABASE",
                "Replace removed VACUUM alias with SWEEP DATABASE",
                "ScratchBird v3 uses SWEEP DATABASE for native database sweep/cleanup review and rejects the removed VACUUM alias."));
        }
        if ("FILTER".equalsIgnoreCase(lead.text())) {
            proposals.add(replaceLead(
                lead,
                "DOC PATH FILTER",
                "Replace removed FILTER alias with DOC PATH FILTER",
                "ScratchBird v3 uses DOC PATH FILTER for document path predicates and rejects the removed FILTER alias."));
        }
        if ("AGGREGATE".equalsIgnoreCase(lead.text())) {
            proposals.add(replaceLead(
                lead,
                "TS BUCKET AGG",
                "Replace removed AGGREGATE alias with TS BUCKET AGG",
                "ScratchBird v3 uses TS BUCKET AGG for time-bucket aggregation and rejects the removed AGGREGATE alias."));
        }
        if ("ANN".equalsIgnoreCase(lead.text())) {
            proposals.add(replaceLead(
                lead,
                "VECTOR ANN QUERY",
                "Replace removed ANN alias with VECTOR ANN QUERY",
                "ScratchBird v3 uses VECTOR ANN QUERY for approximate nearest-neighbor search and rejects the removed ANN alias."));
        }
        return proposals;
    }

    private static void addContextCompletions(
        @NotNull IDocument document,
        int invocationOffset,
        @NotNull List<ICompletionProposal> proposals
    ) throws BadLocationException {
        int caretOffset = Math.max(0, Math.min(invocationOffset, document.getLength()));
        ReplacementTarget target = findReplacementTarget(document, caretOffset);
        List<ScratchBirdV3Completion> completions = ScratchBirdV3Parser.completionsAt(document.get(), caretOffset);
        Set<String> seen = new LinkedHashSet<>();
        proposals.stream()
            .map(ICompletionProposal::getDisplayString)
            .forEach(seen::add);

        for (ScratchBirdV3Completion completion : completions) {
            String replacement = completion.label();
            if (!seen.add("ScratchBird v3: " + replacement)) {
                continue;
            }
            if (target.length() > 0 && replacement.equalsIgnoreCase(document.get(target.offset(), target.length()))) {
                continue;
            }
            proposals.add(new CompletionProposal(
                replacement,
                target.offset(),
                target.length(),
                replacement.length(),
                null,
                "ScratchBird v3: " + replacement,
                null,
                completion.detail()));
        }
    }

    @NotNull
    private static CompletionProposal replaceLead(
        @NotNull StatementLead lead,
        @NotNull String replacement,
        @NotNull String display,
        @NotNull String detail
    ) {
        return new CompletionProposal(
            replacement,
            lead.offset(),
            lead.length(),
            replacement.length(),
            null,
            display,
            null,
            detail);
    }

    @Nullable
    private static StatementLead findStatementLead(@NotNull IDocument document, int invocationOffset) throws BadLocationException {
        int documentLength = document.getLength();
        if (documentLength == 0) {
            return null;
        }

        int offset = Math.max(0, Math.min(invocationOffset, documentLength - 1));
        int start = offset;
        while (start > 0 && document.getChar(start - 1) != ';') {
            start--;
        }
        while (start < documentLength && Character.isWhitespace(document.getChar(start))) {
            start++;
        }
        if (start >= documentLength || !isLeadCharacter(document.getChar(start))) {
            return null;
        }

        int end = start + 1;
        while (end < documentLength && isLeadCharacter(document.getChar(end))) {
            end++;
        }
        return new StatementLead(start, end - start, document.get(start, end - start));
    }

    @NotNull
    private static ReplacementTarget findReplacementTarget(@NotNull IDocument document, int invocationOffset) throws BadLocationException {
        int documentLength = document.getLength();
        int caretOffset = Math.max(0, Math.min(invocationOffset, documentLength));
        int start = caretOffset;
        while (start > 0 && isCompletionTokenCharacter(document.getChar(start - 1))) {
            start--;
        }
        int end = caretOffset;
        while (end < documentLength && isCompletionTokenCharacter(document.getChar(end))) {
            end++;
        }
        return new ReplacementTarget(start, end - start);
    }

    private static boolean isLeadCharacter(char character) {
        return Character.isLetter(character) || character == '_';
    }

    private static boolean isCompletionTokenCharacter(char character) {
        return Character.isLetterOrDigit(character) || character == '_' || character == '.';
    }

    private record StatementLead(int offset, int length, @NotNull String text) {
    }

    private record ReplacementTarget(int offset, int length) {
    }
}
