// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package org.jkiss.dbeaver.ext.scratchbird.ui.handlers;

import org.jkiss.dbeaver.ext.scratchbird.model.ScratchBirdNavigatorActionRegistry;

public class ScratchBirdNewHandler extends ScratchBirdNavigatorCommandHandler {
    public ScratchBirdNewHandler() {
        super(ScratchBirdNavigatorActionRegistry.Action.NEW);
    }
}
