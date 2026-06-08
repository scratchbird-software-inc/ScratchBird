// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import java.sql.Ref;
import java.sql.SQLException;
import java.util.Map;

/**
 * Minimal JDBC Ref implementation used by the ScratchBird driver.
 */
public final class SBRef implements Ref {
    private final String baseTypeName;
    private Object value;

    public SBRef(String baseTypeName, Object value) {
        this.baseTypeName = baseTypeName == null ? "ref" : baseTypeName;
        this.value = value;
    }

    public static SBRef fromObject(Object value) throws SQLException {
        if (value == null) {
            return null;
        }
        if (value instanceof Ref) {
            Ref ref = (Ref) value;
            return new SBRef(ref.getBaseTypeName(), ref.getObject());
        }
        return new SBRef("ref", value);
    }

    @Override
    public String getBaseTypeName() throws SQLException {
        return baseTypeName;
    }

    @Override
    public Object getObject(Map<String, Class<?>> map) throws SQLException {
        return value;
    }

    @Override
    public Object getObject() throws SQLException {
        return value;
    }

    @Override
    public void setObject(Object value) throws SQLException {
        this.value = value;
    }
}
