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

public class ConfigTests
{
    [Fact]
    public void ParseUriDsn()
    {
        var cfg = ScratchBirdConfig.FromConnectionString("scratchbird://user:pass@localhost:3092/mydb?sslmode=require&allow_insecure=true&connect_timeout=3&application_name=app&binary_transfer=false&compression=zstd");

        Assert.Equal("localhost", cfg.Host);
        Assert.Equal(3092, cfg.Port);
        Assert.Equal("mydb", cfg.Database);
        Assert.Equal("user", cfg.Username);
        Assert.Equal("pass", cfg.Password);
        Assert.Equal("require", cfg.SslMode);
        Assert.True(cfg.AllowInsecureDisable);
        Assert.Equal(3000, cfg.ConnectTimeoutMs);
        Assert.Equal("app", cfg.ApplicationName);
        Assert.False(cfg.BinaryTransfer);
        Assert.Equal("zstd", cfg.Compression);
    }

    [Fact]
    public void ParseKeyValueDsn()
    {
        var cfg = ScratchBirdConfig.FromConnectionString("Host=server;Port=4000;Database=db;Username=me;Password=secret;SSL Mode=prefer;AllowInsecure=true;Timeout=5;Socket_Timeout=7");

        Assert.Equal("server", cfg.Host);
        Assert.Equal(4000, cfg.Port);
        Assert.Equal("db", cfg.Database);
        Assert.Equal("me", cfg.Username);
        Assert.Equal("secret", cfg.Password);
        Assert.Equal("prefer", cfg.SslMode);
        Assert.True(cfg.AllowInsecureDisable);
        Assert.Equal(5000, cfg.ConnectTimeoutMs);
        Assert.Equal(7000, cfg.SocketTimeoutMs);
    }

    [Fact]
    public void ParseManagerProxyParams()
    {
        var cfg = ScratchBirdConfig.FromConnectionString("scratchbird://admin:secret@localhost:3092/mydb?front_door_mode=manager_proxy&manager_auth_token=token&manager_client_flags=7");

        Assert.Equal("manager_proxy", cfg.FrontDoorMode);
        Assert.Equal("token", cfg.ManagerAuthToken);
        Assert.Equal((ushort)7, cfg.ManagerClientFlags);
    }

    [Fact]
    public void ParseAuthPluginAndPinningParams()
    {
        var cfg = ScratchBirdConfig.FromConnectionString(
            "scratchbird://user:pass@localhost:3092/mydb" +
            "?client_flags=257&auth_method_id=scratchbird.auth.proxy_assertion" +
            "&auth_method_payload=opaque" +
            "&auth_token=bearer-token" +
            "&auth_payload_json=%7B%22subject%22%3A%22alice%22%7D" +
            "&auth_payload_b64=YWJj" +
            "&auth_provider_profile=corp_primary" +
            "&auth_required_methods=SCRAM_SHA_256%2CTOKEN" +
            "&auth_forbidden_methods=MD5" +
            "&auth_require_channel_binding=true" +
            "&workload_identity_token=jwt-token" +
            "&proxy_principal_assertion=signed-assertion");

        Assert.Equal(257, cfg.ConnectClientFlags);
        Assert.Equal("scratchbird.auth.proxy_assertion", cfg.AuthMethodId);
        Assert.Equal("opaque", cfg.AuthMethodPayload);
        Assert.Equal("bearer-token", cfg.AuthToken);
        Assert.Equal("{\"subject\":\"alice\"}", cfg.AuthPayloadJson);
        Assert.Equal("YWJj", cfg.AuthPayloadB64);
        Assert.Equal("corp_primary", cfg.AuthProviderProfile);
        Assert.Equal("SCRAM_SHA_256,TOKEN", cfg.AuthRequiredMethods);
        Assert.Equal("MD5", cfg.AuthForbiddenMethods);
        Assert.True(cfg.AuthRequireChannelBinding);
        Assert.Equal("jwt-token", cfg.WorkloadIdentityToken);
        Assert.Equal("signed-assertion", cfg.ProxyPrincipalAssertion);
    }

    [Fact]
    public void ParseAuthTokenAliases()
    {
        var uriCfg = ScratchBirdConfig.FromConnectionString(
            "scratchbird://user:pass@localhost:3092/mydb?token=primary-token");
        Assert.Equal("primary-token", uriCfg.AuthToken);

        var kvCfg = ScratchBirdConfig.FromConnectionString(
            "Host=localhost;Port=3092;Database=mydb;Username=user;Password=pass;BearerToken=secondary-token");
        Assert.Equal("secondary-token", kvCfg.AuthToken);
    }

