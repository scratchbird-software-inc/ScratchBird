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

import java.util.*;

/**
 * Result from a query execution.
 */
public class SBQueryResult {
    private List<SBColumnInfo> columns;
    private List<Object[]> rows;
    private SBRowStream stream;
    private String commandTag;
    private long updateCount;

    public List<SBColumnInfo> getColumns() { return columns; }
    public void setColumns(List<SBColumnInfo> columns) { this.columns = columns; }

    public List<Object[]> getRows() { return rows; }
    public void setRows(List<Object[]> rows) { this.rows = rows; }

    public SBRowStream getStream() { return stream; }
    public void setStream(SBRowStream stream) { this.stream = stream; }

    public String getCommandTag() { return commandTag; }
    public void setCommandTag(String commandTag) { this.commandTag = commandTag; }

    public long getUpdateCount() { return updateCount; }
    public void setUpdateCount(long updateCount) { this.updateCount = updateCount; }
}
