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
 * JDBC Savepoint implementation for ScratchBird.
 */
public class SBSavepoint implements Savepoint {
    private final int id;
    private final String name;

    public SBSavepoint(int id, String name) {
        this.id = id;
        this.name = name;
    }

    @Override
    public int getSavepointId() throws SQLException {
        if (id == 0) {
            throw new SQLException("This is a named savepoint", "3B001");
        }
        return id;
    }

    @Override
    public String getSavepointName() throws SQLException {
        return name;
    }
}
