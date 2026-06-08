// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * ScratchBird JDBC Driver
 */
package com.scratchbird.jdbc;

import java.sql.SQLException;
import java.util.List;

interface SBRowStream {
    Object[] nextRow() throws SQLException;
    List<SBColumnInfo> getColumns();
    long getUpdateCount();
    String getCommandTag();
    boolean isDone();
    default void close() throws SQLException {
        // Most row streams are already fully materialized and do not need
        // protocol-level cleanup on close.
    }
}
