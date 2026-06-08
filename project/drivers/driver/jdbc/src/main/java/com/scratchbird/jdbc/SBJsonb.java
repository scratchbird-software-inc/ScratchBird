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

import java.nio.charset.StandardCharsets;

public class SBJsonb {
    private final byte[] raw;
    private final String value;

    public SBJsonb(byte[] raw) {
        this(raw, null);
    }

    public SBJsonb(String value) {
        this(value != null ? value.getBytes(StandardCharsets.UTF_8) : null, value);
    }

    public SBJsonb(byte[] raw, String value) {
        this.raw = raw != null ? raw.clone() : null;
        this.value = value;
    }

    public byte[] getRaw() {
        return raw != null ? raw.clone() : null;
    }

    public String getValue() {
        return value;
    }
}
