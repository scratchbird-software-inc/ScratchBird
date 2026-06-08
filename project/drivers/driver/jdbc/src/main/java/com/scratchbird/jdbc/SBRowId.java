// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import java.nio.charset.StandardCharsets;
import java.sql.RowId;
import java.util.Arrays;

/**
 * Minimal JDBC RowId wrapper used by the ScratchBird driver.
 */
public final class SBRowId implements RowId {
    private final byte[] bytes;

    public SBRowId(byte[] bytes) {
        this.bytes = bytes == null ? new byte[0] : bytes.clone();
    }

    public static SBRowId fromObject(Object value) {
        if (value == null) {
            return null;
        }
        if (value instanceof RowId) {
            return new SBRowId(((RowId) value).getBytes());
        }
        if (value instanceof byte[]) {
            return new SBRowId((byte[]) value);
        }
        return new SBRowId(value.toString().getBytes(StandardCharsets.UTF_8));
    }

    @Override
    public byte[] getBytes() {
        return bytes.clone();
    }

    @Override
    public String toString() {
        return new String(bytes, StandardCharsets.UTF_8);
    }

    @Override
    public int hashCode() {
        return Arrays.hashCode(bytes);
    }

    @Override
    public boolean equals(Object obj) {
        if (this == obj) return true;
        if (!(obj instanceof RowId)) return false;
        return Arrays.equals(bytes, ((RowId) obj).getBytes());
    }
}
