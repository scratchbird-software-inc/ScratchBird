// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

namespace ScratchBird.Data;

public sealed class ScratchBirdAuthMethodSurface
{
    public byte MethodCode { get; init; }
    public string MethodName { get; init; } = string.Empty;
    public string PluginId { get; init; } = string.Empty;
    public bool ExecutableLocally { get; init; }
    public bool BrokerRequired { get; init; }
}

public sealed class ScratchBirdAuthProbeResult
{
    public bool Reachable { get; init; }
    public string FrontDoorMode { get; init; } = "direct";
    public byte RequiredMethodCode { get; init; }
    public string RequiredMethodName { get; init; } = string.Empty;
    public string RequiredAuthPluginId { get; init; } = string.Empty;
    public bool RequiredMethodBrokerRequired { get; init; }
    public IReadOnlyList<ScratchBirdAuthMethodSurface> AdmittedMethods { get; init; } =
        Array.Empty<ScratchBirdAuthMethodSurface>();
}

public sealed class ScratchBirdResolvedAuthContext
{
    public string FrontDoorMode { get; set; } = "direct";
    public bool Attached { get; set; }
    public bool ManagerAuthenticated { get; set; }
    public byte ResolvedMethodCode { get; set; }
    public string ResolvedMethodName { get; set; } = string.Empty;
    public string ResolvedAuthPluginId { get; set; } = string.Empty;

    internal ScratchBirdResolvedAuthContext Clone()
    {
        return new ScratchBirdResolvedAuthContext
        {
            FrontDoorMode = FrontDoorMode,
            Attached = Attached,
            ManagerAuthenticated = ManagerAuthenticated,
            ResolvedMethodCode = ResolvedMethodCode,
            ResolvedMethodName = ResolvedMethodName,
            ResolvedAuthPluginId = ResolvedAuthPluginId
        };
    }
}
