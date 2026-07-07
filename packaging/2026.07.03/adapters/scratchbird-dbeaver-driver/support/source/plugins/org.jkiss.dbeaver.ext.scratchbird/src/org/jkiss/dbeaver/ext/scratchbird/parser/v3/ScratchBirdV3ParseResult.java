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
 * Licensed under the Apache License, Version 2.0
 */
package org.jkiss.dbeaver.ext.scratchbird.parser.v3;

import java.util.List;

public record ScratchBirdV3ParseResult(
    List<ScratchBirdV3Statement> statements,
    List<ScratchBirdV3Diagnostic> diagnostics
) {
    public boolean success() {
        return !statements.isEmpty() && diagnostics.stream()
            .noneMatch(diagnostic -> diagnostic.severity() == ScratchBirdV3Diagnostic.Severity.ERROR);
    }
}
