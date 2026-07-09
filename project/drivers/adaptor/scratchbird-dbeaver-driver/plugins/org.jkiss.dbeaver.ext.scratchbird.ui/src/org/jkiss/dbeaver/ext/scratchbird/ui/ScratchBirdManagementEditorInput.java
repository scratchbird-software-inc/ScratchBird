// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package org.jkiss.dbeaver.ext.scratchbird.ui;

import org.eclipse.jface.resource.ImageDescriptor;
import org.eclipse.ui.IEditorInput;
import org.eclipse.ui.IPersistableElement;
import org.jkiss.code.NotNull;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdNavigatorActionRegistry;
import org.jkiss.dbeaver.model.struct.DBSObject;

import java.util.Objects;

final class ScratchBirdManagementEditorInput implements IEditorInput {

    @NotNull
    private final DBSObject targetObject;
    @NotNull
    private final ScratchBirdNavigatorActionRegistry.Action action;
    @NotNull
    private final String targetPath;

    ScratchBirdManagementEditorInput(
        @NotNull DBSObject targetObject,
        @NotNull ScratchBirdNavigatorActionRegistry.Action action
    ) {
        this.targetObject = targetObject;
        this.action = action;
        this.targetPath = ScratchBirdSelectionUtils.displayPath(targetObject);
    }

    @NotNull
    DBSObject targetObject() {
        return targetObject;
    }

    @NotNull
    ScratchBirdNavigatorActionRegistry.Action action() {
        return action;
    }

    @Override
    public boolean exists() {
        return true;
    }

    @Override
    public @Nullable ImageDescriptor getImageDescriptor() {
        return null;
    }

    @Override
    public @NotNull String getName() {
        return action.name() + " " + targetPath;
    }

    @Override
    public @Nullable IPersistableElement getPersistable() {
        return null;
    }

    @Override
    public @NotNull String getToolTipText() {
        return "ScratchBird " + action.name() + " editor for " + targetPath;
    }

    @Override
    public <T> T getAdapter(Class<T> adapter) {
        if (adapter.isInstance(targetObject)) {
            return adapter.cast(targetObject);
        }
        return null;
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) {
            return true;
        }
        if (!(other instanceof ScratchBirdManagementEditorInput that)) {
            return false;
        }
        return action == that.action && targetPath.equals(that.targetPath);
    }

    @Override
    public int hashCode() {
        return Objects.hash(action, targetPath);
    }
}
