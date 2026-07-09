// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package org.jkiss.dbeaver.ext.scratchbird.ui;

import org.eclipse.core.runtime.IAdapterFactory;
import org.eclipse.swt.widgets.Shell;
import org.eclipse.ui.IWorkbenchWindow;
import org.eclipse.ui.PartInitException;
import org.jkiss.code.Nullable;
import org.jkiss.dbeaver.DBException;
import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdNavigatorActionRegistry;
import org.jkiss.dbeaver.model.navigator.DBNObjectNode;
import org.jkiss.dbeaver.model.struct.DBSObject;
import org.jkiss.dbeaver.ui.navigator.INavigatorObjectManager;

public class ScratchBirdNavigatorObjectManagerAdapterFactory implements IAdapterFactory {

    private static final Class<?>[] ADAPTERS = {
        INavigatorObjectManager.class
    };

    private static final INavigatorObjectManager OBJECT_MANAGER = new ScratchBirdNavigatorObjectManager();

    @Override
    public <T> T getAdapter(Object adaptableObject, Class<T> adapterType) {
        if (adapterType != INavigatorObjectManager.class ||
            !ScratchBirdSelectionUtils.supportsDefaultOpen(adaptableObject)) {
            return null;
        }
        return adapterType.cast(OBJECT_MANAGER);
    }

    @Override
    public Class<?>[] getAdapterList() {
        return ADAPTERS;
    }

    private static final class ScratchBirdNavigatorObjectManager implements INavigatorObjectManager {
        @Override
        public int getSupportedFeatures() {
            return FEATURE_OPEN;
        }

        @Override
        public void openObjectEditor(IWorkbenchWindow window, DBNObjectNode node) throws DBException {
            Object nodeObject = node.getNodeObject();
            if (!(nodeObject instanceof DBSObject object)) {
                throw new DBException("ScratchBird navigator node does not expose a database object.");
            }
            if (!ScratchBirdSelectionUtils.supportsDefaultOpen(object)) {
                throw new DBException("ScratchBird navigator node does not support a ScratchBird editor.");
            }
            try {
                ScratchBirdManagementEditor.openOrDialog(
                    window,
                    resolveShell(window),
                    object,
                    ScratchBirdNavigatorActionRegistry.Action.OPEN);
            } catch (PartInitException e) {
                throw new DBException("ScratchBird management editor could not be opened.", e);
            }
        }

        private static @Nullable Shell resolveShell(@Nullable IWorkbenchWindow window) {
            return window == null ? null : window.getShell();
        }
    }
}
