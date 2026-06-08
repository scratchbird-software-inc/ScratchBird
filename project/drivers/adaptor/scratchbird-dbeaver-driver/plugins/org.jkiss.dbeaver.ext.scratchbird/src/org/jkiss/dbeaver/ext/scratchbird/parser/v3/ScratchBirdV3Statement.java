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

public record ScratchBirdV3Statement(
    ScratchBirdV3StatementKind kind,
    ScratchBirdV3StatementFamily family,
    String surface,
    List<ScratchBirdV3Token> tokens,
    ScratchBirdV3SourceSpan span,
    boolean complete
) {
    public String leadingKeyword() {
        return tokens.isEmpty() ? "" : tokens.get(0).upperText();
    }
}
