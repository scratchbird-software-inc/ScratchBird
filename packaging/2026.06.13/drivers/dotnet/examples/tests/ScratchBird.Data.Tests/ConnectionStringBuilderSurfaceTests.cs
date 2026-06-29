// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using ScratchBird.Data;
using Xunit;

namespace ScratchBird.Data.Tests;

public class ConnectionStringBuilderSurfaceTests
{
    [Fact]
    public void PipelineProperties_HaveExpectedDefaults()
    {
        var builder = new ScratchBirdConnectionStringBuilder();

        Assert.Equal(100, builder.PipelineMaxInFlight);
        Assert.True(builder.PipelineAutoFlush);
        Assert.Equal(10, builder.PipelineAutoFlushThreshold);
        Assert.Equal(5000, builder.PipelineFlushTimeoutMs);
    }

    [Fact]
    public void PipelineProperties_RoundTripThroughConfigParsing()
    {
        var builder = new ScratchBirdConnectionStringBuilder
        {
            Host = "localhost",
            Port = 3092,
            Database = "main",
            PipelineMaxInFlight = 7,
            PipelineAutoFlush = false,
            PipelineAutoFlushThreshold = 3,
            PipelineFlushTimeoutMs = 750
        };

        var cfg = ScratchBirdConfig.FromConnectionString(builder.ConnectionString);
        Assert.Equal(7, cfg.PipelineMaxInFlight);
        Assert.False(cfg.PipelineAutoFlush);
        Assert.Equal(3, cfg.PipelineAutoFlushThreshold);
        Assert.Equal(750, cfg.PipelineFlushTimeoutMs);
    }

    [Fact]
    public void AuthBootstrapProperties_RoundTripThroughConfigParsing()
    {
        var builder = new ScratchBirdConnectionStringBuilder
        {
            Host = "localhost",
            Port = 3092,
            Database = "main",
            FrontDoorMode = "manager_proxy",
            AuthMethodId = "scratchbird.auth.proxy_assertion",
            AuthMethodPayload = "opaque-payload",
            AuthToken = "bearer-token",
            AuthPayloadJson = "{\"sub\":\"alice\"}",
            AuthPayloadB64 = "YWJj",
            AuthProviderProfile = "corp_primary",
            AuthRequiredMethods = "SCRAM_SHA_512,TOKEN",
            AuthForbiddenMethods = "MD5",
            AuthRequireChannelBinding = true,
            WorkloadIdentityToken = "workload-jwt",
            ProxyPrincipalAssertion = "signed-assertion",
            ManagerAuthToken = "manager-token"
        };

        var cfg = ScratchBirdConfig.FromConnectionString(builder.ConnectionString);
        Assert.Equal("manager_proxy", cfg.FrontDoorMode);
        Assert.Equal("scratchbird.auth.proxy_assertion", cfg.AuthMethodId);
        Assert.Equal("opaque-payload", cfg.AuthMethodPayload);
        Assert.Equal("bearer-token", cfg.AuthToken);
        Assert.Equal("{\"sub\":\"alice\"}", cfg.AuthPayloadJson);
        Assert.Equal("YWJj", cfg.AuthPayloadB64);
        Assert.Equal("corp_primary", cfg.AuthProviderProfile);
        Assert.Equal("SCRAM_SHA_512,TOKEN", cfg.AuthRequiredMethods);
        Assert.Equal("MD5", cfg.AuthForbiddenMethods);
        Assert.True(cfg.AuthRequireChannelBinding);
        Assert.Equal("workload-jwt", cfg.WorkloadIdentityToken);
        Assert.Equal("signed-assertion", cfg.ProxyPrincipalAssertion);
        Assert.Equal("manager-token", cfg.ManagerAuthToken);
    }
}
