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

public class SBRange<T> {
    private final T lower;
    private final T upper;
    private final boolean lowerInclusive;
    private final boolean upperInclusive;
    private final boolean lowerInfinite;
    private final boolean upperInfinite;
    private final boolean empty;
    private final Integer rangeOid;

    public SBRange(T lower, T upper) {
        this(lower, upper, false, false, false, false, false, null);
    }

    public SBRange(T lower, T upper, boolean lowerInclusive, boolean upperInclusive,
                   boolean lowerInfinite, boolean upperInfinite, boolean empty, Integer rangeOid) {
        this.lower = lower;
        this.upper = upper;
        this.lowerInclusive = lowerInclusive;
        this.upperInclusive = upperInclusive;
        this.lowerInfinite = lowerInfinite;
        this.upperInfinite = upperInfinite;
        this.empty = empty;
        this.rangeOid = rangeOid;
    }

    public T getLower() { return lower; }
    public T getUpper() { return upper; }
    public boolean isLowerInclusive() { return lowerInclusive; }
    public boolean isUpperInclusive() { return upperInclusive; }
    public boolean isLowerInfinite() { return lowerInfinite; }
    public boolean isUpperInfinite() { return upperInfinite; }
    public boolean isEmpty() { return empty; }
    public Integer getRangeOid() { return rangeOid; }
}
