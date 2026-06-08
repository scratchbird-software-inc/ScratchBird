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

public class SBGeometry {
    private final byte[] wkb;
    private final Integer srid;
    private final String wkt;

    public SBGeometry(byte[] wkb) {
        this(wkb, null, null);
    }

    public SBGeometry(byte[] wkb, Integer srid, String wkt) {
        this.wkb = wkb != null ? wkb.clone() : null;
        this.srid = srid;
        this.wkt = wkt;
    }

    public byte[] getWkb() {
        return wkb != null ? wkb.clone() : null;
    }

    public Integer getSrid() {
        return srid;
    }

    public String getWkt() {
        return wkt;
    }
}
