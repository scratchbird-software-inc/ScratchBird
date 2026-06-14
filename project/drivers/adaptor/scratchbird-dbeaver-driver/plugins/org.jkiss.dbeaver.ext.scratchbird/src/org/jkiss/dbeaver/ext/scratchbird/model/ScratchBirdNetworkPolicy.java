// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

/*
 * DBeaver - Universal Database Manager
 * Copyright (C) 2010-2026 DBeaver Corp and others
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.jkiss.dbeaver.ext.scratchbird.model;

import org.jkiss.code.NotNull;

import java.util.List;

public record ScratchBirdNetworkPolicy(
    boolean telemetryEnabledByDefault,
    boolean updateChecksEnabledByDefault,
    boolean diagnosticsUploadEnabledByDefault,
    @NotNull List<String> allowedEndpointClasses,
    @NotNull List<String> deniedEndpointClasses,
    @NotNull List<String> auditEvents
) {
    @NotNull
    public static ScratchBirdNetworkPolicy defaultPolicy() {
        return new ScratchBirdNetworkPolicy(
            false,
            false,
            false,
            List.of(
                "scratchbird-server-listener",
                "scratchbird-manager-proxy",
                "user-configured-proxy-for-server-route"),
            List.of(
                "plugin-telemetry",
                "diagnostic-upload",
                "marketplace-call",
                "implicit-update-check",
                "third-party-analytics"),
            List.of(
                "dbeaver.network.policy.loaded",
                "dbeaver.network.egress.denied",
                "dbeaver.network.user_route.allowed"));
    }

    public boolean allowsEndpointClass(@NotNull String endpointClass) {
        return allowedEndpointClasses.contains(endpointClass);
    }

    public boolean deniesEndpointClass(@NotNull String endpointClass) {
        return deniedEndpointClasses.contains(endpointClass);
    }

    public boolean isSecretProperty(@NotNull String propertyName) {
        return ScratchBirdSecurityRedactor.isSensitiveProperty(propertyName);
    }

    @NotNull
    public String redactProperty(@NotNull String propertyName, @NotNull String value) {
        return ScratchBirdSecurityRedactor.redactPropertyValue(propertyName, value);
    }

    @NotNull
    public List<String> summaryLines() {
        return List.of(
            "Telemetry enabled by default: " + telemetryEnabledByDefault,
            "Update checks enabled by default: " + updateChecksEnabledByDefault,
            "Diagnostic upload enabled by default: " + diagnosticsUploadEnabledByDefault,
            "Allowed endpoint classes: " + String.join(", ", allowedEndpointClasses),
            "Denied endpoint classes: " + String.join(", ", deniedEndpointClasses),
            "Audit events: " + String.join(", ", auditEvents),
            "Secret properties: shared ScratchBirdSecurityRedactor policy");
    }
}
