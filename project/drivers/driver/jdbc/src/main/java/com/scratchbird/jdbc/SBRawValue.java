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

public class SBRawValue {
    private final int oid;
    private final byte[] data;

    public SBRawValue(int oid, byte[] data) {
        this.oid = oid;
        this.data = data != null ? data.clone() : null;
    }

    public int getOid() {
        return oid;
    }

    public byte[] getData() {
        return data != null ? data.clone() : null;
    }
}