    [Fact]
    public void ParsePoolingOptions()
    {
        var cfg = ScratchBirdConfig.FromConnectionString("Host=localhost;Port=3092;Database=pooling;Username=app;Password=secret;Pooling=true;MinPoolSize=2;MaxPoolSize=25;ConnectionLifetime=60;PoolAcquireTimeoutMs=150");

        Assert.True(cfg.Pooling);
        Assert.Equal(2, cfg.MinPoolSize);
        Assert.Equal(25, cfg.MaxPoolSize);
        Assert.Equal(60, cfg.ConnectionLifetime);
        Assert.Equal(150, cfg.PoolAcquireTimeoutMs);
    }

    [Fact]
    public void ParsePoolingAcquireTimeoutAliases()
    {
        var kvSeconds = ScratchBirdConfig.FromConnectionString(
            "Host=localhost;Port=3092;Database=pooling;Username=app;Password=secret;Pooling=true;PoolingAcquireTimeout=2");
        Assert.Equal(2000, kvSeconds.PoolAcquireTimeoutMs);

        var uriSeconds = ScratchBirdConfig.FromConnectionString(
            "scratchbird://app:secret@localhost:3092/pooling?pool_acquire_timeout=3");
        Assert.Equal(3000, uriSeconds.PoolAcquireTimeoutMs);

        var uriMs = ScratchBirdConfig.FromConnectionString(
            "scratchbird://app:secret@localhost:3092/pooling?pool_acquire_timeout_ms=125");
        Assert.Equal(125, uriMs.PoolAcquireTimeoutMs);
    }

    [Fact]
    public void ParseCircuitBreakerOptions()
    {
        var uriCfg = ScratchBirdConfig.FromConnectionString(
            "scratchbird://app:secret@localhost:3092/mydb" +
            "?cb_failure_threshold=3&cb_recovery_timeout_ms=5000&cb_success_threshold=2&cb_half_open_max_requests=4");

        Assert.Equal(3, uriCfg.CircuitBreakerFailureThreshold);
        Assert.Equal(5000, uriCfg.CircuitBreakerRecoveryTimeoutMs);
        Assert.Equal(2, uriCfg.CircuitBreakerSuccessThreshold);
        Assert.Equal(4, uriCfg.CircuitBreakerHalfOpenMaxRequests);

        var kvCfg = ScratchBirdConfig.FromConnectionString(
            "Host=localhost;Port=3092;Database=mydb;CircuitBreakerFailureThreshold=5;" +
            "CircuitBreakerRecoveryTimeoutMs=7000;CircuitBreakerSuccessThreshold=3;CircuitBreakerHalfOpenMaxRequests=2");

        Assert.Equal(5, kvCfg.CircuitBreakerFailureThreshold);
        Assert.Equal(7000, kvCfg.CircuitBreakerRecoveryTimeoutMs);
        Assert.Equal(3, kvCfg.CircuitBreakerSuccessThreshold);
        Assert.Equal(2, kvCfg.CircuitBreakerHalfOpenMaxRequests);
    }

    [Fact]
    public void ParseKeepaliveOptions()
    {
        var uriCfg = ScratchBirdConfig.FromConnectionString(
            "scratchbird://app:secret@localhost:3092/mydb" +
            "?keepalive_interval_ms=3000&keepalive_max_idle_before_check_ms=9000&keepalive_validation_timeout_ms=400");
        Assert.Equal(3000, uriCfg.KeepaliveIntervalMs);
        Assert.Equal(9000, uriCfg.KeepaliveMaxIdleBeforeCheckMs);
        Assert.Equal(400, uriCfg.KeepaliveValidationTimeoutMs);

        var kvCfg = ScratchBirdConfig.FromConnectionString(
            "Host=localhost;Port=3092;Database=mydb;keepalive=0;" +
            "keepalive_max_idle_ms=7000;keepalivevalidationtimeoutms=250");
        Assert.Equal(0, kvCfg.KeepaliveIntervalMs);
        Assert.Equal(7000, kvCfg.KeepaliveMaxIdleBeforeCheckMs);
        Assert.Equal(250, kvCfg.KeepaliveValidationTimeoutMs);
    }

    [Fact]
    public void ParseLeakOptions()
    {
        var uriCfg = ScratchBirdConfig.FromConnectionString(
            "scratchbird://app:secret@localhost:3092/mydb?leak_threshold_ms=45000&leak_capture_stack=true");
        Assert.Equal(45000, uriCfg.LeakThresholdMs);
        Assert.True(uriCfg.LeakCaptureStackTrace);

        var kvCfg = ScratchBirdConfig.FromConnectionString(
            "Host=localhost;Port=3092;Database=mydb;leakthresholdms=0;leak_capture_stack_trace=1");
        Assert.Equal(0, kvCfg.LeakThresholdMs);
        Assert.True(kvCfg.LeakCaptureStackTrace);
    }

