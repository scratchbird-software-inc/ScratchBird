// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package com.scratchbird.jdbc;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Result of staged bootstrap/auth probing before final credential commitment.
 */
public final class SBAuthProbeResult {
    private final boolean reachable;
    private final String frontDoorMode;
    private final String resolvedHost;
    private final int resolvedPort;
    private final int requiredMethodCode;
    private final String requiredMethodName;
    private final String requiredAuthPluginId;
    private final boolean requiredMethodBrokerRequired;
    private final boolean additionalContinuationPossible;
    private final List<SBAuthMethodSurface> admittedMethods;

    public SBAuthProbeResult(boolean reachable, String frontDoorMode, String resolvedHost, int resolvedPort,
                             int requiredMethodCode, String requiredMethodName, String requiredAuthPluginId,
                             boolean requiredMethodBrokerRequired, boolean additionalContinuationPossible,
                             List<SBAuthMethodSurface> admittedMethods) {
        this.reachable = reachable;
        this.frontDoorMode = frontDoorMode != null ? frontDoorMode : "direct";
        this.resolvedHost = resolvedHost != null ? resolvedHost : "";
        this.resolvedPort = resolvedPort;
        this.requiredMethodCode = requiredMethodCode;
        this.requiredMethodName = requiredMethodName != null ? requiredMethodName : "";
        this.requiredAuthPluginId = requiredAuthPluginId != null ? requiredAuthPluginId : "";
        this.requiredMethodBrokerRequired = requiredMethodBrokerRequired;
        this.additionalContinuationPossible = additionalContinuationPossible;
        this.admittedMethods = admittedMethods == null
            ? Collections.emptyList()
            : Collections.unmodifiableList(new ArrayList<>(admittedMethods));
    }

    public boolean isReachable() {
        return reachable;
    }

    public String getFrontDoorMode() {
        return frontDoorMode;
    }

    public String getResolvedHost() {
        return resolvedHost;
    }

    public int getResolvedPort() {
        return resolvedPort;
    }

    public int getRequiredMethodCode() {
        return requiredMethodCode;
    }

    public String getRequiredMethodName() {
        return requiredMethodName;
    }

    public String getRequiredAuthPluginId() {
        return requiredAuthPluginId;
    }

    public boolean isRequiredMethodBrokerRequired() {
        return requiredMethodBrokerRequired;
    }

    public boolean isAdditionalContinuationPossible() {
        return additionalContinuationPossible;
    }

    public List<SBAuthMethodSurface> getAdmittedMethods() {
        return admittedMethods;
    }
}
