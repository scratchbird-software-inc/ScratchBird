// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 * Copyright (c) 2025 ScratchBird Project
 */
package com.scratchbird.jdbc;

import java.sql.*;

/**
 * JDBC NClob implementation for ScratchBird.
 */
public class SBNClob extends SBClob implements NClob {
    public SBNClob() {
        super();
    }

    public SBNClob(String s) {
        super(s);
    }
}
