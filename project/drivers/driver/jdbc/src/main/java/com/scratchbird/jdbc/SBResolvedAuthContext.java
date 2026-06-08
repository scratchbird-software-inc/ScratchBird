// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

/**
 * Resolved auth state for an established or partially negotiated connection.
 */
public final class SBResolvedAuthContext {
    private String frontDoorMode = "direct";
    private boolean attached;
    private boolean managerAuthenticated;
    private int resolvedMethodCode;
    private String resolvedMethodName = "";
    private String resolvedAuthPluginId = "";

    public String getFrontDoorMode() {
        return frontDoorMode;
    }

    void setFrontDoorMode(String frontDoorMode) {
        this.frontDoorMode = (frontDoorMode == null || frontDoorMode.isBlank()) ? "direct" : frontDoorMode;
    }

    public boolean isAttached() {
        return attached;
    }

    void setAttached(boolean attached) {
        this.attached = attached;
    }

    public boolean isManagerAuthenticated() {
        return managerAuthenticated;
    }

    void setManagerAuthenticated(boolean managerAuthenticated) {
        this.managerAuthenticated = managerAuthenticated;
    }

    public int getResolvedMethodCode() {
        return resolvedMethodCode;
    }

    void setResolvedMethodCode(int resolvedMethodCode) {
        this.resolvedMethodCode = resolvedMethodCode;
    }

    public String getResolvedMethodName() {
        return resolvedMethodName;
    }

    void setResolvedMethodName(String resolvedMethodName) {
        this.resolvedMethodName = resolvedMethodName != null ? resolvedMethodName : "";
    }

    public String getResolvedAuthPluginId() {
        return resolvedAuthPluginId;
    }

    void setResolvedAuthPluginId(String resolvedAuthPluginId) {
        this.resolvedAuthPluginId = resolvedAuthPluginId != null ? resolvedAuthPluginId : "";
    }

    SBResolvedAuthContext copy() {
        SBResolvedAuthContext copy = new SBResolvedAuthContext();
        copy.frontDoorMode = this.frontDoorMode;
        copy.attached = this.attached;
        copy.managerAuthenticated = this.managerAuthenticated;
        copy.resolvedMethodCode = this.resolvedMethodCode;
        copy.resolvedMethodName = this.resolvedMethodName;
        copy.resolvedAuthPluginId = this.resolvedAuthPluginId;
        return copy;
    }
}
