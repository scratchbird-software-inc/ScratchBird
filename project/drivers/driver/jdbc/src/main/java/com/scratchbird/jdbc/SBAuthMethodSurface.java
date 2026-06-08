// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

/**
 * Describes a negotiated auth method exposed during bootstrap probing.
 */
public final class SBAuthMethodSurface {
    private final int methodCode;
    private final String methodName;
    private final String pluginId;
    private final boolean executableLocally;
    private final boolean brokerRequired;

    public SBAuthMethodSurface(int methodCode, String methodName, String pluginId,
                               boolean executableLocally, boolean brokerRequired) {
        this.methodCode = methodCode;
        this.methodName = methodName != null ? methodName : "";
        this.pluginId = pluginId != null ? pluginId : "";
        this.executableLocally = executableLocally;
        this.brokerRequired = brokerRequired;
    }

    public int getMethodCode() {
        return methodCode;
    }

    public String getMethodName() {
        return methodName;
    }

    public String getPluginId() {
        return pluginId;
    }

    public boolean isExecutableLocally() {
        return executableLocally;
    }

    public boolean isBrokerRequired() {
        return brokerRequired;
    }
}