    [Fact]
    public void ParsePipelineOptions()
    {
        var uriCfg = ScratchBirdConfig.FromConnectionString(
            "scratchbird://app:secret@localhost:3092/mydb" +
            "?pipeline_max_in_flight=32" +
            "&pipeline_auto_flush=false" +
            "&pipeline_auto_flush_threshold=7" +
            "&pipeline_flush_timeout_ms=1200");
        Assert.Equal(32, uriCfg.PipelineMaxInFlight);
        Assert.False(uriCfg.PipelineAutoFlush);
        Assert.Equal(7, uriCfg.PipelineAutoFlushThreshold);
        Assert.Equal(1200, uriCfg.PipelineFlushTimeoutMs);

        var kvCfg = ScratchBirdConfig.FromConnectionString(
            "Host=localhost;Port=3092;Database=mydb;" +
            "pipelinemaxinflight=0;pipelineautoflush=1;pipelineautoflushthreshold=9;pipelineflushtimeoutms=25");
        Assert.Equal(0, kvCfg.PipelineMaxInFlight);
        Assert.True(kvCfg.PipelineAutoFlush);
        Assert.Equal(9, kvCfg.PipelineAutoFlushThreshold);
        Assert.Equal(25, kvCfg.PipelineFlushTimeoutMs);
    }

    [Fact]
    public void ParseMetadataExpandSchemaParentsAliases()
    {
        var aliases = new[]
        {
            "metadataExpandSchemaParents",
            "metadata_expand_schema_parents",
            "expandSchemaParents",
            "expand_schema_parents",
            "dbeaverExpandSchemaParents",
            "dbeaver_expand_schema_parents"
        };

        foreach (var alias in aliases)
        {
            var uriCfg = ScratchBirdConfig.FromConnectionString($"scratchbird://user:pass@localhost:3092/mydb?{alias}=true");
            Assert.True(uriCfg.MetadataExpandSchemaParents);

            var kvCfg = ScratchBirdConfig.FromConnectionString($"Host=localhost;Port=3092;Database=mydb;{alias}=1");
            Assert.True(kvCfg.MetadataExpandSchemaParents);
        }
    }

    [Fact]
    public void ParseTelemetryOptions()
    {
        var uriCfg = ScratchBirdConfig.FromConnectionString(
            "scratchbird://user:pass@localhost:3092/mydb" +
            "?telemetry_enable_tracing=false" +
            "&telemetry_enable_metrics=false" +
            "&telemetry_enable_slow_operation_log=true" +
            "&telemetry_slow_operation_threshold_ms=7" +
            "&telemetry_slow_operation_max_entries=3" +
            "&telemetry_sample_rate=0.25" +
            "&telemetry_sanitize_statements=false");

        Assert.False(uriCfg.TelemetryEnableTracing);
        Assert.False(uriCfg.TelemetryEnableMetrics);
        Assert.True(uriCfg.TelemetryEnableSlowOperationLog);
        Assert.Equal(7, uriCfg.TelemetrySlowOperationThresholdMs);
        Assert.Equal(3, uriCfg.TelemetrySlowOperationMaxEntries);
        Assert.Equal(0.25d, uriCfg.TelemetrySampleRate);
        Assert.False(uriCfg.TelemetrySanitizeStatements);

        var kvCfg = ScratchBirdConfig.FromConnectionString(
            "Host=localhost;Port=3092;Database=mydb;telemetry=true;" +
            "telemetry_metrics=0;telemetry_slow_query_log=1;" +
            "telemetry_slow_query_threshold_ms=9;telemetry_slow_query_max_entries=2;telemetry_sample_rate=9;" +
            "telemetry_sanitize_queries=0");

        Assert.True(kvCfg.TelemetryEnableTracing);
        Assert.False(kvCfg.TelemetryEnableMetrics);
        Assert.True(kvCfg.TelemetryEnableSlowOperationLog);
        Assert.Equal(9, kvCfg.TelemetrySlowOperationThresholdMs);
        Assert.Equal(2, kvCfg.TelemetrySlowOperationMaxEntries);
        Assert.Equal(1d, kvCfg.TelemetrySampleRate);
        Assert.False(kvCfg.TelemetrySanitizeStatements);
    }
}
