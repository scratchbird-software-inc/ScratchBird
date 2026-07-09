// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package org.jkiss.dbeaver.ext.scratchbird.ui;

import org.eclipse.core.runtime.IProgressMonitor;
import org.eclipse.swt.widgets.Composite;
import org.eclipse.swt.widgets.Control;
import org.eclipse.swt.widgets.Shell;
import org.eclipse.ui.IEditorInput;
import org.eclipse.ui.IEditorSite;
import org.eclipse.ui.IWorkbenchPage;
import org.eclipse.ui.IWorkbenchWindow;
import org.eclipse.ui.PartInitException;
import org.eclipse.ui.part.EditorPart;
import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdNavigatorActionRegistry;
import org.jkiss.dbeaver.model.struct.DBSObject;

public class ScratchBirdManagementEditor extends EditorPart {

    public static final String EDITOR_ID = "org.jkiss.dbeaver.ext.scratchbird.ui.managementEditor";

    @Nullable
    private Control rootControl;

    public static void openOrDialog(
        @Nullable IWorkbenchWindow window,
        @Nullable Shell fallbackShell,
        @NotNull DBSObject object,
        @NotNull ScratchBirdNavigatorActionRegistry.Action action
    ) throws PartInitException {
        IWorkbenchPage page = window == null ? null : window.getActivePage();
        if (page == null) {
            new ScratchBirdManagementDialog(fallbackShell, object, action).open();
            return;
        }
        page.openEditor(new ScratchBirdManagementEditorInput(object, action), EDITOR_ID, true);
    }

    @Override
    public void init(IEditorSite site, IEditorInput input) throws PartInitException {
        if (!(input instanceof ScratchBirdManagementEditorInput managementInput)) {
            throw new PartInitException("ScratchBird management editor requires a ScratchBird management input.");
        }
        setSite(site);
        setInput(input);
        setPartName(managementInput.getName());
    }

    @Override
    public void createPartControl(Composite parent) {
        ScratchBirdManagementEditorInput input = (ScratchBirdManagementEditorInput) getEditorInput();
        ScratchBirdManagementDialog form = new ScratchBirdManagementDialog(parent.getShell(), input.targetObject(), input.action());
        rootControl = form.createEmbeddedEditorArea(parent);
    }

    @Override
    public void setFocus() {
        if (rootControl != null && !rootControl.isDisposed()) {
            rootControl.setFocus();
        }
    }

    @Override
    public void doSave(IProgressMonitor monitor) {
        // ScratchBird management forms apply through server-admitted commands, not editor save.
    }

    @Override
    public void doSaveAs() {
        // ScratchBird management forms are not file-backed editors.
    }

    @Override
    public boolean isDirty() {
        return false;
    }

    @Override
    public boolean isSaveAsAllowed() {
        return false;
    }
}
